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

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif

static void set_error(char *error, size_t error_len, const char *message)
{
    if (error && error_len > 0)
        snprintf(error, error_len, "%s", message);
}

// Open an absolute path without following symlinks in any component. This
// closes the lstat()-then-open() race for pull/sha256, where an attacker could
// swap an intermediate directory for a symlink.
static int open_nofollow(const char *path, int final_flags)
{
    if (!path || path[0] != '/')
        return open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | final_flags);

    int dir = open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (dir < 0)
        return -1;

    const char *cursor = path;
    while (*cursor != '\0') {
        while (*cursor == '/')
            ++cursor;
        if (*cursor == '\0')
            break;

        char name[256];
        size_t length = 0;
        while (cursor[length] != '\0' && cursor[length] != '/'
               && length < sizeof(name) - 1) {
            name[length] = cursor[length];
            ++length;
        }
        if (cursor[length] != '\0' && cursor[length] != '/') {
            close(dir);
            return -1;
        }
        name[length] = '\0';

        const char *next = cursor + length;
        while (*next == '/')
            ++next;
        if (strcmp(name, "..") == 0) {
            close(dir);
            return -1;
        }
        if (strcmp(name, ".") == 0) {
            cursor = next;
            continue;
        }

        const int fd = openat(dir, name,
                              O_RDONLY | O_CLOEXEC | O_NOFOLLOW
                                  | final_flags
                                  | (*next != '\0' ? O_DIRECTORY : 0));
        close(dir);
        if (fd < 0)
            return -1;
        dir = fd;
        cursor = next;
    }
    return dir;
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
    const int fd = open_nofollow(path, 0);
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

static int sha256_fd(int fd, char out_hex[65])
{
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);

    uint8_t buffer[65536];
    off_t offset = 0;
    for (;;) {
        const ssize_t got = pread(fd, buffer, sizeof(buffer), offset);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (got == 0)
            break;
        crypto_hash_sha256_update(&state, buffer, (size_t)got);
        offset += got;
    }

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
    session->dir_fd = -1;

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

    // Split into parent directory + leaf and hold the parent dirfd. All
    // subsequent fstatat/openat/renameat calls are relative to that fd, so a
    // symlink swapped into an intermediate path component cannot redirect the
    // write after the lexical check.
    char *path_copy = strdup(path);
    if (!path_copy) {
        set_error(error, error_len, "out of memory");
        return -1;
    }
    char *slash = strrchr(path_copy, '/');
    if (!slash || slash[1] == '\0') {
        free(path_copy);
        set_error(error, error_len, "invalid target path");
        return -1;
    }
    char *target_name = strdup(slash + 1);
    if (!target_name) {
        free(path_copy);
        set_error(error, error_len, "out of memory");
        return -1;
    }
    if (slash == path_copy)
        slash[1] = '\0';                // parent = "/"
    else
        *slash = '\0';

    const int dir_fd = open_nofollow(path_copy, O_RDONLY | O_DIRECTORY);
    free(path_copy);
    if (dir_fd < 0) {
        free(target_name);
        set_error(error, error_len, "cannot open target directory");
        return -1;
    }

    struct stat st;
    if (fstatat(dir_fd, target_name, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(st.st_mode)) {
            close(dir_fd);
            free(target_name);
            set_error(error, error_len, "target is a symlink");
            return -1;
        }
        if (!S_ISREG(st.st_mode)) {
            close(dir_fd);
            free(target_name);
            set_error(error, error_len, "target is not a regular file");
            return -1;
        }
        if (!overwrite) {
            close(dir_fd);
            free(target_name);
            set_error(error, error_len, "target exists (overwrite not allowed)");
            return -1;
        }
    } else if (errno != ENOENT) {
        close(dir_fd);
        free(target_name);
        set_error(error, error_len, "cannot stat target");
        return -1;
    }

    char *random_suffix = NULL;
    if (random_hex(8, &random_suffix) != 0) {
        close(dir_fd);
        free(target_name);
        set_error(error, error_len, "out of memory");
        return -1;
    }

    const size_t temp_length = strlen(target_name) + strlen(random_suffix) + 16;
    char *temp_name = malloc(temp_length);
    if (!temp_name) {
        free(random_suffix);
        close(dir_fd);
        free(target_name);
        set_error(error, error_len, "out of memory");
        return -1;
    }
    snprintf(temp_name, temp_length, "%s.tmp.%s", target_name, random_suffix);
    free(random_suffix);

    const int fd = openat(dir_fd, temp_name,
                          O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        free(temp_name);
        close(dir_fd);
        free(target_name);
        set_error(error, error_len, "cannot create temporary file");
        return -1;
    }

    session->fd = fd;
    session->dir_fd = dir_fd;
    session->active = 1;
    session->temp_name = temp_name;
    session->target_name = target_name;
    session->expected_size = size;
    session->written = 0;
    session->mode = (uint32_t)parsed_mode;
    session->overwrite = overwrite;
    snprintf(session->expected_sha256, sizeof(session->expected_sha256), "%s", sha256);
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

    char actual_sha256[65];
    if (sha256_fd(session->fd, actual_sha256) != 0) {
        set_error(error, error_len, "cannot hash temporary file");
        push_abort(session);
        return -1;
    }
    if (strcmp(actual_sha256, session->expected_sha256) != 0) {
        set_error(error, error_len, "sha256 mismatch");
        push_abort(session);
        return -1;
    }

    if (fchmod(session->fd, (mode_t)session->mode) != 0) {
        set_error(error, error_len, "chmod failed");
        push_abort(session);
        return -1;
    }
    close(session->fd);
    session->fd = -1;

    if (renameat(session->dir_fd, session->temp_name,
                 session->dir_fd, session->target_name) != 0) {
        set_error(error, error_len, "rename failed");
        push_abort(session);
        return -1;
    }
    // Durability of the directory entry, not just the file content.
    fsync(session->dir_fd);

    close(session->dir_fd);
    free(session->temp_name);
    free(session->target_name);
    memset(session, 0, sizeof(*session));
    session->fd = -1;
    session->dir_fd = -1;
    return 0;
}

void push_abort(push_session *session)
{
    if (!session)
        return;
    if (session->fd >= 0)
        close(session->fd);
    if (session->dir_fd >= 0 && session->temp_name)
        unlinkat(session->dir_fd, session->temp_name, 0);
    if (session->dir_fd >= 0)
        close(session->dir_fd);
    free(session->temp_name);
    free(session->target_name);
    memset(session, 0, sizeof(*session));
    session->fd = -1;
    session->dir_fd = -1;
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

    const int fd = open_nofollow(path, 0);
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
