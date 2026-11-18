#include "secure/cpace_pair.h"

#include <sodium.h>

#include <stdio.h>
#include <string.h>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            return 1;                                                            \
        }                                                                        \
    } while (0)

int main(void)
{
    CHECK(crypto_cpace_init() >= 0);

    const char *password = "123456";
    const char *client_id = "desktop";
    const char *server_id = "agent";
    const char ad[] = "PhoneToolbox/protocol/1";

    cpace_client_ctx client;
    uint8_t public_data[CPACE_PUBLIC_DATA_LEN];
    uint8_t response[CPACE_RESPONSE_LEN];
    uint8_t server_client_key[CPACE_KEY_LEN];
    uint8_t server_agent_key[CPACE_KEY_LEN];
    uint8_t client_client_key[CPACE_KEY_LEN];
    uint8_t client_agent_key[CPACE_KEY_LEN];

    CHECK(cpace_client_start(&client, password, strlen(password),
                             client_id, server_id, ad, sizeof(ad) - 1,
                             public_data) == 0);
    CHECK(cpace_server_respond(public_data, password, strlen(password),
                               client_id, server_id, ad, sizeof(ad) - 1,
                               response, server_client_key,
                               server_agent_key) == 0);
    CHECK(cpace_client_finish(&client, response, client_client_key,
                              client_agent_key) == 0);

    CHECK(memcmp(server_client_key, client_client_key, CPACE_KEY_LEN) == 0);
    CHECK(memcmp(server_agent_key, client_agent_key, CPACE_KEY_LEN) == 0);
    CHECK(memcmp(server_client_key, server_agent_key, CPACE_KEY_LEN) != 0);

    // 错误密码：要么 CPace 拒绝，要么派生出不同密钥。
    cpace_client_ctx wrong_client;
    uint8_t wrong_public[CPACE_PUBLIC_DATA_LEN];
    uint8_t wrong_response[CPACE_RESPONSE_LEN];
    uint8_t wrong_server_client[CPACE_KEY_LEN];
    uint8_t wrong_server_agent[CPACE_KEY_LEN];
    uint8_t wrong_client_key[CPACE_KEY_LEN];
    uint8_t wrong_agent_key[CPACE_KEY_LEN];

    CHECK(cpace_client_start(&wrong_client, password, strlen(password),
                             client_id, server_id, ad, sizeof(ad) - 1,
                             wrong_public) == 0);
    CHECK(cpace_server_respond(wrong_public, "654321", 6,
                               client_id, server_id, ad, sizeof(ad) - 1,
                               wrong_response, wrong_server_client,
                               wrong_server_agent) == 0);
    const int finish = cpace_client_finish(&wrong_client, wrong_response,
                                           wrong_client_key, wrong_agent_key);
    CHECK(finish != 0
          || memcmp(wrong_client_key, server_client_key, CPACE_KEY_LEN) != 0);

    printf("test_cpace: OK\n");
    return 0;
}
