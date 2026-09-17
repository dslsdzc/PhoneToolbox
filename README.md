# PhoneToolbox

Android 设备多功能工具箱 v0.0.1-beta03 —— 通过 ADB / Fastboot / EDL / MTK DA 协议对 Android 设备进行诊断、管理和维护。

## 功能

| 功能 | 支持模式 | 说明 |
|------|----------|------|
| 设备检测 | ADB / Fastboot / Fastbootd / EDL / MTK DA / MTK BROM | 自动轮询检测设备连接状态和当前模式 |
| 设备信息 | ADB / Fastboot | 型号、Android 版本、SDK、安全补丁级别、Bootloader 锁定状态等 |
| 刷机工具 | Fastboot / Fastbootd / EDL / MTK DA / **MTK BROM 直刷** | GPT 分区匹配、A/B 槽、super 镜像、稀疏镜像检测、断点续传 |
| **MTK BROM 直刷** | **MTK BROM（LEGACY / XFLASH / XML 三代自动路由）** | **按设备代际自动选链：DA 文件解析与选择、DA1→DA2 两阶段、EMI/DRAM 初始化、GPT 分区表（主 GPT；512/4096 探测 + 头/条目双 CRC fail-closed）、按设备分区表逐分区写** |
| 死砖恢复 | EDL / MTK DA | 自动匹配分区文件、校验镜像大小、备份关键分区、支持失败重试 |
| **镜像处理** | 本地 | **29 种格式拖放识别、17 格式解包、sparse 转换、打包、文件系统浏览** |
| **Root 修补** | 本地 | **8 个方案入口全覆盖（Magisk 系 / KernelSU 系 / APatch），boot/init_boot 修补** |
| 系统工具 | ADB | 性能监控（CPU/GPU/内存/温度）、界面定制、分区管理、应用管理、安全隐私、开发调试、**维修诊断** |
| 漏洞扫描 | ADB | 加载本地漏洞库 JSON，自动匹配设备版本，执行检测脚本发现已知漏洞 |
| 漏洞利用 | ADB | 三段式自动化利用流程（检测 → 利用 → 验证），支持二进制 payload 推送 |

> **MTK BROM** 走「刷入」按计划刷写（链路按设备代际自动选择 LEGACY / XFLASH / XML），**不含**
> 「死砖恢复」的备份关键分区 / 失败重试（BROM 模式下分区表按设计清空，单分区与死砖入口不可达；
> 镜像超分区只能跳过，无强写出口）。

## 镜像处理（全格式覆盖）

| 类别 | 格式 |
|------|------|
| 解包 (17 种) | payload.bin（含增量 OTA diff）、sparse、boot/init_boot、tar/tar.md5、super 动态分区、system.new.dat 系、LG KDZ/DZ、华为 update.app/update.bin、索尼 SIN v3、SPD .pac、TWRP .win、GPT 整盘、EROFS、ext4、OPPO/realme OFP（QC/MTK 双变体）、OnePlus OPS（SAHARA/settings.xml 解密） |
| 转换 | sparse ↔ raw 双向、压缩流解压（zstd/lz4/xz/gzip/brotli/bzip2） |
| 打包 | .img 集合 → tar/tar.md5（Odin 兼容）、raw → sparse |
| 浏览 | EROFS/ext4 目录树、文件提取、ext4 替换（metadata_csum 标注） |
| 入口 | 全面板拖放识别 + 格式信息卡 + 动作按钮动态启用 + 后台线程执行 |

## Root 修补（全方案覆盖）

| 系 | 入口 |
|----|------|
| Magisk 系 | 官方 / Magisk Alpha / Kitsune（boot ramdisk 注入，magiskinit + .backup/init 链） |
| KernelSU 系 | 官方 / KernelSU-Next / SukiSU-Ultra / ReSukiSU（LKM init_boot 注入 + KMI 自动匹配 + **非 GKI AnyKernel3 内核替换**） |
| APatch | APatch / KernelPatch（内核段注入，kpimg） |
| 老设备 | SuperSU ramdisk 注入 |
| 模块 | 模块框架安装（Zygisk / LSPosed 类） |
| 安全 | 修补前自动备份（.orig.bak）、注入物运行时下载（缓存 + 手动指定） |

## 支持的设备模式

