#include "audit.h"

#include <sodium.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            return 1;                                                            \
        }                                                                        \
    } while (0)

static int read_lines(const char *path, char lines[8][512], int *count)
{
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;

    *count = 0;
    while (*count < 8 && fgets(lines[*count], 512, file))
        ++(*count);
    fclose(file);
    return 0;
}

static int token_at(char *line, int index, char *out, size_t out_size)
{
    char *save = NULL;
    char *token = strtok_r(line, " \n", &save);
    for (int i = 0; token; ++i) {
        if (i == index) {
            snprintf(out, out_size, "%s", token);
            return 0;
        }
        token = strtok_r(NULL, " \n", &save);
    }
    return -1;
}

static int last_entry_hash(const char *path, char out[65])
{
    FILE *file = fopen(path, "r");
    if (!file)
        return 1;
    char line[512];
    char last[512] = {0};
    while (fgets(line, sizeof(line), file)) {
        snprintf(last, sizeof(last), "%s", line);
    }
    fclose(file);
    if (last[0] == '\0')
        return 1;
    return token_at(last, 6, out, 65);
}

static int make_oversized_log(const char *path, char out_hash[65])
{
    audit_log log;
    if (audit_open(&log, path) != 0)
        return 1;
    struct stat st;
    int appends = 0;
    do {
        if (audit_append(&log, "peer", "exec", "aaaaaaaa", 0) != 0) {
            audit_close(&log);
            return 1;
        }
        ++appends;
        if (stat(path, &st) != 0 || appends > 200000) {
            audit_close(&log);
            return 1;
        }
    } while (st.st_size <= (off_t)(8u * 1024u * 1024u));
    audit_close(&log);
    return last_entry_hash(path, out_hash);
}

static int verify_new_chain(const char *path, const char *expected_prev)
{
    char lines[8][512];
    int count = 0;
    if (read_lines(path, lines, &count) != 0 || count < 1)
        return 1;
    char prev_hex[65];
    if (token_at(lines[0], 0, prev_hex, sizeof(prev_hex)) != 0)
        return 1;
    return strcmp(prev_hex, expected_prev) == 0 ? 0 : 1;
}

