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

#define PEER_NAME_MAX 63
#define PEER_CAPS_MAX 127
#define PEER_LINE_MAX 512

static void set_error(char *error, size_t error_len, const char *message)
{
    if (error && error_len > 0)
        snprintf(error, error_len, "%s", message);
}

static int write_all(int fd, const char *data, size_t size)
{
    size_t done = 0;
    while (done < size) {
        const ssize_t wrote = write(fd, data + done, size - done);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (wrote == 0)
            return -1;
        done += (size_t) wrote;
    }
    return 0;
}

static int ensure_dir(const char *state_dir)
{
    if (mkdir(state_dir, 0700) == 0)
        return 0;
    if (errno == EEXIST)
        return 0;
    return -1;
}

static int is_name_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
}

static int is_caps_char(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
        || c == ',' || c == '_' || c == '-' || c == '.';
}

static int valid_peer_name(const char *name)
{
    const size_t length = name ? strlen(name) : 0;
    if (length == 0 || length > PEER_NAME_MAX)
        return 0;
    for (size_t i = 0; i < length; ++i) {
        if (!is_name_char((unsigned char) name[i]))
            return 0;
    }
    return 1;
}

static int valid_peer_caps(const char *caps)
{
    const size_t length = caps ? strlen(caps) : 0;
    if (length == 0 || length > PEER_CAPS_MAX)
        return 0;
    for (size_t i = 0; i < length; ++i) {
        if (!is_caps_char((unsigned char) caps[i]))
            return 0;
    }
    return 1;
}

typedef struct {
    uint8_t key[AGENT_IDENTITY_KEY_LEN];
    char key_hex[65];
    char name[64];
    char capabilities[128];
} peer_line;

// Strict line grammar: <64 hex> <name> <caps> [trailing whitespace].
// Extra fields, control characters and overlong values are rejected.
static int parse_peer_line(const char *line, peer_line *out)
{
    char key_hex[65] = {0};
    char name[64] = {0};
    char caps[128] = {0};
    int consumed = 0;

    if (sscanf(line, "%64s %63s %127s%n", key_hex, name, caps, &consumed) != 3
        || consumed <= 0)
        return -1;

    for (const char *rest = line + consumed; *rest != '\0'; ++rest) {
        if (*rest != ' ' && *rest != '\t' && *rest != '\r' && *rest != '\n')
            return -1;
    }

    if (strlen(key_hex) != 64 || !valid_peer_name(name)
        || !valid_peer_caps(caps))
        return -1;

    uint8_t key[AGENT_IDENTITY_KEY_LEN];
    if (sodium_hex2bin(key, sizeof(key), key_hex, 64, NULL, NULL, NULL) != 0)
        return -1;

    if (out) {
        memcpy(out->key, key, sizeof(key));
        snprintf(out->key_hex, sizeof(out->key_hex), "%s", key_hex);
        snprintf(out->name, sizeof(out->name), "%s", name);
        snprintf(out->capabilities, sizeof(out->capabilities), "%s", caps);
    }
    return 0;
}

// Returns 1 for a complete line, 2 for an overlong line that was discarded,
// 0 on EOF. Discarding the remainder prevents a long line from being split
// into a second, apparently valid record.
static int read_peer_line(FILE *file, char *line, size_t line_len)
{
    if (!fgets(line, (int) line_len, file))
        return 0;
    if (strchr(line, '\n') == NULL && !feof(file)) {
        int c;
        while ((c = fgetc(file)) != '\n' && c != EOF) {
        }
        return 2;
    }
    return 1;
}

