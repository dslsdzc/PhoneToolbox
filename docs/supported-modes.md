# 支持模式与验证状态

本页保留各底层刷写链的协议细节、证据等级和已知限制。README 只放概览。

## 验证状态总表

| 模式 | 检测 | 刷写链 | 真机 |
|------|------|--------|------|
| ADB | 已验证 | 已验证 | 是 |
| Fastboot / Fastbootd | 已验证 | 已验证 | 是 |
| EDL (9008) | 已验证 | Sahara / Firehose 自研 | 部分 |
| MTK DA | 已验证 | DA 通讯 | 部分 |
| MTK BROM | 已验证 | LEGACY / XFLASH / XML | 全链未验证 |
| 展锐 SPD | 已验证 | ResearchDownload | 未验证 |
| 三星 Odin | 已验证 | PIT + tar.md5 | 未验证 |
| 三星 EUB | 已验证 | EUB → Download | 未验证 |

## ADB

正常 Android 系统模式。设备信息、系统工具、应用管理、漏洞扫描与利用均在此模式运行。

### Android 1.0–4.4 兼容

旧系统的 `toolbox` 没有 `grep`、`wc`、`head`、`awk`、`sed` 或 `$(...)`，因此设备信息、性能监控和分区检测改为：

- 远端只执行 `cat` / `ls` / `dumpsys` 等基础命令，在桌面端解析原始输出；
- 电池优先 `dumpsys battery`，失败回退 `/sys/class/power_supply/battery/capacity`；
- 分区优先 `/proc/mtd` 的友好名称，再探测 `/dev/block/by-name`、`/dev/block/bootdevice/by-name`、`/dev/block/platform/*/by-name`；`/proc/partitions` 的块单位按 1 KiB 处理；
- legacy Root 代理是 minSdk 1 的静态 ARM 可执行文件，固定端口 32145，没有 Activity/Provider。

交叉编译 libsodium 时必须设置 `ac_cv_c_bigendian=no`，否则 configure 会把 ARM 误判成 big-endian，SHA-256/SHA-512 与 CPace 会算错。

## Fastboot / Fastbootd

- **Fastboot**：传统 Bootloader 模式。
- **Fastbootd**：Android 10+ 用户空间 Fastboot。

用于分区刷写、擦除、读取、Bootloader 解锁/回锁等。

## EDL (9008)

Qualcomm 紧急下载模式。自研 Sahara / Firehose 链：

- Sahara：programmer 上传；
- Firehose：configure / program / patch / read；
- 刷写计划：rawprogram / patch + GPT 回填；
- 支持按分区读取、写入、擦除和整包计划。

## MTK DA

MediaTek Download Agent 模式。通过 DA 与设备通讯，支持分区读取、写入、擦除和 FRP 处理。

## MTK BROM

BROM 直刷：DA 文件解析与选择、DA1/DA2 两阶段、EMI/DRAM 初始化（preloader 显式 / 自动导入 / 默认关闭的网络获取）、按设备分区表生成刷写计划与预览。

链路按设备代际自动选择：

- **LEGACY**：PMT + `0xE8` EMI；
- **XFLASH**：12B 小端帧 + `INIT_EXT_RAM` EMI + `WRITE/READ_DATA` + `SHUTDOWN`；
- **XML**：文本命令（`CMD:*` / `OK` / `OK@0x<len>` / `OK!EOT`）+ `WRITE-FLASH` / `READ-FLASH` + `REBOOT`。

XFlash / XML 的分区表为 GPT（主 GPT；512/4096 探测 + 头/条目双 CRC fail-closed）。EMI/DRAM 档位判定（LEGACY）含两处比上游更严的 fail-closed：EMI 应答非 ACK/NACK、档 C/D 的 `dramlength` 超长；见 `docs/superpowers/specs/mtk-brom-facts.md` §4.2。

**检测含 preloader**：preloader 一族的 VID/PID 与 BROM 走同一张白名单、同一个模式（`MODE_MTK_BROM`；设备列表显示「MTK BROM / Preloader」）。两者握手字节相同，身份要握手后由 `GET_BL_VER`（0xFE）判定，检测层不猜。preloader 上的握手/刷写真机未验证（窗口比 BROM 短）。

**验证状态**：离线验证（真 DA / preloader / GPT 样本 + mock 逐帧）；真机全链未验证（三代皆是）；XML 代无真实设备样本（帧与命令全部来自上游 mtkclient 源码）。

## 展锐 SPD（ResearchDownload）

展锐 ResearchDownload 模式（VID `0x1782`）：

1. 选 `.pac` 后自动提取包内 FDL1 / FDL2（提取或落盘失败回退手选；提取为纯解析，逐字节等于包内切片）；
2. HDLC / BSL 上传 FDL；
3. 按分区表逐分区写入；
4. 复位。

真机未验证（既有刷写链为离线 / mock 验证）。

## 三星 Odin

Odin 下载模式（VID `0x04E8` + CDC_DATA 接口类）→ 按 PIT 刷写 BL / AP / CP / CSC 的 `.tar.md5`。

Phase C 代码就绪，真机未验证。

## 三星 Exynos EUB 救援

检测 VID `0x04E8` / PID `0x1234`（先于 Odin 判据认领）→ 打开设备读自述（SoC 名 / SoC ID / Chip ID / USB Booting Version）→ 按 8 个 SoC 的布局表把用户自备的 `sboot.bin` 切段逐段发进设备 RAM（段间重开设备 + 等重枚举）→ 设备进入 Download 模式后交给 Odin 链正常刷写。

- 载荷自备（`sboot.bin` / `sboot.bin.lz4` / `BL_*.tar.md5` 内提取），不内置也不分发任何三星签名二进制；
- 只发 RAM 镜像、不写设备存储，完成后设备仍需正常刷写；
- 真机未验证；真样本已下载核对（5 个三星官方 BL 包，存 `reference/eub-samples/`，gitignored 不进仓库；facts §H）；
- 证据到 mock + 数值断言 + 真样本核对（真样本核对 ≠ 真机验证）；
- 帧头 4 字节与尾 2 字节语义未定（三个可用实现互相矛盾，按表照抄其中一源）；
- 布局表证据等级不一（9610 双源一致 / 8890 单源+实战报告 / 7580 双源分歧已采信 hubble 的 `bl2=0x8000` / 其余 5 个单源），且绑定固件修订；
- Exynos9830 已实现：其参照流程需在分段后另发 `ldfw.img` / `tzsw.img`，本工具从同一个 BL 包内提取并按序另发；来源若是裸 `sboot.bin` / `.lz4` 则在预检即拒绝并指引改选 `BL_*.tar.md5`；该二文件存在于 9830 的 BL 包内由真样本证实（facts §H2），真机仍未验证；
- 进 EUB 需主引导失败或测试点；2025-04 起三星可用 eFuse 永久封堵 EUB。

## 风险提示

底层刷写链的“离线验证 / mock 验证”不等于真机验证。请只在明确知道设备型号、固件版本和分区布局时操作，并先备份关键分区。
