#include "secure/noise_channel.h"

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
    CHECK(sodium_init() >= 0);

    uint8_t initiator_private[32];
    uint8_t responder_private[32];
    uint8_t initiator_public[32];
    uint8_t responder_public[32];
    randombytes_buf(initiator_private, sizeof(initiator_private));
    randombytes_buf(responder_private, sizeof(responder_private));
    crypto_scalarmult_curve25519_base(initiator_public, initiator_private);
    crypto_scalarmult_curve25519_base(responder_public, responder_private);

    const char prologue[] = "PhoneToolbox/root-agent/1";

    noise_channel initiator;
    noise_channel responder;
    char error[256] = {0};

    CHECK(noise_channel_init(&initiator, 1, initiator_private, responder_public,
                             prologue, sizeof(prologue) - 1,
                             error, sizeof(error)) == 0);
    CHECK(noise_channel_init(&responder, 0, responder_private, initiator_public,
                             prologue, sizeof(prologue) - 1,
                             error, sizeof(error)) == 0);

    uint8_t msg1[NOISE_MAX_HANDSHAKE_MSG];
    uint8_t msg2[NOISE_MAX_HANDSHAKE_MSG];
    size_t msg1_len = 0;
    size_t msg2_len = 0;

    CHECK(noise_channel_write_handshake(&initiator, msg1, &msg1_len,
                                        sizeof(msg1), error, sizeof(error)) == 0);
    CHECK(noise_channel_read_handshake(&responder, msg1, msg1_len,
                                       error, sizeof(error)) == 0);
    CHECK(noise_channel_write_handshake(&responder, msg2, &msg2_len,
                                        sizeof(msg2), error, sizeof(error)) == 0);
    CHECK(noise_channel_read_handshake(&initiator, msg2, msg2_len,
                                       error, sizeof(error)) == 0);

    CHECK(noise_channel_finish_handshake(&initiator, error, sizeof(error)) == 0);
    CHECK(noise_channel_finish_handshake(&responder, error, sizeof(error)) == 0);

    const uint8_t hello[] = "hello from desktop";
    uint8_t cipher[256];
    size_t cipher_len = 0;
    CHECK(noise_channel_encrypt(&initiator, hello, sizeof(hello) - 1,
                                cipher, &cipher_len, sizeof(cipher),
                                error, sizeof(error)) == 0);

    uint8_t plain[256];
    size_t plain_len = 0;
    CHECK(noise_channel_decrypt(&responder, cipher, cipher_len,
                                plain, &plain_len, sizeof(plain),
                                error, sizeof(error)) == 0);
    CHECK(plain_len == sizeof(hello) - 1);
    CHECK(memcmp(plain, hello, plain_len) == 0);

    const uint8_t world[] = "world from agent";
    CHECK(noise_channel_encrypt(&responder, world, sizeof(world) - 1,
                                cipher, &cipher_len, sizeof(cipher),
                                error, sizeof(error)) == 0);
    CHECK(noise_channel_decrypt(&initiator, cipher, cipher_len,
                                plain, &plain_len, sizeof(plain),
                                error, sizeof(error)) == 0);
    CHECK(plain_len == sizeof(world) - 1);
    CHECK(memcmp(plain, world, plain_len) == 0);

    // 第二条消息（nonce 必须递增且双方一致）
    CHECK(noise_channel_encrypt(&initiator, hello, sizeof(hello) - 1,
                                cipher, &cipher_len, sizeof(cipher),
                                error, sizeof(error)) == 0);
    CHECK(noise_channel_decrypt(&responder, cipher, cipher_len,
                                plain, &plain_len, sizeof(plain),
                                error, sizeof(error)) == 0);

    // 篡改必须失败
    CHECK(noise_channel_encrypt(&initiator, hello, sizeof(hello) - 1,
                                cipher, &cipher_len, sizeof(cipher),
                                error, sizeof(error)) == 0);
    cipher[0] ^= 0x01;
    CHECK(noise_channel_decrypt(&responder, cipher, cipher_len,
                                plain, &plain_len, sizeof(plain),
                                error, sizeof(error)) != 0);

    noise_channel_free(&initiator);
    noise_channel_free(&responder);

    printf("test_noise: OK\n");
    return 0;
}
