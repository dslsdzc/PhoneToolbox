#include "frame.h"
#include "secure/noise_channel.h"

#include <cJSON.h>
#include <sodium.h>

#include <arpa/inet.h>
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

static const char kPrologue[] = "PhoneToolbox/root-agent/1";

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

static int write_peer_line(const char *peers_path, const char *hex,
                           const char *name, const char *capabilities)
{
    FILE *file = fopen(peers_path, "a");
    if (!file)
        return -1;
    fprintf(file, "%s %s %s\n", hex, name, capabilities);
    fclose(file);
    return 0;
}

// 完成 Noise_IK 发起方握手。agent 侧会遍历 peers.txt，跳过不匹配的公钥。
static int noise_connect(int fd, const uint8_t private_key[32],
                         const uint8_t agent_public[32], noise_channel *channel)
{
    char error[256] = {0};
    if (noise_channel_init(channel, 1, private_key, agent_public, kPrologue,
                           sizeof(kPrologue) - 1, error, sizeof(error)) != 0)
        return -1;

    uint8_t msg1[NOISE_MAX_HANDSHAKE_MSG];
    size_t msg1_length = 0;
    if (noise_channel_write_handshake(channel, msg1, &msg1_length, sizeof(msg1),
                                      error, sizeof(error)) != 0)
        return -1;

    uint8_t *frame = malloc(msg1_length + 1);
    if (!frame)
        return -1;
    frame[0] = 0x11;
    memcpy(frame + 1, msg1, msg1_length);
    const int sent = frame_write(fd, frame, (uint32_t)(msg1_length + 1));
    free(frame);
    if (sent != 0)
        return -1;

    uint8_t *reply = NULL;
    uint32_t reply_length = 0;
    if (frame_read(fd, &reply, &reply_length, FRAME_DEFAULT_MAX) != 0)
        return -1;
    if (reply_length <= 1 || reply[0] != 0x11) {
        free(reply);
        return -1;
    }
    const int read_result = noise_channel_read_handshake(
        channel, reply + 1, reply_length - 1, error, sizeof(error));
    free(reply);
    if (read_result != 0)
        return -1;

    return noise_channel_finish_handshake(channel, error, sizeof(error));
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
    char error[128] = {0};
    const int encrypted = noise_channel_encrypt(
        channel, payload, text_length + 1, cipher, &cipher_length,
        text_length + 1 + NOISE_TAG_LEN, error, sizeof(error));
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

static cJSON *recv_encrypted_json(int fd, noise_channel *channel)
{
    uint8_t *frame = NULL;
    uint32_t frame_length = 0;
    if (frame_read(fd, &frame, &frame_length, FRAME_DEFAULT_MAX) != 0)
        return NULL;
    if (frame_length <= 1 || frame[0] != 0x12) {
        free(frame);
        return NULL;
    }

    uint8_t *plain = malloc(frame_length);
    if (!plain) {
        free(frame);
        return NULL;
    }
    size_t plain_length = 0;
    char error[128] = {0};
    const int decrypted = noise_channel_decrypt(
        channel, frame + 1, frame_length - 1, plain, &plain_length, frame_length,
        error, sizeof(error));
    free(frame);
    if (decrypted != 0 || plain_length <= 1 || plain[0] != 0x01) {
        free(plain);
        return NULL;
    }

    cJSON *response =
        cJSON_ParseWithLength((const char *)plain + 1, plain_length - 1);
    free(plain);
    return response;
}

// 发送 ping，把解密后的 JSON 返回给调用方（需 cJSON_Delete）。
static cJSON *ping(int fd, noise_channel *channel, int id)
{
    cJSON *request = cJSON_CreateObject();
    if (!request)
        return NULL;
    cJSON_AddStringToObject(request, "op", "ping");
    cJSON_AddNumberToObject(request, "id", id);
    if (send_encrypted_json(fd, channel, request) != 0) {
        cJSON_Delete(request);
        return NULL;
    }
    cJSON_Delete(request);
    return recv_encrypted_json(fd, channel);
}

// 执行 shell 命令，返回第一条 exit/error 响应。
static cJSON *exec_shell(int fd, noise_channel *channel, const char *command, int id)
{
    cJSON *request = cJSON_CreateObject();
    if (!request)
        return NULL;
    cJSON_AddStringToObject(request, "op", "exec");
    cJSON_AddNumberToObject(request, "id", id);
    cJSON_AddBoolToObject(request, "shell", 1);
    cJSON_AddStringToObject(request, "command", command);
    cJSON_AddNumberToObject(request, "timeout_ms", 2000);
    if (send_encrypted_json(fd, channel, request) != 0) {
        cJSON_Delete(request);
        return NULL;
    }
    cJSON_Delete(request);

    for (int i = 0; i < 8; ++i) {
        cJSON *message = recv_encrypted_json(fd, channel);
        if (!message)
            return NULL;
        const cJSON *op = cJSON_GetObjectItemCaseSensitive(message, "op");
        if (cJSON_IsString(op)
            && (strcmp(op->valuestring, "exit") == 0
                || strcmp(op->valuestring, "error") == 0)) {
            return message;
        }
        cJSON_Delete(message);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <phone_agent>\n", argv[0]);
        return 1;
    }
    CHECK(sodium_init() >= 0);

    char state_dir[] = "/tmp/agent_multi_XXXXXX";
    CHECK(mkdtemp(state_dir) != NULL);

    // 三台电脑 + 一台陌生设备：第一个 peer 不是发起方，验证握手会继续尝试。
    uint8_t stranger_public[32], stranger_private[32];
    uint8_t a_public[32], a_private[32];
    uint8_t b_public[32], b_private[32];
    crypto_box_keypair(stranger_public, stranger_private);
    crypto_box_keypair(a_public, a_private);
    crypto_box_keypair(b_public, b_private);

    char stranger_hex[65], a_hex[65], b_hex[65];
    sodium_bin2hex(stranger_hex, sizeof(stranger_hex), stranger_public, 32);
    sodium_bin2hex(a_hex, sizeof(a_hex), a_public, 32);
    sodium_bin2hex(b_hex, sizeof(b_hex), b_public, 32);

    char peers_path[512];
    snprintf(peers_path, sizeof(peers_path), "%s/peers.txt", state_dir);
    CHECK(write_peer_line(peers_path, stranger_hex, "stranger",
                          "exec,push,pull") == 0);
    CHECK(write_peer_line(peers_path, a_hex, "desktop-a",
                          "exec,push,pull") == 0);
    CHECK(write_peer_line(peers_path, b_hex, "desktop-b",
                          "exec,push,pull,dangerous") == 0);

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

    // 电脑 A 连接：必须跳过 stranger 才能匹配。
    int fd = connect_localhost(port);
    CHECK(fd >= 0);
    noise_channel channel_a;
    CHECK(noise_connect(fd, a_private, agent_public, &channel_a) == 0);

    cJSON *pong = ping(fd, &channel_a, 1);
    CHECK(pong != NULL);
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(pong, "op");
    CHECK(cJSON_IsString(op) && strcmp(op->valuestring, "pong") == 0);
    cJSON_Delete(pong);

    // Root 代理配对即完整授权：A 的 shell 命令应直接执行。
    cJSON *result = exec_shell(fd, &channel_a, "exit 7", 10);
    CHECK(result != NULL);
    op = cJSON_GetObjectItemCaseSensitive(result, "op");
    const cJSON *a_code = cJSON_GetObjectItemCaseSensitive(result, "code");
    CHECK(cJSON_IsString(op) && strcmp(op->valuestring, "exit") == 0);
    CHECK(cJSON_IsNumber(a_code) && a_code->valueint == 7);
    cJSON_Delete(result);

    noise_channel_free(&channel_a);
    close(fd);

    // 未配对电脑 C 不能完成握手：只看初始化时传入的 remote 公钥是不够的，
    // agent 必须从 msg1 读出真实公钥后拒绝。
    uint8_t c_public[32], c_private[32];
    crypto_box_keypair(c_public, c_private);
    const int c_fd = connect_localhost(port);
    CHECK(c_fd >= 0);
    noise_channel channel_c;
    CHECK(noise_connect(c_fd, c_private, agent_public, &channel_c) != 0);
    close(c_fd);

    // 电脑 B 连接：同样能匹配自己的公钥。
    fd = connect_localhost(port);
    CHECK(fd >= 0);
    noise_channel channel_b;
    CHECK(noise_connect(fd, b_private, agent_public, &channel_b) == 0);

    pong = ping(fd, &channel_b, 2);
    CHECK(pong != NULL);
    op = cJSON_GetObjectItemCaseSensitive(pong, "op");
    CHECK(cJSON_IsString(op) && strcmp(op->valuestring, "pong") == 0);
    cJSON_Delete(pong);

    // B 有 dangerous 能力，shell 可以执行。
    cJSON *b_result = exec_shell(fd, &channel_b, "exit 7", 20);
    CHECK(b_result != NULL);
    op = cJSON_GetObjectItemCaseSensitive(b_result, "op");
    const cJSON *exit_code = cJSON_GetObjectItemCaseSensitive(b_result, "code");
    CHECK(cJSON_IsString(op) && strcmp(op->valuestring, "exit") == 0);
    CHECK(cJSON_IsNumber(exit_code) && exit_code->valueint == 7);
    cJSON_Delete(b_result);

    // 删除 B 的配对后，现有连接必须在下一帧被吊销。
    FILE *file = fopen(peers_path, "r");
    CHECK(file != NULL);
    char kept[2048] = {0};
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, b_hex, 64) == 0)
            continue;
        strncat(kept, line, sizeof(kept) - strlen(kept) - 1);
    }
    fclose(file);
    file = fopen(peers_path, "w");
    CHECK(file != NULL);
    fputs(kept, file);
    fclose(file);

    pong = ping(fd, &channel_b, 3);
    CHECK(pong != NULL);
    op = cJSON_GetObjectItemCaseSensitive(pong, "op");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(pong, "code");
    CHECK(cJSON_IsString(op) && strcmp(op->valuestring, "error") == 0);
    CHECK(cJSON_IsString(code) && strcmp(code->valuestring, "peer_revoked") == 0);
    cJSON_Delete(pong);

    noise_channel_free(&channel_b);
    close(fd);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    fclose(agent_output);

    unlink(peers_path);
    char identity_path[512];
    snprintf(identity_path, sizeof(identity_path), "%s/identity.key", state_dir);
    unlink(identity_path);
    rmdir(state_dir);

    printf("test_multi_peer: OK\n");
    return 0;
}
