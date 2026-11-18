#include "exec.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    uint8_t *data;
    size_t length;
    size_t capacity;
    size_t limit;
    int truncated;
} output_buffer;

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int output_append(output_buffer *buffer, const uint8_t *data, size_t length)
{
    if (buffer->length >= buffer->limit) {
        buffer->truncated = 1;
        return 0;
    }

    size_t accepted = length;
    if (accepted > buffer->limit - buffer->length) {
        accepted = buffer->limit - buffer->length;
        buffer->truncated = 1;
    }
    if (accepted == 0)
        return 0;

    if (buffer->length + accepted > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 4096;
        while (capacity < buffer->length + accepted)
            capacity *= 2;
        if (capacity > buffer->limit)
            capacity = buffer->limit;
        uint8_t *grown = realloc(buffer->data, capacity);
        if (!grown)
            return -1;
        buffer->data = grown;
        buffer->capacity = capacity;
    }

    memcpy(buffer->data + buffer->length, data, accepted);
    buffer->length += accepted;
    return 0;
}

void exec_result_free(exec_result *result)
{
    if (!result)
        return;
    free(result->stdout_data);
    free(result->stderr_data);
    memset(result, 0, sizeof(*result));
}

int run_command(const char *const argv[], int timeout_ms, size_t max_output,
                exec_result *result)
{
    if (!argv || !argv[0] || !result)
        return -1;
    memset(result, 0, sizeof(*result));
    result->exit_code = -1;

    if (max_output == 0)
        max_output = 8u * 1024u * 1024u;

    int stdout_pipe[2];
    int stderr_pipe[2];
    if (pipe2(stdout_pipe, O_CLOEXEC) != 0)
        return -1;
    if (pipe2(stderr_pipe, O_CLOEXEC) != 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return -1;
    }

    const pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        // 子进程：独立进程组，超时可整组 kill。
        setpgid(0, 0);
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
    fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);

    output_buffer out = {0};
    output_buffer err = {0};
    out.limit = max_output;
    err.limit = max_output;

    const int64_t deadline = timeout_ms > 0 ? monotonic_ms() + timeout_ms : 0;
    int child_done = 0;
    int status = 0;
    int stdout_open = 1;
    int stderr_open = 1;
    int timed_out = 0;

    while (!child_done || stdout_open || stderr_open) {
        struct pollfd fds[2];
        nfds_t count = 0;
        if (stdout_open) {
            fds[count].fd = stdout_pipe[0];
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            ++count;
        }
        if (stderr_open) {
            fds[count].fd = stderr_pipe[0];
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            ++count;
        }

        int poll_timeout = 100;
        if (!child_done && deadline > 0) {
            const int64_t remaining = deadline - monotonic_ms();
            if (remaining <= 0) {
                kill(-pid, SIGKILL);
                timed_out = 1;
            } else if (remaining < poll_timeout) {
                poll_timeout = (int)remaining;
            }
        }

        if (count > 0)
            poll(fds, count, poll_timeout);

        // 读 stdout/stderr（非阻塞，直到 EAGAIN）。
        for (int stream = 0; stream < 2; ++stream) {
            const int fd = stream == 0 ? stdout_pipe[0] : stderr_pipe[0];
            int *open_flag = stream == 0 ? &stdout_open : &stderr_open;
            if (!*open_flag)
                continue;

            uint8_t buffer[16384];
            for (;;) {
                const ssize_t got = read(fd, buffer, sizeof(buffer));
                if (got > 0) {
                    output_buffer *target = stream == 0 ? &out : &err;
                    if (output_append(target, buffer, (size_t)got) != 0) {
                        close(stdout_pipe[0]);
                        close(stderr_pipe[0]);
                        kill(-pid, SIGKILL);
                        waitpid(pid, &status, 0);
                        free(out.data);
                        free(err.data);
                        return -1;
                    }
                    continue;
                }
                if (got == 0) {
                    *open_flag = 0;
                    close(fd);
                    break;
                }
                if (errno == EINTR)
                    continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break;
                *open_flag = 0;
                close(fd);
                break;
            }
        }

        if (!child_done) {
            const pid_t waited = waitpid(pid, &status, WNOHANG);
            if (waited == pid)
                child_done = 1;
        }
    }

    if (!child_done) {
        // 理论上循环退出前已经等到；保险起见再 wait。
        waitpid(pid, &status, 0);
    }

    if (WIFEXITED(status)) {
        result->exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result->signaled = 1;
        result->exit_code = -1;
    }

    result->timed_out = timed_out;
    result->truncated = out.truncated || err.truncated;
    result->stdout_data = out.data;
    result->stdout_length = out.length;
    result->stderr_data = err.data;
    result->stderr_length = err.length;
    return 0;
}
