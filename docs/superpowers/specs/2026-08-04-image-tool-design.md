# 镜像处理工具（Image Tool）设计文档

日期: 2026-08-04
状态: 待审查
范围: PhoneToolbox 新增独立镜像处理面板，全自研格式引擎 + 全格式覆盖

## 背景与动机

用户反馈当前刷机功能支持不足。经头脑风暴确定四个扩展方向（镜像处理、一键批量刷机、备份/恢复向导、更多协议），决定**镜像处理先行** —— 它是批量刷机的前置（解包才能匹配分区），且纯本地、不依赖设备连接。

2026 年 Root 生态调研结论：Magisk（修补 boot ramdisk）、KernelSU（LKM 修补 init_boot）、APatch（修补 boot 内核段）三家的共同地基都是 **boot 镜像解包/重打包引擎**，与 payload/super/sparse/tar 格式处理一起构成本工具的核心。

## 目标

1. **全格式原则**：所有能解包/打包的 Android 镜像格式全部支持（不仅限本矩阵，架构采用格式注册表模式，新增格式注册即用，不改面板代码）
2. 全自研实现（引擎与装配逻辑），注入物（magiskinit / kernelsu.ko / kpatch）运行时从官方渠道下载
3. 支持解包与打包双向操作
4. **Root/修补全覆盖原则（2026-08-05 补充）**：凡是能产生 root 能力、与 root 修补相关的方案全部纳入 —— 8 个 boot 修补入口（Magisk 官方/Kitsune/Alpha + KernelSU 官方/Next/SukiSU/ReSukiSU + APatch）+ 通用 ramdisk su 注入（老设备：SuperSU/LineageOS su/早期 su 二进制）+ 模块/框架安装（Zygisk Next/Riru/LSPosed 类，注入已 root 环境）。**诚实边界**：闭源方案（SuperSU 2.80+、KingRoot）注入物提取受限时标注"尽力支持/参考"而非假装完整
5. 支持增量 OTA diff 分区解包（需旧分区镜像）
6. 处理签名/校验层（Odin MD5、AVB footer、解锁提示）

## 格式覆盖矩阵

| 格式 | 来源 | 魔数/特征 | 支持操作 |
|------|------|-----------|---------|
| `payload.bin` | Pixel/一加/摩托等 OTA | `CrAU` | 解包（含 5 类分区特殊处理）、全量打包 |
| zip 刷机包 | 小米/Lineage/fastboot update | `PK\x03\x04` | 内层继续识别（Qt QZipReader） |
| 小米 `.tgz` | 小米/红米固件 | gzip tar | 解包 → 内层 .img 路由 |
| 三星 `.tar.md5` | 三星固件 | tar + 尾部 MD5 | 解包/重打包（重算 MD5 footer） |
| `.img` 集合 | 任意已解包 ROM | 按内部格式 | 识别并路由 |
| sparse 镜像 | 所有现代系统分区 | `0xED26FF3A` | 双向: simg2img / img2simg（4.0 格式） |
| super 分区 | 动态分区设备 | `0x414C5030`("0PLA") | 拆分逻辑分区 / 合并回 super |
| boot / init_boot | 内核+ramdisk（13+ 分离） | `ANDROID!` | header v0-v4 解析、解包/重打包 |
| vendor_boot | Pixel 系（vendor ramdisk） | `VNDRBOOT` | 同上 + vendor_ramdisk_table 关联 |
| `.br` 镜像 | 小米部分分区 | brotli magic | brotli 解压 → 内部再路由 |
| `.lz4/.xz/.gz/.zst` 裸压缩 | 各厂商散装镜像 | 各压缩魔数 | 解压/压缩 |
| dtbo/dtb/vbmeta | 设备树/AVB | `d\r\n`(dtbo) | 查看 + 打包联动（vbmeta 重算哈希） |
| `system.new.dat` 系 | Android 5-9 OTA | `transfer.list` 同目录 | 解包（.dat/.dat.gz/.dat.br + transfer.list 增量应用）/ 打包 |
| TWRP 备份 | TWRP 备份目录 | `.win` 头 | 解包（.win/.win001 分段合并）/ 打包 |
| `.pac`（SPD） | Spreadtrum/Unisoc ResearchDownload 整包（非 MTK —— 2026-08-04 审查更正，MTK 用 scatter 文件） | 旧格式 1220B 头无魔数 / 新格式 BP_R1.0.0·BP_R2.0.1 2124B 头 | 解包（含分区描述，双格式自动识别）/ 打包 |
| EROFS 镜像 | Android 13+ 系统分区 | `E2 E1 F5 E0`@1024 (0xE0F5E1E2) | 目录浏览、文件提取、替换式重打包 |
| ext4 镜像 | Android 12- 系统分区 | `\x53\xef`(superblock) | 目录浏览、文件提取、替换式重打包 |
| LG KDZ / DZ | LG 官方固件 | v2/v3 容器头 | **仅解包**（KDZ→DZ→分区 chunk 按 eMMC 偏移合并；重打包经社区验证不可靠） |
| 华为 `update.app` | 华为安卓固件 | 512B 头 `0x55 0xAA` + 64B 文件表 | 解包/重打包（保持文件表顺序 + 数据段对齐，有工具参考） |
| 华为 `update.bin` | HarmonyOS 5+ | L2 型分区表 87B/条 | 解包（打包需官方签名） |
| 索尼 `.sin` | Xperia 固件 | v3 `03 53 49 4E`，块头 `LZ4A`/`ADDR` | **仅解包**（RSA 签名无法重签） |
| GPT/MBR 整盘镜像 | 全盘备份 | `EFI PART` / 55AA | 分区表解析、单分区提取 / 合成 |

