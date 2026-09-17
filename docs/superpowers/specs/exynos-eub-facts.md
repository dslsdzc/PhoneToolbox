# 三星 Exynos EUB（USB-Boot）救援链 — 事实侦察报告

> **纪律**：本文件只记录**可核对的事实**（每条带出处：仓库内 `file:line` 或公开 URL）。
> 推断与未解之处一律标注，不写成结论。**边界声明**见文末 §F —— 任何下游文档不得把本文件里的
> "某工具这么做" 转述为"我们验证过"。
>
> **侦察日期**：2026-09-17。**侦察起因**：队列项原写作「展锐 EUB 紧急引导模式」，
> 侦察后更正为**三星 Exynos**（见 §A0）。参照仓库见 §G（`reference/` 全目录 gitignored、只读、不构建、不分发）。

---

## A0. 队列项更正：EUB 不是展锐的

| 事实 | 出处 |
|---|---|
| EUB = **Exynos USB Boot**（亦称 USB-Boot mode / USB download mode），是**三星 Exynos BootROM** 的接口，功能位相当于高通的 EDL | `reference/exynos-usbdl/README.md:9`；`reference/exynos9610-usb-emergency-recovery/dltool/readme.txt:1-5`（工具源自 2004 年三星 SMDK 官方下载工具，`(c) 2004,2006 Ben Dooks`） |
| 两次独立检索（中/英）均未发现任何"展锐 EUB"；展锐的对应物是 BSL/HDLC（本仓 F5 已实现） | 侦察记录；本仓 `src/core/modes/spd_*` |

**结论（供队列更正）**：原「展锐 EUB」项应改为「**三星 Exynos EUB 救援**」；展锐线剩下的真实条目
只有「`.pac` 内 FDL 自动提取」。

---

## A. 模式身份与设备自述信息

| # | 事实 | 出处 |
|---|---|---|
| A1 | USB 身份 = **VID `0x04E8` / PID `0x1234`**，全 SoC 一致（不按 SoC 区分 PID） | `reference/exynos-usbdl/exynos-usbdl.c:13-14`；`reference/exynos-usbdl-vdavid003/exynos-usbdl.c:15-16`；`reference/exynos9610-usb-emergency-recovery/dltool/dltool.c:124-126`；`reference/hubble/hubble.py:61` |
| A2 | **iProduct 字符串 = SoC 名**（如 `Exynos9610`）。hubble 直接用它拼表文件名，匹配不到即拒绝 | `reference/hubble/hubble.py:209, 233-237`；exynos-usbdl 同样逐串比对（`identify_target()`，仅 exploit 模式调用）`reference/exynos-usbdl-vdavid003/exynos-usbdl.c:313-343` |
| A3 | **iSerialNumber = SoC ID（前 15 字符）+ Chip ID 后 16 字符**；接口字符串（iInterface）= **"USB Booting Version"**（取 `[12:16]` 四位） | `reference/hubble/hubble.py:210-223` |
| A4 | 极老 SoC 的 iProduct 是 `"SEC S5PC210 Test B/D"`（不报 SoC 名）→ 退化为**从 sboot.bin 内容里正则 `EXYNOS[0-9]+` 反推**（sboot 镜像内含该 ASCII） | `reference/hubble/hubble.py:192-202, 227-230` |
| A5 | **进入条件苛刻**：主引导（UFS/eMMC）**失败**才回退 USB download；零售机默认先试内置存储 | `reference/exynos-usbdl-vdavid003/README.md:44-49`（"USB download mode is only accessible if first boot method has failed"） |
| A6 | 实机可观察特征（8890 时代）：插上后**仅在 Windows 上出现约 1 秒**；**按住电源键**可维持枚举；`dmesg` 可见 `Manufacturer: System MCU` | `reference/exynos8890-exynos-usbdl-recovery/README.md:24, 26`；`reference/hubble/hubble.py:68`（"Tip: Plug in your device with the power button pressed."） |
| A7 | **eFuse 永久封堵**：新 SoC 可烧熔丝彻底禁用 EUB 的 USB 枚举；三星据报自 **2025-04** 更新起这么做；KG-lock 机型出厂即无 EUB | `reference/exynos-usbdl-vdavid003/README.md:25`；ChimeraTool 官方博文 <https://news.chimeratool.com/2025/10/21/samsungs-april-2025-exynos-update-permanently-disables-eub-mode/> |

