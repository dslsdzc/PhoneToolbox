#include "fileops.h"
#include "util.h"

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

static int read_text(const char *path, char *buffer, size_t size)
{
    FILE *file = fopen(path, "rb");
    if (!file)
        return -1;
    const size_t got = fread(buffer, 1, size - 1, file);
    fclose(file);
    buffer[got] = '\0';
    return 0;
}

int main(void)
{
    CHECK(sodium_init() >= 0);

    char directory[] = "/tmp/agent_fileops_XXXXXX";
    CHECK(mkdtemp(directory) != NULL);

    char target[512];
    snprintf(target, sizeof(target), "%s/target.bin", directory);

    const char data[] = "hello world";
    char sha256[65];
    sha256_hex(data, sizeof(data) - 1, sha256);

    push_session push;
    char error[256] = {0};
    CHECK(push_begin(&push, target, sizeof(data) - 1, "0644", sha256, 0,
                     error, sizeof(error)) == 0);
    CHECK(push_write(&push, 0, (const uint8_t *)data, 5, error, sizeof(error)) == 0);
    CHECK(push_write(&push, 5, (const uint8_t *)data + 5, sizeof(data) - 1 - 5,
                     error, sizeof(error)) == 0);
    CHECK(push_commit(&push, error, sizeof(error)) == 0);

    char text[64];
    CHECK(read_text(target, text, sizeof(text)) == 0);
    CHECK(strcmp(text, data) == 0);

    uint64_t size = 0;
    char remote_sha[65];
    CHECK(pull_meta(target, &size, remote_sha, error, sizeof(error)) == 0);
    CHECK(size == sizeof(data) - 1);
    CHECK(strcmp(remote_sha, sha256) == 0);

    uint8_t chunk[16];
    size_t got = 0;
    CHECK(pull_read(target, 6, chunk, 5, &got, error, sizeof(error)) == 0);
    CHECK(got == 5);
    CHECK(memcmp(chunk, "world", 5) == 0);

    // 覆盖保护
    CHECK(push_begin(&push, target, sizeof(data) - 1, "0644", sha256, 0,
                     error, sizeof(error)) != 0);

    // overwrite=0 的提交竞态：目标在 begin 之后出现，linkat 必须原子拒绝。
    char race[512];
    snprintf(race, sizeof(race), "%s/race.bin", directory);
    CHECK(push_begin(&push, race, sizeof(data) - 1, "0644", sha256, 0,
                     error, sizeof(error)) == 0);
    CHECK(push_write(&push, 0, (const uint8_t *)data, sizeof(data) - 1,
                     error, sizeof(error)) == 0);
    FILE *race_file = fopen(race, "w");
    CHECK(race_file != NULL);
    CHECK(fputs("existing", race_file) >= 0);
    fclose(race_file);
    CHECK(push_commit(&push, error, sizeof(error)) != 0);
    CHECK(read_text(race, text, sizeof(text)) == 0);
    CHECK(strcmp(text, "existing") == 0);
    unlink(race);

    CHECK(push_begin(&push, target, sizeof(data) - 1, "0644", sha256, 1,
                     error, sizeof(error)) == 0);
    CHECK(push_write(&push, 0, (const uint8_t *)data, sizeof(data) - 1,
                     error, sizeof(error)) == 0);
    CHECK(push_commit(&push, error, sizeof(error)) == 0);

    // sha256 不匹配：commit 失败且不覆盖原文件
    const char wrong_sha[] = "00000000000000000000000000000000"
                             "00000000000000000000000000000000";
    CHECK(push_begin(&push, target, sizeof(data) - 1, "0644", wrong_sha, 1,
                     error, sizeof(error)) == 0);
    CHECK(push_write(&push, 0, (const uint8_t *)data, sizeof(data) - 1,
                     error, sizeof(error)) == 0);
    CHECK(push_commit(&push, error, sizeof(error)) != 0);
    CHECK(read_text(target, text, sizeof(text)) == 0);
    CHECK(strcmp(text, data) == 0);

    // 符号链接拒绝
    char link_path[512];
    snprintf(link_path, sizeof(link_path), "%s/link", directory);
    CHECK(symlink(target, link_path) == 0);
    CHECK(pull_meta(link_path, &size, remote_sha, error, sizeof(error)) != 0);
    uint8_t symlink_byte = 0;
    size_t symlink_got = 0;
    CHECK(pull_read(link_path, 0, &symlink_byte, 1, &symlink_got,
                    error, sizeof(error)) != 0);
    CHECK(push_begin(&push, link_path, 1, "0644", wrong_sha, 1,
                     error, sizeof(error)) != 0);

    // intermediate-directory symlink: even a non-symlink final component must
    // not be reached through a symlinked directory.
    char real_dir[512];
    char linked_dir[512];
    char nested_file[512];
    snprintf(real_dir, sizeof(real_dir), "%s/real", directory);
    snprintf(linked_dir, sizeof(linked_dir), "%s/linked", directory);
    snprintf(nested_file, sizeof(nested_file), "%s/file", real_dir);
    CHECK(mkdir(real_dir, 0755) == 0);
    FILE *nested = fopen(nested_file, "w");
    CHECK(nested != NULL);
    CHECK(fputs("nested", nested) >= 0);
    fclose(nested);
    CHECK(symlink(real_dir, linked_dir) == 0);
    char via_link[512];
    snprintf(via_link, sizeof(via_link), "%s/file", linked_dir);
    CHECK(pull_read(via_link, 0, &symlink_byte, 1, &symlink_got,
                    error, sizeof(error)) != 0);
    CHECK(push_begin(&push, via_link, 1, "0644", sha256, 1,
                     error, sizeof(error)) != 0);
    unlink(linked_dir);
    unlink(nested_file);
    rmdir(real_dir);

    // 路径穿越拒绝
    CHECK(push_begin(&push, "/tmp/../etc/passwd", 1, "0644", sha256, 1,
                     error, sizeof(error)) != 0);

    unlink(link_path);
    unlink(target);
    rmdir(directory);

    printf("test_fileops: OK\n");
    return 0;
}
