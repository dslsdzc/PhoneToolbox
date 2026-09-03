# PhoneToolbox — OPPO 系固件解包引擎 (Phase A) 设计

日期: 2026-09-03
状态: 已批准（用户逐节确认 + 整体 ok）
范围: Phase A —— 镜像工具面板新增 OFP/OPS 解包引擎。Phase B（EDL 刷写链接入）不在本文档范围，仅记录衔接点。

## 1. 背景与目标

功能清单"规划中"第 1 项: OPPO/一加/realme EDL 刷机链。经调研拆分为两阶段:

- **Phase A（本文档）**: 固件包格式引擎 —— `.ofp`(QC + MTK 变体)与 `.ops`(OnePlus 系)的解密与解包, 以镜像工具面板格式的形式交付。**离线可验证**(用户无真机, 但有固件包), 是 Phase B 刷写链的前置依赖。
- **Phase B（后续单独规划）**: 解出的分区镜像 + programmer → 接入现有 EDLHandler(Sahara/Firehose 已有自研 C++) 走刷写管线; 真机验证。

用户决策记录:
- 测试条件: 只有固件包, 无真机 → Phase A 以离线验证为准, 刷写链诚实边界标"待实测"
- 格式范围: OFP + OPS 都要, 分阶段交付
- 解密密钥: 内置为主 + 外部导入兜底
- 固件样本: 按公开变体实现(拿到样本后补端到端验证)
- 实现路线: 纯 C++ 自研(参照双源核对, 非复制粘贴依赖)

## 2. 格式模型（调研结论, 双源核对）