---

## B. 传输与帧格式

| # | 事实 | 出处 |
|---|---|---|
| B1 | claim **interface 0**；Linux 下若内核驱动占用需先 `detach_kernel_driver(0)` | `reference/hubble/hubble.py:304-308`；`reference/exynos-usbdl/exynos-usbdl.c`（无 detach，libusb 直接 claim） |
| B2 | **OUT 端点 = `0x02`（ep2）**；**IN 端点 = `0x81`（ep1）**，用于读设备回显 | OUT：`reference/exynos9610-usb-emergency-recovery/dltool/dltool.c:339`、`reference/hubble/hubble.py:110`、`reference/exynos-usbdl-vdavid003/exynos-usbdl.c:176`；IN：`reference/hubble/hubble.py:115`、`reference/exynos-usbdl/exynos-usbdl.c:242` |
| B3 | **帧 = 4 字节头字段 + u32 LE 长度 + 数据 + 2 字节尾**；长度为 **`数据长度 + 10`**（= 4+4+2 框开销 + 数据） | `reference/exynos9610-usb-emergency-recovery/dltool/dltool.c:39-43, 58`（`*size = st.st_size + 10`）；`reference/exynos-usbdl-vdavid003/exynos-usbdl.c:126-131, 413`；`reference/hubble/hubble.py:41-45` |
| B4 | ⚠️ **头 4 字节三实现互相矛盾**：exynos-usbdl 发 `0x00000000`（calloc 全零，从不赋值）／dltool 发**下载地址**（默认 `0xFFFFFFFE`，`-a` 可覆盖）／hubble 发 ASCII **`1B 44 4E 57`（`\x1B` + "DNW"）** | `reference/exynos-usbdl-vdavid003/exynos-usbdl.c:126-131`（结构体首字段 `unk0`，全程未赋值）；`reference/exynos9610-usb-emergency-recovery/dltool/dltool.c:21, 39-43, 336`；`reference/hubble/hubble.py:27-29` |
| B5 | ⚠️ **尾 2 字节同样三实现矛盾**：`0x0000`（exynos-usbdl，calloc）／**数据字节 16 位累加和**（dltool `calc_cksum`）／**常量 `0xFFFF`**（hubble） | `reference/exynos9610-usb-emergency-recovery/dltool/dltool.c:88-108, 337`；`reference/hubble/hubble.py:50-51` |
| B6 | **三者都报可用**（各自在自家 SoC 上）→ B4/B5 两字段的语义**未定，且设备很可能不校验**。侦察未找到任何一处解释其含义 | 见 §C 的实测来源；**不推断** |
| B7 | 分块与应答：exynos-usbdl 按 **512 字节**切多次 bulk 传输（`BLOCK_SIZE 512`）；dltool 一次 `usb_bulk_write` 发全帧；hubble 一次 `device.write`（pyusb 内部拆包）。**三者都无逐帧应答** | `reference/exynos-usbdl/exynos-usbdl.c:15, 60`；`reference/exynos9610-usb-emergency-recovery/dltool/dltool.c:339`；`reference/hubble/hubble.py:110` |
| B8 | **阶段之间有两种做法**：① 每次**重新打开设备** + `sleep 1`（shell 脚本路径）；② 一次打开、连续发完所有段（hubble）。cfg 里 `DNW_WAIT` 注释写作 "Wait Re-Enumeration" | ① `reference/exynos-usbdl/scripts/boot-8890.sh:3-9`、`reference/exynos8890-exynos-usbdl-recovery/exynos-usbdl-recover.sh:87-93`；② `reference/hubble/hubble.py:310-327`；cfg `reference/exynos8890-exynos-usbdl-recovery/exynos-usbdl_g930f.cfg` |
| B9 | 8890 脚本路径明确提示**不可靠**："sometimes re-enumerating / re-connecting to the device fails and causes the 4th binary to not be flashed, Trial and error is needed" | `reference/exynos8890-exynos-usbdl-recovery/README.md:13` |

