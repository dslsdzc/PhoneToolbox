#include "audit.h"

#include <sodium.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Rotate before the log can grow without bound. The sidecar anchor preserves
// the chain across rotations (first entry of the new file points at the last
// hash of the rotated file).
#define AUDIT_MAX_BYTES (8u * 1024u * 1024u)

static int write_all(int fd, const char *data, size_t size)
{
    size_t done = 0;
    while (done < size) {
        const ssize_t wrote = write(fd, data + done, size - done);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (wrote == 0)
            return -1;
        done += (size_t) wrote;
    }
    return 0;
}

// Hash input: prev || timestamp || '\n' || peer || '\n' || op || '\n' ||
// argv_sha256 || '\n' || exit. All fields are single tokens (no spaces/newlines).
static void audit_entry_hash(const uint8_t prev[32], const char *timestamp,
                             const char *peer, const char *op,
                             const char *argv_sha256, const char *exit_text,
                             uint8_t out[32])
{
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    crypto_hash_sha256_update(&state, prev, 32);
    crypto_hash_sha256_update(&state, (const unsigned char *) timestamp,
                              strlen(timestamp));
    crypto_hash_sha256_update(&state, (const unsigned char *) "\n", 1);
    crypto_hash_sha256_update(&state, (const unsigned char *) peer,
                              strlen(peer));
    crypto_hash_sha256_update(&state, (const unsigned char *) "\n", 1);
    crypto_hash_sha256_update(&state, (const unsigned char *) op, strlen(op));
    crypto_hash_sha256_update(&state, (const unsigned char *) "\n", 1);
    crypto_hash_sha256_update(&state, (const unsigned char *) argv_sha256,
                              strlen(argv_sha256));
    crypto_hash_sha256_update(&state, (const unsigned char *) "\n", 1);
    crypto_hash_sha256_update(&state, (const unsigned char *) exit_text,
                              strlen(exit_text));
    crypto_hash_sha256_final(&state, out);
}

// Returns 0 only if every line parses, its prev matches the previous entry and
// its entry hash matches the recomputed value. *out_prev receives the last hash.
static int audit_verify_chain(const char *path, const uint8_t initial[32],
                              uint8_t out_prev[32])
{
    memcpy(out_prev, initial, 32);

    FILE *file = fopen(path, "r");
    if (!file)
        return errno == ENOENT ? 0 : -1;

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        const size_t line_length = strlen(line);
        if (line_length == sizeof(line) - 1 && line[line_length - 1] != '\n') {
            int c;
            while ((c = fgetc(file)) != '\n' && c != EOF) {
            }
            fclose(file);
            return -1;
        }

        char prev_hex[65] = {0};
        char timestamp[48] = {0};
        char peer[65] = {0};
        char op[16] = {0};
        char argv_sha256[65] = {0};
        char exit_text[16] = {0};
        char entry_hex[65] = {0};
        int consumed = 0;
        if (sscanf(line,
                   "%64s %47s %64s %15s %64s %15s %64s%n",
                   prev_hex, timestamp, peer, op, argv_sha256, exit_text,
                   entry_hex, &consumed) != 7
            || consumed <= 0) {
            fclose(file);
            return -1;
        }
        for (const char *rest = line + consumed; *rest != '\0'; ++rest) {
            if (*rest != ' ' && *rest != '\t' && *rest != '\r'
                && *rest != '\n') {
                fclose(file);
                return -1;
            }
        }
        // Peer is either an old short marker (peer1/stage1) or a 64-hex
        // fingerprint. All fields are single tokens; spaces cannot appear.
        if (peer[0] == '\0' || strlen(peer) > 64
            || argv_sha256[0] == '\0' || strlen(argv_sha256) > 64
            || strlen(prev_hex) != 64 || strlen(entry_hex) != 64) {
            fclose(file);
            return -1;
        }

        uint8_t prev[32];
        if (sodium_hex2bin(prev, sizeof(prev), prev_hex, 64, NULL, NULL, NULL)
            != 0) {
            fclose(file);
            return -1;
        }
        if (sodium_memcmp(prev, out_prev, 32) != 0) {
            fclose(file);
            return -1;
        }

        uint8_t expected[32];
        audit_entry_hash(out_prev, timestamp, peer, op, argv_sha256, exit_text,
                         expected);
        char expected_hex[65];
        sodium_bin2hex(expected_hex, sizeof(expected_hex), expected, 32);
        if (strcmp(expected_hex, entry_hex) != 0) {
            fclose(file);
            return -1;
        }
        memcpy(out_prev, expected, 32);
    }
    fclose(file);
    return 0;
}

