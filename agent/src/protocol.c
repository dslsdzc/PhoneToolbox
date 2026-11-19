#include "protocol.h"

#include "exec.h"
#include "fileops.h"
#include "frame.h"
#include "util.h"

#include <cJSON.h>
#include <sodium.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CHUNK_SIZE (48u * 1024u)

// 已认证会话的审计 peer 字段 = Noise 静态公钥（64 hex）；明文开发模式回退
// 到旧的 "stage1" 标记。公钥不是秘密，且能区分不同的已配对电脑。
static const char *audit_peer_id(protocol_context *context, char hex[65])
{
    if (!context || sodium_is_zero(context->peer_public,
                                   sizeof(context->peer_public)))
        return "stage1";
    sodium_bin2hex(hex, 65, context->peer_public,
                   sizeof(context->peer_public));
    return hex;
}

static int pair_used_path(protocol_context *context, char *path, size_t path_len)
{
    if (!context || !context->state_dir)
        return -1;
    return snprintf(path, path_len, "%s/pair_code_used", context->state_dir)
                   < (int) path_len
        ? 0
        : -1;
}

static int pair_code_used(protocol_context *context)
{
    char path[4096];
    if (pair_used_path(context, path, sizeof(path)) != 0)
        return -1;
    return access(path, F_OK) == 0 ? 1 : 0;
}

static int mark_pair_code_used(protocol_context *context)
{
    char path[4096];
    if (pair_used_path(context, path, sizeof(path)) != 0)
        return -1;
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0)
        return -1;
    const char value = '1';
    const int wrote = write(fd, &value, 1);
    close(fd);
    if (wrote != 1) {
        unlink(path);
        return -1;
    }
    return 0;
}

typedef struct {
    int client_fd;
    int cancel_fd;
    volatile int stop;
} exec_cancel_watch;

// 命令执行期间主线程阻塞在 run_command；这个观察者只 poll 客户端 socket：
// 断线或收到任何新帧（桌面 cancel）都向 cancel pipe 写一个字节，run_command
// 会 kill 整个进程组。它不消费 socket 数据。
static void *exec_cancel_thread(void *argument)
{
    exec_cancel_watch *watch = (exec_cancel_watch *) argument;
    while (!watch->stop) {
        struct pollfd fd;
        fd.fd = watch->client_fd;
        fd.events = POLLIN;
        fd.revents = 0;
        const int ready = poll(&fd, 1, 100);
        if (ready > 0 && (fd.revents & (POLLIN | POLLHUP | POLLERR))) {
            const char byte = 'x';
            (void) write(watch->cancel_fd, &byte, 1);
            break;
        }
    }
    return NULL;
}

// Pair codes have only 10^6 possibilities. A per-connection sleep() can be
// bypassed by opening many connections, so failures reserve global 1-second
// slots. Successful pairing is not delayed.
static pthread_mutex_t pair_rate_mutex = PTHREAD_MUTEX_INITIALIZER;
static int64_t pair_rate_next_ms;

static void pair_rate_wait(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return;
    const int64_t now = (int64_t) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    int64_t wait_ms = 1000;

    pthread_mutex_lock(&pair_rate_mutex);
    if (now < pair_rate_next_ms) {
        wait_ms = pair_rate_next_ms - now;
        pair_rate_next_ms += 1000;
    } else {
        pair_rate_next_ms = now + 1000;
    }
    pthread_mutex_unlock(&pair_rate_mutex);

    if (wait_ms > 0) {
        struct timespec req;
        req.tv_sec = (time_t) (wait_ms / 1000);
        req.tv_nsec = (long) ((wait_ms % 1000) * 1000000);
        while (nanosleep(&req, &req) != 0 && errno == EINTR) {
        }
    }
}

static int send_payload(protocol_context *context, const uint8_t *payload,
                        uint32_t length){
    if (!context || !payload || length == 0)
        return -1;

    if (!context->noise)
        return frame_write(context->client_fd, payload, length);

    if (length > FRAME_DEFAULT_MAX)
        return -1;

    uint8_t *cipher = malloc((size_t)length + NOISE_TAG_LEN);
    if (!cipher)
        return -1;

    size_t cipher_length = 0;
    char error[128] = {0};
    if (noise_channel_encrypt(context->noise, payload, length, cipher,
                              &cipher_length, (size_t)length + NOISE_TAG_LEN,
                              error, sizeof(error)) != 0) {
        free(cipher);
        return -1;
    }

    uint8_t *frame = malloc(cipher_length + 1);
    if (!frame) {
        free(cipher);
        return -1;
    }
    frame[0] = FRAME_TYPE_NOISE_TRANSPORT;
    memcpy(frame + 1, cipher, cipher_length);
    const int result =
        frame_write(context->client_fd, frame, (uint32_t)(cipher_length + 1));
    free(frame);
    free(cipher);
    return result;
}