static int cap_list_has(const char *caps, const char *capability)
{
    char copy[128];
    snprintf(copy, sizeof(copy), "%s", caps);
    char *save = NULL;
    for (char *token = strtok_r(copy, ",", &save); token != NULL;
         token = strtok_r(NULL, ",", &save)) {
        if (strcmp(token, capability) == 0)
            return 1;
    }
    return 0;
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
        if (write_all(out, (const char *) identity->private_key,
                      AGENT_IDENTITY_KEY_LEN) != 0
            || fsync(out) != 0) {
            close(out);
            unlink(path);
            set_error(error, error_len, "cannot write identity.key");
            return -1;
        }
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

    FILE *file = fopen(path, "r");
    if (!file)
        return errno == ENOENT ? 0 : -1;

    char line[PEER_LINE_MAX];
    int found = 0;
    while (read_peer_line(file, line, sizeof(line)) == 1) {
        peer_line parsed;
        if (parse_peer_line(line, &parsed) != 0)
            continue;
        if (memcmp(parsed.key, public_key, sizeof(parsed.key)) == 0) {
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

    const char *peer_name = (name && name[0]) ? name : "peer";
    const char *peer_caps = (capabilities && capabilities[0])
        ? capabilities
        : "exec,push,pull";
    if (!valid_peer_name(peer_name)) {
        set_error(error, error_len, "invalid peer name");
        return -1;
    }
    if (!valid_peer_caps(peer_caps)) {
        set_error(error, error_len, "invalid peer capabilities");
        return -1;
    }

    char key[65];
    sodium_bin2hex(key, sizeof(key), public_key, AGENT_IDENTITY_KEY_LEN);

    char line[PEER_LINE_MAX];
    const int length = snprintf(line, sizeof(line), "%s %s %s\n", key,
                                peer_name, peer_caps);
    if (length <= 0 || (size_t) length >= sizeof(line)) {
        set_error(error, error_len, "peer record too long");
        return -1;
    }

    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) {
        set_error(error, error_len, "cannot open peers.txt");
        return -1;
    }
    if (fchmod(fd, 0600) != 0
        || write_all(fd, line, (size_t) length) != 0
        || fsync(fd) != 0) {
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
    peer_record *records = calloc((size_t) capacity, sizeof(peer_record));
    if (!records) {
        fclose(file);
        return -1;
    }

    char line[PEER_LINE_MAX];
    while (read_peer_line(file, line, sizeof(line)) == 1) {
        peer_line parsed;
        if (parse_peer_line(line, &parsed) != 0)
            continue;

        int duplicate = 0;
        for (int i = 0; i < used; ++i) {
            if (memcmp(records[i].public_key, parsed.key,
                       sizeof(parsed.key)) == 0) {
                duplicate = 1;
                break;
            }
        }
        if (duplicate)
            continue;

        if (used == capacity) {
            capacity *= 2;
            peer_record *grown = realloc(records, (size_t) capacity * sizeof(peer_record));
            if (!grown) {
                free(records);
                fclose(file);
                return -1;
            }
            records = grown;
        }

        memcpy(records[used].public_key, parsed.key, sizeof(parsed.key));
        snprintf(records[used].name, sizeof(records[used].name), "%s",
                 parsed.name);
        snprintf(records[used].capabilities, sizeof(records[used].capabilities),
                 "%s", parsed.capabilities);
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
    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp", path)
        >= (int) sizeof(temp_path)) {
        fclose(file);
        return -1;
    }
    FILE *temp = fopen(temp_path, "w");
    if (!temp) {
        fclose(file);
        return -1;
    }
    fchmod(fileno(temp), 0600);

    char line[PEER_LINE_MAX];
    int failed = 0;
    while (read_peer_line(file, line, sizeof(line)) == 1) {
        peer_line parsed;
        if (parse_peer_line(line, &parsed) != 0)
            continue;
        if (memcmp(parsed.key, public_key, sizeof(parsed.key)) == 0)
            continue;
        if (fprintf(temp, "%s %s %s\n", parsed.key_hex, parsed.name,
                    parsed.capabilities) < 0) {
            failed = 1;
            break;
        }
    }

    fclose(file);
    if (fflush(temp) != 0 || fsync(fileno(temp)) != 0)
        failed = 1;
    if (fclose(temp) != 0)
        failed = 1;

    if (failed || rename(temp_path, path) != 0) {
        unlink(temp_path);
        return -1;
    }

    const int dir_fd = open(state_dir, O_RDONLY | O_CLOEXEC);
    if (dir_fd >= 0) {
        fsync(dir_fd);
        close(dir_fd);
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

    char line[PEER_LINE_MAX];
    int found = 0;
    while (read_peer_line(file, line, sizeof(line)) == 1) {
        peer_line parsed;
        if (parse_peer_line(line, &parsed) != 0)
            continue;
        if (cap_list_has(parsed.capabilities, capability)) {
            found = 1;
            break;
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

    FILE *file = fopen(path, "r");
    if (!file)
        return 0;

    char line[PEER_LINE_MAX];
    int found = 0;
    while (read_peer_line(file, line, sizeof(line)) == 1) {
        peer_line parsed;
        if (parse_peer_line(line, &parsed) != 0)
            continue;
        if (memcmp(parsed.key, public_key, sizeof(parsed.key)) != 0)
            continue;
        found = cap_list_has(parsed.capabilities, capability);
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

    char line[PEER_LINE_MAX];
    int result = -1;
    while (read_peer_line(file, line, sizeof(line)) == 1) {
        peer_line parsed;
        if (parse_peer_line(line, &parsed) != 0)
            continue;
        memcpy(public_key, parsed.key, sizeof(parsed.key));
        result = 0;
        break;
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
