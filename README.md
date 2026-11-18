# PhoneToolbox

Android 设备多功能工具箱 —— 通过 ADB / Fastboot / EDL / MTK BROM 等协议进行设备诊断、刷写、镜像处理和 Root 修补。

当前版本：`v0.0.1-beta03`。部分底层刷写链仍处于离线验证阶段，请先阅读 [支持模式与验证状态](docs/supported-modes.md)。

## 功能

| 模块 | 说明 |
|------|------|
| 设备检测 / 信息 | ADB、Fastboot、Fastbootd、EDL、MTK DA、MTK BROM；型号、系统版本、安全补丁、Bootloader 状态 |
| 刷机 | Fastboot / Fastbootd / EDL / MTK DA / MTK BROM；GPT 分区、A/B 槽、super 动态分区、稀疏镜像 |
| 镜像处理 | 29 种格式识别、17 种解包、sparse ↔ raw、tar/tar.md5 打包、EROFS/ext4 浏览 |
| Root 修补 | Magisk / KernelSU / APatch 共 8 个入口，boot / init_boot 修补与模块安装 |
| 系统工具 | 性能监控、界面定制、分区管理、应用管理、安全隐私、开发调试、维修诊断 |
| 漏洞扫描 / 利用 | 本地 JSON 漏洞库匹配，三段式检测 → 利用 → 验证 |
| Root 代理 | Android 端 APK + Noise 加密通道，支持 root 命令、文件传输、审计与多电脑配对 |
| 设备侧 API | APK 通过 ContentProvider 提供 ADB 不易获取的数据（应用列表、审计等） |

## 支持模式

| 模式 | 用途 | 状态 |
|------|------|------|
| **ADB** | 正常 Android 系统 | 真机验证 |
| **Fastboot** | 传统 Bootloader | 真机验证 |
| **Fastbootd** | Android 10+ 用户空间 Fastboot | 真机验证 |
| **EDL (9008)** | Qualcomm 紧急下载 | 离线 / 真机部分验证 |
| **MTK DA** | MediaTek Download Agent | 离线 / 真机部分验证 |
| **MTK BROM** | LEGACY / XFLASH / XML 三代自动路由 | 离线验证；真机全链未验证 |
| **展锐 SPD** | ResearchDownload / .pac | 离线验证；真机未验证 |
| **三星 Odin** | Odin 下载模式 / PIT 刷写 | 离线验证；真机未验证 |
| **三星 Exynos EUB** | EUB → Download 模式救援 | 离线 / 真样本核对；真机未验证 |

MTK BROM、SPD、Odin、EUB 的协议细节、证据等级和已知限制集中在 [docs/supported-modes.md](docs/supported-modes.md)。首次使用前请确认设备型号、固件和镜像完全匹配。

## 快速开始

### Linux

```bash
# Arch
sudo pacman -S qt6-base qt6-tools cmake ninja pkg-config libusb libsodium \
  zstd lz4 bzip2 xz brotli

# Ubuntu / Debian
sudo apt install qt6-base-dev qt6-tools-dev cmake ninja-build pkg-config \
  libusb-1.0-0-dev libsodium-dev libzstd-dev liblz4-dev libbz2-dev \
  liblzma-dev libbrotli-dev

cmake -B build -G Ninja
cmake --build build
./build/PhoneToolbox
```

### Windows

```powershell
.\build_windows.ps1 -Static
```

手动构建使用 vcpkg 工具链，详见 `build_windows.ps1`。静态构建会把 Qt6 和 libusb 编进 EXE。

### Android 端代理（可选）

```bash
cmake -S agent -B agent/build -G Ninja
cmake --build agent/build
```

构建 APK 需要 Android NDK、libsodium Android 静态库和 Gradle：

```bash
tools/build_root_agent_apk.sh
```

默认包名 `com.phonetoolbox.agent`，可用 `--app-id` / `--label` 覆盖。

## 测试

```bash
# 桌面端
cmake -B build -G Ninja && cmake --build build
ctest --test-dir build --output-on-failure

# Android agent
cmake -S agent -B agent/build -G Ninja && cmake --build agent/build
ctest --test-dir agent/build --output-on-failure
```

MTK 真样本用例默认在 `reference/mtk-samples/` 缺失时 SKIP；`-DMTK_SAMPLES_REQUIRED=ON` 可改为缺失即 FAIL。

## 依赖

| 依赖 | 用途 |
|------|------|
| Qt6 (Core, Widgets, Network) | UI、网络 |
| libusb-1.0 | USB 通信（EDL / MTK / Odin / EUB） |
| libsodium | Root 代理 Noise / CPace |
| zstd / lz4 / bzip2 / xz / brotli | 镜像压缩 |
| CMake + Ninja + pkg-config | 构建 |
| Android NDK + Gradle + libsodium (Android) | 仅构建 APK 时需要 |

ADB / Fastboot 不随包分发：优先使用系统或 `ANDROID_HOME/platform-tools`，找不到时从 Google 官方下载。

## 目录结构

```
src/core/            设备检测、刷机、EDL/MTK/Odin 协议、Root 代理客户端
src/image_engine/    镜像格式解析与打包
src/root_patcher/    Magisk / KernelSU / APatch 修补
src/ui/              界面与设置面板
src/vuln_db/         漏洞库、匹配与利用引擎
agent/               Android native root agent
android/root-agent/  APK 壳与 ContentProvider
tests/               Qt Test 测试
docs/                设计、协议事实与验证记录
third_party/         桌面端第三方源码
agent/third_party/   agent 第三方源码
tools/               构建与辅助脚本
```

## 漏洞库

在程序内加载本地 JSON 漏洞库；示例和字段说明见 [docs/vulndb.md](docs/vulndb.md)。

## 第三方组件

Qt6、libusb、libsodium、Lua、cJSON、noise-c、cpace，以及 edl / mtkclient 等协议参考项目。完整许可证和来源见 [docs/third-party.md](docs/third-party.md)。

`edl/` 与 `mtkclient/` 以 git submodule 形式存在；其余第三方源码为 vendored（含本地 CMake/补丁），直接随仓库提交。

## 许可证

GNU General Public License v3.0，详见 [LICENSE](LICENSE)。