static int send_json(protocol_context *context, cJSON *object)
{
    if (!context || !object)
        return -1;

    char *text = cJSON_PrintUnformatted(object);
    if (!text) {
        cJSON_Delete(object);
        return -1;
    }

    const size_t text_length = strlen(text);
    if (text_length + 1 > context->max_frame) {
        free(text);
        cJSON_Delete(object);
        return -1;
    }

    uint8_t *payload = malloc(text_length + 1);
    if (!payload) {
        free(text);
        cJSON_Delete(object);
        return -1;
    }
    payload[0] = FRAME_TYPE_JSON;
    memcpy(payload + 1, text, text_length);

    const int result = send_payload(context, payload, (uint32_t)(text_length + 1));
    free(payload);
    free(text);
    cJSON_Delete(object);
    return result;
}

static int send_error(protocol_context *context, int id, const char *code,
                      const char *message)
{
    cJSON *object = cJSON_CreateObject();
    if (!object)
        return -1;
    cJSON_AddNumberToObject(object, "id", id);
    cJSON_AddStringToObject(object, "op", "error");
    cJSON_AddStringToObject(object, "code", code);
    cJSON_AddStringToObject(object, "message", message ? message : "");
    return send_json(context, object);
}

static int send_binary(protocol_context *context, uint64_t offset,
                       const uint8_t *data, size_t length)
{
    if (!context || length + 9 > context->max_frame)
        return -1;

    uint8_t *payload = malloc(length + 9);
    if (!payload)
        return -1;

    payload[0] = FRAME_TYPE_BINARY;
    for (int i = 0; i < 8; ++i)
        payload[1 + i] = (uint8_t)((offset >> (56 - 8 * i)) & 0xff);
    memcpy(payload + 9, data, length);

    const int result = send_payload(context, payload, (uint32_t)(length + 9));
    free(payload);
    return result;
}

static const char *json_string(const cJSON *root, const char *key)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static int json_int(const cJSON *root, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

static int json_bool(const cJSON *root, const char *key, int fallback)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item))
        return cJSON_IsTrue(item) ? 1 : 0;
    if (cJSON_IsNumber(item))
        return item->valueint != 0;
    return fallback;
}

static int constant_time_equal(const char *a, const char *b)
{
    if (!a || !b)
        return 0;
    const size_t a_len = strlen(a);
    const size_t b_len = strlen(b);
    if (a_len != b_len)
        return 0;
    return sodium_memcmp(a, b, a_len) == 0;
}

static void argv_hash(const char *const argv[], char out_hex[65])
{
    crypto_hash_sha256_state state;
    crypto_hash_sha256_init(&state);
    for (size_t i = 0; argv[i]; ++i) {
        const size_t length = strlen(argv[i]);
        const uint8_t prefix[4] = {
            (uint8_t)((length >> 24) & 0xff),
            (uint8_t)((length >> 16) & 0xff),
            (uint8_t)((length >> 8) & 0xff),
            (uint8_t)(length & 0xff),
        };
        crypto_hash_sha256_update(&state, prefix, sizeof(prefix));
        crypto_hash_sha256_update(&state, (const unsigned char *)argv[i], length);
    }
    uint8_t digest[32];
    crypto_hash_sha256_final(&state, digest);
    sodium_bin2hex(out_hex, 65, digest, sizeof(digest));
}

static int send_output_chunks(protocol_context *context, int id, const char *op,
                              const uint8_t *data, size_t length)
{
    size_t offset = 0;
    while (offset < length) {
        const size_t chunk = (length - offset > CHUNK_SIZE) ? CHUNK_SIZE : length - offset;
        const size_t encoded_length =
            sodium_base64_encoded_len(chunk, sodium_base64_VARIANT_ORIGINAL);
        char *encoded = malloc(encoded_length);
        if (!encoded)
            return -1;
        sodium_bin2base64(encoded, encoded_length, data + offset, chunk,
                          sodium_base64_VARIANT_ORIGINAL);

        cJSON *object = cJSON_CreateObject();
        if (!object) {
            free(encoded);
            return -1;
        }
        cJSON_AddNumberToObject(object, "id", id);
        cJSON_AddStringToObject(object, "op", op);
        cJSON_AddStringToObject(object, "data", encoded);
        const int result = send_json(context, object);
        free(encoded);
        if (result != 0)
            return -1;
        offset += chunk;
    }
    return 0;
}

