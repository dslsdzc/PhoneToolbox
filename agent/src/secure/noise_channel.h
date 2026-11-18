#ifndef PHONE_AGENT_NOISE_CHANNEL_H
#define PHONE_AGENT_NOISE_CHANNEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NoiseHandshakeState_s NoiseHandshakeState;
typedef struct NoiseCipherState_s NoiseCipherState;

#define NOISE_STATIC_KEY_LEN 32
#define NOISE_PROTOCOL_NAME "Noise_IK_25519_ChaChaPoly_SHA256"
#define NOISE_MAX_HANDSHAKE_MSG 512
#define NOISE_TAG_LEN 16

typedef struct noise_channel {
    NoiseHandshakeState *handshake;
    NoiseCipherState *send;
    NoiseCipherState *recv;
    int initiator;
} noise_channel;

// 双方都设置自己的静态私钥和对方的静态公钥（IK 模式）。
// prologue 绑定协议版本/包名等上下文；两端必须完全一致。
int noise_channel_init(noise_channel *channel, int initiator,
                       const uint8_t local_static_private[NOISE_STATIC_KEY_LEN],
                       const uint8_t remote_static_public[NOISE_STATIC_KEY_LEN],
                       const void *prologue, size_t prologue_len,
                       char *error, size_t error_len);

int noise_channel_write_handshake(noise_channel *channel,
                                  uint8_t *out, size_t *out_len, size_t max_len,
                                  char *error, size_t error_len);

int noise_channel_read_handshake(noise_channel *channel,
                                 const uint8_t *in, size_t in_len,
                                 char *error, size_t error_len);

// 读取握手消息后，取出对端实际使用的静态公钥。
// Noise_IK responder 不能依赖初始化时传入的 remote 公钥做认证：
// 发起方公钥在 msg1 内，必须读出来后再查白名单。
int noise_channel_remote_static(noise_channel *channel,
                                uint8_t out[NOISE_STATIC_KEY_LEN],
                                char *error, size_t error_len);

// 握手完成后调用一次，得到收发 CipherState。
int noise_channel_finish_handshake(noise_channel *channel,
                                   char *error, size_t error_len);

// 明文长度 + NOISE_TAG_LEN 必须 <= max_len。
int noise_channel_encrypt(noise_channel *channel,
                          const uint8_t *plain, size_t plain_len,
                          uint8_t *out, size_t *out_len, size_t max_len,
                          char *error, size_t error_len);

int noise_channel_decrypt(noise_channel *channel,
                          const uint8_t *cipher, size_t cipher_len,
                          uint8_t *out, size_t *out_len, size_t max_len,
                          char *error, size_t error_len);

void noise_channel_free(noise_channel *channel);

#ifdef __cplusplus
}
#endif

#endif // PHONE_AGENT_NOISE_CHANNEL_H
