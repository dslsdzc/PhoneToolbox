#include "frame.h"
#include "secure/cpace_pair.h"

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

static int read_agent_port(FILE *stream, int *port)
{
    char line[512];
    for (int i = 0; i < 8; ++i) {
        if (!fgets(line, sizeof(line), stream))
            break;
        if (sscanf(line, "PORT=%d", port) == 1)
            return 0;
    }
    return -1;
}

static int send_pair_json(int fd, cJSON *object)
{
    char *text = cJSON_PrintUnformatted(object);
    if (!text)
        return -1;
    const size_t length = strlen(text);
    uint8_t *payload = malloc(length + 1);
    if (!payload) {
        free(text);
        return -1;
    }
    payload[0] = 0x10;
    memcpy(payload + 1, text, length);
    const int result = frame_write(fd, payload, (uint32_t)(length + 1));
    free(payload);
    free(text);
    return result;
}

static cJSON *recv_json(int fd)
{
    uint8_t *payload = NULL;
    uint32_t length = 0;
    if (frame_read(fd, &payload, &length, FRAME_DEFAULT_MAX) != 0)
        return NULL;
    if (length < 2 || payload[0] != 0x01) {
        free(payload);
        return NULL;
    }
    cJSON *root = cJSON_ParseWithLength((const char *)payload + 1, length - 1);
    free(payload);
    return root;
}