- **ADB** — 正常 Android 系统模式
- **Fastboot** — 传统 Bootloader 模式
- **Fastbootd** — Android 10+ 用户空间 Fastboot
- **EDL (9008)** — Qualcomm 紧急下载模式
- **MTK DA** — MediaTek Download Agent 模式
- **MTK BROM（LEGACY / XFLASH / XML 三代自动路由）** — BROM 直刷：DA 文件解析与选择、DA1/DA2 两阶段、EMI/DRAM 初始化（preloader 显式/自动导入/默认关闭的网络获取）、按设备分区表的刷写计划与预览。**链路按设备代际自动选择**：LEGACY = PMT + `0xE8` EMI；XFLASH = 12B 小端帧 + `INIT_EXT_RAM` EMI + `WRITE/READ_DATA` + `SHUTDOWN`；XML = 文本命令（`CMD:*` / `OK` / `OK@0x<len>` / `OK!EOT`）+ `WRITE-FLASH`/`READ-FLASH` + `REBOOT`；XFlash/XML 的分区表为 GPT（主 GPT；512/4096 探测 + 头/条目双 CRC fail-closed）。离线验证（真 DA/preloader/GPT 样本 + mock 逐帧），**真机全链未验证（三代皆是）**；**XML 代无真实设备样本**（帧与命令全部来自上游 mtkclient 源码）
- **三星 Odin** — Odin 下载模式（VID 0x04E8 + CDC_DATA 接口类）→ 按 PIT 刷写 BL/AP/CP/CSC 的 .tar.md5（Phase C 代码就绪，真机未验证）
- **三星 Exynos EUB 救援（EUB → Download 模式）** — 检测 VID `0x04E8` / PID `0x1234`（**先于 Odin 判据认领**）→ 打开设备读自述（SoC 名 / SoC ID / Chip ID / USB Booting Version）→ 按 **8 个 SoC 的布局表**把**用户自备**的 `sboot.bin` 切段逐段发进设备 RAM（段间重开设备 + 等重枚举）→ 设备进入 Download 模式后**交给上面的 Odin 链**正常刷写。载荷自备（`sboot.bin` / `sboot.bin.lz4` / `BL_*.tar.md5` 内提取），**不内置也不分发任何三星签名二进制**；**只发 RAM 镜像、不写设备存储**，完成后设备仍需正常刷写。**真机未验证**；真样本已下载核对（5 个三星官方 BL 包，存 `reference/eub-samples/`，**gitignored 不进仓库**；facts §H）—— 证据到 mock + 数值断言 + 真样本核对（真样本核对 ≠ 真机验证）；帧头 4 字节与尾 2 字节**语义未定**（三个可用实现互相矛盾，按表照抄其中一源）；布局表证据等级不一（9610 双源一致 / 8890 单源+实战报告 / 7580 双源分歧已采信 hubble 的 `bl2=0x8000` / 其余 5 个单源）且**绑定固件修订**；**Exynos9830 已实现**（其参照流程需在分段后另发 `ldfw.img`/`tzsw.img`：本工具从**同一个 BL 包**内提取、在分段之后按序另发；来源若是裸 `sboot.bin`/`.lz4` 则在预检即拒绝并指引改选 `BL_*.tar.md5` —— 不另开第二个文件选择器；该二文件存在于 9830 的 BL 包内由**真样本证实**（facts §H2），**真机仍未验证**）；进 EUB 需主引导失败或测试点，**2025-04 起三星可用 eFuse 永久封堵 EUB**

## 快速开始

### Linux

```bash
# 安装依赖
sudo pacman -S qt6-base qt6-tools cmake ninja pkg-config zstd lz4 bzip2 xz brotli
# 或 Ubuntu/Debian
# sudo apt install qt6-base-dev qt6-tools-dev cmake ninja-build pkg-config libusb-1.0-0-dev \
#   libzstd-dev liblz4-dev libbz2-dev liblzma-dev libbrotli-dev

# 构建
cmake -B build -G Ninja
cmake --build build

# 运行
./build/PhoneToolbox
```

### Windows

```powershell
# 方法一：使用构建脚本（推荐）
.\build_windows.ps1 -Static

# 方法二：手动构建
cmake -B build -G Ninja ^
    -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake ^
    -DVCPKG_TARGET_TRIPLET=x64-windows-static ^
    -DSTATIC_BUILD=ON
cmake --build build
```

> 静态构建（`-DSTATIC_BUILD=ON`）将 Qt6 和 libusb 全部编译进 EXE，无需分发任何 DLL。

## 依赖

| 依赖 | 用途 | Linux | Windows |
|------|------|-------|---------|
| Qt6 (Core, Widgets, Network) | UI 框架 | 系统包管理器 | vcpkg |
| libusb-1.0 | USB 通信（EDL/MTK） | pkg-config | vcpkg |
| zstd / lz4 / bzip2 / xz / brotli | 镜像压缩算法 | 系统包 | vcpkg |
| pthread | 线程支持 | 自带 | 不需要 |