static int write_anchor_file(const char *anchor_path, const uint8_t prev[32])
{
    char hex[65];
    sodium_bin2hex(hex, sizeof(hex), prev, 32);
    const int fd = open(anchor_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                        0600);
    if (fd < 0)
        return -1;
    const int result = write_all(fd, hex, 64);
    fsync(fd);
    close(fd);
    return result;
}

// Best effort: make the rename durable. Old Android filesystems may reject
// directory fsync; the data file itself is still fsynced by write_anchor_file.
static void fsync_parent(const char *path)
{
    char directory[4096];
    const size_t length = strlen(path);
    if (length + 1 > sizeof(directory))
        return;
    memcpy(directory, path, length + 1);
    char *slash = strrchr(directory, '/');
    if (!slash)
        return;
    if (slash == directory)
        slash[1] = '\0';
    else
        *slash = '\0';
    const int fd = open(directory, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    fsync(fd);
    close(fd);
}

static int read_anchor(const char *path, uint8_t anchor[32])
{
    char anchor_path[4096];
    if (snprintf(anchor_path, sizeof(anchor_path), "%s.prev", path)
        >= (int) sizeof(anchor_path))
        return -1;
    FILE *file = fopen(anchor_path, "r");
    if (!file)
        return errno == ENOENT ? 1 : -1;
    char hex[65] = {0};
    const int ok = fscanf(file, "%64s", hex) == 1
        && strlen(hex) == 64
        && sodium_hex2bin(anchor, 32, hex, 64, NULL, NULL, NULL) == 0;
    fclose(file);
    return ok ? 0 : -1;
}

// Recovery for logs rotated by an older build that persisted the new anchor
// before renaming audit.log. In that state the anchor equals the last entry
// hash but verification from the anchor fails on the first line. Verify from
// the chain's own first prev instead; a valid chain whose last hash equals the
// anchor proves the file is intact.
static int verify_after_rotation_crash(const char *path,
                                       const uint8_t anchor[32],
                                       uint8_t out_prev[32])
{
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;
    char line[512];
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return -1;
    }
    fclose(file);

    char first_prev_hex[65] = {0};
    if (sscanf(line, "%64s", first_prev_hex) != 1
        || strlen(first_prev_hex) != 64)
        return -1;
    uint8_t first_prev[32];
    if (sodium_hex2bin(first_prev, sizeof(first_prev), first_prev_hex, 64,
                       NULL, NULL, NULL) != 0)
        return -1;
    if (audit_verify_chain(path, first_prev, out_prev) != 0)
        return -1;
    return sodium_memcmp(out_prev, anchor, 32) == 0 ? 0 : -1;
}

