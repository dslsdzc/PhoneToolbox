#ifndef PHONE_AGENT_CPACE_PAIR_H
#define PHONE_AGENT_CPACE_PAIR_H

#include <stddef.h>
#include <stdint.h>

#include "crypto_cpace.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CPACE_PUBLIC_DATA_LEN crypto_cpace_PUBLICDATABYTES
#define CPACE_RESPONSE_LEN crypto_cpace_RESPONSEBYTES
#define CPACE_KEY_LEN crypto_cpace_SHAREDKEYBYTES

typedef struct {
    crypto_cpace_state state;
} cpace_client_ctx;

// 双方共享：低熵短码或二维码中的高熵串。
// client_id/server_id 绑定身份；ad 绑定协议版本/包名/指纹等上下文。
// 返回 0 成功；-1 参数/内部错误。
int cpace_client_start(cpace_client_ctx *ctx,
                       const void *password, size_t password_len,
                       const char *client_id, const char *server_id,
                       const void *ad, size_t ad_len,
                       uint8_t public_data[CPACE_PUBLIC_DATA_LEN]);

int cpace_server_respond(const uint8_t public_data[CPACE_PUBLIC_DATA_LEN],
                         const void *password, size_t password_len,
                         const char *client_id, const char *server_id,
                         const void *ad, size_t ad_len,
                         uint8_t response[CPACE_RESPONSE_LEN],
                         uint8_t client_key[CPACE_KEY_LEN],
                         uint8_t server_key[CPACE_KEY_LEN]);

int cpace_client_finish(cpace_client_ctx *ctx,
                        const uint8_t response[CPACE_RESPONSE_LEN],
                        uint8_t client_key[CPACE_KEY_LEN],
                        uint8_t server_key[CPACE_KEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif // PHONE_AGENT_CPACE_PAIR_H
