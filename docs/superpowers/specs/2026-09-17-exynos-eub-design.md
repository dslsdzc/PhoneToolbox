# 三星 Exynos EUB 救援链（EUB → Download 模式）设计 spec

> **事实依据**：`docs/superpowers/specs/exynos-eub-facts.md`（下文所有 `§X#` 均指该文件条目）。
> **状态**：已评审通过（用户 2026-09-17 批准）；实施见 `docs/superpowers/plans/2026-09-17-exynos-eub-recovery.md`。
> **同步记录**：§5.5 接口与 §7 取消语义已按 T6 落地实现回填（2026-09-18）。
> **一句话**：检测 EUB 态的 Exynos 设备 → 用**用户自备的原厂 BL**（`sboot.bin`）按公开布局表切段 →
> 逐段下载进设备 RAM → 设备引导进 Download 模式 → 交给**已交付的 Odin 链**正常刷写。

---

## 1. 目标

在 PhoneToolbox 中新增第 7 种设备模式 **三星 EUB（Exynos USB Boot）**及其救援流程：

1. **检测**：每 2s 轮询里认出 EUB 设备（§A1），设备列表显示「三星 EUB (Exynos)」；
2. **识别**：打开设备读取其自述信息（iProduct = SoC 名 §A2；iSerial = SoC ID + Chip ID §A3；
   接口串 = USB Booting Version §A3），据此选中布局表；
3. **切段**：把用户提供的 `sboot.bin`（直接给，或从其 `BL_*.tar.md5` 内提取，含 `.lz4` §C1）按
   该 SoC 的公开偏移表切成若干段（§C3–§C6）；
4. **下载**：按帧格式（§B3）逐段写入设备 RAM（§B2）；段间**重新打开设备**并等待其重枚举（§B8/§B9）；
5. **交接**：提示设备应已进入 **Download 模式**（§D1），并可直接接上现有 Odin 链刷写（§D3）。

**成功判据**：用户在设备已处于 EUB 且 SoC 有公开布局表时，能一键完成 1–5 步；每一步的日志
可复盘（发了哪段、多少字节、重开几次）。

---

## 2. 非目标（明确不做）

| 不做 | 依据 |
|---|---|
| **任何未签名代码加载**（`e`/`e2` 零长度 bulk、GET_CONFIGURATION、houston 一类） | §F7；本仓既有"不做"族 |
| 签名绕过 / eFuse 操作 / 解锁熔丝 | 同上 |
| **内置或分发任何三星签名二进制**（sboot / ldfw / tzsw 等） | §F8；载荷一律由用户自备 |
| 写设备**存储**（本流程只往 RAM 发镜像，不发任何写分区命令） | §F5 |
| Windows Multidownloader 的 `DNW_STORE` cfg 解析 | §C10（`n1/n2` 参数含义未解） |
| 无公开布局的 SoC 上"猜偏移" | §F3；一律 fail-closed |
| 崩溃回显的 8 字符结构化解析 | §C8（见 §3-D7：只落原文） |

---

## 3. 关键决策（每条带依据）

**D1 — 检测只判 VID/PID，SoC 名在打开设备时才读。**
轮询里的认领判据 = `vid == 0x04E8 && pid == 0x1234`（§A1，全 SoC 一致）。SoC 自述串需要**打开设备**
读字符串描述符，而 EUB 设备可能是**瞬态**的（§A6：插上仅出现约 1 秒），在 2s 轮询里反复 open 既昂贵
又易扰动设备。因此设备列表只显示模式名，SoC 信息在救援对话框打开设备后显示（与 hubble 的顺序一致
`reference/hubble/hubble.py:296-299`）。
⚠️ **认领顺序**：该判据必须**先于** Odin 判据执行（§E2：PID `0x1234` 不在 Odin 兜底表内，但若接口
恰好是 `0x0A` 类 + 带批量端点就会被 `isOdinDevice` 抢走）。

