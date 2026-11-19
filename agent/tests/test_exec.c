#include "exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            return 1;                                                            \
        }                                                                        \
    } while (0)

static long elapsed_ms(const struct timespec *start, const struct timespec *end)
{
    return (end->tv_sec - start->tv_sec) * 1000L
        + (end->tv_nsec - start->tv_nsec) / 1000000L;
}

int main(void)
{
    exec_result result = {0};
    const char *echo_argv[] = {"/bin/echo", "hello", NULL};
    CHECK(run_command(echo_argv, 2000, 4096, &result) == 0);
    CHECK(result.exit_code == 0);
    CHECK(!result.timed_out);
    CHECK(result.stdout_length == 6);
    CHECK(memcmp(result.stdout_data, "hello\n", 6) == 0);
    exec_result_free(&result);

    const char *exit_argv[] = {"/bin/sh", "-c", "exit 3", NULL};
    CHECK(run_command(exit_argv, 2000, 4096, &result) == 0);
    CHECK(result.exit_code == 3);
    CHECK(!result.timed_out);
    exec_result_free(&result);

    const char *slow_argv[] = {"/bin/sh", "-c", "sleep 2", NULL};
    CHECK(run_command(slow_argv, 100, 4096, &result) == 0);
    CHECK(result.timed_out == 1);
    exec_result_free(&result);

    // A background grandchild inherits stdout/stderr. The timeout must still
    // apply after the shell exits, and the whole process group must be killed.
    const char *background_argv[] = {"/bin/sh", "-c", "sleep 5 &", NULL};
    struct timespec started;
    struct timespec finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    CHECK(run_command(background_argv, 200, 4096, &result) == 0);
    clock_gettime(CLOCK_MONOTONIC, &finished);
    CHECK(result.timed_out == 1);
    CHECK(elapsed_ms(&started, &finished) < 2000);
    exec_result_free(&result);

    const char *long_argv[] = {"/bin/sh", "-c", "printf 1234567890", NULL};
    CHECK(run_command(long_argv, 2000, 4, &result) == 0);
    CHECK(result.stdout_length == 4);
    CHECK(result.truncated == 1);
    CHECK(memcmp(result.stdout_data, "1234", 4) == 0);
    exec_result_free(&result);

    printf("test_exec: OK\n");
    return 0;
}
