#include "frame.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            return 1;                                                            \
        }                                                                        \
    } while (0)

int main(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);

    const char message[] = "hello agent";
    CHECK(frame_write(fds[1], message, sizeof(message) - 1) == 0);

    uint8_t *payload = NULL;
    uint32_t length = 0;
    CHECK(frame_read(fds[0], &payload, &length, 1024) == 0);
    CHECK(length == sizeof(message) - 1);
    CHECK(memcmp(payload, message, length) == 0);
    free(payload);

    // 空帧拒绝
    CHECK(frame_write(fds[1], message, 0) == -2);

    // 超过上限拒绝
    CHECK(frame_write(fds[1], "12345", 5) == 0);
    CHECK(frame_read(fds[0], &payload, &length, 4) == -2);

    close(fds[0]);
    close(fds[1]);
    printf("test_frame: OK\n");
    return 0;
}