注意: super 镜像魔数为 `0x414C5030`("0PLA")，geometry magic 为 `0x616c4467`("gDla")—— 早期草案中的 "LPML" 有误，已更正。

### 增量 OTA diff 分区解包

| 操作类型 | 含义 | 策略 |
|---------|------|------|
| `SOURCE_COPY` | 从旧分区复制块 | 直接实现 |
| `SOURCE_BSDIFF` | bspatch 打补丁 | 自研 bspatch |
| `BROTLI_BSDIFF` | brotli 压缩 bsdiff | bspatch + brotli 解压 |
| `PUFFDIFF` | Google gzip 差分 | 自研 puffpatch（puffin 格式） |
| `ZUCCHINI` | Chromium 差分 | **暂缓**（安卓 OTA 极少用） |

所有 diff 解包需要用户提供旧分区镜像，UI 检测到 diff 分区时提示选择源镜像。

### 签名/校验层（响应"三星签名"讨论）

- **Odin 刷写层**: 只校验尾部 MD5（完整性），重打包后重算即可刷入
- **Secure boot 层**: boot.img 内嵌厂商 RSA 签名 + AVB 链，无法伪造；**仅已解锁 Bootloader 设备可刷自定义镜像** —— UI 明确提示
- **AVB 处理**（自研 avbtool 核心逻辑）: boot/vendor_boot 重打包后剥离/重算 hash footer（AVB_FOOTER 32 字节 + VBMeta）、重生成 vbmeta（支持 `VERIFICATION_DISABLED` flag）

## 架构