**D2 — 帧 = `[4B 头字段][u32 LE = 数据长 + 10][数据][2B 尾]`；头/尾两字段按来源分派、数据驱动。**
长度字段 `n + 10` 三实现一致（§B3），**恒等不设参数**。头 4 字节与尾 2 字节三实现互相矛盾
（§B4：`0x00000000` / 下载地址 / ASCII `\x1BDNW`；§B5：`0x0000` / 累加和 / `0xFFFF`）且**语义未定**
（§B6）。我们的处理：把它们做成表项里的 **`EubFrameStyle`**，每张表**按其参照源**取风格，并在代码注释
与 UI 里注明"语义未定、照抄自某实现"：

- `ZeroStyle` = 头 `00 00 00 00`、尾 `00 00` —— exynos-usbdl 路径（8890、8895）
- `DnwStyle`  = 头 `1B 44 4E 57`、尾 `FF FF` —— hubble 路径（其余 6 个 SoC）

**D3 — 每段帧**一次写入**，不做 512 字节手工分块。**
exynos-usbdl 按 512B 分块（§B7），但 hubble/dltool 均一次写整帧，且两者覆盖的 SoC 更多；libusb 内部
本来就按 `wMaxPacketSize` 切分传输，手工再切一层没有协议意义。**记录在案**：8890/8895 的实测路径
是 512 分块，若日后真机在 8890 上出问题，第一个该回退的就是这一点。

**D4 — 段间"重新打开设备 + 等待重枚举"，不做单句柄连发。**
两种做法都有人报可用（§B8）。取重开路径，理由：① 它是 8890/7580 的**实测**脚本路径；
② EUB 设备会重枚举（§B8 的 `DNW_WAIT`、§B9），重开天然容忍地址变化；③ 句柄失效时报错点清晰。
每段结束后**等待设备重新出现**（已出现则立即继续），默认超时 10s（可配），超时 → 失败（见 §7）。

**D5 — 内置 8 张表，逐张标注证据等级；7580 采信 hubble 的连续切法。**
见 §5.4 全表。证据等级字段（`EubLoadout::evidence`）取值：
`双源一致`（9610）/`单源+实战报告`（8890）/`双源分歧（已采信 X）`（7580）/`单源`（8895、7885、9810、9820、9830）。
**7580 分歧**（§C4）：`bl2` 长度取 hubble 的 `0x8000`（恰好填到下一段起点，切法连续），
ananjaser 的 `0x7D10` 写进注释与 UI 的"来源"栏。

**D6 — `sboot.bin` 的 SHA-1 与表项不符时：警告 + 勾选确认，不硬拒绝。**
表项绑的是参照工具当年的固件修订（§C9），用户的固件几乎必然不同修订；硬拒绝会让绝大多数人用不了。
沿用本仓既有的「未验证勾选门控」惯例（`功能清单.txt:321`）：对话框列出"表来自 `<型号>`/`sha1 <前 8 位>`、
你的文件 sha1 `<前 8 位>`"，用户勾选"我理解布局可能不匹配"后才可开始。**未提供 sha1 的表项**
（hubble 系）显示"该表未记录修订"。

**D7 — 回显读取只做"读一次、原样落日志"，不解析崩溃结构。**
`response_support = true` 的 SoC（9820、9830 §C7）在每段后可读 `0x81`。§C8 的 8 字符结构化解析
（Version/SoC ID/OM Status…）**本期不做**：解析规则只有单一来源、无第二实现可对拍，且属诊断而非救援
必需。落原文即可让用户/我们事后复盘。

**D8 — 载荷来源三条，优先级从简：**
① 用户直接选 `sboot.bin`；② 选 `sboot.bin.lz4`（用已有 `lz4Decompress`，`src/image_engine/compression/lz4_wrapper.h:6`）；
③ 选 `BL_*.tar.md5`（用已有 `imgtar::indexTarStream`，`src/image_engine/tar_image.h:34`，按名找
`sboot.bin` / `sboot.bin.lz4`，流式读该条目 → 必要时解压）。**不自动从 AP 包找**（BL 才是承载物）。

