#ifndef PHONE_AGENT_IDENTITY_H
#define PHONE_AGENT_IDENTITY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AGENT_IDENTITY_KEY_LEN 32

typedef struct {
    uint8_t public_key[AGENT_IDENTITY_KEY_LEN];
    char name[64];
    char capabilities[128];
} peer_record;

typedef struct {
    uint8_t private_key[AGENT_IDENTITY_KEY_LEN];
    uint8_t public_key[AGENT_IDENTITY_KEY_LEN];
} agent_identity;

// 在 state_dir 下加载或生成 identity.key（0600）。
// 返回 0 成功；-1 失败并填 error。
int identity_load_or_create(agent_identity *identity, const char *state_dir,
                            char *error, size_t error_len);

// peers 记录：每行 `<64 hex> <name>`。
// 返回 1 已配对；0 未配对；-1 读取失败。
int peers_contains(const char *state_dir, const uint8_t public_key[AGENT_IDENTITY_KEY_LEN]);

// 追加一条 peer 记录（原子追加，0600）。返回 0 成功。
int peers_add(const char *state_dir, const uint8_t public_key[AGENT_IDENTITY_KEY_LEN],
              const char *name, const char *capabilities,
              char *error, size_t error_len);

// 第一版单 peer：读取第一行记录的能力列表（逗号分隔）。
int peers_has_capability(const char *state_dir, const char *capability);

// 按公钥读取对应 peer 的能力列表（逗号分隔）。多 peer 必须用这个，
// 避免第一台电脑的 dangerous 权限被其它电脑继承。
int peers_has_capability_for(const char *state_dir,
                             const uint8_t public_key[AGENT_IDENTITY_KEY_LEN],
                             const char *capability);

// 列出全部 peer；无记录时 *count=0 且返回 0。调用方用 peers_free 释放。
int peers_load(const char *state_dir, peer_record **out, int *count);
void peers_free(peer_record *peers);

// 删除指定公钥的 peer（原子重写 peers.txt）。
int peers_remove(const char *state_dir, const uint8_t public_key[AGENT_IDENTITY_KEY_LEN]);

// 取第一条 peer 公钥（兼容旧调用）。返回 0 成功；-1 无记录/读取失败。
int peers_first(const char *state_dir, uint8_t public_key[AGENT_IDENTITY_KEY_LEN]);

// 把 state_dir 下的 identity 公钥做 SHA-256，输出 64 hex + NUL。
int identity_fingerprint(const char *state_dir, char out_hex[65],
                         char *error, size_t error_len);

#ifdef __cplusplus
}
#endif

#endif // PHONE_AGENT_IDENTITY_H
