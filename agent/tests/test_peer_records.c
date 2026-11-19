#include "secure/identity.h"

#include <sodium.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            return 1;                                                            \
        }                                                                        \
    } while (0)

static int peers_path_for(const char *dir, char *out, size_t out_len)
{
    const int written = snprintf(out, out_len, "%s/peers.txt", dir);
    return written > 0 && (size_t)written < out_len ? 0 : -1;
}

static int append_raw(const char *path, const char *text)
{
    FILE *file = fopen(path, "a");
    if (!file)
        return -1;
    fputs(text, file);
    fclose(file);
    return 0;
}

int main(void)
{
    CHECK(sodium_init() >= 0);

    char dir[] = "/tmp/peer_records_XXXXXX";
    CHECK(mkdtemp(dir) != NULL);

    uint8_t key[AGENT_IDENTITY_KEY_LEN];
    uint8_t other[AGENT_IDENTITY_KEY_LEN];
    randombytes_buf(key, sizeof(key));
    randombytes_buf(other, sizeof(other));

    char key_hex[65];
    char other_hex[65];
    sodium_bin2hex(key_hex, sizeof(key_hex), key, sizeof(key));
    sodium_bin2hex(other_hex, sizeof(other_hex), other, sizeof(other));

    char error[256] = {0};

    // Normal record round-trips.
    CHECK(peers_add(dir, key, "desktop", "exec,push,pull",
                    error, sizeof(error)) == 0);
    peer_record *records = NULL;
    int count = 0;
    CHECK(peers_load(dir, &records, &count) == 0);
    CHECK(count == 1);
    CHECK(strcmp(records[0].name, "desktop") == 0);
    CHECK(strcmp(records[0].capabilities, "exec,push,pull") == 0);
    peers_free(records);
    CHECK(peers_contains(dir, key) == 1);
    CHECK(peers_has_capability_for(dir, key, "push") == 1);
    uint8_t first[AGENT_IDENTITY_KEY_LEN];
    CHECK(peers_first(dir, first) == 0);
    CHECK(memcmp(first, key, sizeof(key)) == 0);

    // Control characters and separators must be rejected before writing.
    CHECK(peers_add(dir, other, "desk\ntop", "exec,push,pull",
                    error, sizeof(error)) != 0);
    CHECK(peers_add(dir, other, "desktop", "exec\ninjected",
                    error, sizeof(error)) != 0);
    CHECK(peers_add(dir, other, "desktop", "exec\rpush",
                    error, sizeof(error)) != 0);
    CHECK(peers_add(dir, other, "desk top", "exec,push,pull",
                    error, sizeof(error)) != 0);
    CHECK(peers_add(dir, other, "desktop", "exec push",
                    error, sizeof(error)) != 0);
    CHECK(peers_add(dir, other, "desktop", "exec\tpush",
                    error, sizeof(error)) != 0);

    // Maximum legal field sizes must fit without truncation/overflow.
    char max_name[64];
    char max_caps[128];
    memset(max_name, 'a', sizeof(max_name) - 1);
    max_name[sizeof(max_name) - 1] = '\0';
    memset(max_caps, 'a', sizeof(max_caps) - 1);
    max_caps[sizeof(max_caps) - 1] = '\0';
    CHECK(peers_add(dir, other, max_name, max_caps,
                    error, sizeof(error)) == 0);

    records = NULL;
    count = 0;
    CHECK(peers_load(dir, &records, &count) == 0);
    CHECK(count == 2);
    int found_max = 0;
    for (int i = 0; i < count; ++i) {
        if (memcmp(records[i].public_key, other, sizeof(other)) == 0) {
            CHECK(strlen(records[i].name) == sizeof(max_name) - 1);
            CHECK(strlen(records[i].capabilities) == sizeof(max_caps) - 1);
            found_max = 1;
        }
    }
    peers_free(records);
    CHECK(found_max == 1);

    // Malformed records are ignored by the strict loader.
    char path[512];
    CHECK(peers_path_for(dir, path, sizeof(path)) == 0);
    char injected[512];
    snprintf(injected, sizeof(injected),
             "%s desktop exec,push,pull extra-field\n"
             "short desktop exec,push,pull\n"
             "%s desktop\n"
             "%s name-with-\001-control exec\n",
             key_hex, key_hex, key_hex);
    CHECK(append_raw(path, injected) == 0);

    records = NULL;
    count = 0;
    CHECK(peers_load(dir, &records, &count) == 0);
    CHECK(count == 2);
    peers_free(records);

    // Removing one peer must also drop malformed lines while preserving the
    // other valid record.
    CHECK(peers_remove(dir, key) == 0);
    records = NULL;
    count = 0;
    CHECK(peers_load(dir, &records, &count) == 0);
    CHECK(count == 1);
    CHECK(memcmp(records[0].public_key, other, sizeof(other)) == 0);
    peers_free(records);
    CHECK(peers_contains(dir, key) == 0);
    CHECK(peers_contains(dir, other) == 1);

    unlink(path);
    rmdir(dir);
    printf("test_peer_records: OK\n");
    return 0;
}
