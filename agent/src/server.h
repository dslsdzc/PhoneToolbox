#ifndef PHONE_AGENT_SERVER_H
#define PHONE_AGENT_SERVER_H

#include <stddef.h>

typedef struct {
    int port;               // 0 = 由内核分配，启动后打印 PORT=<n>
    const char *token;      // 仅 --allow-plaintext 开发模式使用
    const char *audit_path; // 可为空
    const char *state_dir;  // identity.key / peers.txt 所在目录
    const char *pair_code;  // 首次配对短码；为空则不允许配对
    int allow_plaintext;    // 开发开关；默认关闭
    int io_timeout_ms;      // 连接读写超时；<=0 使用默认 10000ms
    size_t max_frame;
    size_t max_output;
} server_config;

// 返回 0 正常退出；-1 启动失败。
int server_run(const server_config *config);

#endif // PHONE_AGENT_SERVER_H
