#ifndef PHONE_AGENT_FILEOPS_H
#define PHONE_AGENT_FILEOPS_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int fd;
    int active;
    char *temp_path;
    char *target_path;
    uint64_t expected_size;
    uint64_t written;
    char expected_sha256[65];
    uint32_t mode;
    int overwrite;
} push_session;

// 开始一次 push。路径必须通过 path_is_safe；目标已存在时需 overwrite=1。
// 返回 0 成功；-1 失败并填 error。
int push_begin(push_session *session, const char *path, uint64_t size,
               const char *mode, const char *sha256, int overwrite,
               char *error, size_t error_len);

// 写入一块数据（offset 必须等于已写字节数）。失败会自动 abort。
int push_write(push_session *session, uint64_t offset, const uint8_t *data,
               size_t length, char *error, size_t error_len);

// fsync → 校验 sha256 → chmod → 原子 rename。失败清理临时文件。
int push_commit(push_session *session, char *error, size_t error_len);

// 中止并清理临时文件（可重复调用）。
void push_abort(push_session *session);

// 读取目标文件的元信息并计算 sha256。拒绝符号链接/非普通文件。
int pull_meta(const char *path, uint64_t *size, char sha256_hex[65],
              char *error, size_t error_len);

// 从 offset 读最多 length 字节；返回 0 成功（*got 可为 0）。
int pull_read(const char *path, uint64_t offset, uint8_t *buffer, size_t length,
              size_t *got, char *error, size_t error_len);

// stat：返回大小、权限、是否目录。
int file_stat(const char *path, uint64_t *size, uint32_t *mode, int *is_dir,
              char *error, size_t error_len);

#endif // PHONE_AGENT_FILEOPS_H