static int session_has_capability(const protocol_context *context,
                                  const char *capability)
{
    if (!context || !context->state_dir)
        return 0;
    if (context->has_peer) {
        // Root 代理本身就是受 Noise + 配对保护的完整 root 通道；
        // 配对即授权，不再要求额外的 dangerous 能力位。
        (void)capability;
        return 1;
    }
    // 明文开发模式没有 peer 身份，沿用旧的第一条记录语义。
    return peers_has_capability(context->state_dir, capability);
}

static int command_is_dangerous(const char *const argv[])
{
    static const char *const blocked[] = {
        "dd", "mkfs", "mkfs.ext4", "fastboot", "flash", "erase",
        "format", "wipe", "rm", "reboot", "shutdown", NULL
    };
    // Interpreters can turn an allowed argv into an arbitrary command. In a
    // restricted capability mode they must be treated as dangerous too.
    static const char *const interpreters[] = {
        "sh", "ash", "bash", "busybox", "toybox", "env", "nice", "nohup",
        "setsid", "timeout", "xargs", "find", "python", "python3", "perl",
        "ruby", "node", "php", "lua", "awk", "sed", NULL
    };
    const char *base = argv[0];
    const char *slash = strrchr(base, '/');
    if (slash)
        base = slash + 1;
    for (int i = 0; blocked[i]; ++i) {
        if (strcmp(base, blocked[i]) == 0)
            return 1;
    }
    for (int i = 0; interpreters[i]; ++i) {
        if (strcmp(base, interpreters[i]) == 0)
            return 1;
    }
    for (int i = 1; argv[i]; ++i) {
        if (strstr(argv[i], "/dev/block"))
            return 1;
    }
    return 0;
}

static int handle_exec(const protocol_context *context, const cJSON *root, int id)
{
    const cJSON *argv_json = cJSON_GetObjectItemCaseSensitive(root, "argv");
    const int shell = json_bool(root, "shell", 0);
    const int timeout_ms = json_int(root, "timeout_ms", 30000);

    const char **argv = NULL;
    const char *shell_argv[4] = {"/bin/sh", "-c", NULL, NULL};
    if (shell) {
        const char *command = json_string(root, "command");
        if (!command)
            return send_error(context, id, "bad_request",
                              "shell exec requires command");
        shell_argv[2] = command;
        argv = shell_argv;
    } else {
        if (!cJSON_IsArray(argv_json))
            return send_error(context, id, "bad_request",
                              "argv must be an array");

        const int count = cJSON_GetArraySize(argv_json);
        if (count <= 0 || count > 256)
            return send_error(context, id, "bad_request",
                              "argv size out of range");

        argv = calloc((size_t)count + 1, sizeof(char *));
        if (!argv)
            return send_error(context, id, "internal_error",
                              "out of memory");

        for (int i = 0; i < count; ++i) {
            const cJSON *item = cJSON_GetArrayItem(argv_json, i);
            if (!cJSON_IsString(item) || item->valuestring[0] == '\0') {
                free(argv);
                return send_error(context, id, "bad_request",
                                  "argv entries must be non-empty strings");
            }
            argv[i] = item->valuestring;
        }
        argv[count] = NULL;
    }

    if (timeout_ms <= 0 || timeout_ms > 600000) {
        if (!shell)
            free(argv);
        return send_error(context, id, "bad_request",
                          "timeout_ms out of range");
    }

    if (!session_has_capability(context, "dangerous")) {
        if (shell || command_is_dangerous(argv)) {
            if (!shell)
                free(argv);
            return send_error(context, id, "permission_denied",
                              "dangerous command requires capability");
        }
    }

    exec_result result = {0};
    int cancel_pipe[2] = {-1, -1};
    exec_cancel_watch watch = {context->client_fd, -1, 0};
    pthread_t watcher;
    int watcher_started = 0;
    if (pipe2(cancel_pipe, O_CLOEXEC) == 0) {
        watch.cancel_fd = cancel_pipe[1];
        watcher_started = pthread_create(&watcher, NULL, exec_cancel_thread,
                                         &watch) == 0;
    }
    const int run = run_command_cancellable(
        argv, timeout_ms, context->max_output,
        cancel_pipe[0] >= 0 ? cancel_pipe[0] : -1, &result);
    watch.stop = 1;
    if (watcher_started)
        pthread_join(watcher, NULL);
    if (cancel_pipe[0] >= 0) {
        close(cancel_pipe[0]);
        close(cancel_pipe[1]);
    }

    char hash[65];
    argv_hash(argv, hash);

    if (!shell)
        free(argv);

    if (run != 0) {
        exec_result_free(&result);
        return send_error(context, id, "spawn_failed",
                          "cannot start command");
    }

    int send_result = 0;
    if (send_output_chunks(context, id, "stdout",
                           result.stdout_data, result.stdout_length) != 0)
        send_result = -1;
    if (send_result == 0
        && send_output_chunks(context, id, "stderr",
                              result.stderr_data, result.stderr_length) != 0)
        send_result = -1;

    if (send_result == 0) {
        cJSON *object = cJSON_CreateObject();
        if (!object) {
            send_result = -1;
        } else {
            cJSON_AddNumberToObject(object, "id", id);
            cJSON_AddStringToObject(object, "op", "exit");
            cJSON_AddNumberToObject(object, "code", result.exit_code);
            cJSON_AddBoolToObject(object, "timed_out", result.timed_out);
            cJSON_AddBoolToObject(object, "canceled", result.canceled);
            cJSON_AddBoolToObject(object, "truncated", result.truncated);
            cJSON_AddBoolToObject(object, "signaled", result.signaled);
            send_result = send_json(context, object);
        }
    }

    if (context->audit) {
        char peer_hex[65];
        audit_append(context->audit, audit_peer_id(context, peer_hex), "exec",
                     hash, result.exit_code);
    }

    exec_result_free(&result);
    return send_result;
}