```
src/image_engine/            # 纯格式库层 —— 无 UI 依赖、无网络依赖，可独立测试
  image_detector             # 魔数嗅探 + 格式路由
  sparse_image               # simg2img / img2simg
  super_image                # lp metadata 解析/生成, 逻辑分区拆分/合并
  boot_image                 # boot/init_boot/vendor_boot: header v0-v4, ramdisk
  payload_image              # payload.bin: protobuf wire 手写解析/序列化, 全量解包/打包
  bspatch_image              # bsdiff patch 应用 (SOURCE_BSDIFF/BROTLI_BSDIFF/PUFFDIFF)
  tar_image                  # tar 解包/打包, 三星 .tar.md5 MD5 footer
  avb_image                  # VBMeta 解析/生成, hash footer add/erase, 禁用验证
  dat_image                  # system.new.dat 系: transfer.list 解析 + 增量应用/生成
  twrp_image                 # TWRP .win 备份: 分段合并/拆分
  pac_image                  # MTK .pac 整包: 分区描述解析, 解包/打包
  kdz_image                  # LG KDZ/DZ: 容器头解析, DZ 分区 chunk 按 eMMC 偏移合并
  huawei_image               # update.app (512B 头 + 64B/条文件表, 含重打包) / update.bin (L2 分区表)
  sin_image                  # 索尼 sin v3: ADDR/LZ4A 块描述符 → raw
  disk_image                 # GPT/MBR 整盘: 分区表解析/单分区提取
  fs/                        # 文件系统镜像 (无 FUSE, 纯解析):
    fs_image                 #   FsImage 抽象接口: listFiles/extractFile/replaceFile/repack
    erofs_reader             #   EROFS 解析器 (目录/提取/替换重打包)
    ext4_reader              #   ext4 解析器 (extent tree/inline data/xattr)
  registry.h                 # 格式注册表: detect/unpack/pack/inspect 统一接口, 按魔数路由
  compression/               # zstd / lz4 / bzip2 / xz / brotli 封装层

src/root_patcher/            # 修补层 —— 自研装配逻辑
  RootPatcher (抽象接口)      #   patch(bootImage, config) → patchedImage, 自动备份原镜像
  magisk_patcher             # Magisk 系: 下载 Magisk/Kitsune APK → 提取 magiskinit → 注入 ramdisk + init 链替换
                             #   （入口参数化: 官方 magisk / kitsune 分支, 同一注入机制）
  kernelsu_patcher           # KernelSU 系: 读设备 KMI → 下载匹配 .ko (official/next/suki 源) → 注入 init_boot ramdisk
                             #   （入口参数化: 官方 / KernelSU-Next / SukiSU-Ultra, KMI 匹配范围不同）
  apatch_patcher             # APatch: 下载 APatch APK → 提取 kpatch → 内核段注入
  assets_downloader          # 官方注入物下载器（版本缓存, 失败可手动指定本地文件）

src/ui/image_tool_panel      # 左侧第 5 个独立面板
```

分层原则: `image_engine` 纯库（不碰网络/UI），`root_patcher` 依赖它做修补，`image_tool_panel` 只做编排。

## 数据流与交互

整个面板是拖放目标（`setAcceptDrops(true)`，无单独"拖拽区"），文件拖到任意位置即识别；"打开文件"按钮兜底。

```
拖入文件 → 魔数嗅探 → 格式信息展示(分区列表/版本/元数据)
  → 用户选择动作:
      解包 → 选输出目录 → 进度条 → 完成(列出产物)
      打包 → 选输入目录/文件 → 自动识别组 → 生成
      转换 → sparse↔raw / 压缩↔解压
      修补 → 选 Root 方案(Magisk/KernelSU/APatch) → 下载注入物 → 输出 patched 镜像
      (diff 分区 → 提示选旧镜像 → 应用补丁)
```

UI 布局沿用现有面板风格: 格式识别信息卡、动作按钮组（按识别结果动态启用）、进度条 + 日志（OutputPanel 风格）。

## 错误处理

- 格式损坏 → 定位到具体结构（如 "lp metadata tables_checksum 不匹配"），不抛裸异常
- diff 分区缺旧镜像 → 明确提示需要的分区名，不静默跳过
- 注入物下载失败 → 可手动指定本地文件（APK / .ko / APatch APK）
- 修补/重打包前自动备份原镜像（`xxx.orig.bak`）
- 输出校验: 解包产物校验 SHA-256（payload manifest 自带 data_sha256_hash）

## 测试策略

- `image_engine` 用 Qt Test（项目已依赖 Qt6，零新依赖）:
  - sparse 往返转换、lp metadata 解析（构造二进制样本）、boot header v0-v4 解析、bsdiff patch 应用（小样本）、AVB footer 往返
- 真实样本冒烟测试: 仓库提供 2-3 个小 payload/super 样本下载脚本（不入库）

## 新增依赖

- `zstd`、`lz4`、`bzip2`、`xz`（payload + 裸压缩镜像）
- `brotli`（.br 镜像 + BROTLI_BSDIFF）
- 无 protobuf —— payload 手写 wire format 解析（格式固定，主流 payload_dumper 同做法）

