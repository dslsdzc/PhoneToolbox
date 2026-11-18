#ifndef PHONE_AGENT_UTIL_H
#define PHONE_AGENT_UTIL_H

#include <stddef.h>
#include <stdint.h>

// SHA-256 → 64 字符小写十六进制 + NUL。
void sha256_hex(const void *data, size_t length, char out_hex[65]);

// 用 libsodium 的随机源生成 n 字节，输出 2n 字符十六进制 + NUL。
// 返回 0 成功；-1 参数非法/内存不足。
int random_hex(size_t bytes, char **out_hex);

// 绝对路径、无 "." / ".." 段、无控制字符、非空、非目录。
// 返回 1 安全；0 拒绝。
int path_is_safe(const char *path);

// 读整个文件；成功返回 0 并写 *out/*length（调用方 free）；失败返回 -1。
int read_whole_file(const char *path, uint8_t **out, size_t *length);

#endif // PHONE_AGENT_UTIL_H
