# Root 执行代理（Android / 未来 iOS）设计

- 状态：草案，未实现
- 日期：2026-11-18
- 目标读者：实现者、安全评审
- 关联：`docs/superpowers/specs/` 下的协议类文档

## 1. 目标与非目标

### 目标

1. 在 **ADB shell 没有 `su`** 的设备上，仍然提供一个**确定性**的 root 执行通道。
2. 代理支持：
   - 任意 root 命令（argv 直执行，可选 shell）；
   - 文件 push/pull；
   - 命令超时、输出上限、取消；
   - 审计。
3. 桌面端与代理之间是**双向认证 + 加密**的会话，不是共享 token。
4. 代理是平台适配层：Android 先实现，未来 iOS 用同一桌面端接口。
5. 设备身份不依赖“设备未被篡改”（用户设备本来就可能已 root / bootloader 已解锁）。

### 非目标

1. 不做开机自启、不做持久化守护（避免变成设备上的常驻后门）。
2. 不监听非 localhost 地址；不提供远程（非 USB/ADB）访问。
3. 不防“设备已被完全控制”“桌面端已被入侵”。
4. 不把 Key Attestation 当唯一信任根；不要求 `verifiedBootState=green`。
5. 第一版不做 iOS 代理实现，只保留接口。

## 2. 威胁模型

### 信任边界

| 组件 | 信任假设 |
|---|---|
| 桌面端 | 用户自己的电脑；可能被恶意软件感染，但不是本设计能解决的 |
| ADB | 已通过 Android 的 RSA 调试授权；传输可能被 USB 层观察，但不假设可篡改 |
| APK | 用户手动安装并通过 KernelSU/Magisk 授权 root |
| agent | 由 APK 用 `su` 启动，运行在 root 上下文 |
| 设备上其他 App | 不可信 |
| 网络 | 不参与；仅 `adb forward` 的本地 TCP |

### 攻击者能力

- **A1 同设备恶意 App**：可以连接 `127.0.0.1:<port>`（若知道端口），可以读部分文件，无 root。
- **A2 同电脑其他进程**：可以连接 `adb forward` 暴露的本地端口。
- **A3 USB 观察者**：可以抓包，但不能篡改 ADB 认证。
- **A4 冒充 APK/agent**：安装同包名或伪造 agent。
- **A5 重放/中继**：重放旧的配对消息、Key Attestation、文件传输。
- **A6 降级**：诱导双方使用弱算法或跳过配对确认。
- **A7 路径攻击**：`..`、符号链接、覆盖系统文件。
- **A8 资源耗尽**：超大输出、死循环命令、无限文件。

### 防护目标

- G1 只有持有已配对私钥的桌面端能建立会话。
- G2 桌面端能确认对面是“这台设备上的这个 agent”（防 A4）。
- G3 会话内容加密、完整性保护、抗重放（防 A1/A2/A3/A5）。
- G4 配对过程抗 MITM，即使配对码很短（防 A4/A5）。
- G5 命令执行不经过隐式 shell，避免注入（防 A7）。
- G6 文件写入使用临时文件 + sha256 + 原子 rename；失败清理（防 A7）。
- G7 所有操作有超时和上限（防 A8）。
- G8 算法/版本固定，拒绝降级（防 A6）。

## 3. 总体架构

```
┌──────────────┐   USB/ADB    ┌──────────────────────────┐
│ 桌面端        │◄────────────►│ Android 设备              │
│ PhoneToolbox │ adb forward  │  ┌────────────────────┐  │
│              │              │  │ APK (随机包名)      │  │
│ RootAgent    │              │  │  - 显示二维码/端口  │  │
│ Client       │              │  │  - su 启动 agent    │  │
└──────────────┘              │  └─────────┬──────────┘  │
                              │            │ root         │
                              │  ┌─────────▼──────────┐  │
                              │  │ agent (native)      │  │
                              │  │ 127.0.0.1:<port>    │  │
                              │  │ exec/push/pull/audit│  │
                              │  └────────────────────┘  │
                              └──────────────────────────┘
```

- agent 由 APK 通过 `su -c` 启动，是独立 root 进程。
- APK 只负责：生成/保存密钥、显示配对信息、启动/停止 agent、撤销配对。
- 桌面端通过 `adb forward tcp:<local> tcp:<device-port>` 连接。

## 4. 设备身份

### 4.1 长期密钥

- agent 生成长期 **Ed25519** 身份密钥对。
- 私钥保存在 APK 私有目录（`filesDir`，权限 0600），优先使用 Android Keystore 包装；不写入日志。
- 公钥指纹：`SHA-256(public_key)`，用于二维码/桌面端 pinning。

### 4.2 首次配对：视觉信道 + CPace

- APK 显示二维码，内容：
  - 协议版本；
  - 包名；
  - agent 公钥指纹；
  - 一次性配对会话 ID；
  - 一次性高熵随机串（二维码本身是视觉信道，可承载高熵）。
- 用户用桌面端扫码/输入；若手动输入，使用 6 位短码 + CPace（抗离线爆破）。
- 配对完成后，二维码/短码立即失效。