---

## C. 载荷与每 SoC 布局

| # | 事实 | 出处 |
|---|---|---|
| C1 | **载荷 = 用户自己的原厂 BL** 里的 `sboot.bin`：从 `BL_*.tar.md5` 提取；**现代包内该文件是 `.lz4` 压缩**，需先解压 | `reference/hubble/hubble.py:152-183`（`extract_bl_tar` + `lz4.frame.decompress`）；本仓**已有** lz4 能力：`src/image_engine/compression/lz4_wrapper.h:5-6` |
| C2 | `sboot.bin` 按**每 SoC 一张固定偏移表**切成多段，逐段独立发送；段名各工具不同（`fwbl1`/`epbl`/`bl2`/`bl31`/`u-boot`/`el3_mon`/`bootloader`/`lk`），**语义对应关系以偏移为准** | 见表下注释 |
| C3 | **一来源受第三方实战报告佐证**：<br>• **8890**：`fwbl1 0x0/0x2000` → `el3_mon 0x2000/0x24000` → `bl2 0x26000/0x26D10` → `bootloader 0x61000/0xD1000`（4 段）。**来源是单一份脚本**（ananjaser 的恢复包**直接引用** frederic 的同一脚本，不构成独立第二来源）；其佐证是 ananjaser 的**实测恢复报告** | `reference/exynos-usbdl/scripts/split-sboot-8890.sh:2-5`；复用处与实测报告 `reference/exynos8890-exynos-usbdl-recovery/README.md:45` 与 `exynos-usbdl-recover.sh:87-93` |
| C4 | ⚠️ **双源在 7580 的第 3 段长度上分歧**：两源的前两段与末段**完全一致**（`fwbl1 0x0/0x2000`、`{bl31‖el3_mon} 0x2000/0x30000`、`{u-boot‖bootloader} 0x3A000/0xD1000`，仅命名不同），但第 3 段 `bl2`：ananjaser 脚本 `0x32000/0x7D10`（32016B，其后留 752B 空隙）vs hubble 表 `0x32000/0x8000`（32768B，**恰好填到下一段起点 0x3A000**）。**谁对未定**，下游表项必须写清采信哪一源 | `reference/exynos8890-exynos-usbdl-recovery/A510F/split-sboot-7580.sh:4` ↔ `reference/hubble/ExynosData/Exynos7580.json` |
| C5 | **双源完全一致**：**9610** `fwbl1 0x0/0x2000` → `epbl 0x2000/0x13000` → `bl2 0x15000/0x2F000` → **`repeat-fwbl1`（= 再发一次 fwbl1）** → `u-boot 0x5A000/0x180000` → `el3_mon 0x1DA000/0x40000`（6 段）。"重发 fwbl1"这一怪点在两源里都显式存在（hubble 表里有 `repeat-fwbl1` 键；astarasikov 仓库的 `dltool.c` 里 `part1.bin` 在 `files[]` 中**出现两次**）。**唯一差异**：astarasikov 另有第 7 段 `part6 = 0x21A000/0x101000`，hubble 表中**没有**对应项 | `reference/hubble/ExynosData/Exynos9610.json:5-34` ↔ `reference/exynos9610-usb-emergency-recovery/split_bootloader_a505.sh:1-6` + `dltool/dltool.c:311-319` |
| C6 | **单源（未对拍，弱证据）**：**8895**（6 段，其中第 4 段 = 第 1 段的拷贝，作者自注"stolen from astarasikov dltool.c#L315"；表内**未命名**第 5、6 段）、**7885 / 9810 / 9820 / 9830**（仅 hubble `ExynosData/*.json`） | 8895：`reference/exynos-usbdl/scripts/split-sboot-8895.sh:2-7`（第 5 行即自注）；其余 `reference/hubble/ExynosData/` |
| C7 | **9830 额外要求**：`files_to_send = ["ldfw.img", "tzsw.img"]`（BL 包内**其它**文件，在 sboot 各段之后发送）+ `response_support = true`（设备会回显，可读） | `reference/hubble/ExynosData/Exynos9830.json`；读取逻辑 `reference/hubble/hubble.py:329-341` |
| C8 | **回显/崩溃诊断**（仅 `response_support=true` 的 SoC）：从 `0x81` 读 512B 文本；回显含 `UUUUUUUU` 视为 **BootROM 崩溃**，后续 8 字符一组可解出 Version / SoC ID / Chip ID / Reset Status / OM Status / Key Bank Address / 启动设备 / 状态寄存器 | `reference/hubble/hubble.py:112-141, 77-99` |
| C9 | 布局表**绑定固件修订**：脚本注释给出所用 sboot 的型号与 sha1（例：8890 `G930W8VLS6CSH1`，sha1 `9322ccb4…`；8895 `G950FXXU1AQJ5`，sha1 `648a3e2c…`；7580 `A510FXXS8CTI7`，sha1 `466852d1…`） | `reference/exynos-usbdl/scripts/split-sboot-8890.sh:1`；`…8895.sh:1`；`reference/exynos8890-exynos-usbdl-recovery/A510F/split-sboot-7580.sh:1` |
| C10 | 另一条**Windows 侧**公开流程（等价物）：Multidownloader 的 `DNW_STORE <soc> <label> <n1> <n2> <file>` cfg；`n1/n2` 两参数含义**未解**，仅记录不采用 | `reference/exynos8890-exynos-usbdl-recovery/exynos-usbdl_g930f.cfg`；XDA <https://xdaforums.com/t/creating-correct-cfg.4545255/> |

