# 更多协议/刷机模式 Implementation Plan（计划 F）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 补齐维修行业刷机协议覆盖（2026-08-05 探索增强为四项）：① MTK BROM 直刷（含 V6 新平台 preloader 与 payload 绕过）；② 华为 HDB/HiSuite 通道；③ OPPO/一加/realme EDL 刷机链（OFP/OPS 解密 + Firehose 刷入）；④ 展锐 ResearchDownload 刷写通道。**全部自研**（2026-08-05 用户强制：参照源代码自研，MTK 部分也自研，不编排外部工具）。

**Architecture（2026-08-05 自研原则修订）**：全部协议实现参照权威源码自研（`mtkclient/`、`edl/` 子模块已在仓库，`oppo_decrypt`/`OplusEdlTool` 源码可参照）—— C++ 模块，禁止外部工具子进程编排作为主路径（现有 mtk_bridge 仅作兼容过渡）。

**Tech Stack:** C++17, Qt6, libusb（现有依赖）, 现有 edl/mtkclient 子模块源码（参照）, image_engine（imgsuper 等复用）, 计划 B pac 解包产物。

## Global Constraints

- C++17；**自研原则（用户强制）**：全部协议实现参照源代码自研（mtkclient/edl/oppo_decrypt/OplusEdlTool 源码在可参照范围），**禁止外部工具子进程编排作为主实现路径**（现有 mtk_bridge 仅兼容过渡）
- **默认信息不可信原则（用户要求）**：协议细节/入口必须联网验证 + 对照权威源码（实现时逐条核对，验证记录入报告）
- 签名材料不内置（spec 2026-08-04 原则）：华为 HDB 签名/参数经配置导入或运行时获取
- 每个任务独立 commit，前缀 `feat:`；TDD（协议层单测可 mock USB 端点）
- **启动前文档先行（2026-08-05 用户指示）**：本计划文档完整（任务/接口/边界），用户指示后才启动执行
- 诚实边界：2026-08-05 探索确认的行业趋势（新机型写保护收紧、HSM/AVB 2.0/eFUSE 熔断）—— 各任务如实标注，不假装支持受限设备

---

## 文件结构

```
src/core/modes/
  mtk_brom.h/.cpp          # F1: MTK BROM 协议自研（USB 枚举/命令层/内存/EMMC 读写）
  mtk_emmc.h/.cpp          # F1: EMMC/UFS 读写与分区表（参照 mtkclient 的 emmc.py）
  mtk_payload.h/.cpp       # F1: payload 绕过（SLA/DAA/SBC，参照 generic_patcher）
  hdb_handler.h/.cpp       # F2: 华为 HDB 协议自研
  oppo_crypto.h/.cpp       # F3: OFP/OPS 解密自研（参照 oppo_decrypt）
  sahara_usb.h/.cpp        # F3: Sahara 协议自研（参照 edl 子模块）
  firehose_xml.h/.cpp      # F3: Firehose XML 协议自研（参照 edl 子模块）
  spd_flash.h/.cpp         # F4: 展锐 ResearchDownload 协议自研
tests/
  test_mtk_brom.cpp
  test_mtk_emmc.cpp
  test_mtk_payload.cpp
  test_hdb.cpp
  test_oppo_crypto.cpp
  test_sahara.cpp
  test_firehose.cpp
  test_spd_flash.cpp
```

---

### Task F1-1: MTK BROM — USB 枚举与命令层（自研）

**Files:**
- Create: `src/core/modes/mtk_brom.h/.cpp`
- Create: `tests/test_mtk_brom.cpp`

**Interfaces:**
- Produces: `namespace mtkbrom { struct BromDevice { int vid; int pid; QString portName; }; bool enumerateUsb(QList<BromDevice> &out, QString *error); class BromSession { public: explicit BromSession(const BromDevice &dev); bool connect(QString *error); bool sendCommand(quint8 cmd, const QByteArray &payload, QByteArray &reply, QString *error); bool close(QString *error); }; }`（命令层：SEND_DA/JUMP_DA/GET_TARGET_CONFIG 等命令号**实现时对照 mtkclient brom.py 源码核实**，校验和/长度字段逐字节对齐）