int main(void)
{
    CHECK(sodium_init() >= 0);

    char directory[] = "/tmp/agent_audit_XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char path[512];
    snprintf(path, sizeof(path), "%s/audit.log", directory);

    audit_log log;
    CHECK(audit_open(&log, path) == 0);
    CHECK(audit_append(&log, "peer1", "exec", "aaaaaaaa", 0) == 0);
    CHECK(audit_append(&log, "peer1", "push", "bbbbbbbb", 0) == 0);
    audit_close(&log);

    char lines[8][512];
    int count = 0;
    CHECK(read_lines(path, lines, &count) == 0);
    CHECK(count == 2);

    char first_prev[65], first_entry[65];
    char second_prev[65], second_entry[65];
    char copy[512];

    snprintf(copy, sizeof(copy), "%s", lines[0]);
    CHECK(token_at(copy, 0, first_prev, sizeof(first_prev)) == 0);
    snprintf(copy, sizeof(copy), "%s", lines[0]);
    CHECK(token_at(copy, 6, first_entry, sizeof(first_entry)) == 0);

    snprintf(copy, sizeof(copy), "%s", lines[1]);
    CHECK(token_at(copy, 0, second_prev, sizeof(second_prev)) == 0);
    snprintf(copy, sizeof(copy), "%s", lines[1]);
    CHECK(token_at(copy, 6, second_entry, sizeof(second_entry)) == 0);

    CHECK(strcmp(first_prev, "00000000000000000000000000000000"
                             "00000000000000000000000000000000") == 0);
    CHECK(strcmp(first_entry, second_prev) == 0);

    // 重新打开后从最后一行恢复 prev，继续追加仍连续
    CHECK(audit_open(&log, path) == 0);
    CHECK(audit_append(&log, "peer2", "pull", "cccccccc", 0) == 0);
    audit_close(&log);

    CHECK(read_lines(path, lines, &count) == 0);
    CHECK(count == 3);

    char third_prev[65];
    snprintf(copy, sizeof(copy), "%s", lines[2]);
    CHECK(token_at(copy, 0, third_prev, sizeof(third_prev)) == 0);
    CHECK(strcmp(third_prev, second_entry) == 0);

    // Tampering with an existing entry must not be silently extended: the
    // corrupt log is quarantined and a fresh chain starts.
    FILE *tamper = fopen(path, "r+");
    CHECK(tamper != NULL);
    char first_line[512];
    CHECK(fgets(first_line, sizeof(first_line), tamper) != NULL);
    size_t first_len = strlen(first_line);
    CHECK(first_len > 2);
    first_line[first_len - 2] = (first_line[first_len - 2] == '0') ? '1' : '0';
    CHECK(fseek(tamper, 0, SEEK_SET) == 0);
    CHECK(fwrite(first_line, 1, first_len, tamper) == first_len);
    fclose(tamper);

    CHECK(audit_open(&log, path) == 0);
    CHECK(audit_append(&log, "peer3", "exec", "dddddddd", 0) == 0);
    audit_close(&log);
    CHECK(read_lines(path, lines, &count) == 0);
    CHECK(count == 1);

    unlink(path);
    rmdir(directory);

    // Rotation must preserve the chain across files.
    char rotation_dir[] = "/tmp/agent_audit_rot_XXXXXX";
    CHECK(mkdtemp(rotation_dir) != NULL);
    char rotation_path[512];
    char rotated_path[512];
    char anchor_path[512];
    snprintf(rotation_path, sizeof(rotation_path), "%s/audit.log",
             rotation_dir);
    snprintf(rotated_path, sizeof(rotated_path), "%s/audit.log.1",
             rotation_dir);
    snprintf(anchor_path, sizeof(anchor_path), "%s/audit.log.prev",
             rotation_dir);

    char last_hash[65];
    CHECK(make_oversized_log(rotation_path, last_hash) == 0);
    CHECK(audit_open(&log, rotation_path) == 0);
    CHECK(audit_append(&log, "peer4", "exec", "eeeeeeee", 0) == 0);
    audit_close(&log);

    CHECK(access(rotated_path, F_OK) == 0);
    CHECK(access(anchor_path, F_OK) == 0);
    CHECK(verify_new_chain(rotation_path, last_hash) == 0);
    CHECK(audit_open(&log, rotation_path) == 0);
    CHECK(audit_append(&log, "peer5", "pull", "ffffffff", 0) == 0);
    audit_close(&log);

    unlink(rotation_path);
    unlink(rotated_path);
    unlink(anchor_path);
    rmdir(rotation_dir);

    // Simulate a crash after an older build persisted the new anchor but
    // before it renamed audit.log. The file is valid and must be rotated,
    // not quarantined as corrupt.
    char crash_dir[] = "/tmp/agent_audit_crash_XXXXXX";
    CHECK(mkdtemp(crash_dir) != NULL);
    char crash_path[512];
    char crash_rotated[512];
    char crash_anchor[512];
    snprintf(crash_path, sizeof(crash_path), "%s/audit.log", crash_dir);
    snprintf(crash_rotated, sizeof(crash_rotated), "%s/audit.log.1",
             crash_dir);
    snprintf(crash_anchor, sizeof(crash_anchor), "%s/audit.log.prev",
             crash_dir);

    CHECK(make_oversized_log(crash_path, last_hash) == 0);
    FILE *ahead = fopen(crash_anchor, "w");
    CHECK(ahead != NULL);
    CHECK(fwrite(last_hash, 1, 64, ahead) == 64);
    fclose(ahead);

    CHECK(audit_open(&log, crash_path) == 0);
    CHECK(audit_append(&log, "peer6", "exec", "11111111", 0) == 0);
    audit_close(&log);
    CHECK(access(crash_rotated, F_OK) == 0);
    CHECK(audit_open(&log, crash_path) == 0);
    audit_close(&log);

    unlink(crash_path);
    unlink(crash_rotated);
    unlink(crash_anchor);
    rmdir(crash_dir);

    printf("test_audit: OK\n");
    return 0;
}