### 4.3 可选：Android Key Attestation

- 只用于证明“agent 公钥在 TEE/StrongBox 中”，**不要求 `verifiedBootState=green`**。
- 已 root / bootloader 解锁的设备会得到 `orange`，如实显示，不作为拒绝条件。
- **必须处理 freshness**：challenge 含桌面端签名的时间戳 + nonce；验证证书链、包名、签名摘要、challenge。不要只验证证书有效。
- attestation 不可用时降级为 Keystore/文件密钥 + 视觉信道确认。

### 4.4 不采用

- DICE：Android 应用层没有直接 API，属于 bootloader/AVB 层。
- Play Integrity / SafetyNet：依赖 Google 服务，离线/自建工具不适用，且对 root 设备必然失败。
- 单纯 TOFU：只作为用户跳过二维码确认时的降级路径，并在 UI 标注风险。

## 5. 配对协议（CPace）

角色：桌面端 D（发起方）、agent A（响应方）。
共享秘密：二维码中的高熵串，或用户输入的 6 位短码。

```
A → D: pair_hello {
         version, session_id, agent_eph_pub,
         agent_static_pub, agent_fingerprint
       }
D → A: pair_response {
         desktop_eph_pub, desktop_static_pub, desktop_name
       }
A,D:   CPace(ISK)  // 用共享秘密 + 双方临时公钥
D → A: pair_confirm { HMAC(ISK, transcript) }
A:     设备端显示 desktop_name + 桌面端公钥指纹，用户确认
A → D: pair_accept { HMAC(ISK, transcript2), capabilities }
D:     保存 agent 公钥指纹（pinning）+ 配对记录
```

要求：

- CPace 使用 X25519 套件；具体 ciphersuite 在实现前对照 CFRG 草案固定。
- `transcript` 包含所有握手消息，防篡改。
- 配对失败不泄露“密码对不对”以外的信息；限速，防在线爆破。
- 配对记录包含：`peer_id = SHA-256(desktop_static_pub)`、能力、创建时间、最后使用时间。
- 撤销：APK 删除记录；agent 立即拒绝该公钥。

### 5.1 多台电脑（实现补充）

- `peers.txt` 每行一个 peer：`<64 hex 公钥> <名称> <能力>`，允许多行。
- 配对不再要求“设备尚未配对”；同一公钥重新配对会按本次选择重写能力。
- APK 的“已配对电脑”列表调用 agent 的 `--list-peers`，删除单条调用
  `--remove-peer <hex>`；不需要清除全部配对。
- **Noise_IK responder 不能依赖初始化时传入的 remote 公钥做认证**：发起方
  静态公钥在 msg1 内，读完后必须调用 `noise_channel_remote_static()` 取出，
  再对照 `peers.txt`；否则任意客户端都能完成握手。
- 握手成功后 agent 记住该会话的公钥；每条命令前检查它仍在 `peers.txt`，
  删除配对会立即吊销现有会话（`peer_revoked`）。
- 能力检查按当前会话公钥查表，不能复用“第一条 peer”的能力。

## 6. 会话协议（Noise）

- 模式：`Noise_IK`（桌面端已 pinning agent 公钥），或 `Noise_XX`（首次无 pinning 时的降级）。
- 算法固定：X25519 / ChaCha20-Poly1305 / SHA-256；不协商弱算法。
- 握手后所有帧加密。
- 帧格式：`uint32_be length | ciphertext`；最大 1 MiB；超限断开。
- 版本：握手首帧带 `protocol_version`；不支持则拒绝，不降级。

## 7. 命令协议

### 7.1 exec

```json
{"id":1,"op":"exec","argv":["id"],"timeout_ms":5000,"shell":false}
```

- 默认 `argv` 直接 `execve`，不经过 shell。
- `shell=true` 时才用 `/system/bin/sh -c`，桌面端需二次确认。
- 输出流：

```json
{"id":1,"op":"stdout","data_b64":"..."}
{"id":1,"op":"stderr","data_b64":"..."}
{"id":1,"op":"exit","code":0,"timed_out":false,"truncated":false}
```

- 输出上限 8 MiB；超出截断并标记。
- 超时 kill 整个进程组。

### 7.2 push

```json
{"id":2,"op":"push","path":"/data/local/tmp/x","size":12345,
 "mode":"0644","sha256":"..."}
```

- 随后二进制块（offset + length + data）。
- agent 写临时文件，校验 sha256 后原子 rename；失败清理临时文件。
- 拒绝 `..`、空路径、符号链接目标；路径必须是绝对路径。
- 覆盖已存在文件需显式 `overwrite=true`。

### 7.3 pull

```json
{"id":3,"op":"pull","path":"/data/local/tmp/x"}
```

- agent 返回 `{"op":"pull_meta","size":...}`，随后二进制块。
- 桌面端校验 sha256（agent 在 meta 中给出）。

### 7.4 其他

- `ping`：返回 agent 版本、协议版本、root uid、可用能力。
- `shutdown`：停止 agent（仅已配对客户端）。
- `stat`：文件大小/权限/类型。