Linux 系统包 / Windows vcpkg 获取。

## CMake 变更

- 新增 `image_engine`、`root_patcher` 静态库目标
- `image_tool_panel` 编入主程序
- 可选 `ENABLE_IMAGE_TESTS` 开关生成测试二进制

## 产物结构

```
解包 payload → <输出>/system.img, vendor.img, boot.img, init_boot.img ...（sparse 保留/转 raw 可选）
解包 zip/tgz/tar.md5 → 原目录结构 + 每镜像再路由
修补 boot → boot_patched.img + .orig.bak
拆分 super → system.img, vendor.img, product.img ...
```

## 签名材料获取原则（2026-08-04 补充）

华为等私有厂商固件（update.app / update.bin / KDZ 等）的签名验证材料与密钥参数**不内置、不捆绑分发**，但支持**运行时从外部获取 + 手动导入**两种途径：

1. **运行时下载（优先）**：从外部公开仓库（如提供签名工具/材料的逆向仓库）下载签名处理工具与参数配置 —— 复用 `AssetsDownloader` 模式（与 Magisk/KernelSU/APatch 注入物、AdbEmbedded 的 Google platform-tools 下载同一模式），下载源列表可配置（JSON 配置：仓库 URL + 文件路径 + 版本），产物缓存于用户数据目录（`QStandardPaths::AppDataLocation + "/patcher/"`）
2. **手动导入（兜底）**：签名处理参数配置文件（JSON：签名头类型 "08"/"06"、签名长度偏移规则）+ 厂商签名密钥/证书文件路径，UI 提供"导入签名材料"入口

使用规则：
- 未获取/未导入签名材料时，相应操作（重打包/打包）禁用并提示"需要签名材料（可从仓库下载或手动导入）"
- 下载的签名工具与材料**不写入代码库**，仅缓存于用户数据目录
- 签名材料按 key 版本化缓存（如 `huawei-sign/`、`lg-kdz/`），下载失败可回退手动导入

### 候选外部下载源（2026-08-04 搜索确认，全厂商）