**D9 — UI：FlashPanel 三星模式下新增「EUB 救援…」按钮，弹独立对话框。**
理由：EUB 是"把设备弄进 Download 模式"的**前置**流程，终点正是三星 Odin 模式；放同一模式面板下，
用户动线最短（按钮复用 `SamsungPlanDialog::buildAndShow` 的调用位置与风格 `src/ui/flash_panel.cpp:800-816`）。
对话框形态与既有「未验证勾选门控」一致：来源选择 → 设备/表匹配结果 → 段预览 → 勾选 → 开始 → 进度 → 交接提示。

**D10 — 交接不做自动跳转。** 完成后提示"设备应已进入 Download 模式，请继续刷写"，并**在检测到
Download 模式设备时把提示点亮**（不自动打开刷写对话框——是否刷、刷什么由用户决定）。

---

## 4. 架构与模块

```
src/core/eub/
  eub_transport.h              # IEubTransport（纯字节管道；镜像 odin_transport.h 的语义与措辞）
  eub_libusb_transport.{h,cpp}# 真机实现 + isEubDevice(PID) 判据 + 设备自述串读取
  samsung_mode.{h,cpp}         # 纯函数：三星三态认领（EUB 优先于 Odin）—— 检测层调用它
  eub_protocol.{h,cpp}         # 帧构造（纯函数）+ 段发送（依赖 IEubTransport，可 mock）
  eub_loadout.{h,cpp}          # 8 张表 + 查表 + 切段 + SHA-1
  eub_session.{h,cpp}          # 编排：逐段重开 + 等待重枚举 + 进度 + 失败语义
src/ui/
  eub_recovery_dialog.{h,cpp}  # 对话框
```

依赖方向（与既有层同款）：`eub_session → IEubTransport ← LibusbEubTransport`；`eub_session → eub_loadout`、
`eub_session → eub_protocol`；`eub_loadout`/`eub_protocol` **不碰 libusb**（可用纯单测覆盖）。
UI 只依赖 session 与 loadout。

---

## 5. 接口与数据结构（签名级）

### 5.1 传输层

```cpp
// src/core/eub/eub_transport.h
namespace eub {

// 设备自述信息（打开设备后读一次；读不到的字段留空）
struct EubDeviceInfo {
    QString socName;        // iProduct，如 "Exynos9610"（§A2；可能是 "SEC S5PC210 Test B/D" §A4）
    QString socId;          // iSerialNumber[0:15]（§A3）
    QString chipId;         // iSerialNumber[15:31]（§A3）
    QString usbBootVersion; // iInterface[12:16]（§A3）
    quint16 vid = 0; quint16 pid = 0; quint8 bus = 0; quint8 address = 0;
};

class IEubTransport {
public:
    virtual ~IEubTransport() = default;
    virtual bool open(QString *error) = 0;                    // 按 VID/PID 打开（重枚举后地址会变，故不锁 bus/addr）
    virtual void close() = 0;                                 // 幂等；close 后可再 open（段间必用）
    virtual bool readDeviceInfo(EubDeviceInfo &out, QString *error) = 0;
    virtual bool writeBulk(const QByteArray &data, QString *error) = 0;   // 一次整帧（§D3）
    virtual QByteArray readBulk(int maxBytes, int timeoutMs, QString *error) = 0; // 回显（§D7）
};
} // namespace eub
```

`LibusbEubTransport`：claim interface 0（Linux 先 detach kernel driver，§B1）；**从描述符解析 interface 0
的 bulk OUT/IN 端点**，若解析不到则回退 `0x02`/`0x81` 并记日志（§B2 三实现都硬编码这对值）；
`isEubDevice(quint16 vid, quint16 pid)` = `vid == 0x04E8 && pid == 0x1234`（§A1）。

### 5.2 三星模式认领（把"顺序"变成纯函数，§D1 的时序风险由此被封死）

