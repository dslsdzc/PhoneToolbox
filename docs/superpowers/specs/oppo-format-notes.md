# OPPO/OnePlus/realme 固件包格式速查（OFP QC/MTK + OPS）

调研日期: 2026-09-03。双源核对: bkerler/oppo_decrypt(Python, 文件头 MIT)与 Uotan-Dev/FirmwareKit.Oppo(C#, MIT, 2026 活跃), 逐项一致; 另有 bkerler/edl 佐证。配套 spec: `2026-09-03-oppo-unpack-phase-a-design.md`。

## 共性

- `.ofp`/`.ops` 无文件头魔数; 文件表/清单是**文件尾部加密 XML**; 判定在末页。
- `.ofp` = OPPO/realme MSM 包（QC 变体 + 结构不同的 MTK 变体); `.ops` = OnePlus MSM 包。**两者是兄弟族, 非新旧代**。
- 公开密钥覆盖 ~2020-2021 机型; 2022+ 新包密钥不公开（bkerler 2025 issue 持续报 Unknown key）→ 可插拔密钥库。

## OFP-QC

- 尾页 0x200 或 0x1000: `+0x10` LE32 魔数 `0x7CEF`; `+0x14` XML 偏移(页单位); `+0x18` XML 长度。
- XML 整段 AES-128-CFB(segment=128) 加密 → `<?xml` 起始（ProFile.xml）。老包 xmlLength<200 时按 `(file_size-page)-xml_offset-0x57` 重算。
- 分组: `Firmware`/`DigestsToSign`/`ChainedTableOfDigests` = 明文; `Sahara` = 全解密; 其余 = 仅前 `min(0x40000,size)` 解密。
- 文件属性: `Path`/`filename`, `FileOffsetInSrc`(页单位), `SizeInByteInSrc`, `SizeInSectorInSrc`, `md5`, `sha256`, `sparse`。

### QC key triplet 表（`key = md5(hex(nibble_swap(x XOR mc)))[:16]` ASCII）

| KeyId | mc | userkey | ivec | 机型（注释） |
|---|---|---|---|---|
| V1.4.17 | 27827963787265EF89D126B69A495A21 | 82C50203285A2CE7D8C3E198383CE94C | 422DD5399181E223813CD8ECDF2E4D72 | R9s/A57t |
| V1.6.17 | E11AA7BB558A436A8375FD15DDD4651F | 77DDF6A0696841F6B74782C097835169 | A739742384A44E8BA45207AD5C3700EA | a3s |
| V1.5.13 | 67657963787565E837D226B69A495D21 | F6C50203515A2CE7D8C3E1F938B7E94C | 42F2D5399137E2B2813CD8ECDF2F4D72 | legacy |
| V1.6.6/1.6.9/1.6.17/1.6.24/1.6.26/1.7.6 | 3C2D518D9BF2E4279DC758CD535147C3 | 87C74A29709AC1BF2382276C4E8DF232 | 598D92E967265E9BCABE2469FE4A915E | R15 Pro/Find X/R17/Reno 系/A5 2020/K3/Realme 3 Pro |
| V1.7.2 | 8FB8FB261930260BE945B841AEFA9FD4 | E529E82B28F5A2F8831D860AE39E425D | 8A09DA60ED36F125D64709973372C1CF | Realme X RMX1901/5/5 Pro |
| V2.0.3 | E8AE288C0192C54BF10C5707E9C4705B | D64FC385DCD52A3C9B5FBA8650F92EDA | 79051FD8D8B6297E2E4559E997F63B7F | OW19W8AP_11_A.23_200715 |

尝试顺序: V1.4.17 → V1.6.17(a3s) → V1.5.13 → V1.6.6 族 → V1.7.2 → V2.0.3; 判定 = 解出 `<?xml`。

派生自测断言（用于测试）: V1.5.13 → key `94d62e831cf1a1a0` iv `7ab5e33bd50d81ca`; V1.4.17 → `d154afeeaafa958f`/`2c040f5786829207`; V1.6.17a3s → `2e96d7f462591a0f`/`17cc63224c208708`; V1.6.6 族 → `4a837229e6fc77d4`/`00bed47b80eec9d7`; V1.7.2 → `3398699acebda0da`/`b39a46f5cc4f0d45`; V2.0.3 → `b4b7358eea220991`/`e9077e26ab102d1b`。