| 厂商 | 仓库 | 签名/加密机制 | 能力与限制 |
|------|------|--------------|-----------|
| 华为/荣耀 | [unpack_huawei_package](https://github.com/SimomYung/unpack_huawei_package)、[HuaweiUpdateExtractor](https://github.com/Project-Satori/HuaweiUpdateExtractor)、[unpacker_huawei](https://github.com/scue/unpacker_huawei)、[huextract](https://github.com/echo-devim/huextract)、[huawei-playground](https://github.com/R0rt1z2/huawei-playground) | update.app signature（type 0x05）复合结构 + update.bin "08"/"06" 签名头 | 解析参数可获取；**重签名不可行**（保留原签名仅改分区） |
| LG | IOMonster kdztools（unkdz/undz）、[dumpyara](https://github.com/sebaubuntu-python/dumpyara)、[DumprX](https://github.com/dkpost3/DumprX) | KDZ 加密段 | 解密可获取；新版加密（V60 后）部分不支持 |
| HTC | [kmdm/ruuveal](https://github.com/kmdm/ruuveal)、[kmdm/unruu](https://github.com/kmdm/unruu)、[topjohnwu/HTC-RUU-Decrypt-Tool](https://github.com/topjohnwu/HTC-RUU-Decrypt-Tool) | RUU 加密 ZIP（RC4/AES-CBC，70+ 机型） | 可解密/再加密；**刷写仍需 S-OFF**（无私钥无法正确签名） |
| OPPO/OnePlus/realme | [bkerler/oppo_decrypt](https://github.com/bkerler/oppo_decrypt)（含 [realme fork](https://github.com/djdoolky76/oppo-realme_decrypt)） | ofp 加密（QC/MTK）、ops 加解密 | 解密逻辑可获取；ofs 无公开资料 |
| 三星 | **[Heimdall](https://github.com/benjamin-dobell/Heimdall)**（Odin 3 协议完整开源实现，MIT）、[dumpyara](https://github.com/sebaubuntu-python/dumpyara)（解包） | Odin 3 协议 + sboot RSA 签名 | **可集成刷机协议**（不依赖闭源 Odin）；签名层仍需解锁（Knox 熔断） |
| 索尼 | sin2raw（munjeni）、Flashtool | sin RSA 签名（v3 ADDR/LZ4A） | 可解包；**不可重签** |
| 小米 | 无专用签名仓库（通用 [avbtool](https://android.googlesource.com/platform/external/avb)） | AVB 2.0 vbmeta | avbtool 通用处理（disable verification） |
| Google Pixel | avbtool（AOSP） | AVB | 开源通用 |
| vivo/iQOO | **无公开签名逆向资源**（如实标注） | 私有加密 | 无 |
| 摩托罗拉 | （Firmware Extractor 组件） | xml.zip + 签名 | 无专项公开工具 |
| 联发科 MTK | [mtkclient](https://github.com/bkerler/mtkclient)（已有子模块） | DA 签名 | GPLv3 子模块已有 |

**已知边界（如实标注）**：
- 华为 signature 为证书链+时间戳+设备唯一标识复合结构，**通用重签名大概率失败** —— 安全做法是保留原固件 signature 仅修改 system/boot 分区
- HTC 重加密后**仍需 S-OFF 才能刷**（无私钥无法正确签名）
- 三星**无公开重签工具**，但刷机协议有 Heimdall 开源实现（Odin 3）；签名层官方路径为 bootloader 解锁（Knox 熔断）
- vivo/iQOO 无公开签名逆向资源

**维修行业真实通道（2026-08-04 搜索确认，对应"死局"质疑）**：签名层不存在"破解"，但刷机有完整通道 —— ① 解锁 Bootloader（官方/工程解锁，已有实现）；② 协议级开源实现（Heimdall 三星 Odin、EDL 高通、mtkclient 联发科，已有/待集成）；③ 售后/工程工具（华为 HDB 协议 + 旧版 HiSuite 通道、高通 msmdownloadtool 工程版）。工具覆盖面按此三通道规划，不依赖签名破解。

### BL 解锁策略架构（2026-08-04 补充，回应"通用解法进不去"）

**现实**：官方通用解锁命令（`oem unlock`/`flashing unlock`）大量机型进不去（命令移除/封禁/机型不支持）；真实解锁 = 官方命令（两轨之一）+ **机型特定漏洞利用**（Towelroot/Dirty Pipe/Mali CVE/Mtk-Su/小米 HyperOS 绕过等）。

**架构**：解锁模块改为**策略注册表**（现有 `unlockBootloader` 多策略探测的扩展）：
- 策略类型 1：官方命令策略（已有：多品牌解锁命令 + 探测顺序）
- 策略类型 2：漏洞利用策略 —— 挂在现有 vuln_db/exploit_engine 框架上（CVE 匹配 + 三段式利用已实现），漏洞库条目扩展 `unlock: true` 标记，扫描命中后走"检测→利用→验证"流程完成解锁
- 策略类型 3：EDL/MTK 强通道（已有 edl/mtkclient 子模块，testpoint 短接说明文档化）

**可引用资源**：[awesome-android-root-exploits](https://github.com/DuncanParSky/awesome-android-root-exploits)（锁定 BL 设备漏洞合集）、[Xiaomi-HyperOS-BootLoader-Bypass](https://github.com/MlgmXyysd/Xiaomi-HyperOS-BootLoader-Bypass)、[cn-bootloader-unlock-wall-of-shame](https://github.com/Hydro3ia/cn-bootloader-unlock-wall-of-shame)（厂商难度分级，UI 展示各品牌解锁路径指引）。

**风险标注**：漏洞利用解锁有硬件熔断风险（三星 Knox/TEE 永久损坏、小米硬件级风险）—— UI 在解锁前明确警示（现有解锁流程已有提示，补强机型特定风险）。

受影响模块：华为 update.app/update.bin（B5/B6）、LG KDZ（重打包需签名验证材料时）、HTC RUU（E 阶段后续）。

## 维修诊断模块（计划 E，2026-08-05 重新设计）

面向手机维修场景。**2026-08-05 简化决策（用户）**：不做 12 项自研测试 —— 屏幕/触摸/传感器/音频/相机/按键/电池自检由**厂商工程模式模块**承担（厂商验证过的自检流程，比自研可靠）。计划 E 收敛为一个功能：

### 「打开工程模式」按钮

- 位置：`system_tool_panel` **「开发调试」分类下新增"维修诊断"子分组**（单按钮，2026-08-05 用户决策：非独立分类，挂现有分类）
- 实现：识别设备品牌/芯片（`ro.product.brand` / `ro.hardware` / 型号）→ 查表映射到对应工程模式入口（拨号码或 Activity）→ `am start` 启动
- **入口表实现时联网验证**（默认信息不可信原则）：MTK `*#*#3646633#*#*` / 高通 `*#*#4636#*#*` / 三星 `*#0*#` / 小米 `*#*#6484#*#*` / 华为 `*#*#2846579#*#*` / OPPO 等 —— 以搜索验证为准，不凭记忆
- 工程模式不可用/无入口时：明确提示 + 可选降级（少量关键项如 `dumpsys battery` 状态显示，评估后决定）
- 约束：纯 ADB（拨号盘/am start），无需 Root；EEPROM/字库读写（EDL/MTK）仍为后续扩展

## 明确不做（YAGNI）

- ZUCCHINI diff 解包（暂缓，标注提示）
- 三星 secure boot 重签、索尼 sin 重签（技术上不可能，仅提示解锁）
- LG KDZ 重打包（社区验证不可靠，仅解包）
- 增量 OTA 打包生成（只做解包方向，打包为全量）
- Magisk 模块 / KernelSU 元模块的编辑管理
- **从零 mkfs 打包文件系统**（EROFS/ext4 只做"基于原镜像的替换式重打包"，参考 e2fsprogs debugfs 思想；从零生成文件系统为后续扩展）
- f2fs 文件系统浏览（列为 fs 模块后续扩展点）

## 参考实现（学思想，对照格式，不直接搬代码）

| 模块 | 参考项目 | 许可证 | 学习点 |
|------|---------|--------|--------|
| boot/init_boot/vendor_boot | magiskboot (topjohnwu/Magisk) | GPLv3 | boot 解包/重打包权威实现 |
| payload 解包 | payload_dumper-go (ssut)、ota-dump (Rust) | GPLv3 / Apache-2.0 | 手写 protobuf + 压缩分派 |
| super 拆分 | lpunpack (unix3dgforce)、AOSP liblp | GPLv3 / Apache-2.0 | lp metadata 结构 |
| sparse 转换 | AOSP libsparse | Apache-2.0 | sparse 4.0 格式 |
| AVB | avbtool (AOSP external/avb) | BSD-2 | VBMeta/hash footer/禁用验证 |
| bsdiff/puffdiff | bsdiff (Colin Percival)、puffin (Google) | BSD-2 / Apache-2.0 | 增量补丁算法 |
| 三星重打包 | SamsungImageRepacker (Mnky313) | GPLv3 | tar.md5 重打包 |
| LG KDZ/DZ | unkdz/undz (IOMonster)、dumpyara (sebaubuntu-python) | MIT/AGPL | KDZ→DZ→chunk 合并 |
| 华为 update.app | huextract (echo-devim)、HuaweiUpdateExtractor | MIT/GPLv2 | 文件表 + 重打包对齐 |
| 华为 update.bin | unpack_huawei_package (SimomYung) | MIT | L2 分区表解析 |
| 索尼 sin | sin2raw (munjeni)、Flashtool | GPLv2 | SIN v3 ADDR/LZ4A 块 |
| EROFS | erofs-utils (Linux 内核社区) | GPLv2 | fs 解析/重打包 |
| ext4 | e2fsprogs (debugfs write 思想) | GPLv2 | extent tree/替换重打包 |
| Magisk 修补 | Magisk (topjohnwu) | GPLv3 | magiskinit 注入装配 |
| KernelSU 修补 | ksud (tiann/KernelSU) | GPLv3 | init_boot 注入 + KMI 匹配 |
| APatch 修补 | APatch/KernelPatch (bmax121) | GPLv3 | kpatch 内核段注入 |

所有许可证与 GPLv3 兼容，与项目现有 edl/、mtkclient/ 子模块风格一致。