**⚠️ 参照源码（强制）**：命令号、帧格式、校验和算法逐条对照 `mtkclient/mtk/brom.py`（子模块在仓库）与 `mtkclient/Setup/Linux` 的 usb 交互；实现时核对，不凭记忆。

**测试**：mock USB 端点（抽象 USB 接口注入，单测命令构造/校验和/响应解析）；不依赖真实设备。

**诚实边界**：V6 新平台 BROM 已修补 —— connect 阶段检测到修补行为时返回明确错误（不假装）。

---

### Task F1-2: MTK BROM — 内存协议与 EMMC/UFS 读写（自研）

**Files:**
- Create: `src/core/modes/mtk_emmc.h/.cpp`
- Modify: `tests/test_mtk_brom.cpp`（或新建 test_mtk_emmc.cpp）

**Interfaces:**
- Produces: `namespace mtkbrom { struct EmmcLayout { quint64 startSector; quint64 numSectors; QString name; }; bool readMemory(BromSession &s, quint64 addr, QByteArray &out, QString *error); bool writeMemory(BromSession &s, quint64 addr, const QByteArray &data, QString *error); bool readEmmc(BromSession &s, quint64 sector, quint64 count, QByteArray &out, QString *error); bool writeEmmc(BromSession &s, quint64 sector, const QByteArray &data, QString *error); bool listPartitions(BromSession &s, QList<EmmcLayout> &out, QString *error); }`（内存命令/EMMC 命令号与参数布局**对照 mtkclient brom.py + emmc.py 核实**）

**测试**：mock 会话（命令字节序列断言）。

---

### Task F1-3: MTK payload 绕过（SLA/DAA/SBC）与刷写流程集成（自研）

**Files:**
- Create: `src/core/modes/mtk_payload.h/.cpp`
- Modify: 现有 `mtk_handler`（BROM 会话 API 集成：模式选择 DA/BROM/V6-preloader）

**Interfaces:**
- Produces: `bool mtkbrom::sendPayload(BromSession &s, const QByteArray &payload, QString *error);`（payload 二进制从哪来：**参照 generic_patcher 源码编译/生成逻辑**，实现时核实；或标注"payload 生成自研（后续任务）"）；`bool flashPartition(BromSession &s, const QString &name, const QByteArray &image, QString *error);`

**诚实边界**：DAA/SLA/Remote-Auth 无公开方案（2026-08-05 探索确认）—— 检测到这些状态时返回明确错误。

---

### Task F2: 华为 HDB/HiSuite 通道（自研）

**Files:**
- Create: `src/core/modes/hdb_handler.h/.cpp`
- Create: `tests/test_hdb.cpp`

**Interfaces:**
- Produces: `namespace imghdb { struct HdbDevice { QString serial; QString model; }; bool enumerate(QList<HdbDevice> &out, QString *error); bool handshake(const HdbDevice &dev, QString *error); bool flashPackage(const HdbDevice &dev, const QString &appPath, const imghw::SignConfig *sign, QString *error); }`（update.app 解包复用 B5/B6）

**⚠️ 参照逆向资料（强制，不可信原则）**：HDB 协议细节（握手序列/命令字/刷写流程）实现时联网核实 + 对照逆向资料（HiSuite 逆向手册、huawei-playground 源码、SimomYung/unpack_huawei_package 的签名头规则）；不凭记忆。

**测试**：协议单测（mock 传输层，命令序列断言）。

**诚实边界**：旧版 HiSuite 通道适配机型有限（2018-2020 Mate/P/Nova 系）；签名材料不内置（SignConfig 导入/运行时获取）。

---

### Task F3-1: OPPO — OFP/OPS 解密（自研）

**Files:**
- Create: `src/core/modes/oppo_crypto.h/.cpp`
- Create: `tests/test_oppo_crypto.cpp`

