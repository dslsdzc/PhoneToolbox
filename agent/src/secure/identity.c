#include "identity.h"
#include "util.h"

#include <sodium.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void set_error(char *error, size_t error_len, const char *message)
{
    if (error && error_len > 0)
        snprintf(error, error_len, "%s", message);
}

static int ensure_dir(const char *state_dir)
{
    if (mkdir(state_dir, 0700) == 0)
        return 0;
    if (errno == EEXIST)
        return 0;
    return -1;
}

int identity_load_or_create(agent_identity *identity, const char *state_dir,
                            char *error, size_t error_len)
{
    if (!identity || !state_dir || state_dir[0] == '\0') {
        set_error(error, error_len, "invalid state directory");
        return -1;
    }
    if (ensure_dir(state_dir) != 0) {
        set_error(error, error_len, "cannot create state directory");
        return -1;
    }

    char path[4096];
    if (snprintf(path, sizeof(path), "%s/identity.key", state_dir) >= (int)sizeof(path)) {
        set_error(error, error_len, "state directory path too long");
        return -1;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        const ssize_t got = read(fd, identity->private_key, AGENT_IDENTITY_KEY_LEN);
        close(fd);
        if (got != AGENT_IDENTITY_KEY_LEN) {
            set_error(error, error_len, "identity.key is corrupt");
            return -1;
        }
    } else if (errno == ENOENT) {
        uint8_t public_key[AGENT_IDENTITY_KEY_LEN];
        crypto_box_keypair(public_key, identity->private_key);

        const int out = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (out < 0) {
            set_error(error, error_len, "cannot create identity.key");
            return -1;
        }
        const ssize_t wrote = write(out, identity->private_key, AGENT_IDENTITY_KEY_LEN);
        if (wrote != AGENT_IDENTITY_KEY_LEN) {
            close(out);
            unlink(path);
            set_error(error, error_len, "cannot write identity.key");
            return -1;
        }
        fsync(out);
        close(out);
    } else {
        set_error(error, error_len, "cannot open identity.key");
        return -1;
    }

    crypto_scalarmult_curve25519_base(identity->public_key, identity->private_key);
    return 0;
}

static int peers_path(const char *state_dir, char *path, size_t path_len)
{
    return snprintf(path, path_len, "%s/peers.txt", state_dir) < (int)path_len ? 0 : -1;
}

int peers_contains(const char *state_dir, const uint8_t public_key[AGENT_IDENTITY_KEY_LEN])
{
    if (!state_dir || !public_key)
        return -1;

    char path[4096];
    if (peers_path(state_dir, path, sizeof(path)) != 0)
        return -1;

    char expected[65];
    sodium_bin2hex(expected, sizeof(expected), public_key, AGENT_IDENTITY_KEY_LEN);

    FILE *file = fopen(path, "r");
    if (!file)
        return errno == ENOENT ? 0 : -1;

    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), file)) {
        char key[65];
        if (sscanf(line, "%64s", key) == 1 && strcmp(key, expected) == 0) {
            found = 1;
            break;
        }
    }
    fclose(file);
    return found;
}

int peers_add(const char *state_dir, const uint8_t public_key[AGENT_IDENTITY_KEY_LEN],
              const char *name, const char *capabilities,
              char *error, size_t error_len)
{
    if (!state_dir || !public_key) {
        set_error(error, error_len, "invalid arguments");
        return -1;
    }
    if (ensure_dir(state_dir) != 0) {
        set_error(error, error_len, "cannot create state directory");
        return -1;
    }

    char path[4096];
    if (peers_path(state_dir, path, sizeof(path)) != 0) {
        set_error(error, error_len, "state directory path too long");
        return -1;
    }

    char key[65];
    sodium_bin2hex(key, sizeof(key), public_key, AGENT_IDENTITY_KEY_LEN);

    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
        set_error(error, error_len, "cannot open peers.txt");
        return -1;
    }

    char line[256];
    const int length = snprintf(line, sizeof(line), "%s %s %s\n", key,
                                name && name[0] ? name : "peer",
                                capabilities && capabilities[0]
                                    ? capabilities
                                    : "exec,push,pull");
    if (length <= 0 || write(fd, line, (size_t)length) != length) {
        close(fd);
        set_error(error, error_len, "cannot write peers.txt");
        return -1;
    }
    close(fd);
    return 0;
}