static void confirm_mac(const uint8_t key[CPACE_KEY_LEN],
                        const uint8_t client_static[32],
                        const uint8_t agent_static[32], uint8_t out[32])
{
    crypto_auth_hmacsha256_state state;
    crypto_auth_hmacsha256_init(&state, key, CPACE_KEY_LEN);
    crypto_auth_hmacsha256_update(&state, (const unsigned char *)"pair-confirm", 12);
    crypto_auth_hmacsha256_update(&state, client_static, 32);
    crypto_auth_hmacsha256_update(&state, agent_static, 32);
    crypto_auth_hmacsha256_final(&state, out);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <phone_agent>\n", argv[0]);
        return 1;
    }
    CHECK(crypto_cpace_init() >= 0);

    char state_dir[] = "/tmp/agent_pair_XXXXXX";
    CHECK(mkdtemp(state_dir) != NULL);

    int pipefd[2];
    CHECK(pipe(pipefd) == 0);
    const pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        execl(argv[1], argv[1], "--port", "0", "--state-dir", state_dir,
              "--pair-code", "123456", (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    FILE *output = fdopen(pipefd[0], "r");
    CHECK(output != NULL);

    int port = 0;
    CHECK(read_agent_port(output, &port) == 0);

    uint8_t client_static[32];
    uint8_t client_private[32];
    crypto_box_keypair(client_static, client_private);

    const char *pair_code = "123456";
    const char *ad = "PhoneToolbox/root-agent/1";
    cpace_client_ctx cpace;
    uint8_t public_data[CPACE_PUBLIC_DATA_LEN];
    CHECK(cpace_client_start(&cpace, pair_code, strlen(pair_code),
                             "desktop", "agent", ad, strlen(ad),
                             public_data) == 0);

    char public_hex[CPACE_PUBLIC_DATA_LEN * 2 + 1];
    char client_hex[65];
    sodium_bin2hex(public_hex, sizeof(public_hex), public_data, sizeof(public_data));
    sodium_bin2hex(client_hex, sizeof(client_hex), client_static, sizeof(client_static));

    const int fd = connect_localhost(port);
    CHECK(fd >= 0);

    cJSON *start = cJSON_CreateObject();
    CHECK(start != NULL);
    cJSON_AddStringToObject(start, "op", "pair_start");
    cJSON_AddStringToObject(start, "public_data", public_hex);
    cJSON_AddStringToObject(start, "client_static", client_hex);
    cJSON_AddStringToObject(start, "client_name", "test-desktop");
    CHECK(send_pair_json(fd, start) == 0);
    cJSON_Delete(start);

    cJSON *response = recv_json(fd);
    CHECK(response != NULL);
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(response, "op");
    CHECK(cJSON_IsString(op) && strcmp(op->valuestring, "pair_response") == 0);

    const char *response_hex =
        cJSON_GetObjectItemCaseSensitive(response, "response")->valuestring;
    const char *agent_hex =
        cJSON_GetObjectItemCaseSensitive(response, "agent_static")->valuestring;
    uint8_t response_bin[CPACE_RESPONSE_LEN];
    uint8_t agent_static[32];
    CHECK(sodium_hex2bin(response_bin, sizeof(response_bin), response_hex, 64,
                         NULL, NULL, NULL) == 0);
    CHECK(sodium_hex2bin(agent_static, sizeof(agent_static), agent_hex, 64,
                         NULL, NULL, NULL) == 0);
    cJSON_Delete(response);

    uint8_t client_key[CPACE_KEY_LEN];
    uint8_t server_key[CPACE_KEY_LEN];
    CHECK(cpace_client_finish(&cpace, response_bin, client_key, server_key) == 0);

    uint8_t mac[32];
    confirm_mac(client_key, client_static, agent_static, mac);
    char mac_hex[65];
    sodium_bin2hex(mac_hex, sizeof(mac_hex), mac, sizeof(mac));

    cJSON *confirm = cJSON_CreateObject();
    CHECK(confirm != NULL);
    cJSON_AddStringToObject(confirm, "op", "pair_confirm");
    cJSON_AddStringToObject(confirm, "mac", mac_hex);
    CHECK(send_pair_json(fd, confirm) == 0);
    cJSON_Delete(confirm);

    cJSON *ok = recv_json(fd);
    CHECK(ok != NULL);
    const cJSON *ok_op = cJSON_GetObjectItemCaseSensitive(ok, "op");
    CHECK(cJSON_IsString(ok_op) && strcmp(ok_op->valuestring, "pair_ok") == 0);
    cJSON_Delete(ok);

    // 配对记录已落盘
    char peers_path[512];
    snprintf(peers_path, sizeof(peers_path), "%s/peers.txt", state_dir);
    FILE *peers = fopen(peers_path, "r");
    CHECK(peers != NULL);
    char line[256];
    CHECK(fgets(line, sizeof(line), peers) != NULL);
    fclose(peers);
    CHECK(strncmp(line, client_hex, 64) == 0);

    // The code is single-use: a second pair_start must be rejected even while
    // the first paired session is still alive.
    const int fd2 = connect_localhost(port);
    CHECK(fd2 >= 0);
    cJSON *second = cJSON_CreateObject();
    CHECK(second != NULL);
    cJSON_AddStringToObject(second, "op", "pair_start");
    cJSON_AddStringToObject(second, "public_data", public_hex);
    cJSON_AddStringToObject(second, "client_static", client_hex);
    cJSON_AddStringToObject(second, "client_name", "second-desktop");
    CHECK(send_pair_json(fd2, second) == 0);
    cJSON_Delete(second);
    cJSON *second_response = recv_json(fd2);
    CHECK(second_response != NULL);
    const cJSON *second_op =
        cJSON_GetObjectItemCaseSensitive(second_response, "op");
    CHECK(cJSON_IsString(second_op)
          && strcmp(second_op->valuestring, "error") == 0);
    cJSON_Delete(second_response);
    close(fd2);

    close(fd);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    fclose(output);

    char used_path[512];
    snprintf(used_path, sizeof(used_path), "%s/pair_code_used", state_dir);
    unlink(used_path);
    unlink(peers_path);
    char identity_path[512];
    snprintf(identity_path, sizeof(identity_path), "%s/identity.key", state_dir);
    unlink(identity_path);
    rmdir(state_dir);

    printf("test_pair_session: OK\n");
    return 0;
}