## OFP-MTK

- 明文 `MMM` 起始(试解首 16B 判定)。
- 尾 0x6C 混淆头: nibble_swap(XOR), key ASCII `geyixue`(7B)。字段: prjname[46], u64@48, reserved[4]@56, cpu[7]@60, flashtype[5]@67, entry_count u16@72, prjinfo[32]@74, crc u16@106。
- 文件表紧邻其前, 0x60B 条目: name[32], start u64@32, length u64@40, encrypted_length u64@48, filename[32]@56, crc u64@88。
- 每文件前 `encrypted_length` 字节 AES-128-CFB, 余明文。
- 密钥 MTK0…MTK8（MTK0-7 混淆 triplet 同 QC 派生方案但 shuffle 变体; MTK8 直接: key `ab3f76d7989207f2`, IV `2bf515b3a9737835`）。MTK0 = 与 QC V1.5.13 同 triplet（交叉校验通过）。派生断言: MTK1 → `52dddab2c46aab56`/`35f19b6877f9c360`; MTK5 → `443ec2fc7f543de6`/`f02df2210580c734`。

## OPS

- 尾页末 0x200: `+0x00` version=2, `+0x04` flags=1, `+0x10` `0x7CEF`, `+0x14` settings.xml 扇区位置(0x200 单位), `+0x18` 加密长度, `+0x1C` project id[16] ASCII, `+0x2C` firmware 名。
- 物理布局顺序: [SAHARA(加密)] [UFS_PROVISION(明文)] [Program(明文, 含 sparse 镜像)] [settings.xml(加密)] [尾页]。
- 密码**非标准 AES**: 自定义 CFB —— 128-bit 4-word 状态, 初始常量 `d1b5e39e5eea049d671dd5abd2afcbaf`; 轮密钥 = mbox blob(62B; 前 16B 有效; +0x3C = 轮数 0x0A); 整块 mbox 轮更新 + 密文反馈, 尾块(<16B)走 S-box 路径。
- mbox（首 16B）: mbox5 `608A3F2D686BD423510CD095BB40E976`(一加 7 Pro 族 guacamole), mbox6 `AA69829E5DDEB13D30BB81A34665A3E1`(一加 8 族 instantnoodle), mbox4 `C45D057199DDBBEE29A16DC7ADBFA43F`(一加 7T Pro 族 guacamoleT)。尝试顺序 mbox5 → mbox6 → mbox4。
- 哈希: .ops 内文件 sha256 按 0x1000 边界补零后计算（校验细节实现时对照）。
- 载荷: 仅 SAHARA 文件 + settings.xml 加密; Program/UFS_PROVISION 明文 —— 这也是镜像大文件能"半明文"存在的结构原因。

## 刷写衔接（Phase B 用）

- Firehose programmer（`prog_ufs_firehose_*.elf` / `prog_emmc_firehose_*.elf`）= SAHARA 组文件（.ofp 中全解密; .ops 中仅此组加密）。
- rawprogram/patch XML + 分区镜像在 Program/UFS_PROVISION 区域。
- MSM 工具流程 = 标准高通 EDL 流: 9008 → Sahara 载 programmer → Firehose program/patch → 复位。Linux 上解密后可用 edl/qdl 刷。
- Phase B 接入点: 现有 `EDLHandler`(Sahara/Firehose 自研已有) + 解出的 programmer + rawprogram 映射分区。

## 源码参照

- bkerler/oppo_decrypt: `ofp_qc_decrypt.py`, `ofp_mtk_decrypt.py`, `opscrypto.py`, `ops_decrypt_frida.py`, `backdoor.py`(后两者仅背景知识, 不做)
- Uotan-Dev/FirmwareKit.Oppo: `QcKeyDatabase.cs`, `MtkKeyDatabase.cs`, `OpsKeyDatabase.cs`, `OfpMtkParser`, `OpsFormatParser`, FormatDetectorTests
- 许可: oppo_decrypt 各脚本头 MIT("(c) B.Kerler"); FirmwareKit.Oppo MIT。均与项目 GPLv3 兼容（本项目为独立实现, 仅引用格式事实/公开常量）。
