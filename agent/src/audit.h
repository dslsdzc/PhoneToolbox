#ifndef PHONE_AGENT_AUDIT_H
#define PHONE_AGENT_AUDIT_H

#include <pthread.h>
#include <stdint.h>

// append-only hash-chain 审计：
//   prev_hex timestamp peer op argv_sha256 exit entry_hex
// entry_hex = SHA256(prev || timestamp || peer || op || argv_sha256 || exit)
//
// 只记录摘要，不记录命令完整输出/文件内容。

typedef struct {
    int fd;
    uint8_t prev[32];
    pthread_mutex_t mutex;
} audit_log;

// 打开/创建（0644）。多连接共享同一个 audit_log，写入由 mutex 串行化。已存在时从最后一行恢复 prev。
// 返回 0 成功；-1 失败。
int audit_open(audit_log *log, const char *path);

// 追加一条。argv_sha256 为 64 字符十六进制。
int audit_append(audit_log *log, const char *peer, const char *op,
                 const char *argv_sha256, int exit_code);

void audit_close(audit_log *log);

#endif // PHONE_AGENT_AUDIT_H
