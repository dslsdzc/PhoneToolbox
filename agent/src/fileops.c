#include "fileops.h"
#include "util.h"

#include <sodium.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void set_error(char *error, size_t error_len, const char *message)
{
    if (error && error_len > 0)
        snprintf(error, error_len, "%s", message);
}

static int mode_from_text(const char *text, mode_t *mode)
{
    if (!text || text[0] == '\0') {
        *mode = 0644;
        return 0;
    }

    char *end = NULL;
    const unsigned long value = strtoul(text, &end, 8);
    if (!end || *end != '\0' || value > 0777)
        return -1;
    *mode = (mode_t)value;
    return 0;
}

static int sha256_file(const char *path, char out_hex[65])
{
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);

    uint8_t buffer[65536];
    for (;;) {
        const ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            close(fd);
            return -1;
        }
        if (got == 0)
            break;
        crypto_hash_sha256_update(&state, buffer, (size_t)got);
    }
    close(fd);

    uint8_t digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256_final(&state, digest);
    sodium_bin2hex(out_hex, 65, digest, sizeof(digest));
    return 0;
}

int push_begin(push_session *session, const char *path, uint64_t size,
               const char *mode, const char *sha256, int overwrite,
               char *error, size_t error_len)
{
    if (!session || !path || !sha256) {
        set_error(error, error_len, "invalid arguments");
        return -1;
    }
    memset(session, 0, sizeof(*session));
    session->fd = -1;

    if (!path_is_safe(path)) {
        set_error(error, error_len, "unsafe path");
        return -1;
    }
    if (strlen(sha256) != 64) {
        set_error(error, error_len, "sha256 must be 64 hex characters");
        return -1;
    }

    mode_t parsed_mode;
    if (mode_from_text(mode, &parsed_mode) != 0) {
        set_error(error, error_len, "invalid mode");
        return -1;
    }

    struct stat st;
    if (lstat(path, &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            set_error(error, error_len, "target is a symlink");
            return -1;
        }
        if (!S_ISREG(st.st_mode)) {
            set_error(error, error_len, "target is not a regular file");
            return -1;
        }
        if (!overwrite) {
            set_error(error, error_len, "target exists (overwrite not allowed)");
            return -1;
        }
    } else if (errno != ENOENT) {
        set_error(error, error_len, "cannot stat target");
        return -1;
    }

    char *random_suffix = NULL;
    if (random_hex(8, &random_suffix) != 0) {
        set_error(error, error_len, "out of memory");
        return -1;
    }

    const size_t temp_length = strlen(path) + strlen(random_suffix) + 16;
    char *temp_path = malloc(temp_length);
    if (!temp_path) {
        free(random_suffix);
        set_error(error, error_len, "out of memory");
        return -1;
    }
    snprintf(temp_path, temp_length, "%s.tmp.%s", path, random_suffix);
    free(random_suffix);

    const int fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        free(temp_path);
        set_error(error, error_len, "cannot create temporary file");
        return -1;
    }

    session->fd = fd;
    session->active = 1;
    session->temp_path = temp_path;
    session->target_path = strdup(path);
    session->expected_size = size;
    session->written = 0;
    session->mode = (uint32_t)parsed_mode;
    session->overwrite = overwrite;
    snprintf(session->expected_sha256, sizeof(session->expected_sha256), "%s", sha256);

    if (!session->target_path) {
        push_abort(session);
        set_error(error, error_len, "out of memory");
        return -1;
    }
    return 0;
}

int push_write(push_session *session, uint64_t offset, const uint8_t *data,
               size_t length, char *error, size_t error_len)
{
    if (!session || !session->active || !data) {
        set_error(error, error_len, "push session is not active");
        return -1;
    }
    if (offset != session->written) {
        set_error(error, error_len, "non-contiguous chunk");
        push_abort(session);
        return -1;
    }
    if (length > session->expected_size - session->written) {
        set_error(error, error_len, "chunk exceeds declared size");
        push_abort(session);
        return -1;
    }

    size_t written = 0;
    while (written < length) {
        const ssize_t got = pwrite(session->fd, data + written, length - written,
                                   (off_t)(offset + written));
        if (got < 0) {
            if (errno == EINTR)
                continue;
            set_error(error, error_len, "write failed");
            push_abort(session);
            return -1;
        }
        if (got == 0) {
            set_error(error, error_len, "write returned zero");
            push_abort(session);
            return -1;
        }
        written += (size_t)got;
    }

    session->written += length;
    return 0;
}

