#include "cpace_pair.h"

#include <string.h>

static int ids_ok(const char *client_id, const char *server_id)
{
    if (!client_id || !server_id)
        return 0;
    const size_t client_len = strlen(client_id);
    const size_t server_len = strlen(server_id);
    // cpace API 的 id 长度是 unsigned char。
    return client_len <= 255 && server_len <= 255;
}

int cpace_client_start(cpace_client_ctx *ctx,
                       const void *password, size_t password_len,
                       const char *client_id, const char *server_id,
                       const void *ad, size_t ad_len,
                       uint8_t public_data[CPACE_PUBLIC_DATA_LEN])
{
    if (!ctx || !password || !public_data || !ids_ok(client_id, server_id))
        return -1;
    if (password_len > 255)
        return -1;

    return crypto_cpace_step1(&ctx->state, public_data,
                              (const char *)password, password_len,
                              client_id, (unsigned char)strlen(client_id),
                              server_id, (unsigned char)strlen(server_id),
                              (const unsigned char *)ad, ad_len) == 0
               ? 0
               : -1;
}

int cpace_server_respond(const uint8_t public_data[CPACE_PUBLIC_DATA_LEN],
                         const void *password, size_t password_len,
                         const char *client_id, const char *server_id,
                         const void *ad, size_t ad_len,
                         uint8_t response[CPACE_RESPONSE_LEN],
                         uint8_t client_key[CPACE_KEY_LEN],
                         uint8_t server_key[CPACE_KEY_LEN])
{
    if (!public_data || !password || !response || !client_key || !server_key
        || !ids_ok(client_id, server_id) || password_len > 255)
        return -1;

    crypto_cpace_shared_keys keys;
    if (crypto_cpace_step2(response, public_data, &keys,
                           (const char *)password, password_len,
                           client_id, (unsigned char)strlen(client_id),
                           server_id, (unsigned char)strlen(server_id),
                           (const unsigned char *)ad, ad_len) != 0)
        return -1;

    memcpy(client_key, keys.client_sk, CPACE_KEY_LEN);
    memcpy(server_key, keys.server_sk, CPACE_KEY_LEN);
    return 0;
}

int cpace_client_finish(cpace_client_ctx *ctx,
                        const uint8_t response[CPACE_RESPONSE_LEN],
                        uint8_t client_key[CPACE_KEY_LEN],
                        uint8_t server_key[CPACE_KEY_LEN])
{
    if (!ctx || !response || !client_key || !server_key)
        return -1;

    crypto_cpace_shared_keys keys;
    if (crypto_cpace_step3(&ctx->state, &keys, response) != 0)
        return -1;

    memcpy(client_key, keys.client_sk, CPACE_KEY_LEN);
    memcpy(server_key, keys.server_sk, CPACE_KEY_LEN);
    return 0;
}