```cpp
// src/core/eub/samsung_mode.h  —— 纯函数，不碰 libusb；由检测层调用
namespace eub {
enum class SamsungMode { NotSamsung, Eub, Odin };

// 认领顺序在此函数内**固化**：EUB 判据先于 Odin 判据。
// 传入 = VID/PID + 描述符特征（与 odin::LibusbOdinTransport::isOdinDevice 同参）
SamsungMode samsungModeFor(quint16 vid, quint16 pid,
                           const QList<quint8> &interfaceClasses, bool hasBulkInOut);
}
```

`device_detector.cpp` 的 `0x04E8` 分支改为调用本函数并按返回值分派（`Eub` → `MODE_SAMSUNG_EUB`、
`Odin` → 现有路径、`NotSamsung` → 落到其它分支）。**为什么值得单独抽**：PID `0x1234` 不在 Odin 的兜底
PID 表里，但若它同时满足"接口类 0x0A + 批量 in/out"，`isOdinDevice` 也会返回 true（§E2）——顺序写错
就会把 EUB 设备当成 Download 模式设备。抽成纯函数后，这条优先级可以被**测试钉死**（§8），
而不是只靠代码评审。

### 5.3 协议层

```cpp
// src/core/eub/eub_protocol.h
namespace eub {

struct EubFrameStyle { QByteArray header; QByteArray trailer; };  // 均 4B / 2B（§D2）
QByteArray buildEubFrame(const QByteArray &payload, const EubFrameStyle &style, QString *error);

// 发送一段：构造帧 → 一次 writeBulk（§D3）；payload 为空 → 失败（fail-closed）
bool sendSegment(IEubTransport &t, const QByteArray &payload,
                 const EubFrameStyle &style, QString *error);

constexpr int kFrameOverhead = 10;   // 4 + 4 + 2（§B3）
} // namespace eub
```

### 5.4 布局表（全部数值与出处）

```cpp
struct EubSegment { QString name; quint64 offset; quint64 length; };
struct EubLoadout {
    QString soc;                 // "Exynos9610"
    QStringList models;          // 参照里出现过的机型（仅展示用）
    QString evidence;            // §D5 的四档
    QString sourceNote;          // 出处（文件:行 或 URL）
    QByteArray sbootSha1;        // 参照的原始 sboot 修订（可空）
    EubFrameStyle style;         // §D2
    QList<EubSegment> segments;  // 按序发送；repeat 段照列
    QStringList extraFiles;      // 9830: {"ldfw.img","tzsw.img"}（§C7）
    bool responseSupport = false;// 是否读回显（§C7/§C8）
};

// 查表：SoC 名大小写不敏感；未知 → false（fail-closed）
bool eubLoadoutFor(const QString &socName, EubLoadout &out, QString *error);
// 切段：任一 (offset+length) 越界 → false（**绝不截断**）；返回段数据与名字
bool splitSboot(const QByteArray &sboot, const EubLoadout &lo,
                QList<QPair<QString, QByteArray>> &out, QString *error);
```

**表（`offset` / `length`，十六进制；全部来自 §C3–§C6，测试逐值钉死）：**

