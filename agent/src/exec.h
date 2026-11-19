#ifndef PHONE_AGENT_EXEC_H
#define PHONE_AGENT_EXEC_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int exit_code;      // 正常退出时的退出码；异常退出为 -1
    int signaled;       // 是否被信号终止
    int timed_out;      // 是否因超时被 kill
    int canceled;       // 是否因客户端 cancel/断开被 kill
    int truncated;      // 输出是否超过上限被截断
    uint8_t *stdout_data;
    size_t stdout_length;
    uint8_t *stderr_data;
    size_t stderr_length;
} exec_result;

void exec_result_free(exec_result *result);

// argv 以 NULL 结尾；直接 execvp（不经过 shell）。
// timeout_ms <= 0 表示不设超时（调用方应避免）。
// max_output 为每个流的字节上限；超出部分丢弃并置 truncated。
// 返回 0 表示命令已执行完（含非零退出码）；-1 表示内部错误。
int run_command(const char *const argv[], int timeout_ms, size_t max_output,
                exec_result *result);

// 同 run_command，但额外监视 cancel_fd：可读即 kill 整个进程组并置 canceled。
// cancel_fd < 0 表示不可取消。用于客户端 cancel / 断线。
int run_command_cancellable(const char *const argv[], int timeout_ms,
                            size_t max_output, int cancel_fd,
                            exec_result *result);

#endif // PHONE_AGENT_EXEC_H