## 目录结构

```
PhoneToolbox/
├── CMakeLists.txt              # CMake 构建配置（支持 STATIC_BUILD）
├── build_windows.ps1           # Windows 构建脚本
├── resources/
│   └── resources.qrc           # Qt 资源文件
├── src/
│   ├── main.cpp                # 入口
│   ├── core/                   # 核心模块
│   │   ├── adb_embedded.cpp    # ADB 自动检测/下载/缓存
│   │   ├── device_detector.cpp # 设备检测（ADB/Fastboot/EDL/MTK/Odin）
│   │   ├── flash_tool.cpp      # 刷机核心逻辑
│   │   ├── engineer_mode.cpp   # 维修诊断: 工程模式入口映射（12 品牌）
│   │   ├── modes/              # EDL/MTK 通讯协议实现
│   │   ├── edl/                # EDL 刷写链（自研 Sahara/Firehose）
│   │   │   ├── edl_transport.h          # 可注入传输接口
│   │   │   ├── edl_libusb_transport.cpp # libusb 传输实现（超时换算/重枚举）
│   │   │   ├── edl_session.cpp          # 会话编排 + 数据面（分块/ZLP）
│   │   │   ├── sahara.cpp               # Sahara 协议（programmer 上传）
│   │   │   ├── firehose.cpp             # Firehose 协议（configure/program/patch）
│   │   │   └── flash_plan.cpp           # 刷写计划构建（rawprogram/patch + GPT 回填）
│   │   ├── odin/               # 三星 Odin 刷写链（自研, Phase C；真机未验证）
│   │   │   ├── odin_transport.h          # 可注入传输接口（4 方法, 纯字节管道）
│   │   │   ├── odin_libusb_transport.cpp # 真机传输（CDC_DATA 类匹配 + 老 PID 兜底 + 轮询换算）
│   │   │   ├── pit.cpp                   # PIT 解析（28B 头 + 132B 条目 + 尾部签名容忍）
│   │   │   ├── samsung_plan.cpp          # 刷写计划（tar.md5 流式索引 + 文件名优先匹配）
│   │   │   ├── odin_protocol.cpp         # 协议帧（1024B 控制包 / 版本化分片 / ACK 判定）
│   │   │   └── odin_session.cpp          # 会话编排（读设备 PIT + 对账 + 逐分区写入）
│   │   └── ...
│   ├── image_engine/           # 镜像格式引擎（纯库, 全自研）
│   │   ├── payload_image.cpp   # payload.bin（手写 protobuf + 增量 diff）
│   │   ├── sparse_image.cpp    # sparse 转换
│   │   ├── boot_image.cpp      # boot/init_boot v0-v4
│   │   ├── super_image.cpp     # 动态分区
│   │   ├── kdz_image.cpp       # LG KDZ/DZ
│   │   ├── huawei_image.cpp    # update.app/update.bin + 签名材料接口
│   │   ├── sin_image.cpp       # 索尼 SIN v3
│   │   ├── pac_image.cpp       # SPD .pac
│   │   ├── oppo_crypto.cpp     # OPPO 系密码核心（AES-128 自实现 + OPS 自定义流密码）
│   │   ├── oppo_keys.cpp       # OPPO 密钥库（QC/MTK triplet 派生 + OPS mbox, 外部 JSON 追加）
│   │   ├── oppo_ofp.cpp        # OPPO/realme OFP（QC 尾页清单 / MTK 混淆头 + 文件表）
│   │   ├── oppo_ops.cpp        # OnePlus OPS（尾页 + settings.xml 判据）
│   │   ├── oppo_extract.cpp    # OFP/OPS 流式解包 + 摘要校验
│   │   ├── fs/                 # EROFS/ext4 文件系统
│   │   └── ...
│   ├── root_patcher/           # Root 修补层（8 入口, 自研装配）
│   │   ├── magisk_patcher.cpp  # Magisk 系（官方/Alpha/Kitsune）
│   │   ├── kernelsu_patcher.cpp# KernelSU 系（官方/Next/SukiSU/ReSukiSU + 非 GKI）
│   │   ├── apatch_patcher.cpp  # APatch/KernelPatch
│   │   ├── ramdisk_su_patcher.cpp # SuperSU 老设备
│   │   └── module_installer.cpp  # 模块框架安装
│   ├── ui/                     # 界面
│   │   ├── main_window.cpp     # 主窗口
│   │   ├── tool_panel.cpp      # 左侧工具面板
│   │   ├── image_tool_panel.cpp# 镜像工具面板（第 5 工具）
│   │   ├── fs_browser_dialog.cpp # 文件系统浏览
│   │   ├── flash_plan_dialog.cpp # 刷写计划预览（EDL；逐条目表格 + 未验证勾选门控）
│   │   ├── plan_preview_widget.cpp # 通用刷写计划预览控件（EDL/三星共用）
│   │   ├── samsung_plan_dialog.cpp # 三星刷写计划预览（PIT 来源/校验结论/未验证勾选门控）
│   │   └── ...
│   └── vuln_db/                # 漏洞数据库框架
│       ├── vuln_entry.cpp      # 漏洞条目数据结构及 JSON 序列化
│       ├── vuln_db.cpp         # 漏洞数据库（本地 JSON 持久化）
│       ├── vuln_matcher.cpp    # 版本/补丁/平台匹配引擎
│       ├── exploit_engine.cpp  # ADB 脚本自动化执行引擎
│       └── importers/          # 本地 JSON 导入
├── tests/                      # 单元测试（Qt Test, 62 个测试源文件；ctest 目标共 63 个）
├── third_party/                # 第三方二进制
│   └── mtk_bridge/             # MTK DA 通讯桥（兼容过渡）
├── edl/                        # bkerler/edl 子模块（GPLv3, 协议参考）
├── mtkclient/                  # bkerler/mtkclient 子模块（GPLv3, 协议参考）
└── vulndb.json                 # 用户自定义漏洞数据库
```

