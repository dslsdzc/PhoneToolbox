#include "util.h"

#include <sodium.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void sha256_hex(const void *data, size_t length, char out_hex[65])
{
    uint8_t digest[crypto_hash_sha256_BYTES];
    crypto_hash_sha256(digest, (const unsigned char *)data, length);
    sodium_bin2hex(out_hex, 65, digest, sizeof(digest));
}

int random_hex(size_t bytes, char **out_hex)
{
    if (!out_hex || bytes == 0 || bytes > 256)
        return -1;

    uint8_t buffer[256];
    randombytes_buf(buffer, bytes);

    char *hex = malloc(bytes * 2 + 1);
    if (!hex)
        return -1;
    sodium_bin2hex(hex, bytes * 2 + 1, buffer, bytes);
    *out_hex = hex;
    return 0;
}

int path_is_safe(const char *path)
{
    if (!path || path[0] != '/')
        return 0;

    const size_t length = strlen(path);
    if (length < 2 || length > 4096)
        return 0;

    // 目录形式（以 '/' 结尾）不是本协议的文件目标。
    if (path[length - 1] == '/')
        return 0;

    // 拒绝控制字符与换行（日志/协议可读性）。
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = (unsigned char)path[i];
        if (c < 0x20 || c == 0x7f)
            return 0;
    }

    // 逐段检查 "." / ".."，并拒绝连续 '//'（避免规范化歧义）。
    const char *segment = path + 1;
    while (*segment) {
        const char *slash = strchr(segment, '/');
        const size_t segment_length = slash ? (size_t)(slash - segment) : strlen(segment);

        if (segment_length == 0)
            return 0;
        if (segment_length == 1 && segment[0] == '.')
            return 0;
        if (segment_length == 2 && segment[0] == '.' && segment[1] == '.')
            return 0;

        if (!slash)
            break;
        segment = slash + 1;
    }

    return 1;
}

int read_whole_file(const char *path, uint8_t **out, size_t *length)
{
    if (!path || !out || !length)
        return -1;

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;

    size_t capacity = 64 * 1024;
    size_t used = 0;
    uint8_t *buffer = malloc(capacity);
    if (!buffer) {
        close(fd);
        return -1;
    }

    for (;;) {
        if (used == capacity) {
            if (capacity >= 64u * 1024u * 1024u) { // 64 MiB 上限，防止误读超大文件
                free(buffer);
                close(fd);
                return -1;
            }
            capacity *= 2;
            uint8_t *grown = realloc(buffer, capacity);
            if (!grown) {
                free(buffer);
                close(fd);
                return -1;
            }
            buffer = grown;
        }

        const ssize_t got = read(fd, buffer + used, capacity - used);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            free(buffer);
            close(fd);
            return -1;
        }
        if (got == 0)
            break;
        used += (size_t)got;
    }

    close(fd);
    *out = buffer;
    *length = used;
    return 0;
}
