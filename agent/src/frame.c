#include "frame.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_full(int fd, void *buffer, size_t length)
{
    uint8_t *cursor = (uint8_t *)buffer;
    while (length > 0) {
        const ssize_t got = read(fd, cursor, length);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (got == 0)
            return -1;
        cursor += (size_t)got;
        length -= (size_t)got;
    }
    return 0;
}

static int write_full(int fd, const void *buffer, size_t length)
{
    const uint8_t *cursor = (const uint8_t *)buffer;
    while (length > 0) {
        const ssize_t wrote = write(fd, cursor, length);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (wrote == 0)
            return -1;
        cursor += (size_t)wrote;
        length -= (size_t)wrote;
    }
    return 0;
}

int frame_read(int fd, uint8_t **payload, uint32_t *length, uint32_t max_length)
{
    if (!payload || !length)
        return -2;

    uint8_t header[FRAME_HEADER_SIZE];
    if (read_full(fd, header, sizeof(header)) != 0)
        return -1;

    const uint32_t size = ((uint32_t)header[0] << 24)
                        | ((uint32_t)header[1] << 16)
                        | ((uint32_t)header[2] << 8)
                        | (uint32_t)header[3];

    if (size == 0 || size > max_length)
        return -2;

    uint8_t *buffer = malloc(size);
    if (!buffer)
        return -3;

    if (read_full(fd, buffer, size) != 0) {
        free(buffer);
        return -1;
    }

    *payload = buffer;
    *length = size;
    return 0;
}

int frame_write(int fd, const void *payload, uint32_t length)
{
    if (length == 0)
        return -2;

    uint8_t header[FRAME_HEADER_SIZE] = {
        (uint8_t)((length >> 24) & 0xff),
        (uint8_t)((length >> 16) & 0xff),
        (uint8_t)((length >> 8) & 0xff),
        (uint8_t)(length & 0xff),
    };

    if (write_full(fd, header, sizeof(header)) != 0)
        return -1;
    if (write_full(fd, payload, length) != 0)
        return -1;
    return 0;
}
