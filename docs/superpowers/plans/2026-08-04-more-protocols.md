# 更多协议/刷机模式 Implementation Plan（计划 F）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 补齐维修行业刷机协议覆盖（2026-08-05 探索增强为四项）：① MTK BROM 直刷（含 V6 新平台 preloader 与 payload 绕过）；② 华为 HDB/HiSuite 通道；③ **OPPO/一加/realme EDL 刷机链（OFP/OPS 解密 + Firehose 刷入，2026-08-05 探索发现 OplusEdlTool v2 + oppo_decrypt）**；④ **展锐 ResearchDownload 刷写通道（pac 解包已有、刷写缺失）**。后续候选：三星 Heimdall（Odin 协议开源实现，已调研）、维修工具 DLL 解析、工作站化。

**Architecture:** 沿用 `mtk_handler` 的 mtk_bridge 子进程 JSON-RPC 桥接模式（mtkclient 封装）：扩展 mtk_bridge 暴露 BROM 模式命令（`--preloader` 直刷/分区读写/printgpt/payload）。华为通道复用 `assets_downloader`/`SignConfig` 模式（签名材料不内置，运行时获取/手动导入）。OPPO 链复用现有 edl 子模块（Sahara/Firehose）+ 外部工具编排（oppo_decrypt 解密 OFP/OPS → 提取镜像 → EDL 刷入）。展锐刷写以社区工具（SPD ResearchDownload 生态）为参考，复用 pac 解包（计划 B）产物。

**Tech Stack:** C++17, Qt6, 现有 mtk_bridge（Python，mtkclient 封装）、image_engine（签名材料接口 B6 已建）。

## Global Constraints

- C++17；mtk_bridge 扩展为 Python（既有 mtkclient 生态）
- **默认信息不可信原则（用户要求）**：协议细节/入口必须联网验证（本计划 F1 探索已确认 BROM 现状，实现时再核对 mtkclient 源码）
- 签名材料不内置（spec 2026-08-04 原则）：华为 HDB 通道的签名/参数经配置导入或运行时获取
- 每个任务独立 commit，前缀 `feat:`
- 计划 E 完成后执行（用户：先把 E 搞）

---

### Task F1: MTK BROM 直刷模式接入

**背景（2026-08-05 探索确认）**：
- 旧平台：mtkclient BROM 模式免 DA 直刷（`--preloader` 参数，支持 `w`/`r`/`wo`/`ro`/`printgpt`/`rl` 等；`wf` 整盘写仅 DA）
- 新平台 V6 芯片（MT6781/MT6789/MT6855/MT6886/MT6895/MT6983/MT8985 等）：bootrom 已修补，BROM 模式不可用 → 必须 `--loader`（Loaders/V6 目录 DA）+ preloader 模式（免按键直连）；仅未熔断设备
- 免 DA 替代：`payload` 命令（BROM 下绕过 SLA/DAA/SBC）→ 保持连接 → 外部工具刷写

**实现**：
- 扩展 `tools/mtk_bridge.py`：暴露 BROM 模式命令（`brom_w`/`brom_r`/`brom_wo`/`brom_ro`/`brom_printgpt`/`brom_payload`），参数透传 mtkclient（`--preloader`/`--loader` 支持）
- `src/core/modes/mtk_handler.cpp`：新增 BROM 会话 API（连接模式选择：DA/BROM/V6-preloader），复用现有 JSON-RPC 通道
- UI（后续计划 F-UI 或并入面板）：刷机面板 MTK 区加模式选择
- **诚实边界**：新平台仅未熔断设备；DAA/SLA/Remote-Auth 无公开方案（标注）；`wf` 仅 DA

**验证**：mtk_bridge 单测（mock mtkclient 命令构造）+ 真实设备冒烟（若环境可用）

---

### Task F2: 华为刷机通道接入（HDB/HiSuite）