---

## D. 结局与衔接

| # | 事实 | 出处 |
|---|---|---|
| D1 | 段发完后设备**进入 Download（Odin）模式**；工具末句直说需要"reflash the stock firmware as the bootloader will still be wiped" | `reference/hubble/hubble.py:352` |
| D2 | 商业工具同款**两步式**：载入四件套（BL/AP/CP/CSC）→ Start（引导进 Download 模式）→ 再 Start 刷写 | ErosFlashTool v1.7.5 发行说明 <https://github.com/Gabriel2392/ErosFlashTool/releases>（其 GitHub 仓库仅站点页，**桌面实现不开源**） |
| D3 | 本仓已有可复用的一侧：Download 模式的检测判据与整套 Odin 刷写链（Phase C 已交付） | `src/core/odin/odin_libusb_transport.h:19-26`；`src/core/odin/odin_session.h` |

---

## E. 与本仓的集成面

| # | 事实 | 出处 |
|---|---|---|
| E1 | 检测层已有三星分支（VID 0x04E8 + 接口类 0x0A 判据 + 老 PID 兜底），在 `detectProtocolDevices()` 里 | `src/core/device_detector.cpp:571-620` |
| E2 | ⚠️ **认领顺序风险**：EUB（PID `0x1234`）**不在** `isOdinDevice` 的兜底 PID 表（`0x6601/0x685D/0x68C3`）中，但若 EUB 接口恰好是 `0x0A` 类且带批量 in/out，就会被**误认成 Odin 下载模式**而走错链路 → 新判据必须**先于** Odin 判据执行 | `src/core/odin/odin_libusb_transport.cpp:98-113`（判据本体）；`src/core/device_detector.cpp:592-597`（现状） |
| E3 | 新模式（如 `MODE_SAMSUNG_ODIN = 10`）的既有集成面 = 4 处：枚举 / 检测 / 通道分派 / UI 模式感知 | `src/core/device_detector.h:29`、`src/core/device_detector.cpp:606-616, 759`、`src/core/flash_tool.cpp:1231`、`src/ui/flash_panel.cpp:264, 363` |