static int handle_pull(const protocol_context *context, const cJSON *root, int id)
{
    const char *path = json_string(root, "path");
    if (!path)
        return send_error(context, id, "bad_request",
                          "pull requires path");

    uint64_t size = 0;
    char sha256[65] = {0};
    char error[256] = {0};
    if (pull_meta(path, &size, sha256, error, sizeof(error)) != 0)
        return send_error(context, id, "io_error", error);

    cJSON *meta = cJSON_CreateObject();
    if (!meta)
        return -1;
    cJSON_AddNumberToObject(meta, "id", id);
    cJSON_AddStringToObject(meta, "op", "pull_meta");
    cJSON_AddNumberToObject(meta, "size", (double)size);
    cJSON_AddStringToObject(meta, "sha256", sha256);
    if (send_json(context, meta) != 0)
        return -1;

    uint8_t buffer[CHUNK_SIZE];
    uint64_t offset = 0;
    while (offset < size) {
        const size_t want = (size - offset > CHUNK_SIZE) ? CHUNK_SIZE
                                                         : (size_t)(size - offset);
        size_t got = 0;
        if (pull_read(path, offset, buffer, want, &got, error, sizeof(error)) != 0)
            return send_error(context, id, "io_error", error);
        if (got == 0)
            break;
        if (send_binary(context, offset, buffer, got) != 0)
            return -1;
        offset += got;
    }

    if (offset != size)
        return send_error(context, id, "io_error",
                          "file changed while reading");

    cJSON *done = cJSON_CreateObject();
    if (!done)
        return -1;
    cJSON_AddNumberToObject(done, "id", id);
    cJSON_AddStringToObject(done, "op", "pull_done");
    if (context->audit) {
        char peer_hex[65];
        audit_append(context->audit, audit_peer_id(context, peer_hex), "pull",
                     sha256, 0);
    }
    return send_json(context, done);
}

static int handle_stat(const protocol_context *context, const cJSON *root, int id)
{
    const char *path = json_string(root, "path");
    if (!path)
        return send_error(context, id, "bad_request",
                          "stat requires path");

    uint64_t size = 0;
    uint32_t mode = 0;
    int is_dir = 0;
    char error[256] = {0};
    if (file_stat(path, &size, &mode, &is_dir, error, sizeof(error)) != 0)
        return send_error(context, id, "io_error", error);

    cJSON *object = cJSON_CreateObject();
    if (!object)
        return -1;
    cJSON_AddNumberToObject(object, "id", id);
    cJSON_AddStringToObject(object, "op", "stat_result");
    cJSON_AddNumberToObject(object, "size", (double)size);
    char mode_text[8];
    snprintf(mode_text, sizeof(mode_text), "%04o", mode & 07777);
    cJSON_AddStringToObject(object, "mode", mode_text);
    cJSON_AddBoolToObject(object, "is_dir", is_dir != 0);
    return send_json(context, object);
}

// 明文模式直接读帧；Noise 模式读 0x12 帧并解密出原始 payload。
static int read_next_payload(protocol_context *context, uint8_t **payload,
                             uint32_t *length)
{
    if (frame_read(context->client_fd, payload, length,
                   (uint32_t)context->max_frame) != 0)
        return -1;

    if (!context->noise)
        return 0;

    if (*length < 1 || (*payload)[0] != FRAME_TYPE_NOISE_TRANSPORT) {
        free(*payload);
        *payload = NULL;
        return -1;
    }

    uint8_t *plain = malloc(*length);
    if (!plain) {
        free(*payload);
        *payload = NULL;
        return -1;
    }

    size_t plain_length = 0;
    char error[128] = {0};
    if (noise_channel_decrypt(context->noise, *payload + 1, *length - 1,
                              plain, &plain_length, *length,
                              error, sizeof(error)) != 0) {
        free(*payload);
        free(plain);
        *payload = NULL;
        return -1;
    }

    free(*payload);
    *payload = plain;
    *length = (uint32_t)plain_length;
    return 0;
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    if (!hex || strlen(hex) != out_len * 2)
        return -1;
    return sodium_hex2bin(out, out_len, hex, out_len * 2, NULL, NULL, NULL) == 0
               ? 0
               : -1;
}