**背景（2026-08-04/05 已调研）**：
- 华为自研 HDB 协议（比 ADB 底层，eRecovery/DFU 可触达）；旧版 HiSuite 10.1.0.550 支持 `fastboot oem unlock-state`、全分区刷写
- 候选参考：SimomYung/unpack_huawei_package（签名头规则）、HuaweiUpdateExtractor（profiles.xml）、huawei-playground（oeminfo/CM3 解析）—— 见 spec 候选下载源表
- 签名材料不内置：`imghw::loadSignConfig/fetchSignConfig`（B6 已建）

**实现**：
- HDB 协议对接（独立模块 `src/core/modes/hdb_handler.cpp` 或复用 FlashTool 通道）：握手/设备枚举/刷写命令
- 刷机包：update.app 解包（B5/B6 已支持）→ 分区匹配 → HDB 刷写
- 签名材料：配置文件导入（B6 SignConfig 复用）+ 外部仓库获取（AssetsDownloader 模式）
- **诚实边界**：HDB 协议细节以逆向资料为准（不可信原则，实现时联网核实）；旧版 HiSuite 通道适配机型有限（2018-2020 Mate/P/Nova 系）

**验证**：协议解析单测 + 真实设备冒烟（若环境可用）

---

### Task F3: OPPO/一加/realme EDL 刷机链（2026-08-05 探索新增）

**背景**：2026-08-05 搜索确认 [OplusEdlTool v2](https://github.com/salokrwhite/OplusEdlTool)（OPPO 系完整 EDL 刷机：OFP/OPS 加密包 + devprg Firehose + super 自动合并）+ [bkerler/oppo_decrypt](https://github.com/bkerler/oppo_decrypt)（ofp QC/MTK 解密 + ops 加解密，spec 候选源已列）。

**实现**：
- OFP/OPS 加密包解密（oppo_decrypt 编排，子进程模式如 kptools）→ 提取分区镜像（含 super 拆分，复用计划 B imgsuper）
- EDL 刷入：复用现有 edl 子模块（Sahara/Firehose），devprg*.mbn 加载 → 分区写入
- **诚实边界**：新机型写保护收紧（开端口只能读不能写）、HSM/AVB 2.0/eFUSE（标注）；`backdoor.py`（MsmDownloadTool readback）能力评估后定
- **验证**：解密链路单测（构造 ofp 样本） + EDL 命令编排冒烟（若设备可用）

---

### Task F4: 展锐 ResearchDownload 刷写通道（2026-08-05 探索新增）

**背景**：计划 B 已支持 pac 解包（双格式识别）；刷写通道缺失。展锐 ResearchDownload（SPD 刷机工具生态）是最后缺口。

**实现**：
- 刷写协议接入：以社区工具/逆向资料为参考（不可信原则，实现时联网核实 ResearchDownload 协议细节）
- 复用 pac 解包产物 → 分区匹配 → ResearchDownload 刷写
- **诚实边界**：展锐生态工具多为闭源（SPD Research Tool 等），公开协议资料有限 —— 若协议细节不可得，标注"仅解包 + 引导用户使用官方工具"，不假装支持

---

## Self-Review 记录

- **Spec 覆盖**：spec"维修行业三通道"中通道②（协议级开源实现）的 MTK BROM 补全 + OPPO EDL 链 + 展锐刷写；通道③（售后/工程工具）的华为 HDB —— 2026-08-05 探索后计划 F 扩为四项。
- **不可信原则**：F1 已探索确认（mtkclient 现状）；F2 HDB / F4 展锐协议细节实现时联网核实（强制）；F3 参考仓库已确认存在。
- **诚实边界**：2026-08-05 搜索确认的行业趋势（新机型写保护收紧、HSM/AVB 2.0/eFUSE 熔断）—— 各任务如实标注，不假装支持受限设备。
- **依赖顺序**：F1 依赖 mtk_handler/mtk_bridge；F2 依赖 B5/B6；F3 依赖 edl 子模块 + oppo_decrypt 编排 + B 计划 imgsuper；F4 依赖 B 计划 pac；四者独立可并行。
- **后续候选**（本计划范围外，另立）：三星 Heimdall 集成、维修工具 DLL 解析（用户方案）、工作站化（QDockWidget 布局，用户已定不加菜单栏）。
