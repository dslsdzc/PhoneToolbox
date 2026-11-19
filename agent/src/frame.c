#include "frame.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int64_t frame_monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_readable(int fd, int64_t deadline_ms)
{
    if (deadline_ms < 0)
        return 0;

    int64_t remaining = deadline_ms - frame_monotonic_ms();
    if (remaining <= 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (remaining > 3600 * 1000)
        remaining = 3600 * 1000;

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    int result;
    do {
        result = poll(&pfd, 1, (int)remaining);
    } while (result < 0 && errno == EINTR);

    if (result == 0) {
        errno = ETIMEDOUT;
        return -1;
    }
    if (result < 0)
        return -1;
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
        && !(pfd.revents & POLLIN)) {
        errno = ECONNRESET;
        return -1;
    }
    return 0;
}

static int read_full_until(int fd, void *buffer, size_t length,
                           int64_t deadline_ms)
{
    uint8_t *cursor = (uint8_t *)buffer;
    while (length > 0) {
        if (wait_readable(fd, deadline_ms) != 0)
            return -1;
        const ssize_t got = read(fd, cursor, length);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // SO_RCVTIMEO fired. Without an absolute deadline this is a
                // normal idle timeout; with one, only the deadline decides.
                if (deadline_ms < 0 || frame_monotonic_ms() >= deadline_ms)
                    return -1;
                continue;
            }
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

int frame_read_until(int fd, uint8_t **payload, uint32_t *length,
                     uint32_t max_length, int64_t deadline_ms)
{
    if (!payload || !length)
        return -2;

    uint8_t header[FRAME_HEADER_SIZE];
    if (read_full_until(fd, header, sizeof(header), deadline_ms) != 0)
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

    if (read_full_until(fd, buffer, size, deadline_ms) != 0) {
        free(buffer);
        return -1;
    }

    *payload = buffer;
    *length = size;
    return 0;
}

int frame_read(int fd, uint8_t **payload, uint32_t *length, uint32_t max_length)
{
    return frame_read_until(fd, payload, length, max_length, -1);
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