## 测试

```bash
cmake -B build -G Ninja
cmake --build build
ctest --test-dir build    # 63 个测试目标, 全绿

# MTK 真样本用例（reference/mtk-samples/ 缺失时默认 SKIP；开启后缺失即 FAIL）
cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build
```

## ADB 获取策略

PhoneToolbox 自动检测 ADB 工具，无需手动安装：

1. 检查系统 PATH 和 `$ANDROID_HOME/platform-tools`
2. 若未找到，自动从 Google 官方下载 platform-tools
3. 解压到临时目录后使用

## 漏洞数据库

新建 `vulndb.json`，按以下格式编写：

```json
{
  "vuln_db_version": "2.0.0",
  "entries": [
    {
      "id": "CVE-2025-XXXXX",
      "summary": "漏洞描述",
      "severity": "CRITICAL",
      "cvss_score": 9.8,
      "affected": {
        "android_version_min": "14",
        "android_version_max": "14",
        "patch_level_before": "2025-04-01",
        "sdk_min": 34,
        "sdk_max": 34,
        "platforms": ["google/pixel_*"]
      },
      "exploit": {
        "detect": ["echo '检测脚本...'", "echo 'VULNERABLE'"],
        "exploit": ["echo '利用脚本...'"],
        "verify": ["echo '验证脚本...'", "echo 'EXPLOIT_SUCCESS'"]
      }
    }
  ]
}
```

在程序内加载后即可扫描检测和自动利用。

## 开源协议

本项目基于 **GNU General Public License v3.0 (GPLv3)** 发布，详见项目根目录的 [LICENSE](LICENSE) 文件。

## 第三方开源致谢

