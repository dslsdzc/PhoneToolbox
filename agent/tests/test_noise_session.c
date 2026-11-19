#include "frame.h"
#include "secure/noise_channel.h"

#include <cJSON.h>
#include <sodium.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            return 1;                                                            \
        }                                                                        \
    } while (0)

static int connect_localhost(int port)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons((uint16_t)port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int read_agent_info(FILE *stream, int *port, char pubkey_hex[65])
{
    char line[512];
    int got_port = 0;
    int got_key = 0;

    for (int i = 0; i < 8 && (!got_port || !got_key); ++i) {
        if (!fgets(line, sizeof(line), stream))
            break;
        if (sscanf(line, "PORT=%d", port) == 1)
            got_port = 1;
        else if (sscanf(line, "PUBKEY=%64s", pubkey_hex) == 1)
            got_key = 1;
    }
    return got_port && got_key ? 0 : -1;
}

static int send_encrypted_json(int fd, noise_channel *channel, cJSON *object)
{
    char *text = cJSON_PrintUnformatted(object);
    if (!text)
        return -1;

    const size_t text_length = strlen(text);
    uint8_t *payload = malloc(text_length + 1);
    if (!payload) {
        free(text);
        return -1;
    }
    payload[0] = 0x01;
    memcpy(payload + 1, text, text_length);

    uint8_t *cipher = malloc(text_length + 1 + NOISE_TAG_LEN);
    if (!cipher) {
        free(payload);
        free(text);
        return -1;
    }
    size_t cipher_length = 0;
    char error[256] = {0};
    const int encrypted =
        noise_channel_encrypt(channel, payload, (size_t)text_length + 1,
                              cipher, &cipher_length,
                              text_length + 1 + NOISE_TAG_LEN,
                              error, sizeof(error));
    free(payload);
    free(text);
    if (encrypted != 0) {
        free(cipher);
        return -1;
    }

    uint8_t *frame = malloc(cipher_length + 1);
    if (!frame) {
        free(cipher);
        return -1;
    }
    frame[0] = 0x12;
    memcpy(frame + 1, cipher, cipher_length);
    const int result = frame_write(fd, frame, (uint32_t)(cipher_length + 1));
    free(frame);
    free(cipher);
    return result;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <phone_agent>\n", argv[0]);
        return 1;
    }
    CHECK(sodium_init() >= 0);

    char state_dir[] = "/tmp/agent_noise_XXXXXX";
    CHECK(mkdtemp(state_dir) != NULL);

    uint8_t client_public[32];
    uint8_t client_private[32];
    crypto_box_keypair(client_public, client_private);

    char client_hex[65];
    sodium_bin2hex(client_hex, sizeof(client_hex), client_public, 32);

    char peers_path[512];
    snprintf(peers_path, sizeof(peers_path), "%s/peers.txt", state_dir);
    FILE *peers = fopen(peers_path, "w");
    CHECK(peers != NULL);
    fprintf(peers, "%s test-client exec,push,pull\n", client_hex);
    fclose(peers);

    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    const pid_t pid = fork();
    CHECK(pid >= 0);

    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl(argv[1], argv[1], "--port", "0", "--state-dir", state_dir,
              "--allow-plaintext", "--token", "dev", (char *)NULL);
        _exit(127);
    }

    close(pipefd[1]);
    FILE *agent_output = fdopen(pipefd[0], "r");
    CHECK(agent_output != NULL);

    int port = 0;
    char pubkey_hex[65] = {0};
    CHECK(read_agent_info(agent_output, &port, pubkey_hex) == 0);

    uint8_t agent_public[32];
    CHECK(sodium_hex2bin(agent_public, sizeof(agent_public), pubkey_hex, 64,
                         NULL, NULL, NULL) == 0);

    const int fd = connect_localhost(port);
    CHECK(fd >= 0);

    static const char prologue[] = "PhoneToolbox/root-agent/1";
    noise_channel channel;
    char error[256] = {0};
    CHECK(noise_channel_init(&channel, 1, client_private, agent_public,
                             prologue, sizeof(prologue) - 1,
                             error, sizeof(error)) == 0);

    uint8_t msg1[NOISE_MAX_HANDSHAKE_MSG];
    size_t msg1_length = 0;
    CHECK(noise_channel_write_handshake(&channel, msg1, &msg1_length,
                                        sizeof(msg1), error, sizeof(error)) == 0);

    uint8_t *frame = malloc(msg1_length + 1);
    CHECK(frame != NULL);
    frame[0] = 0x11;
    memcpy(frame + 1, msg1, msg1_length);
    CHECK(frame_write(fd, frame, (uint32_t)(msg1_length + 1)) == 0);
    free(frame);

    uint8_t *reply = NULL;
    uint32_t reply_length = 0;
    CHECK(frame_read(fd, &reply, &reply_length, FRAME_DEFAULT_MAX) == 0);
    CHECK(reply_length > 1 && reply[0] == 0x11);
    CHECK(noise_channel_read_handshake(&channel, reply + 1, reply_length - 1,
                                       error, sizeof(error)) == 0);
    free(reply);

    CHECK(noise_channel_finish_handshake(&channel, error, sizeof(error)) == 0);

    cJSON *ping = cJSON_CreateObject();
    CHECK(ping != NULL);
    cJSON_AddStringToObject(ping, "op", "ping");
    cJSON_AddNumberToObject(ping, "id", 1);
    CHECK(send_encrypted_json(fd, &channel, ping) == 0);
    cJSON_Delete(ping);

    reply = NULL;
    reply_length = 0;
    CHECK(frame_read(fd, &reply, &reply_length, FRAME_DEFAULT_MAX) == 0);
    CHECK(reply_length > 1 && reply[0] == 0x12);

    uint8_t *plain = malloc(reply_length);
    CHECK(plain != NULL);
    size_t plain_length = 0;
    CHECK(noise_channel_decrypt(&channel, reply + 1, reply_length - 1,
                                plain, &plain_length, reply_length,
                                error, sizeof(error)) == 0);
    free(reply);
    CHECK(plain_length > 1 && plain[0] == 0x01);

    cJSON *pong = cJSON_ParseWithLength((const char *)plain + 1,
                                        plain_length - 1);
    free(plain);
    CHECK(pong != NULL);
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(pong, "op");
    CHECK(cJSON_IsString(op) && strcmp(op->valuestring, "pong") == 0);
    cJSON_Delete(pong);

    noise_channel_free(&channel);
    close(fd);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    fclose(agent_output);

    unlink(peers_path);
    char identity_path[512];
    snprintf(identity_path, sizeof(identity_path), "%s/identity.key", state_dir);
    unlink(identity_path);
    rmdir(state_dir);

    printf("test_noise_session: OK\n");
    return 0;
}