---

## F. 诚实边界（下游文档不得越界）

1. **无真机**：本次侦察全部来自公开实现与文档；本机**没有任何 Exynos 设备**。任何实现都只能是
   mock/离线验证。
2. **B4/B5 两字段语义未定**：三个"可用"实现互相矛盾。我们的实现只能**按所选参照照抄并在表里标注**，
   不得声称理解其语义。
3. **弱证据标注**：C6 的 5 个 SoC（8895/7885/9810/9820/9830）**单源**，未与第二个实现对拍；
   C4 的 7580 第 3 段**两源长度分歧**（0x7D10 vs 0x8000）未定谁对 —— 表项必须写明采信哪一源。
4. **布局表绑修订**：C9 的 sha1 是对应工具所用 rom 的；换修订即可能错位。表项必须带出处，
   且**校验和不符时默认拒绝**。
5. **不改写存储**：本流程只往设备 **RAM** 发镜像（无任何写分区命令）；D1 明说 bootloader 仍处于被擦状态，
   需随后正常刷写。
6. **前置条件由用户承担**：设备必须已回退到 EUB（引导失败/测试点），且**未被 eFuse 封堵**（A7，2025-04+）。
7. **不使用漏洞利用**：本链只走**签名 bootloader**的正常下载路径（`exynos-usbdl` 的 `n` 模式 = 其 README
   "send a signed bootloader" 语义）。`e`/`e2`（零长度 bulk / GET_CONFIGURATION）与 houston 一类
   **未签名代码加载**属"不做"族，本文件只作存在性记录、不采用。
8. **不分发三星签名二进制**：载荷一律由用户自备（其自有固件包）；参照仓库里附带的二进制仅存于
   gitignored 的 `reference/`，不进构建、不进提交。

---

## G. 参照清单与版本锚点

| 参照 | 本地路径（gitignored） | 版本锚点 | 许可 |
|---|---|---|---|
| exynos-usbdl（原版，含 split 脚本） | `reference/exynos-usbdl/` | `81599c2` | GPL-3.0-or-later（`exynos-usbdl.c:1-3`） |
| exynos-usbdl（fork，新增 GET_CONFIGURATION exploit 与 README 事实） | `reference/exynos-usbdl-vdavid003/` | `ef74603` | 见仓库 LICENSE |
| 8890/7580 救援包（含 cfg 与恢复脚本） | `reference/exynos8890-exynos-usbdl-recovery/` | `2f4591c` | 未声明 |
| 9610 救援包（dltool，SMDK 血统） | `reference/exynos9610-usb-emergency-recovery/` | `c4d33b9` | 未声明（dltool 源自 Ben Dooks 2004 工具） |
| hubble（含 `ExynosData/*.json` 布局表） | `reference/hubble/` | `71bbb52` | GPL-2.0（`hubble.py:270`） |
| ChimeraTool 博文（eFuse 封堵） | — | 2025-10-21 | 商业方公开文章 |
| ErosFlashTool 发行说明（EUB 教程/SoC 名单） | — | tag `175`(v1.7.5) / `180L` | 桌面实现不开源 |

**未纳入本线的相关物**（存在性记录）：`VDavid003/exynos-usbdl` 的 `e`/`e2` 未签名代码加载、
`halal-beef/houston-pub`（CVE-2024-56426）、ChimeraTool 商业实现。
