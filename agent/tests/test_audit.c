#include "audit.h"

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

    printf("test_audit: OK\n");
    return 0;
}