来源核对: bkerler/oppo_decrypt(Python, 各文件头 MIT)+ Uotan-Dev/FirmwareKit.Oppo(C#, MIT), 两实现逐项比对一致; 加 bkerler/edl 佐证刷写流程。详见 `docs/superpowers/specs/oppo-format-notes.md`(格式速查, 随 spec 提交)。

**关键事实**: `.ofp`/`.ops` 无文件头魔数; 分区清单是文件尾部的加密 XML; 判定与解析全在尾部。

### 2.1 OFP-QC 变体（OPPO/realme 高通机, Find X/Reno/realme X 系）

- 尾页尺寸 0x200 或 0x1000, 取末页:
  - +0x10: LE32 魔数 `0x7CEF`
  - +0x14: LE32 XML 偏移(页单位, ×页尺寸)
  - +0x18: LE32 XML 长度(字节)
- XML 整段 AES-128-CFB 加密(segment=128), 解密后以 `<?xml` 起始(ProFile.xml)
- 老包怪癖: xmlLength < 200 时按 `(file_size - page_size) - xml_offset - 0x57` 重算
- 清单分组与载荷加密范围:
  - `Firmware` / `DigestsToSign` / `ChainedTableOfDigests` 组: **明文**
  - `Sahara` 组: 全量解密
  - 其余(Config/Provision 等): 仅前 `min(0x40000, size)` 字节解密, 余下明文
- 文件属性: `Path`/`filename`, `FileOffsetInSrc`(页单位), `SizeInByteInSrc`, `SizeInSectorInSrc`, `md5`, `sha256`, `sparse`
- 个别老 realme 包是密码 ZIP(`PK` 头), 密码公开, 见 §7 延后项
- 密钥: key triplet 表, `key = md5(hex(nibble_swap(x XOR mc)))[:16]` 取 ASCII; 尝试顺序 V1.4.17 → V1.6.17(a3s) → V1.5.13 → V1.6.6 族 → V1.7.2 → V2.0.3, 以 `<?xml` 前缀判定

### 2.2 OFP-MTK 变体（realme/OPPO MTK 机）

- 解密后明文以 `MMM` 起始(逐 key 试解首 16B 判定)
- 尾 0x6C 字节混淆头: nibble_swap(XOR), key = ASCII `geyixue`(7B); 字段含 prjname[46], cpu[7], flashtype[5], filetable_entry_count u16, prjinfo[32] 等
- 文件表: 0x60B 条目: name[32], start u64, length u64, encrypted_length u64, filename[32], crc
- 每文件前 `encrypted_length` 字节 AES-128-CFB 解密, 其余明文
- 密钥: MTK0…MTK7 混淆 triplet + MTK8 直接 ASCII 钥/IV (`ab3f76d7989207f2` / `2bf515b3a9737835`)

### 2.3 OPS 变体（OnePlus MSM 包, 一加 6~9 系）

- 尾页(末 0x200): +0x00 version=2, +0x04 flags=1, +0x10 `0x7CEF`, +0x14 settings.xml 扇区位置, +0x18 加密 XML 长度, +0x1C project id[16], +0x2C firmware 名
- settings.xml 加密; 物理布局: [SAHARA 组(加密)] [UFS_PROVISION(明文)] [Program 组(明文)] [settings.xml]
- **密码非标准 AES**: 自定义 CFB 流密码 —— 128-bit 4-word 状态初始化为常量 `d1b5e39e5eea049d671dd5abd2afcbaf`, 轮密钥来自 mbox blob(62B, 前 16B 有效, +0x3C 轮数 0x0A); 整 16B 块 mbox 轮密钥更新 + 密文反馈; 末尾不足 16B 部分走 S-box 路径
- mbox4/5/6 公开(一加 7T Pro / 7 Pro / 8 系); 尝试顺序 mbox5 → mbox6 → mbox4
- 载荷: 仅 SAHARA 文件 + settings.xml 加密, Program 组(含 sparse 分区镜像)明文

### 2.4 诚实边界: 2022+ 新包

公开密钥覆盖 ~2020-2021 机型。bkerler 2025 年 issue 持续报新包 "Unknown key"。→ 引擎密钥库可插拔(§4.4), 未知密钥报明确错误, 走外部导入。

## 3. 模块架构

```
src/image_engine/
  oppo_crypto.h/cpp   # AES-128(自实现 FIPS-197) + CFB + QC/MTK 密钥派生 + OPS 自定义密码
  oppo_ofp.h/cpp      # OFP QC/MTK: 尾页扫描 / XML 解密 / 清单解析 / 分区提取
  oppo_ops.h/cpp      # OPS: 尾页 + 自定义密码 + 文件提取
  oppo_keys.h/cpp     # 密钥库: 内置公开常量 + 外部 JSON 追加 + 未知 key 明确报错
```

设计原则(沿用镜像引擎既有风格):
- 纯函数模块, 无 QObject, 命名空间 `imgopp`
- 输入 QIODevice/路径 + 输出目录 + QString* error; 流式处理大文件(内存 O(chunk)), 与 pac/huawei 引擎同风格
- 所有格式常量带来源注释(参照文件 + 行号语义), 同 pac/sin 引擎惯例
- 无 Q_OBJECT → worker 线程直接调用, 不需信号桥接

### 3.1 oppo_crypto（自研密码核心）

- `aes128_ecb_decrypt_block / aes128_cfb_decrypt`(segment=128): FIPS-197 实现, 自测向量 NIST FIPS-197 Appendix B/C + 与 bkerler 输出对拍
- `deriveQcKeyIv(obfuscatedTriplet) -> (key, iv)`: 按 §2.1 派生规则
- OPS 自定义密码: 状态密码 + mbox 轮密钥(双源核对移植); 整块路径 + 尾块路径都需对拍
- 依赖: Qt `QCryptographicHash`(SHA-256/MD5, manifest 校验), 不引外部密码库

### 3.2 oppo_ofp

```
enum class OfpVariant { Qc, Mtk };
bool detectOFP(const QByteArray &tail, quint64 fileSize, OfpVariant &out, QString *error);
bool parseOFP(const QString &path, OfpInfo &info, QString *error);      // 元数据卡用
bool extractOFP(const QString &path, const QString &outDir,
                const OppoKeyFile *extraKeys, extractProgressCb, QString *error);
```
- Qc: 双页尺寸扫描 → key 试解 XML → 解析分组文件表 → 按组策略提取(明文/全解/前 0x40000)
- Mtk: 尾 0x6C 解混淆 → 文件表 → 逐文件前 encrypted_length 解密
- OfpInfo 供面板信息卡: 变体/机型/版本/分区数/keyId

### 3.3 oppo_ops

```
bool detectOPS(const QByteArray &tail, QString *error);
bool parseOPS / extractOPS(…同 OFP 签名…);
```
- 尾页校验 + settings.xml 解密(mbox 试解, `xml ` 前缀判定)→ 物理区域定位 → SAHARA 全解密 + Program 明文复制

### 3.4 oppo_keys（密钥库）

- 内置: 全部公开 triplet/mbox 常量(§2), 来源注释 bkerler oppo_decrypt + FirmwareKit KeyDatabase(MIT)
- 外部追加: JSON 文件(格式随实现文档化), 用户可加新包密钥 —— 与 huawei SignConfig 的"外部配置导入"同模式; 无 Qt6::Network 下载接线(骨架留扩展)
- 未知 key 错误信息格式: `OPPO 包使用的密钥未知（固件晚于 2021?）—— 可尝试在设置中导入密钥文件`（文案定稿见实现）

## 4. UI 接线（镜像工具面板）

- `imgreg::Format` 增 `OFP`、`OPS` 两个枚举值
- `registry.cpp`: 探测需读文件尾 —— 现有 detect() 只收头字节。实现方式: 面板拖放探测处(读头部现有逻辑)之外, 镜像 worker 对未知/扩展名命中(.ofp/.ops)的文件追加"尾页采样"探测(`detectOFP/detectOPS`), 或 detect() 签名扩展传文件句柄 —— 以 worker 实现最小改动为准(实现计划定稿)
- 扩展名兜底: `.ofp` → OFP, `.ops` → OPS
- 信息卡: 变体(QC/MTK)、分区数、固件版本字段(如有)
- 动作: "解包" —— worker 线程跑 `extractOFP/extractOPS`, 进度回调接线到现有进度条, 产物入选定输出目录; 完成后日志列产物清单
- sparse 产物原样输出(不自动转 raw —— 镜像引擎已有 sparse→raw 转换, 用户后续一键处理; 清单如带 sparse="true" 仅日志标注)

## 5. 错误处理与边界

| 情形 | 行为 |
|---|---|
| 尾页无 0x7CEF / 混淆头校验失败 | 明确错误"不是 OFP/OPS 包" |
| 全部 key 试解失败 | "密钥未知或文件损坏"(建议外部密钥导入) |
| 解出 XML 非 `<?xml` 起始 | 同上(试解判定失败) |
| 文件表越界/长度溢出 | 拒绝 + 报错(恶意输入防护, 同现有引擎惯例) |
| 输出路径 == 输入路径 | 拒绝(流式写保护, 沿用 worker 既有守卫模式) |
| 提取中途失败 | 已写文件保留 + 错误列出, 日志提示可重跑(断点续提不在本阶段) |

## 6. 测试与验证

无真机, 分三层:

1. **密码核心单测**(tests/, 项目现无单测基建 —— 以独立自测可执行或镜像引擎既有的自检模式落地, 实现计划定稿):
   - AES-128 FIPS-197 Appendix B/C 向量
   - 密钥派生断言: QC V1.5.13 → key `94d62e831cf1a1a0`/iv `7ab5e33bd50d81ca`; V1.6.6 族 → `4a837229e6fc77d4`/`00bed47b80eec9d7`; MTK1 → `52dddab2c46aab56`; MTK8 直接对
   - OPS 密码: 与 bkerler opscrypto.py 对拍(明文环回 / 已知密文构造)
2. **结构自测**: 构造最小合成包(按 §2 布局写合成器, 测试用) → detect/parse/extract 全链路; 恶意输入(截断/翻转)拒绝路径
3. **端到端(外部)**: 真实 .ofp/.ops 样本验证 —— 用户固件包或公开样例; 交付时若样本未就位, 明确记录"代码就绪 + 合成包验证通过, 真包待验"

## 7. 明确不做（Phase A 范围外）

- Phase B 刷写链(EDL/FlashTool 接入) —— 后续规划
- 密码 ZIP 老包(.ofp 内嵌 zip, 密码 `flash@realme$…`, 个别老 realme 包): 探测到 `PK` 头时报明确错误"老式密码 ZIP 包暂不支持"(本阶段不做 zip 密码解开), 后续追加
- super_map.csv 多分卷 super 合并 —— 与既有 super 引擎解出后手工合并路径衔接即可
- 重打包/加密(ozip/ops repack) —— 只读解包, 同镜像引擎只读惯例
- OPS/MTK 的加密写路径、backdoor 类工具 —— 明确不做

## 8. 参照物与许可

- 主参照: bkerler/oppo_decrypt —— `reference/` 下新增镜像(与 edl/mtkclient 同模式)。各脚本头 MIT("(c) B.Kerler, MIT license"), README 注明可分享/修改/署名
- 副参照: Uotan-Dev/FirmwareKit.Oppo(MIT, C#, 2026 活跃, 自带 key database 与测试)
- 本项目 GPLv3; 自研代码按"独立实现, 格式事实与公开常量可引用"标注来源; 不做整文件复制(与 F1 mtk_brom "协议对照源码逐条核实" 同风格)
- 参照镜像仅存 reference/ 不参与构建(与 geekflashcore 等同地位), 功能清单"十八、第三方依赖"更新行

## 9. 交付物清单

- [ ] reference/ 新增 oppo_decrypt + FirmwareKit.Oppo 镜像(浅克隆) + LICENSE 记录
- [ ] src/image_engine/oppo_crypto.{h,cpp} 等 6 文件(§3)
- [ ] registry 枚举 + 探测 + 面板信息卡/动作接线 + worker 分派
- [ ] 密码核心自测(向量) + 合成包结构测试
- [ ] 真实样本验证记录(样本到位后补)
- [ ] 功能清单.txt 更新(Phase A 交付行)