| SoC | 段序列（名: offset/length） | 风格 | 证据 |
|---|---|---|---|
| **Exynos8890** | fwbl1 `0/0x2000` → el3_mon `0x2000/0x24000` → bl2 `0x26000/0x26D10` → bootloader `0x61000/0xD1000` | Zero | 单源+实战报告（`exynos-usbdl/scripts/split-sboot-8890.sh:2-5`；`exynos8890…/exynos-usbdl-recover.sh:87-93`） |
| **Exynos8895** | fwbl1 `0/0x2000` → bl31 `0x2000/0x28000` → bl2 `0x2A000/0x30000` → **fwbl1（重发）** `0/0x2000` → part5 `0x72000/0xD1000` → part6 `0x143000/0x80000` | Zero | 单源（`…/split-sboot-8895.sh:2-7`；后两段脚本未命名） |
| **Exynos7580** | fwbl1 `0/0x2000` → bl31 `0x2000/0x30000` → bl2 `0x32000/**0x8000**` → u-boot `0x3A000/0xD1000` | Dnw | 双源分歧（§C4：采信 `hubble/ExynosData/Exynos7580.json`；ananjaser 作 `0x7D10`） |
| **Exynos7885** | fwbl1 `0/0x2000` → bl31 `0x2000/0x25000` → bl2 `0x27000/0x2A000` → **fwbl1（重发）** `0/0x2000` → u-boot `0x61800/0xD1000` | Dnw | 单源（`hubble/ExynosData/Exynos7885.json`） |
| **Exynos9610** | fwbl1 `0/0x2000` → epbl `0x2000/0x13000` → bl2 `0x15000/0x2F000` → **fwbl1（重发）** `0/0x2000` → u-boot `0x5A000/0x180000` → el3_mon `0x1DA000/0x40000` | Dnw | **双源一致**（`hubble/…/Exynos9610.json:5-34` ↔ `exynos9610…/split_bootloader_a505.sh:1-6` + `dltool/dltool.c:311-319`；后者多一段 part6 `0x21A000/0x101000`，**不采纳**，注释记录） |
| **Exynos9810** | fwbl1 `0/0x2000` → bl31 `0x2000/0x13000` → bl2 `0x15000/0x4F000` → **fwbl1（重发）** `0/0x2000` → u-boot `0x7D000/0x180000` → el3_mon `0x1FD000/0x40000` | Dnw | 单源（`hubble/ExynosData/Exynos9810.json`） |
| **Exynos9820** | fwbl1 `0/0x3000` → epbl `0x3000/0x13000` → bl2 `0x16000/0x52000` → u-boot `0xA4000/0x180000` → el3_mon `0x224000/0x40000` | Dnw | 单源（`hubble/ExynosData/Exynos9820.json`）；`responseSupport=true` |
| **Exynos9830** | fwbl1 `0/0x3000` → epbl `0x3000/0x13000` → bl2 `0x16000/0x6C000` → lk `0xDB000/0x280000` → el3_mon `0x35B000/0x40000` | Dnw | 单源（`hubble/ExynosData/Exynos9830.json`）+ 论坛帖对 `el3_mon 0x35B000` 的部分佐证；`responseSupport=true`；`extraFiles={"ldfw.img","tzsw.img"}` |

> **表的可核对性**：`sourceNote` 必须能落到 `reference/` 里的 `file:line`；测试对每一行的每个数
> 做硬断言（改表必改测试，且测试里写明出处）。

### 5.5 会话层

```cpp
// src/core/eub/eub_session.h —— **已按 T6 落地实现同步**（2026-09-18）
struct EubOptions {
    int  segmentGapMs    = 1000;   // 段间基础等待（§B8：脚本用 sleep 1）
    int  revolveAttempts = 20;     // open 重试**次数**（总上限 ≈ 次数 × 间隔）
    int  revolvePollMs   = 500;    // 重试间隔
    bool readResponse    = true;   // responseSupport 表项：每段后读一次回显（§D7）
    std::function<void(int)> sleepFn;   // 注入用：空 = QThread::msleep（用例注入空实现 → 不等真实时间）
};
struct EubProgress { QString stage; QString detail; int percent = 0; };  // stage: identify/send/done
using EubProgressFn = std::function<void(const EubProgress &)>;

class EubSession {
public:
    explicit EubSession(IEubTransport &t, const EubOptions &opt = {}, EubProgressFn progress = {});
    // 打开 → 读自述 → 查表 → **关闭**（不持有句柄：设备可能瞬态消失，facts §A6）
    bool identify(EubLoadout &out, QString *error);
    // 切段（失败即中止、不写任何字节）→ 逐段 [open(带重试) → 发送 →（可选）读回显 → close]
    bool run(const EubLoadout &lo, const QByteArray &sboot, QString *error);
};
```

