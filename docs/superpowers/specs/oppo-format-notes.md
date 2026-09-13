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
- **更正（Phase B 核查）**: 前句"rawprogram/patch XML 在 Program/UFS_PROVISION 区域"**不成立** —— `.ops` 解包产物只有镜像与 `settings.xml`，rawprogram/patch XML 需从 `<Program{N}>`/`<Patch{N}>` 块生成；`.ofp`-QC 的元数据分组里连 `Program` 都没有。证据见 `oppo-flash-protocol-facts.md` §5。
- MSM 工具流程 = 标准高通 EDL 流: 9008 → Sahara 载 programmer → Firehose program/patch → 复位。Linux 上解密后可用 edl/qdl 刷。
- Phase B 接入点: 现有 `EDLHandler`(Sahara/Firehose 自研已有) + 解出的 programmer + rawprogram 映射分区。

## 实现期口径（Phase A 落地时确认/修正）

以下为 C++ 引擎实现时经参照源与实测确认的口径，与本文件前文的早期描述有出入处**以本节为准**。

1. **QC 与 MTK 的密钥派生是同一个函数**：`ofp_qc_decrypt.py::deobfuscate()` 与 `ofp_mtk_decrypt.py::mtk_shuffle2()` 恒等（`nibbleSwap(x ^ mc)`，XOR 可交换）。前文"MTK0-7 混淆 triplet 同 QC 派生方案但 shuffle 变体"易误解——**变体指的是另一个函数** `mtk_shuffle()`（MTK 尾部 0x6C 混淆头与文件表用：先摆半字节、再异或 key，方向反了会静默解出乱码）。
2. **OPS 状态初值不来自 mbox**：`opscrypto.py:53` 是固定常量 `d1b5e39e5eea049d671dd5abd2afcbaf` 的 4 个 LE u32；62B mbox blob 只提供轮密钥材料（`asbox[0..3]` 参与首轮 XOR、`asbox[4..7]`、`asbox[8..]` 轮常量）与轮数 `asbox[0x3C] = 0x0A`。
3. **`key_custom` 的分支由补齐到 4 倍数后的长度决定**：`pad4 > 0xF` → 块路（每 16B 先 `key_update(rkey, mbox_blob)`，末块不足 16B 的缺失词按 0 参与，输出按 16B 补齐、**由调用方截断**）；`0 < pad4 <= 0xF` → 尾路（`key_update(rkey, **sbox**)`，逐 4 字节词、不足 4B 补 0）。参照的两个调用方（`decryptfile()` L428-430、`encryptsubsub()` L440-446）都先补到 4 的倍数，故 13/14/15 字节的条目走**块路**；直接裸调 helper 这三档会翻转成尾路——是审计陷阱，不是格式特性。
4. **settings.xml 的对齐**：`xmllength = LE32@尾页+0x18`；`xmlpad = 0x200 - (xmllength % 0x200)`（**恰为 0x200 倍数时 pad = 0x200，不是 0**）；数据段起点 `filesize - 0x200 - (xmllength + xmlpad)`；命中判定 `"xml "` 子串。
5. **识别必须定序：先 OPS 后 OFP**。`.ops` 尾页与 OFP-QC 尾页**共用 +0x10 处的 `0x7CEF` 魔数**，OPS 只是额外要求 `+0x00 version==2`、`+0x04 flags==1`；若先判 OFP，真实 `.ops` 会被判成 OFP-QC 并在解析阶段误报"密钥未知"。OFP-MTK 用**文件首 16B 试解是否以 `MMM` 开头**判定，与尾页无关。
6. **Phase A 诚实边界**：无真实 `.ofp`/`.ops` 样本验证（全部验证为离线：参照双源核对 + 合成包 + 参照实产密文对拍 + FIPS-197/NIST 向量）；2022+ 机型密钥未公开（未知密钥报明确中文错误，可导入外部密钥 JSON）；`+0x00/+0x04` 恰为 `02`/`01` 的真实 OFP-QC 包会被误判成 OPS（真包待验）；OPS 的 settings 偏移取尾页字段 `+0x14 × 0x200`，与参照的末端反算式在页对齐包上等价。**MTK 变体无任何完整性校验**：条目表 `crc`（u64@88）与参照一样只解析不使用，MTK 侧也没有 md5/sha256 属性，故密文损坏/密钥不符时会**静默落盘乱码**（QC/OPS 有 sha256 兜底路径）；真机使用者需自行比对产物。

## 源码参照

- bkerler/oppo_decrypt: `ofp_qc_decrypt.py`, `ofp_mtk_decrypt.py`, `opscrypto.py`, `ops_decrypt_frida.py`, `backdoor.py`(后两者仅背景知识, 不做)
- Uotan-Dev/FirmwareKit.Oppo: `QcKeyDatabase.cs`, `MtkKeyDatabase.cs`, `OpsKeyDatabase.cs`, `OfpMtkParser`, `OpsFormatParser`, FormatDetectorTests
- 许可: oppo_decrypt 各脚本头 MIT("(c) B.Kerler"); FirmwareKit.Oppo MIT。均与项目 GPLv3 兼容（本项目为独立实现, 仅引用格式事实/公开常量）。
