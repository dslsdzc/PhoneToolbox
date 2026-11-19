#ifndef PHONE_AGENT_FRAME_H
#define PHONE_AGENT_FRAME_H

#include <stddef.h>
#include <stdint.h>

// 帧格式：4 字节大端长度 + payload。
// 与传输层（阶段 1 明文 localhost，阶段 2 Noise）解耦：
// 加密层只负责把整帧当字节流保护，不改变这里的边界。

#define FRAME_HEADER_SIZE 4
#define FRAME_DEFAULT_MAX (1024u * 1024u)

// 返回 0 成功；-1 读失败/对端关闭/超过期限；-2 长度非法；-3 内存不足。
int frame_read(int fd, uint8_t **payload, uint32_t *length, uint32_t max_length);

// 带绝对期限的版本：deadline_ms 是 CLOCK_MONOTONIC 毫秒；<0 表示不设期限。
// 认证阶段用它，防止每读一个字节都刷新 SO_RCVTIMEO 的慢速发送攻击。
int frame_read_until(int fd, uint8_t **payload, uint32_t *length,
                     uint32_t max_length, int64_t deadline_ms);

int64_t frame_monotonic_ms(void);

// 返回 0 成功；-1 写失败；-2 长度非法。
int frame_write(int fd, const void *payload, uint32_t length);

#endif // PHONE_AGENT_FRAME_H