int peers_load(const char *state_dir, peer_record **out, int *count)
{
    if (!state_dir || !out || !count)
        return -1;
    *out = NULL;
    *count = 0;

    char path[4096];
    if (peers_path(state_dir, path, sizeof(path)) != 0)
        return -1;

    FILE *file = fopen(path, "r");
    if (!file)
        return errno == ENOENT ? 0 : -1;

    int capacity = 4;
    int used = 0;
    peer_record *records = calloc((size_t)capacity, sizeof(peer_record));
    if (!records) {
        fclose(file);
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        char key_hex[65] = {0};
        char name[64] = {0};
        char caps[128] = {0};
        if (sscanf(line, "%64s %63s %127s", key_hex, name, caps) < 1)
            continue;

        uint8_t key[AGENT_IDENTITY_KEY_LEN];
        if (sodium_hex2bin(key, sizeof(key), key_hex, 64, NULL, NULL, NULL) != 0)
            continue;

        if (used == capacity) {
            capacity *= 2;
            peer_record *grown = realloc(records, (size_t)capacity * sizeof(peer_record));
            if (!grown) {
                free(records);
                fclose(file);
                return -1;
            }
            records = grown;
        }

        memcpy(records[used].public_key, key, sizeof(key));
        snprintf(records[used].name, sizeof(records[used].name), "%s",
                 name[0] ? name : "peer");
        snprintf(records[used].capabilities, sizeof(records[used].capabilities), "%s",
                 caps[0] ? caps : "exec,push,pull");
        ++used;
    }
    fclose(file);

    *out = records;
    *count = used;
    return 0;
}

void peers_free(peer_record *peers)
{
    free(peers);
}

int peers_remove(const char *state_dir, const uint8_t public_key[AGENT_IDENTITY_KEY_LEN])
{
    if (!state_dir || !public_key)
        return -1;

    char path[4096];
    if (peers_path(state_dir, path, sizeof(path)) != 0)
        return -1;

    FILE *file = fopen(path, "r");
    if (!file)
        return -1;

    char temp_path[4128];
    snprintf(temp_path, sizeof(temp_path), "%s.tmp", path);
    FILE *temp = fopen(temp_path, "w");
    if (!temp) {
        fclose(file);
        return -1;
    }
    chmod(temp_path, 0600);

    char expected[65];
    sodium_bin2hex(expected, sizeof(expected), public_key, AGENT_IDENTITY_KEY_LEN);

    char line[512];
    while (fgets(line, sizeof(line), file)) {
        char key_hex[65] = {0};
        if (sscanf(line, "%64s", key_hex) == 1 && strcmp(key_hex, expected) == 0)
            continue;
        fputs(line, temp);
    }

    fclose(file);
    fflush(temp);
    fsync(fileno(temp));
    fclose(temp);

    if (rename(temp_path, path) != 0) {
        unlink(temp_path);
        return -1;
    }
    return 0;
}

int peers_has_capability(const char *state_dir, const char *capability)
{
    if (!state_dir || !capability)
        return 0;

    char path[4096];
    if (peers_path(state_dir, path, sizeof(path)) != 0)
        return 0;

    FILE *file = fopen(path, "r");
    if (!file)
        return 0;

    char line[512];
    int found = 0;
    if (fgets(line, sizeof(line), file)) {
        char key[65], name[128], caps[256];
        if (sscanf(line, "%64s %127s %255s", key, name, caps) == 3) {
            char *save = NULL;
            for (char *token = strtok_r(caps, ",", &save); token;
                 token = strtok_r(NULL, ",", &save)) {
                if (strcmp(token, capability) == 0) {
                    found = 1;
                    break;
                }
            }
        }
    }
    fclose(file);
    return found;
}

int peers_has_capability_for(const char *state_dir,
                             const uint8_t public_key[AGENT_IDENTITY_KEY_LEN],
                             const char *capability)
{
    if (!state_dir || !public_key || !capability)
        return 0;

    char path[4096];
    if (peers_path(state_dir, path, sizeof(path)) != 0)
        return 0;

    char expected[65];
    sodium_bin2hex(expected, sizeof(expected), public_key, AGENT_IDENTITY_KEY_LEN);

    FILE *file = fopen(path, "r");
    if (!file)
        return 0;

    char line[512];
    int found = 0;
    while (fgets(line, sizeof(line), file)) {
        char key[65], name[128], caps[256];
        if (sscanf(line, "%64s %127s %255s", key, name, caps) != 3)
            continue;
        if (strcmp(key, expected) != 0)
            continue;

        char *save = NULL;
        for (char *token = strtok_r(caps, ",", &save); token;
             token = strtok_r(NULL, ",", &save)) {
            if (strcmp(token, capability) == 0) {
                found = 1;
                break;
            }
        }
        break;
    }
    fclose(file);
    return found;
}

int peers_first(const char *state_dir, uint8_t public_key[AGENT_IDENTITY_KEY_LEN])
{
    if (!state_dir || !public_key)
        return -1;

    char path[4096];
    if (peers_path(state_dir, path, sizeof(path)) != 0)
        return -1;

    FILE *file = fopen(path, "r");
    if (!file)
        return -1;

    char line[256];
    int result = -1;
    if (fgets(line, sizeof(line), file)) {
        char key[65];
        if (sscanf(line, "%64s", key) == 1
            && sodium_hex2bin(public_key, AGENT_IDENTITY_KEY_LEN, key, 64,
                              NULL, NULL, NULL) == 0)
            result = 0;
    }
    fclose(file);
    return result;
}

int identity_fingerprint(const char *state_dir, char out_hex[65],
                         char *error, size_t error_len)
{
    agent_identity identity;
    if (identity_load_or_create(&identity, state_dir, error, error_len) != 0)
        return -1;
    sha256_hex(identity.public_key, AGENT_IDENTITY_KEY_LEN, out_hex);
    return 0;
}