int push_commit(push_session *session, char *error, size_t error_len)
{
    if (!session || !session->active) {
        set_error(error, error_len, "push session is not active");
        return -1;
    }

    if (session->written != session->expected_size) {
        set_error(error, error_len, "size mismatch");
        push_abort(session);
        return -1;
    }

    if (fsync(session->fd) != 0) {
        set_error(error, error_len, "fsync failed");
        push_abort(session);
        return -1;
    }
    close(session->fd);
    session->fd = -1;

    char actual_sha256[65];
    if (sha256_file(session->temp_path, actual_sha256) != 0) {
        set_error(error, error_len, "cannot hash temporary file");
        push_abort(session);
        return -1;
    }
    if (strcmp(actual_sha256, session->expected_sha256) != 0) {
        set_error(error, error_len, "sha256 mismatch");
        push_abort(session);
        return -1;
    }

    if (chmod(session->temp_path, (mode_t)session->mode) != 0) {
        set_error(error, error_len, "chmod failed");
        push_abort(session);
        return -1;
    }

    if (rename(session->temp_path, session->target_path) != 0) {
        set_error(error, error_len, "rename failed");
        push_abort(session);
        return -1;
    }

    free(session->temp_path);
    free(session->target_path);
    memset(session, 0, sizeof(*session));
    session->fd = -1;
    return 0;
}

void push_abort(push_session *session)
{
    if (!session)
        return;
    if (session->fd >= 0)
        close(session->fd);
    if (session->temp_path) {
        unlink(session->temp_path);
        free(session->temp_path);
    }
    free(session->target_path);
    memset(session, 0, sizeof(*session));
    session->fd = -1;
}

int pull_meta(const char *path, uint64_t *size, char sha256_hex[65],
              char *error, size_t error_len)
{
    if (!path || !size || !sha256_hex) {
        set_error(error, error_len, "invalid arguments");
        return -1;
    }
    if (!path_is_safe(path)) {
        set_error(error, error_len, "unsafe path");
        return -1;
    }

    struct stat st;
    if (lstat(path, &st) != 0) {
        set_error(error, error_len, "cannot stat file");
        return -1;
    }
    if (S_ISLNK(st.st_mode)) {
        set_error(error, error_len, "target is a symlink");
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        set_error(error, error_len, "target is not a regular file");
        return -1;
    }

    if (sha256_file(path, sha256_hex) != 0) {
        set_error(error, error_len, "cannot hash file");
        return -1;
    }

    *size = (uint64_t)st.st_size;
    return 0;
}

int pull_read(const char *path, uint64_t offset, uint8_t *buffer, size_t length,
              size_t *got, char *error, size_t error_len)
{
    if (!path || !buffer || !got) {
        set_error(error, error_len, "invalid arguments");
        return -1;
    }
    if (!path_is_safe(path)) {
        set_error(error, error_len, "unsafe path");
        return -1;
    }

    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        set_error(error, error_len, "cannot open file");
        return -1;
    }

    ssize_t n;
    do {
        n = pread(fd, buffer, length, (off_t)offset);
    } while (n < 0 && errno == EINTR);

    close(fd);

    if (n < 0) {
        set_error(error, error_len, "read failed");
        return -1;
    }
    *got = (size_t)n;
    return 0;
}

int file_stat(const char *path, uint64_t *size, uint32_t *mode, int *is_dir,
              char *error, size_t error_len)
{
    if (!path) {
        set_error(error, error_len, "invalid arguments");
        return -1;
    }
    if (!path_is_safe(path)) {
        set_error(error, error_len, "unsafe path");
        return -1;
    }

    struct stat st;
    if (lstat(path, &st) != 0) {
        set_error(error, error_len, "cannot stat file");
        return -1;
    }

    if (size)
        *size = (uint64_t)st.st_size;
    if (mode)
        *mode = (uint32_t)(st.st_mode & 07777);
    if (is_dir)
        *is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    return 0;
}
