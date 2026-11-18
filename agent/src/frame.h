#ifndef PHONE_AGENT_FRAME_H
#define PHONE_AGENT_FRAME_H

#include <stddef.h>
#include <stdint.h>

// 帧格式：4 字节大端长度 + payload。
// 与传输层（阶段 1 明文 localhost，阶段 2 Noise）解耦：
// 加密层只负责把整帧当字节流保护，不改变这里的边界。

#define FRAME_HEADER_SIZE 4
#define FRAME_DEFAULT_MAX (1024u * 1024u)

// 返回 0 成功；-1 读失败/对端关闭；-2 长度非法；-3 内存不足。
int frame_read(int fd, uint8_t **payload, uint32_t *length, uint32_t max_length);

// 返回 0 成功；-1 写失败；-2 长度非法。
int frame_write(int fd, const void *payload, uint32_t length);

#endif // PHONE_AGENT_FRAME_H
