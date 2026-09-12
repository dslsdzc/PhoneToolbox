# PhoneToolbox

Android 设备多功能工具箱 v0.0.1-beta03 —— 通过 ADB / Fastboot / EDL / MTK DA 协议对 Android 设备进行诊断、管理和维护。

## 功能

| 功能 | 支持模式 | 说明 |
|------|----------|------|
| 设备检测 | ADB / Fastboot / Fastbootd / EDL / MTK DA | 自动轮询检测设备连接状态和当前模式 |
| 设备信息 | ADB / Fastboot | 型号、Android 版本、SDK、安全补丁级别、Bootloader 锁定状态等 |
| 刷机工具 | Fastboot / Fastbootd / EDL / MTK DA | GPT 分区匹配、A/B 槽、super 镜像、稀疏镜像检测、断点续传 |
| 死砖恢复 | EDL / MTK DA | 自动匹配分区文件、校验镜像大小、备份关键分区、支持失败重试 |
| **镜像处理** | 本地 | **29 种格式拖放识别、17 格式解包、sparse 转换、打包、文件系统浏览** |
| **Root 修补** | 本地 | **8 个方案入口全覆盖（Magisk 系 / KernelSU 系 / APatch），boot/init_boot 修补** |
| 系统工具 | ADB | 性能监控（CPU/GPU/内存/温度）、界面定制、分区管理、应用管理、安全隐私、开发调试、**维修诊断** |
| 漏洞扫描 | ADB | 加载本地漏洞库 JSON，自动匹配设备版本，执行检测脚本发现已知漏洞 |
| 漏洞利用 | ADB | 三段式自动化利用流程（检测 → 利用 → 验证），支持二进制 payload 推送 |

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
│   │   ├── device_detector.cpp # 设备检测（ADB/Fastboot/EDL/MTK）
│   │   ├── flash_tool.cpp      # 刷机核心逻辑
│   │   ├── engineer_mode.cpp   # 维修诊断: 工程模式入口映射（12 品牌）
│   │   ├── modes/              # EDL/MTK 通讯协议实现
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
│   │   └── ...
│   └── vuln_db/                # 漏洞数据库框架
│       ├── vuln_entry.cpp      # 漏洞条目数据结构及 JSON 序列化
│       ├── vuln_db.cpp         # 漏洞数据库（本地 JSON 持久化）
│       ├── vuln_matcher.cpp    # 版本/补丁/平台匹配引擎
│       ├── exploit_engine.cpp  # ADB 脚本自动化执行引擎
│       └── importers/          # 本地 JSON 导入
├── tests/                      # 单元测试（Qt Test, 32 个测试目标）
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
ctest --test-dir build    # 33 个测试目标, 全绿
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
| [KHwang9883/MobileModels-csv](https://github.com/KHwang9883/MobileModels-csv) | [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/) | 设备型号数据（运行时下载） |
| [Google platform-tools](https://developer.android.com/tools/releases/platform-tools) (adb, fastboot) | [Apache 2.0](https://www.apache.org/licenses/LICENSE-2.0) | 设备通讯协议 |
| zstd / lz4 / bzip2 / xz / brotli | BSD/MIT 系 | 镜像压缩算法 |

> **注意**：
> - ADB 和 Fastboot 工具由 Google 提供，PhoneToolbox **不内置分发**其二进制，而是在运行时自动检测系统环境或从 Google 官方下载。
> - **bkerler/edl**：EDL Sahara/Firehose 协议代码源自 [bkerler/edl](https://github.com/bkerler/edl)（GPLv3），对应许可证文件见 `edl/LICENSE`。
> - **bkerler/mtkclient**：MTK DA 通讯功能基于 [bkerler/mtkclient](https://github.com/bkerler/mtkclient)（GPLv3），包含 `third_party/mtk_bridge/` 预编译二进制及完整源码树（`mtkclient/`），对应许可证文件见 `mtkclient/LICENSE`。
> - **MobileModels-csv** 数据仅在运行时从 GitHub 拉取，不内置分发，使用 CC BY-NC-SA 4.0 许可证（非商业用途）。

### 许可证兼容性

本项目中使用的开源库许可证均与 **GPLv3** 兼容：
- **LGPL v3 / LGPL v2.1**：库动态/静态链接至 GPLv3 程序中使用合法
- **Apache 2.0**：与 GPLv3 兼容（Apache 2.0 代码可作为 GPLv3 项目的组成部分分发）
- **CC BY-NC-SA 4.0**：仅用于运行时数据获取，非代码分发，不涉及许可证传染