**与本节初稿的两处有意偏离**（T6 实施时定案，理由是让 UI 能"先预览再开跑"）：
① `identify()` 与 `run()` **分成两次调用**——UI 需要在发送前展示段表/sha1 对照并等用户确认
（初稿的单次 `run(EubRequest, …)` 无法在中间插入人工确认）；② 超时用**次数**（`revolveAttempts` ×
`revolvePollMs`）而非墙钟，`sleepFn` 可注入 → 用例确定、不等真实时间。载荷的 sha1 对照由 UI 层
在 `identify()` 之后自行计算与展示（`EubLoadout::sbootSha1` 对 `sha1Hex(bytes)`），故 `EubRequest`
结构不再需要。
```

---

## 6. 数据流（一次救援的完整时序）

```
[轮询] device_detector: VID 0x04E8 + PID 0x1234 → MODE_SAMSUNG_EUB（先于 Odin 判据，§D1）
   ↓ 用户点「EUB 救援…」
[UI] 选来源（sboot.bin / .lz4 / BL tar）→ 读成 bytes（§D8）
   ↓
[session] open() → readDeviceInfo() → socName
   ↓ eubLoadoutFor(socName)   — 无表 → 失败："该 SoC 无公开布局表，本工具不做猜测"（§F3）
   ↓ splitSboot(bytes, lo)    — 越界 → 失败（不截断）
   ↓ [UI] 段预览 + sha1 对照 + 勾选确认（§D6）
   ↓
[逐段] for seg in lo.segments:
        open()（失败 → waitForDevice() 轮询直到超时）
        sendSegment(seg.data, lo.style)
        if lo.responseSupport && opt.readResponse: readBulk(512, 50ms) → 原文落日志（§D7）
        close()
        sleep(segmentGapMs)
   ↓