static void pair_confirm_mac(const uint8_t key[CPACE_KEY_LEN],
                             const uint8_t client_static[AGENT_IDENTITY_KEY_LEN],
                             const uint8_t agent_static[AGENT_IDENTITY_KEY_LEN],
                             uint8_t out[32])
{
    crypto_auth_hmacsha256_state state;
    crypto_auth_hmacsha256_init(&state, key, CPACE_KEY_LEN);
    crypto_auth_hmacsha256_update(&state, (const unsigned char *)"pair-confirm", 12);
    crypto_auth_hmacsha256_update(&state, client_static, AGENT_IDENTITY_KEY_LEN);
    crypto_auth_hmacsha256_update(&state, agent_static, AGENT_IDENTITY_KEY_LEN);
    crypto_auth_hmacsha256_final(&state, out);
}

// 首次配对：pair_start（CPace）→ pair_response → pair_confirm → pair_ok。
// 返回 0 继续等待，1 配对完成，-1 失败/结束。
static int serve_pair(protocol_context *context, const uint8_t *payload,
                      uint32_t length)
{
    if (!context->identity || !context->state_dir || length < 2)
        return -1;

    cJSON *root = cJSON_ParseWithLength((const char *)payload + 1, length - 1);
    if (!root) {
        send_error(context, 0, "bad_request", "invalid pair JSON");
        return -1;
    }

    const char *op = json_string(root, "op");
    int result = -1;

    if (op && strcmp(op, "pair_start") == 0) {
        // 配对码一次性：成功配对后写 pair_code_used，APK 重启时轮换新码。
        if (pair_code_used(context) != 0) {
            send_error(context, 0, "pairing_disabled", "pair code already used");
            goto done;
        }
        // 允许多台电脑配对：这里不再拒绝已配对状态，重复公钥在 confirm 阶段去重。
        if (!context->pair_code || context->pair_code[0] == '\0') {
            send_error(context, 0, "pairing_disabled", "no pair code provided");
            goto done;
        }

        uint8_t public_data[CPACE_PUBLIC_DATA_LEN];
        uint8_t client_static[AGENT_IDENTITY_KEY_LEN];
        if (hex_to_bytes(json_string(root, "public_data"), public_data,
                         sizeof(public_data)) != 0
            || hex_to_bytes(json_string(root, "client_static"), client_static,
                            sizeof(client_static)) != 0) {
            send_error(context, 0, "bad_request", "invalid public_data/client_static");
            goto done;
        }

        static const char ad[] = "PhoneToolbox/root-agent/1";
        uint8_t response[CPACE_RESPONSE_LEN];
        uint8_t client_key[CPACE_KEY_LEN];
        uint8_t server_key[CPACE_KEY_LEN];
        if (cpace_server_respond(public_data, context->pair_code,
                                 strlen(context->pair_code), "desktop", "agent",
                                 ad, sizeof(ad) - 1, response, client_key,
                                 server_key) != 0) {
            send_error(context, 0, "pair_failed", "CPace failed");
            goto done;
        }

        context->pair_pending = 1;
        memcpy(context->pair_client_static, client_static,
               sizeof(context->pair_client_static));
        memcpy(context->pair_client_key, client_key, sizeof(context->pair_client_key));
        const char *client_name = json_string(root, "client_name");
        snprintf(context->pair_client_name, sizeof(context->pair_client_name), "%s",
                 client_name ? client_name : "desktop");
        const char *capabilities = json_string(root, "capabilities");
        snprintf(context->pair_capabilities, sizeof(context->pair_capabilities), "%s",
                 capabilities && capabilities[0] ? capabilities : "exec,push,pull");

        char response_hex[65];
        char agent_hex[65];
        sodium_bin2hex(response_hex, sizeof(response_hex), response, sizeof(response));
        sodium_bin2hex(agent_hex, sizeof(agent_hex), context->identity->public_key,
                       AGENT_IDENTITY_KEY_LEN);

        cJSON *reply = cJSON_CreateObject();
        if (!reply)
            goto done;
        cJSON_AddStringToObject(reply, "op", "pair_response");
        cJSON_AddStringToObject(reply, "response", response_hex);
        cJSON_AddStringToObject(reply, "agent_static", agent_hex);
        result = send_json(context, reply) == 0 ? 0 : -1;
    } else if (op && strcmp(op, "pair_confirm") == 0) {
        if (!context->pair_pending) {
            send_error(context, 0, "bad_request", "no pairing in progress");
            goto done;
        }

        uint8_t received[32];
        if (hex_to_bytes(json_string(root, "mac"), received, sizeof(received)) != 0) {
            send_error(context, 0, "bad_request", "invalid mac");
            goto done;
        }

        uint8_t expected[32];
        pair_confirm_mac(context->pair_client_key, context->pair_client_static,
                         context->identity->public_key, expected);
        if (sodium_memcmp(received, expected, sizeof(expected)) != 0) {
            // 6 位配对码只有 10^6 空间；失败后全局限速，在线爆破降到 1 次/秒。
            pair_rate_wait();
            // 绝不要把 CPace 密钥、MAC 或配对码写进日志。
            fprintf(stderr, "pair: confirm MAC mismatch\n");
            send_error(context, 0, "pair_failed", "confirm MAC mismatch");
            goto done;
        }

        char error[256] = {0};
        const int already = peers_contains(context->state_dir,
                                           context->pair_client_static);
        if (already < 0) {
            send_error(context, 0, "io_error", "cannot read peers.txt");
            goto done;
        }
        // 已存在的公钥先删除旧行，再按本次配对的能力重写（可能收紧/放开 dangerous）。
        if (already == 1
            && peers_remove(context->state_dir,
                            context->pair_client_static) != 0) {
            send_error(context, 0, "io_error", "cannot update peers.txt");
            goto done;
        }
        if (mark_pair_code_used(context) != 0) {
            send_error(context, 0, "io_error", "cannot mark pair code used");
            goto done;
        }
        if (peers_add(context->state_dir, context->pair_client_static,
                     context->pair_client_name, context->pair_capabilities,
                     error, sizeof(error)) != 0) {
            char used_path[4096];
            if (pair_used_path(context, used_path, sizeof(used_path)) == 0)
                unlink(used_path);
            send_error(context, 0, "io_error", error);
            goto done;
        }

        printf("PAIRING_OK=1\n");
        printf("PAIRED=1\n");
        fflush(stdout);

        cJSON *ok = cJSON_CreateObject();
        if (!ok)
            goto done;
        cJSON_AddStringToObject(ok, "op", "pair_ok");
        send_json(context, ok);
        result = 1;
    } else {
        send_error(context, 0, "bad_request", "unknown pair op");
    }

done:
    cJSON_Delete(root);
    return result;
}

