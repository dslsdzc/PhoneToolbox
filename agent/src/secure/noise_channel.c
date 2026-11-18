#include "noise_channel.h"

#include <noise/protocol.h>

#include <stdio.h>
#include <string.h>

static void set_error(char *error, size_t error_len, const char *message)
{
    if (error && error_len > 0)
        snprintf(error, error_len, "%s", message);
}

static void set_noise_error(char *error, size_t error_len, int code, const char *prefix)
{
    if (!error || error_len == 0)
        return;
    char detail[128] = {0};
    noise_strerror(code, detail, sizeof(detail));
    snprintf(error, error_len, "%s: %s", prefix,
             detail[0] ? detail : "unknown");
}

int noise_channel_init(noise_channel *channel, int initiator,
                       const uint8_t local_static_private[NOISE_STATIC_KEY_LEN],
                       const uint8_t remote_static_public[NOISE_STATIC_KEY_LEN],
                       const void *prologue, size_t prologue_len,
                       char *error, size_t error_len)
{
    if (!channel || !local_static_private || !remote_static_public) {
        set_error(error, error_len, "invalid arguments");
        return -1;
    }
    memset(channel, 0, sizeof(*channel));
    channel->initiator = initiator != 0;

    const int role = channel->initiator ? NOISE_ROLE_INITIATOR : NOISE_ROLE_RESPONDER;
    int result = noise_handshakestate_new_by_name(&channel->handshake,
                                                 NOISE_PROTOCOL_NAME, role);
    if (result != NOISE_ERROR_NONE) {
        set_noise_error(error, error_len, result, "handshake init");
        noise_channel_free(channel);
        return -1;
    }

    if (prologue && prologue_len > 0) {
        result = noise_handshakestate_set_prologue(channel->handshake, prologue,
                                                  prologue_len);
        if (result != NOISE_ERROR_NONE) {
            set_noise_error(error, error_len, result, "prologue");
            noise_channel_free(channel);
            return -1;
        }
    }

    NoiseDHState *local =
        noise_handshakestate_get_local_keypair_dh(channel->handshake);
    result = noise_dhstate_set_keypair_private(local, local_static_private,
                                               NOISE_STATIC_KEY_LEN);
    if (result != NOISE_ERROR_NONE) {
        set_noise_error(error, error_len, result, "local static key");
        noise_channel_free(channel);
        return -1;
    }

    NoiseDHState *remote =
        noise_handshakestate_get_remote_public_key_dh(channel->handshake);
    result = noise_dhstate_set_public_key(remote, remote_static_public,
                                          NOISE_STATIC_KEY_LEN);
    if (result != NOISE_ERROR_NONE) {
        set_noise_error(error, error_len, result, "remote static key");
        noise_channel_free(channel);
        return -1;
    }

    result = noise_handshakestate_start(channel->handshake);
    if (result != NOISE_ERROR_NONE) {
        set_noise_error(error, error_len, result, "handshake start");
        noise_channel_free(channel);
        return -1;
    }

    return 0;
}

int noise_channel_write_handshake(noise_channel *channel,
                                  uint8_t *out, size_t *out_len, size_t max_len,
                                  char *error, size_t error_len)
{
    if (!channel || !channel->handshake || !out || !out_len) {
        set_error(error, error_len, "invalid arguments");
        return -1;
    }

    uint8_t buffer[NOISE_MAX_HANDSHAKE_MSG];
    NoiseBuffer message;
    noise_buffer_set_output(message, buffer, sizeof(buffer));

    const int result =
        noise_handshakestate_write_message(channel->handshake, &message, NULL);
    if (result != NOISE_ERROR_NONE) {
        set_noise_error(error, error_len, result, "handshake write");
        return -1;
    }
    if (message.size > max_len) {
        set_error(error, error_len, "handshake message too large");
        return -1;
    }

    memcpy(out, buffer, message.size);
    *out_len = message.size;
    return 0;
}