**Interfaces:**
- Produces: `namespace oppoc { struct OfpImage { QString name; QByteArray data; }; bool decryptOfp(const QString &ofpPath, const QString &outDir, QList<OfpImage> &out, QString *error); bool decryptOps(const QString &opsPath, const QString &outDir, QString *error); }`（QC/MTK 双变体；super 镜像产物可走 imgsuper 拆分）

**⚠️ 参照源码（强制）**：OFP/OPS 加密算法（AES 密钥派生/IV/块布局）对照 `oppo_decrypt` 源码（ofp_qc_decrypt.py/ofp_mtk_decrypt.py/opscrypto.py）逐条实现；不凭记忆。

**测试**：构造 ofp 样本（按算法布局）往返验证。

**诚实边界**：新机型写保护收紧（开端口只能读不能写）—— 解密产物可提取，写入可行性按设备标注。

---

### Task F3-2: OPPO — Sahara/Firehose 自研 + 刷写集成

**Files:**
- Create: `src/core/modes/sahara_usb.h/.cpp`, `src/core/modes/firehose_xml.h/.cpp`
- Create: `tests/test_sahara.cpp`, `tests/test_firehose.cpp`

**Interfaces:**
- Produces: `namespace imgsahara { struct SaharaDevice { int vid; int pid; }; bool connect(const SaharaDevice &dev, const QByteArray &prog, QString *error); }`（Sahara 命令：HELLO/DONE/RESET 等**对照 edl 子模块源码核实**）；`namespace imgfirehose { bool sendXml(const QString &xml, QString *error); bool readPartition(const QString &name, const QString &outPath, QString *error); bool writePartition(const QString &name, const QString &imgPath, QString *error); bool listPartitions(QList<QString> &out, QString *error); }`（Firehose XML 命令构造**对照 edl 子模块 + QPST 行为核实**）

**测试**：mock USB 端点；XML 构造/响应解析单测。

---

### Task F4: 展锐 ResearchDownload 刷写通道（自研）

**Files:**
- Create: `src/core/modes/spd_flash.h/.cpp`
- Create: `tests/test_spd_flash.cpp`

**Interfaces:**
- Produces: `namespace imgspd { bool flashFirmware(const QString &pacPath, const QString &devicePort, QString *error); }`（pac 解包复用 B8；分区匹配 → ResearchDownload 刷写）

**⚠️ 参照逆向资料（强制）**：ResearchDownload 协议细节实现时联网核实（社区逆向资料/工具源码）；**资料不可得时诚实标注**（"仅解包 + 引导用户使用官方工具"，不假装支持）。

---

## Self-Review 记录

- **Spec 覆盖**：spec"维修行业三通道"通道②（MTK BROM + OPPO EDL + Sahara/Firehose 自研）+ 通道③（华为 HDB）；展锐补齐刷写缺口 —— 2026-08-05 探索后计划 F 扩为四项。
- **自研原则**：每任务强制"对照源码逐条实现、不凭记忆"；禁止外部工具编排主路径（mtk_bridge 仅过渡）。
- **不可信原则**：F1 对照 mtkclient 子模块源码；F2/F4 对照逆向资料（实现时联网核实）；F3 对照 oppo_decrypt/edl 子模块源码。
- **诚实边界**：V6 BROM 修补、DAA/SLA 无公开方案、新机型写保护、展锐协议资料可能不可得 —— 全部标注，不假装。
- **依赖顺序**：F1-1→F1-2→F1-3 顺序；F2/F3-1/F4 独立；F3-2 依赖 F3-1；全部依赖现有 edl/mtkclient 子模块源码（参照）。
- **类型一致性**：`mtkbrom::BromSession/EmmcLayout`、`imghdb::HdbDevice`、`oppoc::OfpImage`、`imgsahara::SaharaDevice`、`imgfirehose`、`imgspd` 命名跨任务一致。
- **后续候选**（本计划范围外，另立）：三星 Heimdall 集成、维修工具 DLL 解析（用户方案）、工作站化（QDockWidget 布局，用户已定不加菜单栏）。