// Noise_IK responder：读 msg1 → 校验发起方静态公钥 → 写 msg2 → split。
// 注意：Noise_IK 的发起方静态公钥在 msg1 内加密传输，初始化时传入的
// remote 公钥不会参与认证；读完后必须取出实际公钥并对照 peers.txt。
static int serve_noise_handshake(protocol_context *context,
                                 const uint8_t *first, uint32_t first_length)
{
    if (!context->identity || !context->state_dir)
        return -1;

    peer_record *peers = NULL;
    int count = 0;
    if (peers_load(context->state_dir, &peers, &count) != 0 || count <= 0) {
        fprintf(stderr, "noise: no paired peer\n");
        peers_free(peers);
        return -1;
    }

    if (first_length <= 1) {
        peers_free(peers);
        return -1;
    }

    static const char prologue[] = "PhoneToolbox/root-agent/1";
    noise_channel channel;
    char error[256] = {0};
    // 用第一条 peer 作为初始化占位；真正的发起方公钥从 msg1 读取。
    if (noise_channel_init(&channel, 0, context->identity->private_key,
                           peers[0].public_key, prologue, sizeof(prologue) - 1,
                           error, sizeof(error)) != 0) {
        fprintf(stderr, "noise: init failed: %s\n", error);
        peers_free(peers);
        return -1;
    }
    peers_free(peers);

    if (noise_channel_read_handshake(&channel, first + 1, first_length - 1,
                                     error, sizeof(error)) != 0) {
        fprintf(stderr, "noise: read msg1 failed: %s\n", error);
        noise_channel_free(&channel);
        return -1;
    }

    uint8_t actual_peer[AGENT_IDENTITY_KEY_LEN];
    if (noise_channel_remote_static(&channel, actual_peer,
                                    error, sizeof(error)) != 0) {
        fprintf(stderr, "noise: cannot read peer key: %s\n", error);
        noise_channel_free(&channel);
        return -1;
    }

    if (peers_contains(context->state_dir, actual_peer) != 1) {
        char hex[65];
        sodium_bin2hex(hex, sizeof(hex), actual_peer, sizeof(actual_peer));
        fprintf(stderr, "noise: rejected unknown peer %s\n", hex);
        noise_channel_free(&channel);
        return -1;
    }

    uint8_t msg2[NOISE_MAX_HANDSHAKE_MSG];
    size_t msg2_length = 0;
    if (noise_channel_write_handshake(&channel, msg2, &msg2_length,
                                      sizeof(msg2), error, sizeof(error)) != 0) {
        fprintf(stderr, "noise: write msg2 failed: %s\n", error);
        noise_channel_free(&channel);
        return -1;
    }

    uint8_t *frame = malloc(msg2_length + 1);
    if (!frame) {
        noise_channel_free(&channel);
        return -1;
    }
    frame[0] = FRAME_TYPE_NOISE_HANDSHAKE;
    memcpy(frame + 1, msg2, msg2_length);
    const int sent = frame_write(context->client_fd, frame,
                                 (uint32_t)(msg2_length + 1));
    free(frame);
    if (sent != 0) {
        noise_channel_free(&channel);
        return -1;
    }

    if (noise_channel_finish_handshake(&channel, error, sizeof(error)) != 0) {
        fprintf(stderr, "noise: split failed: %s\n", error);
        noise_channel_free(&channel);
        return -1;
    }

    context->noise = malloc(sizeof(noise_channel));
    if (!context->noise) {
        noise_channel_free(&channel);
        return -1;
    }

    context->has_peer = 1;
    memcpy(context->peer_public, actual_peer, sizeof(context->peer_public));
    *context->noise = channel;
    fprintf(stderr, "noise: authenticated peer\n");
    return 0;
}