## 8. 权限模型（capability）

- 配对时用户在 APK 上确认桌面端获得的能力：
  - `exec`：执行命令；
  - `push` / `pull`：文件传输；
  - `dangerous`：写分区、dd、rm、flash 等。
- agent 为每个 peer 保存能力位图。
- `dangerous` 操作在桌面端二次确认，agent 记录审计；可选“设备端确认”模式。
- 第一版默认不授予 `dangerous`，由用户在 APK 里显式打开。

## 9. 审计

- agent 追加 hash-chain 日志：

```
prev_hash | timestamp | peer_id | op | argv_sha256 | exit | entry_hash
```

- `entry_hash = SHA-256(prev_hash || 上述字段)`。
- 日志存 APK 私有目录（0600），可通过 `pull` 拉取校验。
- 只记录摘要，不记录文件内容/命令完整输出。

## 10. 生命周期与失败模式

### 启动

1. 用户打开 APK；
2. APK 读取/生成密钥与配对记录；
3. 用户点“启动代理”；
4. APK 复制 agent 到 `filesDir`，`chmod 0700`；
5. `su -c "<agent> --port 0 --token <pair-token>"`；
6. agent 绑定 `127.0.0.1:0`，输出实际端口；
7. APK 显示端口 + 二维码。

### 停止

- APK 点“停止” → 通过本地管理通道或 `kill` pid；
- 设备重启后 agent 不自动启动。

### 失败模式

| 场景 | 行为 |
|---|---|
| su 被拒绝 | APK 显示“未授权 root”，不重试循环 |
| 端口绑定失败 | agent 退出并返回错误，APK 显示 |
| 未配对客户端 | 拒绝，关闭连接，记录审计 |
| 公钥不匹配 | 拒绝，提示可能 MITM |
| 配对码错误 | CPace 失败，限速，不泄露额外信息 |
| 传输中断 | 删除临时文件，不覆盖目标 |
| 命令超时 | kill 进程组，返回 `timed_out=true` |
| agent 崩溃 | APK 显示；不自动重启 |
| 磁盘满 | push 失败，清理临时文件，返回明确错误 |

## 11. 安全测试与形式化验证

### 测试

- 单元：帧编解码、argv 转义、sha256、路径校验、超时、审计链。
- 协议：本地假 agent/客户端，正常/异常/恶意帧/大帧/慢速攻击。
- 模糊测试：JSON、长度字段、文件路径、符号链接。
- 真机：启动/配对/exec/push/pull/断线/撤销/root 授权撤销。
- 负面：未配对连接、重放、降级、路径穿越、端口扫描。

### 形式化

- Noise：用 Noise Explorer 生成 ProVerif 模型，验证 secrecy + authentication。
- CPace：引用论文证明；拼接协议（配对 → 会话 → 权限）用 ProVerif/Tamarin 建模。
- 验证目标：
  - mutual authentication；
  - session key secrecy；
  - forward secrecy；
  - no replay；
  - no downgrade。

## 12. 实现阶段

0. 本文档评审（当前）。
1. 协议库（C，本机 Linux 测试）：帧、exec、push/pull、超时、审计。
2. 密码层：libsodium + CPace + noise-c；对照测试向量。
3. APK 壳 + NDK agent。
4. 桌面端客户端 + 配对 UI。
5. 接入工具箱：root 判定 → 需要 root 的功能逐步迁移。
6. 形式化验证（可与 3/4 并行）。

## 13. 依赖与许可证

| 依赖 | 用途 | 许可证（需核实） |
|---|---|---|
| libsodium | Ed25519/X25519/ChaCha20 | ISC |
| `jedisct1/cpace` | CPace 实现 | 需核实 |
| `rweather/noise-c` | Noise 协议 | MIT（需核实） |
| Android NDK | agent 交叉编译 | Apache-2.0 |

所有第三方代码 vendored 进 `third_party/`，保留 LICENSE。

## 14. 参考文献

- Fomichev et al., *Survey and Systematization of Secure Device Pairing*, arXiv:1709.02690.
- *CPace, a balanced composable PAKE*, draft-irtf-cfrg-cpace-21; eprint 2021/839.
- Jarecki, Krawczyk, Xu, *OPAQUE*, EUROCRYPT 2018/2020.
- Perrin, *The Noise Protocol Framework*; Noise Explorer; *Noise: A Library of Verified High-Performance Secure Channel Protocol Implementations*, IEEE S&P 2022.
- McCune et al., *Seeing-Is-Believing: Using Camera Phones for Human-Verifiable Authentication*, IEEE S&P 2005.
- Aldoseri et al., *Symbolic modelling of remote attestation protocols for device and app integrity on Android*, AsiaCCS 2023（Android Key Attestation freshness issue）。
- Android Developers, *Verify hardware-backed key pairs with key attestation*.
- Android Open Source Project, *DICE*.
- NIST IR 7966, *Security of Interactive and Automated Access Management Using Secure Shell (SSH)*.