| 项目 | 许可证 | 用途 |
|------|--------|------|
| [Qt 6](https://www.qt.io/) (Core, Widgets, Network) | [LGPL v3](https://doc.qt.io/qt-6/lgpl.html) | UI 框架 |
| [libusb-1.0](https://libusb.info/) | [LGPL v2.1](https://github.com/libusb/libusb/blob/master/COPYING) | USB 通讯（EDL/MTK DA） |
| [bkerler/edl](https://github.com/bkerler/edl) | [GPL v3](https://github.com/bkerler/edl/blob/master/LICENSE) | EDL Sahara/Firehose 协议（参照自研） |
| [bkerler/mtkclient](https://github.com/bkerler/mtkclient) | [GPL v3](https://github.com/bkerler/mtkclient/blob/main/LICENSE) | MTK DA/BROM 协议（参照自研） |
| [ReCoreShift/mtk-gpt-tool](https://github.com/ReCoreShift/mtk-gpt-tool) | [MPL-2.0](https://www.mozilla.org/en-US/MPL/2.0/) | GPT / scatter 测试夹具（**仅作离线样本**，见下注） |
| [frederic/exynos-usbdl](https://github.com/frederic/exynos-usbdl) | [GPL-3.0-or-later](https://www.gnu.org/licenses/gpl-3.0.html) | Exynos EUB 帧格式与 sboot 切段脚本的参照（**仅作事实参照**，见下注） |
| [VDavid003/exynos-usbdl](https://github.com/VDavid003/exynos-usbdl) | [GPL-3.0-or-later](https://www.gnu.org/licenses/gpl-3.0.html) | 同上（fork：含设备自述串读取与 README 事实） |
| [halal-beef/hubble](https://github.com/halal-beef/hubble) | [GPL-2.0](https://www.gnu.org/licenses/old-licenses/gpl-2.0.html) | Exynos EUB 救援流程与 `ExynosData/*.json` 布局表 |
| [ananjaser1211/exynos8890-exynos-usbdl-recovery](https://github.com/ananjaser1211/exynos8890-exynos-usbdl-recovery) | 未声明（仓库内无许可证文件） | 8890 / 7580 救援包（cfg + 恢复脚本 + 实战报告） |
| [astarasikov/exynos9610-usb-emergency-recovery](https://github.com/astarasikov/exynos9610-usb-emergency-recovery) | 未声明（仓库内无许可证文件） | 9610 救援包（dltool，SMDK 血统） |
| [KHwang9883/MobileModels-csv](https://github.com/KHwang9883/MobileModels-csv) | [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/) | 设备型号数据（运行时下载） |
| [Google platform-tools](https://developer.android.com/tools/releases/platform-tools) (adb, fastboot) | [Apache 2.0](https://www.apache.org/licenses/LICENSE-2.0) | 设备通讯协议 |
| zstd / lz4 / bzip2 / xz / brotli | BSD/MIT 系 | 镜像压缩算法 |

> **注意**：
> - ADB 和 Fastboot 工具由 Google 提供，PhoneToolbox **不内置分发**其二进制，而是在运行时自动检测系统环境或从 Google 官方下载。
> - **bkerler/edl**：EDL Sahara/Firehose 协议代码源自 [bkerler/edl](https://github.com/bkerler/edl)（GPLv3），对应许可证文件见 `edl/LICENSE`。
> - **bkerler/mtkclient**：MTK DA 通讯功能基于 [bkerler/mtkclient](https://github.com/bkerler/mtkclient)（GPLv3），包含 `third_party/mtk_bridge/` 预编译二进制及完整源码树（`mtkclient/`），对应许可证文件见 `mtkclient/LICENSE`。其中**芯片代际表（hw_code → damode，89 条）为整表转写**——GPL-3.0 与本项目 GPLv3 兼容，转写只取静态事实；生成脚本 `tools/gen_mtk_chip_table.py`，来源 URL 与 commit 见生成物 `src/core/modes/mtk_chip_table.cpp` 头部注释。
> - **ReCoreShift/mtk-gpt-tool**（**MPL-2.0**）：MTK BROM 的 **GPT / scatter 测试夹具**（`PGPT.img` / `SGPT.img` /
>   `sgdisk_print.txt` / `MT6789_Android_scatter.xml`）取自该仓库的 `tests/fixtures/`，**只作离线验证样本** ——
>   存放于 `reference/mtk-samples/`（**gitignored**），**不随仓库分发、不参与构建**；来源 URL、sha256 与实测值见
>   `docs/superpowers/specs/mtk-xflash-facts.md`。**这些样本不是我们自己的设备**（第三方仓库的测试夹具）。
> - **Exynos EUB 参照仓库**（`frederic/exynos-usbdl`、`VDavid003/exynos-usbdl`、`halal-beef/hubble`、
>   `ananjaser1211/exynos8890-exynos-usbdl-recovery`、`astarasikov/exynos9610-usb-emergency-recovery`）：
>   **仅作事实参照** —— 存放于 `reference/`（**gitignored**）、**不随仓库分发、不参与构建**；本仓的 EUB
>   实现为自研，只取帧格式、偏移表等静态事实。其中**两个救援包未声明许可证**，仅作离线事实核对之用；
>   救援包内附带的 sboot 各段二进制（上游从对应型号固件切出，如 8890 包的
>   `fwbl1/bl2/el3_mon/bootloader.bin`）同属 gitignored，**不进构建、不进提交、不分发**。
>   上游 commit 锚点与逐条出处见 `docs/superpowers/specs/exynos-eub-facts.md` §G。
> - **MobileModels-csv** 数据仅在运行时从 GitHub 拉取，不内置分发，使用 CC BY-NC-SA 4.0 许可证（非商业用途）。

### 许可证兼容性

本项目中使用的开源库许可证均与 **GPLv3** 兼容：
- **LGPL v3 / LGPL v2.1**：库动态/静态链接至 GPLv3 程序中使用合法
- **Apache 2.0**：与 GPLv3 兼容（Apache 2.0 代码可作为 GPLv3 项目的组成部分分发）
- **CC BY-NC-SA 4.0**：仅用于运行时数据获取，非代码分发，不涉及许可证传染
