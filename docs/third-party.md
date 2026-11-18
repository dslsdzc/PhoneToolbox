# 第三方组件与许可证

## 运行时依赖

| 项目 | 许可证 | 用途 |
|------|--------|------|
| [Qt 6](https://www.qt.io/) (Core, Widgets, Network) | LGPL v3 / GPLv3 例外 | UI 框架 |
| [libusb-1.0](https://libusb.info/) | LGPL v2.1 | USB 通信（EDL / MTK / Odin / EUB） |
| [libsodium](https://libsodium.org/) | ISC | Root 代理 Noise / CPace |
| Lua | MIT | MTK BROM UI 脚本沙箱 |
| zstd / lz4 / bzip2 / xz / brotli | BSD / MIT 系 | 镜像压缩 |

## Vendored 源码

以下第三方源码随仓库提交，包含本地 CMake 或构建补丁，因此不使用 git submodule：

| 项目 | 许可证 | 位置 |
|------|--------|------|
| [Lua](https://www.lua.org/) | MIT | `third_party/lua/` |
| [libusb](https://github.com/libusb/libusb) | LGPL v2.1 | `third_party/libusb/` |
| [cJSON](https://github.com/DaveGamble/cJSON) | MIT | `agent/third_party/cjson/` |
| [noise-c](https://github.com/rweather/noise-c) | MIT | `agent/third_party/noise-c/` |
| cpace | 见目录内许可证 | `agent/third_party/cpace/` |

## Git submodules

| 项目 | 许可证 | 用途 |
|------|--------|------|
| [bkerler/edl](https://github.com/bkerler/edl) | GPLv3 | EDL Sahara / Firehose 协议参考 |
| [bkerler/mtkclient](https://github.com/bkerler/mtkclient) | GPLv3 | MTK DA / BROM 协议参考与芯片代际表转写 |

克隆后需要：

```bash
git submodule update --init --recursive
```

## 协议与事实参照（不随仓库分发）

以下内容只作事实参照或离线样本，放在 gitignored 的 `reference/`，不随仓库分发、不参与构建：

- [ReCoreShift/mtk-gpt-tool](https://github.com/ReCoreShift/mtk-gpt-tool)（MPL-2.0）：MTK BROM 的 GPT / scatter 测试夹具，位于 `reference/mtk-samples/`；来源 URL、sha256 与实测值见 `docs/superpowers/specs/mtk-xflash-facts.md`。
- [frederic/exynos-usbdl](https://github.com/frederic/exynos-usbdl)（GPL-3.0-or-later）：Exynos EUB 帧格式与 sboot 切段脚本参照。
- [VDavid003/exynos-usbdl](https://github.com/VDavid003/exynos-usbdl)（GPL-3.0-or-later）：同上，含设备自述串读取。
- [halal-beef/hubble](https://github.com/halal-beef/hubble)（GPL-2.0）：EUB 救援流程与 `ExynosData/*.json` 布局表。
- [ananjaser1211/exynos8890-exynos-usbdl-recovery](https://github.com/ananjaser1211/exynos8890-exynos-usbdl-recovery)（未声明许可证）：8890 / 7580 救援包。
- [astarasikov/exynos9610-usb-emergency-recovery](https://github.com/astarasikov/exynos9610-usb-emergency-recovery)（未声明许可证）：9610 救援包。

上述仓库中的 sboot 各段二进制属 gitignored，不进构建、不进提交、不分发。

## 运行时下载的数据

| 项目 | 许可证 | 说明 |
|------|--------|------|
| [KHwang9883/MobileModels-csv](https://github.com/KHwang9883/MobileModels-csv) | CC BY-NC-SA 4.0 | 设备型号数据，仅运行时从 GitHub 拉取，不内置分发；非商业用途 |
| [Google platform-tools](https://developer.android.com/tools/releases/platform-tools) | Apache 2.0 | adb / fastboot；运行时检测或从 Google 官方下载，不内置分发 |

## 许可证兼容性

本项目以 GPLv3 发布。上述依赖中：

- LGPL v3 / LGPL v2.1：动态或静态链接至 GPLv3 程序合法；
- Apache 2.0：与 GPLv3 兼容；
- MIT / ISC / BSD：与 GPLv3 兼容；
- CC BY-NC-SA 4.0：仅用于运行时数据获取，不涉及代码分发。