int protocol_serve_client(protocol_context *context)
{
    if (!context || context->client_fd < 0)
        return -1;

    uint8_t *payload = NULL;
    uint32_t length = 0;
    if (frame_read(context->client_fd, &payload, &length,
                   (uint32_t)context->max_frame) != 0)
        return -1;

    if (payload[0] == FRAME_TYPE_NOISE_HANDSHAKE) {
        const int noise_result = serve_noise_handshake(context, payload, length);
        free(payload);
        if (noise_result != 0)
            return -1;
        // Noise_IK 已完成双向认证；进入加密循环。
    } else if (payload[0] == FRAME_TYPE_PAIR_CONTROL) {
        int pair_result = serve_pair(context, payload, length);
        free(payload);
        while (pair_result == 0) {
            payload = NULL;
            length = 0;
            if (frame_read(context->client_fd, &payload, &length,
                           (uint32_t)context->max_frame) != 0)
                return -1;
            if (payload[0] != FRAME_TYPE_PAIR_CONTROL) {
                free(payload);
                return -1;
            }
            pair_result = serve_pair(context, payload, length);
            free(payload);
        }
        return -1; // 配对完成/失败都关闭，要求客户端用 Noise 重连
    } else {
        // 明文 token 只允许 --allow-plaintext 开发模式；默认拒绝。
        if (payload[0] != FRAME_TYPE_JSON || !context->allow_plaintext) {
            free(payload);
            return -1;
        }

        int authenticated = 0;
        cJSON *root = cJSON_ParseWithLength((const char *)payload + 1, length - 1);
        if (root) {
            const char *op = json_string(root, "op");
            const char *token = json_string(root, "token");
            if (op && strcmp(op, "auth") == 0
                && constant_time_equal(token, context->token)) {
                cJSON *ok = cJSON_CreateObject();
                if (ok) {
                    cJSON_AddStringToObject(ok, "op", "auth_ok");
                    cJSON_AddNumberToObject(ok, "protocol", AGENT_PROTOCOL_VERSION);
                    cJSON_AddStringToObject(ok, "version", AGENT_VERSION);
                    authenticated = send_json(context, ok) == 0;
                }
            }
            cJSON_Delete(root);
        }
        free(payload);
        if (!authenticated)
            return -1;
    }

    push_session push;
    memset(&push, 0, sizeof(push));
    push.fd = -1;
    push.dir_fd = -1;

    int result = 0;
    while (result == 0) {
        payload = NULL;
        length = 0;
        if (read_next_payload(context, &payload, &length) != 0) {
            result = -1;
            break;
        }

        // 已删除配对的电脑不能继续使用现有连接：每帧前检查 peers.txt。
        if (context->has_peer
            && peers_contains(context->state_dir, context->peer_public) != 1) {
            fprintf(stderr, "noise: peer was removed, closing session\n");
            send_error(context, 0, "peer_revoked", "pairing was removed");
            free(payload);
            result = -1;
            break;
        }

        if (payload[0] == FRAME_TYPE_BINARY) {
            if (!push.active || length < 9) {
                free(payload);
                result = -1;
                break;
            }
            uint64_t offset = 0;
            for (int i = 0; i < 8; ++i)
                offset = (offset << 8) | payload[1 + i];
            char error[256] = {0};
            if (push_write(&push, offset, payload + 9, length - 9,
                           error, sizeof(error)) != 0) {
                send_error(context, 0, "io_error", error);
                free(payload);
                result = -1;
                break;
            }
            free(payload);

            if (push.written == push.expected_size) {
                if (push_commit(&push, error, sizeof(error)) != 0) {
                    send_error(context, 0, "io_error", error);
                    result = -1;
                    break;
                }
                if (context->audit) {
                    char peer_hex[65];
                    audit_append(context->audit,
                                 audit_peer_id(context, peer_hex), "push",
                                 "n/a", 0);
                }
                cJSON *done = cJSON_CreateObject();
                if (!done) {
                    result = -1;
                    break;
                }
                cJSON_AddStringToObject(done, "op", "push_done");
                result = send_json(context, done);
            }
            continue;
        }

        if (payload[0] != FRAME_TYPE_JSON) {
            free(payload);
            result = -1;
            break;
        }

        cJSON *root = cJSON_ParseWithLength((const char *)payload + 1, length - 1);
        free(payload);
        if (!root) {
            send_error(context, 0, "bad_request", "invalid JSON");
            return -1;
        }

        const char *op = json_string(root, "op");
        const int id = json_int(root, "id", 0);

        if (!op) {
            result = send_error(context, id, "bad_request",
                                "missing op");
        } else if (strcmp(op, "ping") == 0) {
            cJSON *pong = cJSON_CreateObject();
            if (!pong) {
                result = -1;
            } else {
                cJSON_AddNumberToObject(pong, "id", id);
                cJSON_AddStringToObject(pong, "op", "pong");
                cJSON_AddNumberToObject(pong, "protocol", AGENT_PROTOCOL_VERSION);
                cJSON_AddStringToObject(pong, "version", AGENT_VERSION);
                cJSON_AddNumberToObject(pong, "uid", (int)getuid());
                result = send_json(context, pong);
            }
        } else if (strcmp(op, "cancel") == 0) {
            // The exec watcher already killed the process group when it saw
            // this frame; just acknowledge so the stream stays in sync.
            cJSON *reply = cJSON_CreateObject();
            if (!reply) {
                result = -1;
            } else {
                cJSON_AddNumberToObject(reply, "id", id);
                cJSON_AddStringToObject(reply, "op", "cancel_ok");
                result = send_json(context, reply);
            }
        } else if (strcmp(op, "exec") == 0) {
            result = handle_exec(context, root, id);
        } else if (strcmp(op, "push") == 0) {
            if (push.active) {
                result = send_error(context, id, "busy",
                                    "another push is active");
            } else {
                const char *path = json_string(root, "path");
                const char *sha256 = json_string(root, "sha256");
                const char *mode = json_string(root, "mode");
                const cJSON *size_item =
                    cJSON_GetObjectItemCaseSensitive(root, "size");
                const double size_value =
                    cJSON_IsNumber(size_item) ? size_item->valuedouble : -1;
                if (!path || !sha256 || size_value < 0
                    || size_value > 9.0e15) {
                    result = send_error(context, id, "bad_request",
                                        "push requires path/size/sha256");
                } else if (strstr(path, "/dev/block")
                           && !session_has_capability(context, "dangerous")) {
                    result = send_error(context, id, "permission_denied",
                                        "writing /dev/block requires dangerous capability");
                } else {
                    char error[256] = {0};
                    if (push_begin(&push, path, (uint64_t)size_value, mode,
                                   sha256, json_bool(root, "overwrite", 0),
                                   error, sizeof(error)) != 0) {
                        result = send_error(context, id, "io_error",
                                            error);
                    } else {
                        cJSON *ready = cJSON_CreateObject();
                        if (!ready) {
                            push_abort(&push);
                            result = -1;
                        } else {
                            cJSON_AddNumberToObject(ready, "id", id);
                            cJSON_AddStringToObject(ready, "op", "push_ready");
                            result = send_json(context, ready);
                        }
                    }
                }
            }
        } else if (strcmp(op, "pull") == 0) {
            result = handle_pull(context, root, id);
        } else if (strcmp(op, "stat") == 0) {
            result = handle_stat(context, root, id);
        } else if (strcmp(op, "shutdown") == 0) {
            cJSON *bye = cJSON_CreateObject();
            if (bye) {
                cJSON_AddNumberToObject(bye, "id", id);
                cJSON_AddStringToObject(bye, "op", "bye");
                send_json(context, bye);
            }
            result = -1; // 关闭连接，server 退出由 main 决定
        } else {
            result = send_error(context, id, "bad_request",
                                "unknown op");
        }

        cJSON_Delete(root);
    }

    push_abort(&push);
    return result;
}