int noise_channel_read_handshake(noise_channel *channel,
                                 const uint8_t *in, size_t in_len,
                                 char *error, size_t error_len)
{
    if (!channel || !channel->handshake || !in || in_len == 0) {
        set_error(error, error_len, "invalid arguments");
        return -1;
    }

    NoiseBuffer message;
    noise_buffer_set_input(message, (uint8_t *)in, in_len);

    const int result =
        noise_handshakestate_read_message(channel->handshake, &message, NULL);
    if (result != NOISE_ERROR_NONE) {
        set_noise_error(error, error_len, result, "handshake read");
        return -1;
    }
    return 0;
}

int noise_channel_remote_static(noise_channel *channel,
                                uint8_t out[NOISE_STATIC_KEY_LEN],
                                char *error, size_t error_len)
{
    if (!channel || !channel->handshake || !out) {
        set_error(error, error_len, "handshake not ready");
        return -1;
    }

    NoiseDHState *remote =
        noise_handshakestate_get_remote_public_key_dh(channel->handshake);
    if (!remote
        || noise_dhstate_get_public_key_length(remote) != NOISE_STATIC_KEY_LEN) {
        set_error(error, error_len, "remote static key unavailable");
        return -1;
    }

    const int result =
        noise_dhstate_get_public_key(remote, out, NOISE_STATIC_KEY_LEN);
    if (result != NOISE_ERROR_NONE) {
        set_noise_error(error, error_len, result, "remote static");
        return -1;
    }
    return 0;
}

int noise_channel_finish_handshake(noise_channel *channel,
                                   char *error, size_t error_len)
{
    if (!channel || !channel->handshake) {
        set_error(error, error_len, "handshake not ready");
        return -1;
    }

    const int result =
        noise_handshakestate_split(channel->handshake, &channel->send, &channel->recv);
    if (result != NOISE_ERROR_NONE) {
        set_noise_error(error, error_len, result, "handshake split");
        return -1;
    }

    noise_handshakestate_free(channel->handshake);
    channel->handshake = NULL;
    return 0;
}

int noise_channel_encrypt(noise_channel *channel,
                          const uint8_t *plain, size_t plain_len,
                          uint8_t *out, size_t *out_len, size_t max_len,
                          char *error, size_t error_len)
{
    if (!channel || !channel->send || !plain || !out || !out_len) {
        set_error(error, error_len, "channel not ready");
        return -1;
    }
    if (plain_len + NOISE_TAG_LEN > max_len) {
        set_error(error, error_len, "plaintext too large");
        return -1;
    }

    memcpy(out, plain, plain_len);
    NoiseBuffer buffer;
    noise_buffer_set_inout(buffer, out, plain_len, max_len);

    const int result =
        noise_cipherstate_encrypt_with_ad(channel->send, NULL, 0, &buffer);
    if (result != NOISE_ERROR_NONE) {
        set_noise_error(error, error_len, result, "encrypt");
        return -1;
    }

    *out_len = buffer.size;
    return 0;
}

int noise_channel_decrypt(noise_channel *channel,
                          const uint8_t *cipher, size_t cipher_len,
                          uint8_t *out, size_t *out_len, size_t max_len,
                          char *error, size_t error_len)
{
    if (!channel || !channel->recv || !cipher || !out || !out_len) {
        set_error(error, error_len, "channel not ready");
        return -1;
    }
    if (cipher_len > max_len) {
        set_error(error, error_len, "ciphertext too large");
        return -1;
    }

    memcpy(out, cipher, cipher_len);
    NoiseBuffer buffer;
    noise_buffer_set_inout(buffer, out, cipher_len, max_len);

    const int result =
        noise_cipherstate_decrypt_with_ad(channel->recv, NULL, 0, &buffer);
    if (result != NOISE_ERROR_NONE) {
        set_noise_error(error, error_len, result, "decrypt");
        return -1;
    }

    *out_len = buffer.size;
    return 0;
}

void noise_channel_free(noise_channel *channel)
{
    if (!channel)
        return;
    if (channel->send)
        noise_cipherstate_free(channel->send);
    if (channel->recv)
        noise_cipherstate_free(channel->recv);
    if (channel->handshake)
        noise_handshakestate_free(channel->handshake);
    memset(channel, 0, sizeof(*channel));
}
