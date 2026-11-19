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

static int recover_prev(int fd, uint8_t prev[32])
{
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;

    memset(prev, 0, 32);
    if (st.st_size == 0)
        return 0; // 新日志：prev 全零

    // 读文件尾部，找最后一个完整行，取最后 64 字符作为 prev。
    const off_t tail_size = st.st_size > 1024 ? 1024 : st.st_size;
    char tail[1025];
    if (pread(fd, tail, (size_t)tail_size, st.st_size - tail_size) != tail_size)
        return -1;
    tail[tail_size] = '\0';

    // 去掉尾部换行，定位最后一行的起点。
    size_t end = (size_t)tail_size;
    while (end > 0 && (tail[end - 1] == '\n' || tail[end - 1] == '\r'))
        --end;
    if (end < 64)
        return -1;

    char hex[65];
    memcpy(hex, tail + end - 64, 64);
    hex[64] = '\0';

    if (sodium_hex2bin(prev, 32, hex, 64, NULL, NULL, NULL) != 0)
        return -1;
    return 0;
}

int audit_open(audit_log *log, const char *path)
{
    if (!log || !path)
        return -1;
    log->fd = -1;

    const int fd = open(path, O_RDWR | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    // Older builds created 0644 audit.log; tighten it on open.
    if (fchmod(fd, 0600) != 0) {
        close(fd);
        return -1;
    }

    if (recover_prev(fd, log->prev) != 0) {
        close(fd);
        return -1;
    }

    if (pthread_mutex_init(&log->mutex, NULL) != 0) {
        close(fd);
        return -1;
    }

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

    // 哈希输入：字段之间用 '\n' 分隔，避免拼接歧义。
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    crypto_hash_sha256_update(&state, log->prev, 32);
    crypto_hash_sha256_update(&state, (const unsigned char *)timestamp, strlen(timestamp));
    crypto_hash_sha256_update(&state, (const unsigned char *)"\n", 1);
    crypto_hash_sha256_update(&state, (const unsigned char *)peer, strlen(peer));
    crypto_hash_sha256_update(&state, (const unsigned char *)"\n", 1);
    crypto_hash_sha256_update(&state, (const unsigned char *)op, strlen(op));
    crypto_hash_sha256_update(&state, (const unsigned char *)"\n", 1);
    crypto_hash_sha256_update(&state, (const unsigned char *)argv_sha256, strlen(argv_sha256));
    crypto_hash_sha256_update(&state, (const unsigned char *)"\n", 1);
    crypto_hash_sha256_update(&state, (const unsigned char *)exit_text, strlen(exit_text));

    uint8_t digest[32];
    crypto_hash_sha256_final(&state, digest);

    char line[512];
    const int length = snprintf(line, sizeof(line), "%s %s %s %s %s %s ",
                                prev_hex, timestamp, peer, op, argv_sha256, exit_text);
    if (length <= 0 || (size_t)length + 65 > sizeof(line)) {
        pthread_mutex_unlock(&log->mutex);
        return -1;
    }

    sodium_bin2hex(line + length, sizeof(line) - (size_t)length, digest, sizeof(digest));

    const size_t total = strlen(line);
    if (write(log->fd, line, total) != (ssize_t)total) {
        pthread_mutex_unlock(&log->mutex);
        return -1;
    }
    if (write(log->fd, "\n", 1) != 1) {
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