[交接] done → UI："设备应已进入 Download 模式，请继续刷写"（§D1）；检测到 Download 设备时点亮提示（§D10）
```

---

## 7. 错误处理与失败语义

| 情形 | 行为 |
|---|---|
| 设备未识别/无表 | 失败，中文文案：SoC 名 + "该 SoC 无公开布局表"（列出支持的 8 个） |
| `sboot.bin` 短于表所需 | 失败："表需要 ≥ `0x…` 字节，你的文件是 `0x…`"（**绝不截断**） |
| SHA-1 与表不符 | **不阻断**，UI 展示对照 + 需勾选确认（§D6） |
| 某段写入失败 | 立即停止，报"第 N 段 `<名>`（`offset`/`length`）写入失败：<err>"；**不支持从中间续传**（引导链必须从第一段起，文案说明"请重新上电/重新进入 EUB 后从头再试"） |
| 段后设备未重现 | `waitForDevice` 超时 → 失败，文案含"设备可能已进入 Download 模式（请检查）或需要重新进入 EUB" |
| 读回显失败/为空 | **不判失败**（best-effort，只落日志） |
| 中途用户取消 | **本期不做**（T6 定案）：本流程只发 RAM 镜像、不发收尾命令，中途取消没有需要清理的设备侧状态；用户直接关闭进度窗口即可。若日后要做，入口是 `EubOptions` 加 `std::function<bool()> cancelled` 并在段间检查 |

---

## 8. 测试策略

**无真样本**（§F1：sboot 是三星二进制，不进仓库）——本线**如实标注为 mock + 数值断言**，
不冒充"真样本验证"。合成样本：确定性图案 + 已知位置写入 ASCII `EXYNOS9610`（测 §A4 的退化识别路径）。

| 目标 | 覆盖 |
|---|---|
| `test_eub_protocol.cpp` | 帧字节逐字节（两种风格各一）；长度字段 = `n+10`；空载荷 → 失败；大载荷（>2 MiB）；`sendSegment` 的写序列（次数字节）|
| `test_eub_loadout.cpp` | **8 张表逐值硬断言**（含 7580 采信值与注释、8895/9610 的重发段、9830 的 `extraFiles`）；未知 SoC → false；越界 → false 且**不产出半段**；SHA-1 计算；SoC 名大小写不敏感 |
| `test_eub_session.cpp` | mock transport 记录 `open/close/write/read` 全序列：N 段 → N 次重开；段间等待；重枚举（注入"设备先消失后出现"）；每类失败路径（第 2 段写失败 → 只发 2 段 + 错误文案含段名）；超时路径 |
| `test_eub_transport.cpp`（新目标，链接 `eub_libusb_transport.cpp` + `odin_libusb_transport.cpp`） | `isEubDevice(0x04E8, 0x1234)` 命中、其它 PID 不命中；**`samsungModeFor` 的优先级**：`(0x04E8, 0x1234, {0x0A}, true)` → `Eub`（**即使 Odin 判据也会命中**，§E2 的回归钉）、老 PID `0x6601` → `Odin`、非三星 VID → `NotSamsung` |

---

## 9. 交付清单

1. 传输层 + 检测：`eub_transport.h`/`eub_libusb_transport.{h,cpp}`、`samsung_mode.{h,cpp}`、`isEubDevice`、`MODE_SAMSUNG_EUB`，检测层改走 `samsungModeFor`（**认领顺序由纯函数+测试固化**）
2. 协议层：`eub_protocol.{h,cpp}`（帧构造 + `sendSegment`）+ 测试
3. 布局层：`eub_loadout.{h,cpp}`（8 张表 + 查表 + 切段 + SHA-1）+ 测试
4. 会话层：`eub_session.{h,cpp}`（逐段重开/等待/进度/失败语义）+ 测试
5. 载荷获取：`sboot.bin` / `.lz4` / `BL_*.tar.md5` 提取（复用 `imgtar::indexTarStream` + `lz4Decompress`）
6. UI：`eub_recovery_dialog.{h,cpp}` + FlashPanel 三星模式入口 + 交接提示（§D9/§D10）
7. 文案/文档/清单：`功能清单.txt`（**队列项更正**：展锐 EUB → 三星 Exynos EUB §A0）、README（模式表 + 测试目标计数）、本 spec 与事实报告的终稿
8. 终验：全新构建 0 警告（**全新目录**）+ 全量 ctest + 各新目标 0 skipped 的如实记录

---

## 10. 待评审决策（需要你拍板）

| # | 议题 | 我的建议 |
|---|---|---|
| Q1 | 表覆盖面：8 个 SoC（含 4 个单源）还是只做双源 3 个（8890/7580/9610） | **8 个全做**，逐张标证据等级 + UI 展示来源 |
| Q2 | 帧头/尾两字段按来源分派（Zero/Dnw 两风格） | 采纳（§D2）；若你认为该统一成一种，请指定哪一种 |
| Q3 | 7580 `bl2` 长度采信 hubble `0x8000`（连续切法） | 采纳；ananjaser 的 `0x7D10` 写进注释与 UI |
| Q4 | SHA-1 不符：警告+勾选（不硬拒） | 采纳（§D6，沿用既有"未验证勾选门控"惯例） |
| Q5 | 回显（9820/9830）：只读一次原样落日志，不解析崩溃结构 | 采纳（§D7） |
| Q6 | UI 入口：FlashPanel 三星模式按钮（不新增独立面板） | 采纳（§D9） |
| Q7 | 5 个参照仓库已放入 gitignored 的 `reference/`（exynos-usbdl ×2 / 8890 救援 / 9610 救援 / hubble） | 保留（便于计划与审查按 `file:line` 引用） |

---

## 11. 诚实边界（不得被文案或文档说成"已验证"）

1. **真机未验证**：本机无 Exynos 设备；全部实现只能到 mock + 数值断言（§8）。
2. **两字段语义未定**（§B4/§B5/§B6）：我们照抄参照实现，不声称理解。
3. **单源表**（§C6）+ **7580 分歧未决**（§C4）：证据标签必须随表展示，不得藏在代码里。
4. **布局绑固件修订**（§C9）：sha1 对照是提示，不是保证。
5. **前置条件在用户侧**：设备须已回退到 EUB 且未被 eFuse 封堵（§A5/§A7）。
6. **不改写存储**：本流程只发 RAM 镜像；结束后设备**仍需正常刷写**（§D1/§F5）。
7. **不使用漏洞利用**：只走签名 bootloader 的正常下载路径（§F7）。