int audit_open(audit_log *log, const char *path)
{
    if (!log || !path)
        return -1;
    log->fd = -1;

    uint8_t anchor[32] = {0};
    const int anchor_state = read_anchor(path, anchor);
    if (anchor_state < 0)
        return -1;
    const uint8_t *initial = anchor_state == 0 ? anchor : (const uint8_t[32]){0};

    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > (off_t) AUDIT_MAX_BYTES) {
        uint8_t last[32];
        int verified = audit_verify_chain(path, initial, last);
        if (verified != 0 && anchor_state == 0)
            verified = verify_after_rotation_crash(path, anchor, last);
        if (verified != 0)
            return -1;

        char rotated[4096];
        char anchor_path[4096];
        char temporary[4096];
        if (snprintf(rotated, sizeof(rotated), "%s.1", path)
                >= (int) sizeof(rotated)
            || snprintf(anchor_path, sizeof(anchor_path), "%s.prev", path)
                >= (int) sizeof(anchor_path)
            || snprintf(temporary, sizeof(temporary), "%s.prev.tmp", path)
                >= (int) sizeof(temporary))
            return -1;

        // Prepare the new anchor first, but do not publish it until the log
        // has been renamed. A crash between the two steps leaves the old
        // anchor matching the still-intact audit.log.
        if (write_anchor_file(temporary, last) != 0)
            return -1;
        if (rename(path, rotated) != 0)
            return -1;
        fsync_parent(path);
        if (rename(temporary, anchor_path) != 0)
            return -1;
        fsync_parent(path);
        memcpy(anchor, last, 32);
        initial = anchor;
    }

    uint8_t last[32];
    if (audit_verify_chain(path, initial, last) != 0) {
        // A corrupt chain is quarantined rather than silently extended.
        char corrupt[4096];
        if (snprintf(corrupt, sizeof(corrupt), "%s.corrupt.%d", path,
                     (int) getpid()) >= (int) sizeof(corrupt))
            return -1;
        if (rename(path, corrupt) != 0 && errno != ENOENT)
            return -1;
        memcpy(last, initial, 32);
    }

    const int fd = open(path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    // Older builds created 0644 audit.log; tighten it on open.
    if (fchmod(fd, 0600) != 0) {
        close(fd);
        return -1;
    }

    if (pthread_mutex_init(&log->mutex, NULL) != 0) {
        close(fd);
        return -1;
    }

    memcpy(log->prev, last, 32);
    log->fd = fd;
    return 0;
}

int audit_append(audit_log *log, const char *peer, const char *op,
                 const char *argv_sha256, int exit_code)
{
    if (!log || log->fd < 0 || !peer || !op || !argv_sha256)
        return -1;

    if (pthread_mutex_lock(&log->mutex) != 0)
        return -1;

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        pthread_mutex_unlock(&log->mutex);
        return -1;
    }

    char timestamp[48];
    snprintf(timestamp, sizeof(timestamp), "%lld.%03ld",
             (long long)ts.tv_sec, ts.tv_nsec / 1000000L);

    char exit_text[16];
    snprintf(exit_text, sizeof(exit_text), "%d", exit_code);

    char prev_hex[65];
    sodium_bin2hex(prev_hex, sizeof(prev_hex), log->prev, 32);

    uint8_t digest[32];
    audit_entry_hash(log->prev, timestamp, peer, op, argv_sha256, exit_text,
                     digest);

    char line[512];
    const int length = snprintf(line, sizeof(line), "%s %s %s %s %s %s ",
                                prev_hex, timestamp, peer, op, argv_sha256,
                                exit_text);
    if (length <= 0 || (size_t)length + 65 > sizeof(line)) {
        pthread_mutex_unlock(&log->mutex);
        return -1;
    }

    sodium_bin2hex(line + length, sizeof(line) - (size_t)length, digest,
                   sizeof(digest));
    const size_t total = strlen(line);
    if (write_all(log->fd, line, total) != 0
        || write_all(log->fd, "\n", 1) != 0) {
        pthread_mutex_unlock(&log->mutex);
        return -1;
    }

    memcpy(log->prev, digest, sizeof(digest));
    pthread_mutex_unlock(&log->mutex);
    return 0;
}

void audit_close(audit_log *log)
{
    if (!log)
        return;
    pthread_mutex_lock(&log->mutex);
    if (log->fd >= 0) {
        close(log->fd);
        log->fd = -1;
    }
    pthread_mutex_unlock(&log->mutex);
    pthread_mutex_destroy(&log->mutex);
}
