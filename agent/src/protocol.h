#ifndef PHONE_AGENT_PROTOCOL_H
#define PHONE_AGENT_PROTOCOL_H

#include "audit.h"
#include "secure/cpace_pair.h"
#include "secure/identity.h"
#include "secure/noise_channel.h"

#include <stddef.h>
#include <stdint.h>

#define AGENT_PROTOCOL_VERSION 1
#define AGENT_VERSION "0.1.0-beta04"

// 帧 payload 首字节：
//   0x01 = JSON 控制帧（UTF-8）
//   0x02 = 二进制块：uint64_be offset + data
//   0x10 = 配对控制（明文 JSON；只在未配对时接受）
//   0x11 = Noise 握手消息
//   0x12 = Noise 传输密文（解密后是 0x01/0x02 payload）
#define FRAME_TYPE_JSON 0x01
#define FRAME_TYPE_BINARY 0x02
#define FRAME_TYPE_PAIR_CONTROL 0x10
#define FRAME_TYPE_NOISE_HANDSHAKE 0x11
#define FRAME_TYPE_NOISE_TRANSPORT 0x12

typedef struct {
    int client_fd;
    const char *token;      // 仅 --allow-plaintext 开发模式使用
    audit_log *audit;
    size_t max_frame;
    size_t max_output;
    agent_identity *identity;
    const char *state_dir;
    const char *pair_code;  // APK 生成并传入；为空则不允许配对
    noise_channel *noise;   // 非空 = 后续帧走 Noise 加密
    int allow_plaintext;    // 开发/测试开关，默认关闭
    // 配对中间状态（同一条连接内 pair_start → pair_confirm）
    int pair_pending;
    uint8_t pair_client_static[AGENT_IDENTITY_KEY_LEN];
    uint8_t pair_client_key[CPACE_KEY_LEN];
    char pair_client_name[64];
    char pair_capabilities[128];
    // Noise 握手成功后记录当前会话的 peer 静态公钥：
    // 每条命令前检查它是否仍在 peers.txt，删除配对可立即吊销现有会话。
    int has_peer;
    uint8_t peer_public[AGENT_IDENTITY_KEY_LEN];
} protocol_context;

// 返回 0 正常结束；-1 连接错误/需关闭。
int protocol_serve_client(protocol_context *context);

#endif // PHONE_AGENT_PROTOCOL_H
