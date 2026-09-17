# 三星 Exynos EUB 救援链 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 PhoneToolbox 能认出处于 EUB（Exynos USB Boot）态的三星设备，用用户自备的原厂 `sboot.bin`
按公开的每 SoC 布局表分段注入设备 RAM，把设备引导进 Download 模式，随后交给已交付的 Odin 链刷写。

**Architecture:** 六层，依赖方向单向向下，UI 只依赖会话与布局层：
`eub_libusb_transport`（唯一 libusb 依赖点）→ `IEubTransport`（纯字节管道）→ `eub_session`（编排）
+ `eub_protocol`（帧，纯函数）+ `eub_loadout`（表/切段，纯函数）+ `eub_payload`（载荷来源解析）；
`samsung_mode` 是检测层调用的三态认领纯函数。

**Tech Stack:** C++17、Qt6（Core/Gui/Widgets/Test）、CMake+Ninja、libusb-1.0、
复用既有 `imgtar::indexTarStream`（`src/image_engine/tar_image.h`）与 `imgcomp::lz4Decompress`
（`src/image_engine/compression/lz4_wrapper.h`，**LZ4 frame 格式**，与三星 `.lz4` 一致）。

**事实来源：** `docs/superpowers/specs/exynos-eub-facts.md`（下称 **facts**，条目号 `§A1` 等）
与设计 spec `docs/superpowers/specs/2026-09-17-exynos-eub-design.md`（下称 **spec**）。
参照实现（全部 gitignored、只读，**只引用不拷贝代码**）在 `reference/`：`exynos-usbdl/`、
`exynos-usbdl-vdavid003/`、`exynos8890-exynos-usbdl-recovery/`、`exynos9610-usb-emergency-recovery/`、`hubble/`。

---

## Global Constraints

以下为**全局约束**，每个任务的要求都隐含包含本节：

1. **帧格式（逐字节，不得改动）**：`[4 字节头字段][u32 小端 = 数据长 + 10][数据 n 字节][2 字节尾]`（facts §B3）。
   - `ZeroStyle` 头 = `00 00 00 00`、尾 = `00 00`（exynos-usbdl 路径，facts §B4/§B5）。
   - `DnwStyle` 头 = `1B 44 4E 57`（ASCII `ESC`+`DNW`）、尾 = `FF FF`（hubble 路径，facts §B4/§B5）。
   - 头/尾两字段的**语义未定**（三实现互相矛盾，facts §B6）：注释与 UI 必须写"照抄自哪个实现"，
     **不得**声称理解其含义。
2. **8 张布局表的数值以 spec §5.4 为唯一来源**，逐值硬断言进测试；每条 `sourceNote` 必须是
   `reference/` 里可核对的 `file:line`。**不得**新增未在 spec §5.4 里出现的表或改动任何数值——
   若要改（例如发现新证据），先回来改 spec 再改代码。
3. **不做清单**（spec §2）：未签名代码加载、签名绕过、eFuse、内置或分发任何三星签名二进制、
   写设备存储（本流程只发 RAM 镜像）、Multidownloader cfg 解析、无表 SoC 猜偏移。
4. **无真机**：本机没有 Exynos 设备。任何实现、注释、文档、提交信息**不得**写"已验证/已实测"；
   证据最高只能到"mock + 数值断言"。真机行为留给持机人。
5. **注释密度与出处**：本仓的风格是"每个非显然决定都带出处"。新代码里凡是取值/顺序/判据来自
   事实报告的，注释必须写 `facts §X#` 或 `reference/<repo>/<file>:<line>`。
6. **构建与验证纪律**：`/tmp` 在本机是满的 → 全新构建目录放 `/home`；"0 警告"必须用
   **全新目录**核（增量构建不重放警告）；`ctest` 必须 100% 通过；本线**没有真样本**，
   测试目标报告必须如实说明（不得写成"真样本验证"）。
   ⚠️ **相位级判据修正（T5 审查发现）**：本仓 CMake **不开任何警告开关** —— 实测编译 flags 只有
   `-std=gnu++17 -mno-direct-extern-access`，`grep -c -- "-Wall\|-Wextra" build.ninja` = 0。
   故**裸构建的"0 警告"没有甄别力**（对编译器没被要求发出的警告沉默）。合格跑法 = 在全新构建配置上
   显式追加 `-DCMAKE_CXX_FLAGS="-Wall -Wextra -Wsign-compare"`，并把日志留成产物。
   （控制方已对 T1–T5 的 19 个 TU 补做审计：本仓来源诊断 **0** 条，日志 `/home/DslsDZC/eub-warn-audit.log`。）
7. **提交纪律**：只 `git add` 明确路径，**绝不** `git add -A/-u/commit -a`（`build/` 有 4 个历史
   跟踪文件；`build-h1/`、`build-release/` 是既存未跟踪产物，别碰）。提交信息写清"改了什么/为什么/边界"。
8. **UI 文案中文**，错误信息给出可行动的信息（哪一段、多少字节、下一步怎么做）。
9. **brief 里的用例是起点，不是不可质疑的字面**：每个任务的测试槽都经过控制方复核，但**恒真断言**
   （缺陷存在时仍会通过）是本仓出现过两次的缺陷类型（T1 的 M1 加固即一例：旧顺序下
   `!writeBulk({}) && !err.isEmpty()` 同样为真）。若你在实现中发现某条断言**无法区分"实现了"与"没实现"**，
   按 T1 先例**加固它**（加一条只在正确实现下才成立的断言），并在报告里写明"原断言恒真 + 加固方式 +
   变异证据（改回错误实现 → 该 slot 变红）"。**不要**默默照抄，也**不要**改弱或删除既有断言。
   加固/新增 slot 后，该任务 Step 4 的 `Totals:` 期望值会同步变大 —— **以实际通过为准**，报告里写明
   实际数与差值原因（T1/T2/T3 三次实现都因加固而增了 slot；期望值是起点不是判据）。

---

### Task 1: EUB 传输层（接口 + libusb 实现 + 判据纯函数）

**Files:**
- Create: `src/core/eub/eub_transport.h`
- Create: `src/core/eub/eub_libusb_transport.h`
- Create: `src/core/eub/eub_libusb_transport.cpp`
- Test: `tests/test_eub_transport.cpp`
- Modify: `CMakeLists.txt`（测试源清单 + 该目标 extra sources + libusb 链）

**Interfaces:**
- Consumes: 无（本任务是最底层）
- Produces:
  - `eub::EubDeviceInfo{ QString socName, socId, chipId, usbBootVersion; quint16 vid, pid; quint8 bus, address; }`
  - `eub::IEubTransport`（`open/close/readDeviceInfo/writeBulk/readBulk/notes`）
  - `eub::LibusbEubTransport`（真机实现）+ 纯静态 `isEubDevice(quint16 vid, quint16 pid)`、
    `noDeviceError()`、`effectiveTimeoutMs(int)`；常量 `kWriteTimeoutMs = 50000`、`kReadTimeoutMs = 50`

- [ ] **Step 1: 写失败测试**

创建 `tests/test_eub_transport.cpp`：

```cpp
// tests/test_eub_transport.cpp
//
// EUB 传输层的**离线可测面**：只有纯函数（设备判据、超时换算、文案）。
// 真机路径（open/描述符解析/claim/bulk 读写）本机无设备，**未验证**（facts §F1）。
#include <QtTest>
#include "core/eub/eub_libusb_transport.h"

using eub::LibusbEubTransport;

class TestEubTransport : public QObject
{
    Q_OBJECT

private slots:
    // facts §A1：VID 0x04E8 / PID 0x1234，全 SoC 一致
    void isEubDeviceMatchesExactIds()
    {
        QVERIFY(LibusbEubTransport::isEubDevice(0x04E8, 0x1234));
    }

    void isEubDeviceRejectsNeighbours()
    {
        QVERIFY(!LibusbEubTransport::isEubDevice(0x04E8, 0x1233));
        // Heimdall 的三个老 Odin PID 是**下载模式**，不是 EUB（facts §E2 的邻接风险）
        QVERIFY(!LibusbEubTransport::isEubDevice(0x04E8, 0x6601));
        QVERIFY(!LibusbEubTransport::isEubDevice(0x04E8, 0x685D));
    }

    void isEubDeviceRejectsOtherVendors()
    {
        QVERIFY(!LibusbEubTransport::isEubDevice(0x18D1, 0x1234));  // Google VID、同 PID
        QVERIFY(!LibusbEubTransport::isEubDevice(0x0E8D, 0x0003));  // MTK BROM
    }

    void effectiveTimeoutNeverZero()   // libusb 的 0 = 无限等待，见 odin 同款红线
    {
        QCOMPARE(LibusbEubTransport::effectiveTimeoutMs(0), 1);
        QCOMPARE(LibusbEubTransport::effectiveTimeoutMs(-7), 1);
        QCOMPARE(LibusbEubTransport::effectiveTimeoutMs(50), 50);
    }

    void noDeviceErrorMentionsIds()
    {
        const QString msg = LibusbEubTransport::noDeviceError();
        QVERIFY(msg.contains(QStringLiteral("04e8")) || msg.contains(QStringLiteral("0x04e8")));
        QVERIFY(msg.contains(QStringLiteral("1234")));
    }
};

QTEST_APPLESS_MAIN(TestEubTransport)
#include "test_eub_transport.moc"
```

- [ ] **Step 2: 跑测试确认 RED**

Run: `cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_eub_transport`
Expected: 编译/链接失败（`eub_libusb_transport.h` 不存在）

- [ ] **Step 3: 实现**

`src/core/eub/eub_transport.h`：

```cpp
// src/core/eub/eub_transport.h
//
// EUB 传输抽象：设备自述信息 + 纯字节管道（唯一设备依赖点是它的 libusb 实现）。
// 语义与措辞对齐 src/core/odin/odin_transport.h（同款分层：会话只经本接口碰设备）。
//
// 与 Odin 传输的三处差异：
//   ① 多一个 readDeviceInfo：EUB 设备用字符串描述符自报 SoC 名 / SoC ID / Chip ID /
//      USB Booting Version（facts §A2/§A3），会话靠它选布局表；
//   ② 没有 ZLP 语义：EUB 的下载是一段一帧、发完即走（facts §B7），不存在"空写表示结束"；
//   ③ open() 会**重新枚举并打开**设备 —— EUB 设备在段间可能重枚举（facts §B8/§B9），
//      所以每次 open 都是全新查找，"打开后再打开"必须先 close（实现里这么做）。
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

namespace eub {

// 设备自述信息（打开后读一次；读不到的字段留空 —— 老 SoC 可能没有某些串，facts §A4）
struct EubDeviceInfo {
    QString socName;         // iProduct，如 "Exynos9610"（facts §A2）
    QString socId;           // iSerialNumber[0:15]（facts §A3）
    QString chipId;          // iSerialNumber[15:31]（facts §A3）
    QString usbBootVersion;  // iInterface[12:16]（facts §A3）
    quint16 vid = 0;
    quint16 pid = 0;
    quint8  bus = 0;
    quint8  address = 0;
};

class IEubTransport
{
public:
    virtual ~IEubTransport() = default;

    // 查找并打开 EUB 设备（VID/PID，facts §A1）；close 后可再 open。
    // **不锁 bus/addr**：段间设备会重枚举，地址会变（facts §B8）。
    virtual bool open(QString *error) = 0;
    virtual void close() = 0;                       // 幂等；未打开时 no-op
    virtual bool readDeviceInfo(EubDeviceInfo &out, QString *error) = 0;
    // 一次写整帧（facts §B7：hubble/dltool 都是整帧一次写；libusb 内部按包长切分）。
    virtual bool writeBulk(const QByteArray &data, QString *error) = 0;
    // 读回显（仅 responseSupport 的表项会调，facts §C7/§C8）；超时/无数据 → 空返回 + error。
    virtual QByteArray readBulk(int maxBytes, int timeoutMs, QString *error) = 0;

    // 打开过程中的非致命说明（如端点回退），会话把它转发到日志。默认空。
    virtual QStringList notes() const { return {}; }
};

} // namespace eub
```

`src/core/eub/eub_libusb_transport.h`：

```cpp
// src/core/eub/eub_libusb_transport.h
//
// IEubTransport 的 libusb 实现。**本仓 EUB 侧唯一的 libusb 依赖点**（依赖缝，eub_transport.h 头注释）。
//
// 端点策略（与参照的差异，写在明处）：三个参照实现都**硬编码** OUT=0x02、IN=0x81
// （exynos-usbdl.c:60 与 :242；dltool.c:339；hubble.py:110,115 —— facts §B2）。我们优先从
// 描述符解析（优先 interface 0，否则第一个带批量对的接口），**解析不到才回退这对常数**并在
// notes() 里记一笔 —— 因为"硬编码的值在别的机型上是否成立"我们无法离线验证。
//
// 配置态：设备可能尚未被配置（bConfigurationValue==0）—— dltool 显式 set_configuration(1)
// （dltool.c:305）。**libusb 官方文档**：设备已处于目标配置时再调用 = 一次**轻量复位**
// （重发 SET_CONFIGURATION，altsetting/端点 halt/toggle 归零）；而 `LIBUSB_ERROR_BUSY`
// 的语义是"**接口已被认领**"，且"认领之后不能再改配置"。故补设必须发生在 claim **之前**，
// 且只在"读不到活动配置"时才补设（否则平白复位一次设备状态）。
// （初稿注释把 BUSY 误写成"已配置"——T1 实现者按官方文档纠正，见 eub-task-1-report.md 疑虑 1。）
//
// 超时：写 50 s（hubble `timeout=50000` hubble.py:110；dltool 50*1000*1000 µs dltool.c:339
// —— 两家一致）；读 50 ms（hubble.py:115 的 `timeout=50`，仅用于段后回显）。
// libusb 的 timeout=0 = **无限等待**，故经 effectiveTimeoutMs() 换算（<=0 → 1 ms）。
#pragma once
#include <QStringList>

#include "eub_transport.h"

struct libusb_context;
struct libusb_device_handle;

namespace eub {

class LibusbEubTransport : public IEubTransport
{
public:
    LibusbEubTransport();
    ~LibusbEubTransport() override;

    // ---- 纯函数（离线可测）----
    // facts §A1：VID 0x04E8 且 PID 0x1234（全 SoC 一致，不按 SoC 区分）
    static bool isEubDevice(quint16 vid, quint16 pid);
    // 找不到设备时的中文文案（含 VID/PID 与**现实前提**：进不去 EUB / eFuse 已封）
    static QString noDeviceError();
    // libusb 的 0 = 无限等待（sync.c），本接口的 0 语义是"立即返回" —— 换算成 1 ms，
    // **绝不把 0 交给 libusb**（odin_libusb_transport.h:73-76 同款）。公开仅为单测。
    static int effectiveTimeoutMs(int requested);

    // ---- IEubTransport ----
    bool open(QString *error) override;
    void close() override;
    bool readDeviceInfo(EubDeviceInfo &out, QString *error) override;
    bool writeBulk(const QByteArray &data, QString *error) override;
    QByteArray readBulk(int maxBytes, int timeoutMs, QString *error) override;
    QStringList notes() const override { return m_notes; }

    static constexpr int kWriteTimeoutMs = 50000;   // hubble.py:110 / dltool.c:339
    static constexpr int kReadTimeoutMs  = 50;      // hubble.py:115（回显只读一次）

private:
    bool claimInterface(QString *error);

    libusb_context       *m_ctx = nullptr;
    libusb_device_handle *m_dev = nullptr;
    int m_iface = -1;
    int m_epOut = 0;
    int m_epIn  = 0;
    int m_bus = 0;
    int m_address = 0;
    quint16 m_vid = 0;
    quint16 m_pid = 0;
    QStringList m_notes;
};

} // namespace eub
```

`src/core/eub/eub_libusb_transport.cpp`：结构照抄 `src/core/odin/odin_libusb_transport.cpp`
（open 的枚举/打开/认领骨架、claim→detach→claim、write/read 的短写与超时判定逐条对齐），
差异为本任务的头文件注释所列三处。要点：

```cpp
namespace {
constexpr quint16 kEubVid = 0x04e8;   // facts §A1
constexpr quint16 kEubPid = 0x1234;   // facts §A1
// ⚠️ 出处必须写全仓库段：reference/ 下有两个同名 exynos-usbdl.c（原版与 VDavid003 fork），
// 短写会解析到错的那份（T1 审查 M6 的教训）
constexpr int kFallbackEpOut = 0x02;  // reference/exynos-usbdl/exynos-usbdl.c:60 / reference/exynos9610-usb-emergency-recovery/dltool/dltool.c:339 / reference/hubble/hubble.py:110
constexpr int kFallbackEpIn  = 0x81;  // reference/exynos-usbdl/exynos-usbdl.c:242 / reference/hubble/hubble.py:115

QString usbErr(int rc) { return QString::fromLatin1(libusb_error_name(rc)); }

// 字符串描述符（失败/为空 → 空串，不置 error：老 SoC 可能没有这些串，facts §A4）
QString readString(libusb_device_handle *dev, quint8 index);

struct Endpoints { int iface = -1; int epOut = 0; int epIn = 0; bool fromDescriptor = false; };
// 优先 interface 0 的批量对；否则第一个带批量对的接口；都没有 → fromDescriptor=false
Endpoints resolveEndpoints(libusb_device *dev);
} // namespace
```

`readDeviceInfo` 的字段映射（照抄 facts §A2/§A3，不得自创）：

```cpp
    out.socName        = readString(m_dev, desc.iProduct);          // facts §A2
    const QString serial = readString(m_dev, desc.iSerialNumber);   // facts §A3
    out.socId  = serial.left(15);       // hubble.py:219 的 [0:15]
    out.chipId = serial.mid(15, 16);    // hubble.py:220 的 [15:31]
    // 接口串（USB Booting Version）：活动配置的 interface[0].altsetting[0].iInterface，
    // 取 [12:16]（hubble.py:211,223）
    out.usbBootVersion = ifStr.mid(12, 4);
```

CMakeLists 改动（三处，按现有模式）：
1. `IMAGE_TEST_SOURCES` 清单里加 `${CMAKE_CURRENT_SOURCE_DIR}/tests/test_eub_transport.cpp`；
2. `_test_stem STREQUAL "test_eub_transport"` 的 extra sources 分支：
   `src/core/eub/eub_libusb_transport.cpp`（本任务只需这一个源；`samsung_mode.cpp` 在 Task 2 追加）；
3. 需要 libusb 的 stem 名列表（`if(_test_stem STREQUAL "test_mtk_brom" OR ...)` 那一段）里加
   `OR _test_stem STREQUAL "test_eub_transport"`。
   ⚠️ 改完 CMakeLists **必须先重配**（`cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON`）再 build，
   否则链接边是旧图（本仓踩过：症状是链接期 undefined symbol，见 `.superpowers/sdd/progress.md`）。

- [ ] **Step 4: 跑测试确认 GREEN**

Run: `cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_eub_transport && ./build/image_engine_tests_test_eub_transport`
Expected: `Totals: 7 passed, 0 failed, 0 skipped`（QtTest 的 Totals = 用例 slots(5) + initTestCase/cleanupTestCase(2)；仓内既有目标同款：`test_mtk_gpt` 声明 6 slots → Totals 8）

- [ ] **Step 5: 提交**

```bash
git add src/core/eub/eub_transport.h src/core/eub/eub_libusb_transport.h src/core/eub/eub_libusb_transport.cpp tests/test_eub_transport.cpp CMakeLists.txt
git commit -m "feat(eub): EUB 传输层（接口 + libusb 实现 + 设备判据纯函数）"
```

---

### Task 2: 三态认领纯函数 + 检测层接入

**Files:**
- Create: `src/core/eub/samsung_mode.h`
- Create: `src/core/eub/samsung_mode.cpp`
- Test: `tests/test_eub_samsung_mode.cpp`
- Modify: `src/core/device_detector.h`（枚举 +1）
- Modify: `src/core/device_detector.cpp`（0x04E8 分支改走 pure 函数 + 显示名）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `eub::LibusbEubTransport::isEubDevice`（Task 1）、`odin::LibusbOdinTransport::isOdinDevice`
- Produces: `eub::SamsungMode{ NotSamsung, Eub, Odin }`、`eub::samsungModeFor(vid, pid, interfaceClasses, hasBulkInOut)`、
  `DeviceDetector::MODE_SAMSUNG_EUB = 11`

- [ ] **Step 1: 写失败测试**

创建 `tests/test_eub_samsung_mode.cpp`（**认领顺序**是本任务的核心回归点，facts §E2）：

```cpp
#include <QtTest>
#include "core/eub/samsung_mode.h"

class TestEubSamsungMode : public QObject
{
    Q_OBJECT

private slots:
    // facts §E2：PID 0x1234 不在 Odin 兜底表里，但若接口是 0x0A 类 + 批量 in/out，
    // isOdinDevice 也会命中 —— 顺序写反就会把 EUB 设备当成 Download 模式设备。
    void eubWinsOverOdinWhenBothPredicatesMatch()
    {
        QCOMPARE(eub::samsungModeFor(0x04E8, 0x1234, {0x0A}, true), eub::SamsungMode::Eub);
    }

    void eubWinsWithoutAnyDescriptorHints()
    {
        QCOMPARE(eub::samsungModeFor(0x04E8, 0x1234, {}, false), eub::SamsungMode::Eub);
    }

    void odinUnchangedForCdcDevices()
    {
        QCOMPARE(eub::samsungModeFor(0x04E8, 0x6860, {0x0A, 0x06}, true), eub::SamsungMode::Odin);
    }

    void odinUnchangedForLegacyPids()
    {
        QCOMPARE(eub::samsungModeFor(0x04E8, 0x6601, {}, false), eub::SamsungMode::Odin);
    }

    void nonSamsungIsNotClaimed()
    {
        QCOMPARE(eub::samsungModeFor(0x18D1, 0x4EE0, {0x0A}, true), eub::SamsungMode::NotSamsung);
        QCOMPARE(eub::samsungModeFor(0x0E8D, 0x0003, {}, false), eub::SamsungMode::NotSamsung);
    }

    void samsungVidWithoutOdinShapeIsNotClaimed()
    {
        // 正常开机/充电的三星手机：VID 0x04E8 但接口是 MTP(0x06)/ADB(0xFF)，无 CDC_DATA
        // （odin_libusb_transport.h:19-20 的同款理由）
        QCOMPARE(eub::samsungModeFor(0x04E8, 0x6860, {0x06, 0xFF}, true),
                 eub::SamsungMode::NotSamsung);
    }
};

QTEST_APPLESS_MAIN(TestEubSamsungMode)
#include "test_eub_samsung_mode.moc"
```

- [ ] **Step 2: 跑测试确认 RED**

Run: `cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_eub_samsung_mode`
Expected: 失败（`core/eub/samsung_mode.h` 不存在）

- [ ] **Step 3: 实现**

`src/core/eub/samsung_mode.h`：

```cpp
// src/core/eub/samsung_mode.h
//
// 三星设备的三态认领：**认领顺序在这里固化**，检测层只消费结论。
//
// 为什么值得单独抽一层：EUB 的 PID 0x1234 不在 Odin 的兜底 PID 表
// （0x6601/0x685D/0x68C3，odin_libusb_transport.cpp:108-112）里，但它同样满足 Odin 的
// "接口类 0x0A + 批量 in/out" 判据 —— 顺序写反就会把 EUB 设备认成 Download 模式设备，
// 用户会被引到一条注定失败的刷写链上（facts §E2）。抽成纯函数后这条优先级被单测钉死。
#pragma once
#include <QList>
#include <QString>

namespace eub {

enum class SamsungMode { NotSamsung, Eub, Odin };

// 纯函数：不碰 libusb。参数与 odin::LibusbOdinTransport::isOdinDevice 同形
// （interfaceClasses = 全部接口的 bInterfaceClass 扁平表；hasBulkInOut = 设备级事实）。
SamsungMode samsungModeFor(quint16 vid, quint16 pid,
                           const QList<quint8> &interfaceClasses, bool hasBulkInOut);

} // namespace eub
```

`src/core/eub/samsung_mode.cpp`：

```cpp
#include "samsung_mode.h"

#include "core/eub/eub_libusb_transport.h"
#include "core/odin/odin_libusb_transport.h"

namespace eub {

SamsungMode samsungModeFor(quint16 vid, quint16 pid,
                           const QList<quint8> &interfaceClasses, bool hasBulkInOut)
{
    // 顺序即契约：EUB 先于 Odin（facts §E2）。两判据互不蕴含。
    if (LibusbEubTransport::isEubDevice(vid, pid))
        return SamsungMode::Eub;
    if (odin::LibusbOdinTransport::isOdinDevice(vid, pid, interfaceClasses, hasBulkInOut))
        return SamsungMode::Odin;
    return SamsungMode::NotSamsung;
}

} // namespace eub
```

`src/core/device_detector.h` 枚举末尾加一项（保持显式编号）：

```cpp
        MODE_SAMSUNG_ODIN = 10,     // 三星 Odin 下载模式（Phase C）
        MODE_SAMSUNG_EUB = 11       // 三星 Exynos EUB（USB-Boot）救援模式
```

`src/core/device_detector.cpp`：
1. 头部 include 区加 `#include "core/eub/samsung_mode.h"`；
2. `getModeDisplayName()` 的 switch 里 `MODE_SAMSUNG_ODIN` 分支后加：
   `case MODE_SAMSUNG_EUB: return QStringLiteral("三星 EUB (Exynos)");`
3. `detectProtocolDevices()` 的 `0x04E8` 分支：把 `isOdinDevice(...)` 那一次调用与随后的
   认领/emit 段改为按 `samsungModeFor` 的三态分派（**保留**原有注释里"只对三星 VID 深挖描述符"
   与"老 PID 兜底不看描述符"的说明，并补一句指向 samsung_mode.h 的认领顺序）。现有代码形如：

```cpp
            if (odin::LibusbOdinTransport::isOdinDevice(desc.idVendor, desc.idProduct,
                                                        classes, hasBulkInOut)) {
                const QString devId = ...;
                DeviceInfo info;
                info.serialNumber = devId;
                info.mode = MODE_SAMSUNG_ODIN;
                ...
                continue;
            }
```

改为：

```cpp
            const eub::SamsungMode sm =
                eub::samsungModeFor(desc.idVendor, desc.idProduct, classes, hasBulkInOut);
            if (sm != eub::SamsungMode::NotSamsung) {
                // 认领顺序（EUB 先于 Odin）由 samsungModeFor 固化并单测覆盖（samsung_mode.h 头注释）
                const DeviceMode claimed = (sm == eub::SamsungMode::Eub) ? MODE_SAMSUNG_EUB
                                                                        : MODE_SAMSUNG_ODIN;
                const QString devId = QStringLiteral("usb-%1-%2")
                                          .arg(libusb_get_bus_number(list[i]))
                                          .arg(libusb_get_device_address(list[i]));
                DeviceInfo info;
                info.serialNumber = devId;
                info.mode = claimed;
                info.model = getModeDisplayName(claimed);
                newDevices[devId] = info;
                if (!m_currentDevices.contains(devId)) {
                    qDebug() << "Samsung protocol device connected:" << devId;
                    emit deviceConnected(info);
                } else if (m_currentDevices[devId].mode != claimed) {
                    emit deviceModeChanged(devId, claimed);
                    qDebug() << "Samsung protocol device mode changed:" << devId
                             << "to" << getModeDisplayName(claimed);
                }
                continue;                       // 已认领，不再走下面的 PID 表
            }
```

CMakeLists：`test_eub_samsung_mode` 加进测试源清单；其 extra sources 分支列
`src/core/eub/samsung_mode.cpp` + `src/core/eub/eub_libusb_transport.cpp` +
`src/core/odin/odin_libusb_transport.cpp`；并把该 stem 加进需要 libusb 的列表
（`samsung_mode.cpp` 静态调用 `isOdinDevice`，而 `odin_libusb_transport.cpp` 里有 libusb 调用）。

- [ ] **Step 4: 跑测试确认 GREEN**

Run: `cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build && ./build/image_engine_tests_test_eub_samsung_mode && ./build/image_engine_tests_test_eub_transport`
Expected: 两个目标各自 `Totals: 8 passed, 0 failed`（本任务 slots 6 + 2）/ `8 passed, 0 failed`（T1 目标经 T1 修复后为 6 slots + 2）；全项目 build 通过。

- [ ] **Step 5: 提交**

```bash
git add src/core/eub/samsung_mode.h src/core/eub/samsung_mode.cpp tests/test_eub_samsung_mode.cpp src/core/device_detector.h src/core/device_detector.cpp CMakeLists.txt
git commit -m "feat(eub): 三星三态认领纯函数（EUB 优先于 Odin）+ 检测层接入"
```

---

### Task 3: 帧构造与段发送（eub_protocol）

**Files:**
- Create: `src/core/eub/eub_protocol.h`
- Create: `src/core/eub/eub_protocol.cpp`
- Create: `tests/mock_eub_transport.h`（写入记录型 mock，后续 Task 6/7 复用）
- Test: `tests/test_eub_protocol.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `eub::IEubTransport`（Task 1）
- Produces: `eub::EubFrameStyle{ QByteArray header; QByteArray trailer; }`、`eub::zeroStyle()`、
  `eub::dnwStyle()`、`eub::kFrameOverhead = 10`、`eub::buildEubFrame(payload, style, QString*)`、
  `eub::sendSegment(t, payload, style, QString*)`

- [ ] **Step 1: 写 mock 与失败测试**

创建 `tests/mock_eub_transport.h`：

```cpp
// tests/mock_eub_transport.h
//
// 脚本化 EUB 传输：写入全量记录、调用顺序保留、open 可注入连续失败
// （模拟"设备还没出现"与"段间重枚举"两种时序，facts §B8/§B9）。
// 本线无真机（facts §F1），断言手段 = **写出的帧逐字节比对**。
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include "core/eub/eub_transport.h"

namespace eub {

class MockEubTransport : public IEubTransport
{
public:
    QList<QByteArray> writes;      // 每次 writeBulk 追加（一帧一条）
    QStringList       calls;       // "open"/"close"/"info"/"write"/"read"
    int  openFailures = 0;         // 前 N 次 open 失败（0 = 一次就成功）
    int  failWriteAt = -1;         // 第 N 次 write 失败（0 基），-1 = 不失败
    bool infoResult = true;
    EubDeviceInfo info;            // readDeviceInfo 返回内容
    QByteArray    response;        // readBulk 返回内容
    QStringList   noteList;        // notes() 返回内容

    bool open(QString *error) override {
        calls << QStringLiteral("open");
        if (openFailures > 0) {
            --openFailures;
            if (error) *error = QStringLiteral("注入的打开失败");
            return false;
        }
        return true;
    }
    void close() override { calls << QStringLiteral("close"); }
    bool readDeviceInfo(EubDeviceInfo &out, QString *error) override {
        calls << QStringLiteral("info");
        if (!infoResult) { if (error) *error = QStringLiteral("注入的信息读取失败"); return false; }
        out = info;
        return true;
    }
    bool writeBulk(const QByteArray &data, QString *error) override {
        if (failWriteAt >= 0 && writes.size() == failWriteAt) {
            if (error) *error = QStringLiteral("注入的写失败");
            return false;
        }
        calls << QStringLiteral("write");
        writes.append(data);
        return true;
    }
    QByteArray readBulk(int maxBytes, int timeoutMs, QString *error) override {
        Q_UNUSED(maxBytes) Q_UNUSED(timeoutMs) Q_UNUSED(error)
        calls << QStringLiteral("read");
        return response;
    }
    QStringList notes() const override { return noteList; }
};

} // namespace eub
```

创建 `tests/test_eub_protocol.cpp`：

```cpp
// tests/test_eub_protocol.cpp
//
// 帧格式的**逐字节**断言。数值全部来自 facts §B3/§B4/§B5：
//   [4B 头字段][u32 LE = 数据长 + 10][数据][2B 尾]
//   ZeroStyle 头 00 00 00 00 / 尾 00 00（exynos-usbdl）；DnwStyle 头 1B 44 4E 57 / 尾 FF FF（hubble）
#include <QtTest>

#include "core/eub/eub_protocol.h"
#include "mock_eub_transport.h"

class TestEubProtocol : public QObject
{
    Q_OBJECT

private slots:
    void zeroStyleFrameBytes()
    {
        QString err;
        const QByteArray f = eub::buildEubFrame(QByteArray("AB"), eub::zeroStyle(), &err);
        QVERIFY2(!f.isEmpty(), qPrintable(err));
        QCOMPARE(f.size(), 12);                                   // 4 + 4 + 2 + 2
        QCOMPARE(f.left(4), QByteArray::fromHex("00000000"));     // facts §B4 的 exynos-usbdl 值
        QCOMPARE(f.mid(4, 4), QByteArray::fromHex("0c000000"));   // 12 = n(2) + 10，小端
        QCOMPARE(f.mid(8, 2), QByteArray("AB"));
        QCOMPARE(f.right(2), QByteArray::fromHex("0000"));        // facts §B5
    }

    void dnwStyleFrameBytes()
    {
        QString err;
        const QByteArray f = eub::buildEubFrame(QByteArray("AB"), eub::dnwStyle(), &err);
        QVERIFY2(!f.isEmpty(), qPrintable(err));
        QCOMPARE(f.left(4), QByteArray::fromHex("1b444e57"));     // facts §B4 的 hubble 值（ESC "DNW"）
        QCOMPARE(f.mid(4, 4), QByteArray::fromHex("0c000000"));
        QCOMPARE(f.right(2), QByteArray::fromHex("ffff"));        // facts §B5
    }

    void lengthFieldIsPayloadPlusOverheadForLargePayload()
    {
        QString err;
        const QByteArray payload(2 * 1024 * 1024, 'x');
        const QByteArray f = eub::buildEubFrame(payload, eub::dnwStyle(), &err);
        QCOMPARE(f.size(), 4 + 4 + payload.size() + 2);
        const quint32 expect = quint32(payload.size()) + 10u;     // kFrameOverhead
        QByteArray le(4, Qt::Uninitialized);
        for (int i = 0; i < 4; ++i) le[i] = char((expect >> (8 * i)) & 0xFF);
        QCOMPARE(f.mid(4, 4), le);
    }

    void emptyPayloadFails()
    {
        QString err;
        QVERIFY(eub::buildEubFrame(QByteArray(), eub::zeroStyle(), &err).isEmpty());
        QVERIFY(!err.isEmpty());
    }

    void malformedStyleFails()
    {
        QString err;
        eub::EubFrameStyle bad;
        bad.header  = QByteArray(3, '\0');   // 必须 4 字节
        bad.trailer = QByteArray(2, '\0');
        QVERIFY(eub::buildEubFrame(QByteArray("A"), bad, &err).isEmpty());
        QVERIFY(!err.isEmpty());

        eub::EubFrameStyle bad2;
        bad2.header  = QByteArray(4, '\0');
        bad2.trailer = QByteArray(1, '\0');  // 必须 2 字节
        QVERIFY(eub::buildEubFrame(QByteArray("A"), bad2, &err).isEmpty());
    }

    void sendSegmentWritesExactlyOneFrame()
    {
        eub::MockEubTransport t;
        QString err;
        QVERIFY2(eub::sendSegment(t, QByteArray("AB"), eub::dnwStyle(), &err), qPrintable(err));
        QCOMPARE(t.writes.size(), 1);
        QCOMPARE(t.writes.first(), eub::buildEubFrame(QByteArray("AB"), eub::dnwStyle(), &err));
        QCOMPARE(t.calls, QStringList{QStringLiteral("write")});
    }

    void sendSegmentPropagatesWriteFailure()
    {
        eub::MockEubTransport t;
        t.failWriteAt = 0;
        QString err;
        QVERIFY(!eub::sendSegment(t, QByteArray("AB"), eub::zeroStyle(), &err));
        QVERIFY(!err.isEmpty());
        QCOMPARE(t.writes.size(), 0);
    }

    void sendSegmentRejectsEmptyPayloadBeforeWriting()
    {
        eub::MockEubTransport t;
        QString err;
        QVERIFY(!eub::sendSegment(t, QByteArray(), eub::zeroStyle(), &err));
        QCOMPARE(t.writes.size(), 0);      // fail-closed：不发出空帧
        QCOMPARE(t.calls.size(), 0);
    }
};

QTEST_APPLESS_MAIN(TestEubProtocol)
#include "test_eub_protocol.moc"
```

- [ ] **Step 2: 跑测试确认 RED**

Run: `cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_eub_protocol`
Expected: 失败（`core/eub/eub_protocol.h` 不存在）

- [ ] **Step 3: 实现**

`src/core/eub/eub_protocol.h`：

```cpp
// src/core/eub/eub_protocol.h
//
// EUB 下载帧：`[4B 头字段][u32 小端 = 数据长 + 10][数据][2B 尾]`（facts §B3，三实现一致）。
// 头字段与尾 2 字节**语义未定**：三个可用实现互相矛盾（facts §B4/§B5 的对照表）——
//   * exynos-usbdl：头 00 00 00 00、尾 00 00（calloc 全零，从不赋值）
//   * dltool：头 = 下载地址（默认 0xFFFFFFFE）、尾 = 数据字节 16 位累加和
//   * hubble：头 1B 44 4E 57（ASCII ESC+"DNW"）、尾 FF FF（常量）
// 本层只提供**照抄**能力：两种内置风格 + 任意自定义风格；注释与 UI 必须说明"照抄自哪个实现"，
// 不得声称理解其语义（facts §B6）。dltool 的"地址 + 累加和"风格本仓**不采用**（它服务的是
// 另一条 Windows 流程，facts §C10），需要时可用自定义 EubFrameStyle 表达。
#pragma once
#include <QByteArray>
#include <QString>

#include "eub_transport.h"

namespace eub {

// 头 4 字节 + 尾 2 字节（尺寸由 buildEubFrame 校验）
struct EubFrameStyle {
    QByteArray header;
    QByteArray trailer;
};

// 帧开销：4(头) + 4(长度) + 2(尾) —— 长度字段恒为 数据长 + kFrameOverhead（facts §B3）
constexpr int kFrameOverhead = 10;

EubFrameStyle zeroStyle();   // exynos-usbdl 路径（8890/8895 的表用它，facts §B4/§B5）
EubFrameStyle dnwStyle();    // hubble 路径（其余 6 个 SoC 的表用它）

// 构造一帧。前提不满足（载荷为空 / 风格尺寸不为 4+2）→ 返回空 + 中文 *error（fail-closed）。
QByteArray buildEubFrame(const QByteArray &payload, const EubFrameStyle &style, QString *error);

// 发送一段：构造帧 → **一次** writeBulk（facts §B7：hubble/dltool 都是整帧一次写）。
// 空载荷/风格非法/写失败 → false + *error；失败时不发出任何字节。
bool sendSegment(IEubTransport &t, const QByteArray &payload,
                 const EubFrameStyle &style, QString *error);

} // namespace eub
```

`src/core/eub/eub_protocol.cpp`：

```cpp
#include "eub_protocol.h"

namespace eub {
namespace {
void setErr(QString *error, const QString &msg) { if (error) *error = msg; }

QByteArray le32(quint32 v)
{
    QByteArray out(4, Qt::Uninitialized);
    for (int i = 0; i < 4; ++i)
        out[i] = char((v >> (8 * i)) & 0xFF);
    return out;
}
} // namespace

EubFrameStyle zeroStyle()
{
    return {QByteArray::fromHex("00000000"), QByteArray::fromHex("0000")};   // facts §B4/§B5
}

EubFrameStyle dnwStyle()
{
    return {QByteArray::fromHex("1b444e57"), QByteArray::fromHex("ffff")};   // facts §B4/§B5
}

QByteArray buildEubFrame(const QByteArray &payload, const EubFrameStyle &style, QString *error)
{
    if (payload.isEmpty()) {
        setErr(error, QStringLiteral("EUB 帧载荷为空：无处可发（fail-closed）"));
        return {};
    }
    if (style.header.size() != 4 || style.trailer.size() != 2) {
        setErr(error, QStringLiteral("EUB 帧风格非法：头必须 4 字节（当前 %1）、尾必须 2 字节（当前 %2）")
                          .arg(style.header.size()).arg(style.trailer.size()));
        return {};
    }
    if (payload.size() > std::numeric_limits<quint32>::max() - quint32(kFrameOverhead)) {
        setErr(error, QStringLiteral("EUB 帧载荷过大：%1 字节超出长度字段上限").arg(payload.size()));
        return {};
    }
    QByteArray frame;
    frame.reserve(4 + 4 + payload.size() + 2);
    frame.append(style.header);
    frame.append(le32(quint32(payload.size()) + quint32(kFrameOverhead)));   // facts §B3
    frame.append(payload);
    frame.append(style.trailer);
    return frame;
}

bool sendSegment(IEubTransport &t, const QByteArray &payload,
                 const EubFrameStyle &style, QString *error)
{
    const QByteArray frame = buildEubFrame(payload, style, error);
    if (frame.isEmpty())
        return false;    // buildEubFrame 已置 error（空载荷/风格非法）
    return t.writeBulk(frame, error);
}

} // namespace eub
```

⚠️ `std::numeric_limits` 需要 `#include <limits>`（照 `odin_libusb_transport.cpp:16` 的先例）。

CMakeLists：测试源清单加 `tests/test_eub_protocol.cpp`；extra sources 分支 = 
`src/core/eub/eub_protocol.cpp`（**不含** libusb 源，不需要进 libusb 列表 —— 本目标只编纯函数）。

- [ ] **Step 4: 跑测试确认 GREEN**

Run: `cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_eub_protocol && ./build/image_engine_tests_test_eub_protocol`
Expected: `Totals: 10 passed, 0 failed, 0 skipped`（brief 用例 slots 8 + 2）—— 实现实测为 **12**
（按约束 9 新增 2 个加固 slot：`kFrameOverhead` 与线上字节脱钩的恒真空洞、mock 空帧语义无承重）。

- [ ] **Step 5: 提交**

```bash
git add src/core/eub/eub_protocol.h src/core/eub/eub_protocol.cpp tests/mock_eub_transport.h tests/test_eub_protocol.cpp CMakeLists.txt
git commit -m "feat(eub): 下载帧构造与段发送（两种内置风格 + fail-closed）+ 写记录 mock"
```

---

### Task 4: 布局表与切段（eub_loadout）

**Files:**
- Create: `src/core/eub/eub_loadout.h`
- Create: `src/core/eub/eub_loadout.cpp`
- Test: `tests/test_eub_loadout.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `eub::EubFrameStyle` / `zeroStyle()` / `dnwStyle()`（Task 3）
- Produces: `eub::EubSegment{ QString name; quint64 offset; quint64 length; }`、
  `eub::EubLoadout{ ... }`、`eub::allLoadouts()`、`eub::eubLoadoutFor(socName, out, error)`、
  `eub::splitSboot(sboot, lo, out, error)`、`eub::detectSocFromImage(bytes)`、`eub::sha1Hex(bytes)`

- [ ] **Step 1: 写失败测试**

创建 `tests/test_eub_loadout.cpp`（**8 张表的逐值硬断言**是本任务的判据；数值改动必须先改 spec §5.4）：

```cpp
// tests/test_eub_loadout.cpp
//
// 布局表的**数值硬断言**。每个数字都能在 reference/ 里核对（每条 sourceNote 给了 file:line）。
// 本线无真机、也无真样本（sboot.bin 是三星签名二进制，不进仓库，facts §F1/§F8）——
// 本用例只钉住"表内容 == spec §5.4"。
#include <QtTest>

#include "core/eub/eub_loadout.h"

using Seg = QPair<QString, QPair<quint64, quint64>>;   // 名字, (offset, length)

static eub::EubLoadout mustLoad(const QString &soc)
{
    eub::EubLoadout lo;
    QString err;
    if (!eub::eubLoadoutFor(soc, lo, &err))
        qFatal("查表失败: %s", qPrintable(err));
    return lo;
}

static void expectSegs(const eub::EubLoadout &lo, const QList<Seg> &want)
{
    QCOMPARE(lo.segments.size(), want.size());
    for (int i = 0; i < want.size(); ++i) {
        QCOMPARE(lo.segments[i].name, want[i].first);
        QCOMPARE(lo.segments[i].offset, want[i].second.first);
        QCOMPARE(lo.segments[i].length, want[i].second.second);
    }
}

class TestEubLoadout : public QObject
{
    Q_OBJECT

private slots:
    void table8890()   // reference/exynos-usbdl/scripts/split-sboot-8890.sh:2-5
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos8890"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x2000}}, {"el3_mon", {0x2000, 0x24000}},
                        {"bl2", {0x26000, 0x26D10}}, {"bootloader", {0x61000, 0xD1000}}});
        QCOMPARE(lo.style.header, eub::zeroStyle().header);
        QCOMPARE(lo.style.trailer, eub::zeroStyle().trailer);
        QVERIFY(lo.evidence.contains(QStringLiteral("单源")));
        QVERIFY(lo.sourceNote.contains(QStringLiteral("reference/")));
        QVERIFY(!lo.sbootSha1.isEmpty());   // scripts/split-sboot-8890.sh:1 给了 sha1
    }

    void table8895()   // reference/exynos-usbdl/scripts/split-sboot-8895.sh:2-7（第 5 行是作者自注的"重发"）
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos8895"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x2000}}, {"bl31", {0x2000, 0x28000}},
                        {"bl2", {0x2A000, 0x30000}}, {"fwbl1", {0x0, 0x2000}},
                        {"part5", {0x72000, 0xD1000}}, {"part6", {0x143000, 0x80000}}});
        QVERIFY(lo.evidence.contains(QStringLiteral("单源")));
    }

    void table7580()   // reference/hubble/ExynosData/Exynos7580.json ↔ 8890 救援包的 split-sboot-7580.sh:4
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos7580"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x2000}}, {"bl31", {0x2000, 0x30000}},
                        {"bl2", {0x32000, 0x8000}}, {"u-boot", {0x3A000, 0xD1000}}});
        QVERIFY(lo.evidence.contains(QStringLiteral("分歧")));   // facts §C4：另一源作 0x7D10
        QVERIFY(lo.sourceNote.contains(QStringLiteral("0x7D10")));
    }

    void table7885()   // reference/hubble/ExynosData/Exynos7885.json
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos7885"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x2000}}, {"bl31", {0x2000, 0x25000}},
                        {"bl2", {0x27000, 0x2A000}}, {"fwbl1", {0x0, 0x2000}},
                        {"u-boot", {0x61800, 0xD1000}}});
    }

    void table9610()   // 双源：hubble/ExynosData/Exynos9610.json:5-34 ↔ 9610 救援包 split_bootloader_a505.sh + dltool.c:311-319
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos9610"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x2000}}, {"epbl", {0x2000, 0x13000}},
                        {"bl2", {0x15000, 0x2F000}}, {"fwbl1", {0x0, 0x2000}},
                        {"u-boot", {0x5A000, 0x180000}}, {"el3_mon", {0x1DA000, 0x40000}}});
        QVERIFY(lo.evidence.contains(QStringLiteral("双源一致")));
        QVERIFY(!lo.responseSupport);
        QVERIFY(lo.extraFiles.isEmpty());
    }

    void table9810()   // reference/hubble/ExynosData/Exynos9810.json
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos9810"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x2000}}, {"bl31", {0x2000, 0x13000}},
                        {"bl2", {0x15000, 0x4F000}}, {"fwbl1", {0x0, 0x2000}},
                        {"u-boot", {0x7D000, 0x180000}}, {"el3_mon", {0x1FD000, 0x40000}}});
    }

    void table9820()   // reference/hubble/ExynosData/Exynos9820.json（response_support=true）
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos9820"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x3000}}, {"epbl", {0x3000, 0x13000}},
                        {"bl2", {0x16000, 0x52000}}, {"u-boot", {0xA4000, 0x180000}},
                        {"el3_mon", {0x224000, 0x40000}}});
        QVERIFY(lo.responseSupport);
    }

    void table9830()   // reference/hubble/ExynosData/Exynos9830.json（files_to_send + response_support）
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos9830"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x3000}}, {"epbl", {0x3000, 0x13000}},
                        {"bl2", {0x16000, 0x6C000}}, {"lk", {0xDB000, 0x280000}},
                        {"el3_mon", {0x35B000, 0x40000}}});
        QCOMPARE(lo.extraFiles, QStringList({"ldfw.img", "tzsw.img"}));
        QVERIFY(lo.responseSupport);
    }

    void lookupIsCaseInsensitiveAndRejectsUnknown()
    {
        eub::EubLoadout lo; QString err;
        QVERIFY(eub::eubLoadoutFor(QStringLiteral("exynos9610"), lo, &err));
        QCOMPARE(lo.soc, QStringLiteral("Exynos9610"));
        QVERIFY(!eub::eubLoadoutFor(QStringLiteral("Exynos9999"), lo, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(err.contains(QStringLiteral("Exynos9610")));   // 文案列出支持的 SoC（可行动）
    }

    void splitCutsExactRangesAndRepeatsAreIdentical()
    {
        // 合成样本：长度必须 ≥ 9610 表的最大末段（0x1DA000 + 0x40000 = 0x21A000）
        // （facts §F1：无真样本，用合成夹具）
        QByteArray sboot(0x220000, '\0');
        // 非周期填充（xorshift32）：`i & 0xFF` 一类 256 周期图案会让"偏移错 256 的整数倍"**漏检** ——
        // T4 实现者用变异证明过（brief 原夹具下 offset 错 0x100 时 split 槽仍全绿）。
        quint32 x = 0x12345678u;
        for (int i = 0; i < sboot.size(); ++i) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            sboot[i] = char(x & 0xFF);
        }

        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos9610"));
        QList<QPair<QString, QByteArray>> parts;
        QString err;
        QVERIFY2(eub::splitSboot(sboot, lo, parts, &err), qPrintable(err));
        QCOMPARE(parts.size(), lo.segments.size());

        QCOMPARE(parts[0].first, QStringLiteral("fwbl1"));
        QCOMPARE(parts[0].second, sboot.mid(0x0, 0x2000));
        QCOMPARE(parts[2].second, sboot.mid(0x15000, 0x2F000));
        // 重发段（facts §C5）：第 4 段与第 1 段**逐字节相同**
        QCOMPARE(parts[3].second, parts[0].second);
    }

    void splitFailsClosedWhenImageTooShort()
    {
        QByteArray shortImage(0x1000, '\xAB');      // 小于 8890 第一段之后的任何段
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos8890"));
        QList<QPair<QString, QByteArray>> parts;
        QString err;
        QVERIFY(!eub::splitSboot(shortImage, lo, parts, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(parts.isEmpty());                   // **绝不**产出半段
        QVERIFY(err.contains(QStringLiteral("0x"))); // 文案给出所需/实际尺寸
    }

    void sha1HexMatchesKnownVector()
    {
        QCOMPARE(eub::sha1Hex(QByteArray("abc")),
                 QStringLiteral("a9993e364706816aba3e25717850c26c9cd0d89d"));
    }

    void detectSocFromImageFindsExynosToken()   // facts §A4：hubble.py:192-202
    {
        QByteArray img(0x8000, '\0');
        img.replace(0x1234, 12, QByteArray("EXYNOS9610xx"));
        QCOMPARE(eub::detectSocFromImage(img), QStringLiteral("Exynos9610"));
        QCOMPARE(eub::detectSocFromImage(QByteArray(1024, '\0')), QString());
    }
};

QTEST_APPLESS_MAIN(TestEubLoadout)
#include "test_eub_loadout.moc"
```

- [ ] **Step 2: 跑测试确认 RED**

Run: `cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_eub_loadout`
Expected: 失败（`core/eub/eub_loadout.h` 不存在）

- [ ] **Step 3: 实现**

`src/core/eub/eub_loadout.h`（字段名以本表为准，**不要自创**）：

```cpp
// src/core/eub/eub_loadout.h
//
// 每 SoC 的 sboot.bin 切段表 + 切段/查表/摘要工具。**表的唯一来源是设计 spec §5.4**
// （`docs/superpowers/specs/2026-09-17-exynos-eub-design.md`），每条 sourceNote 指到
// reference/ 里的 file:line；改动任何数字前先改 spec。
//
// 证据等级（EubLoadout::evidence，取值固定这四个字符串）：
//   "双源一致"        —— 两个独立实现对同一 SoC 的偏移/长度完全一致（9610）
//   "单源+实战报告"   —— 只有一份脚本，但第三方用它做过实测恢复（8890）
//   "双源分歧（采信 hubble 连续切法）" —— 两源不一致，本仓采信 hubble 的连续切法（7580）
//   "单源"            —— 单一来源，未对拍（8895/7885/9810/9820/9830）
#pragma once
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include "eub_protocol.h"

namespace eub {

struct EubSegment {
    QString name;       // 段名（工具间命名不一，以偏移为准；facts §C2）
    quint64 offset = 0; // sboot.bin 内绝对偏移
    quint64 length = 0;
};

struct EubLoadout {
    QString soc;               // "Exynos9610"（与设备 iProduct 对齐）
    QStringList models;        // 参照里出现过的机型（仅展示）
    QString evidence;          // 上面四档之一
    QString sourceNote;        // 出处（reference/<repo>/<file>:<line>）
    QByteArray sbootSha1;      // 参照所用 sboot 修订的 sha1（hex 文本；无记录则为空）
    EubFrameStyle style;       // 帧头/尾风格（facts §B4/§B5）
    QList<EubSegment> segments;// 按序发送；重发段照列（facts §C5）
    QStringList extraFiles;    // 段之后另发的 BL 包内文件（9830：ldfw.img/tzsw.img，facts §C7）
    bool responseSupport = false;  // 设备会回显（facts §C7/§C8）
};

// 全部 8 张表（顺序 = spec §5.4 的行序）
QList<EubLoadout> allLoadouts();

// 按 SoC 名查表：大小写不敏感（"exynos9610" == "Exynos9610"）；未知 → false + 列出支持的 SoC
bool eubLoadoutFor(const QString &socName, EubLoadout &out, QString *error);

// 切段：任一 (offset + length) 越界 → false 且 out **保持为空**（绝不截断，spec §7）
bool splitSboot(const QByteArray &sboot, const EubLoadout &lo,
                QList<QPair<QString, QByteArray>> &out, QString *error);

// 从镜像内容反推 SoC 名（facts §A4：hubble.py:195 的 `EXYNOS[0-9]+`；结果形如 "Exynos9610"）。
// 设备 iProduct 是 "SEC S5PC210 Test B/D" 之类的极老 SoC 才需要它。找不到 → 空串。
QString detectSocFromImage(const QByteArray &sboot);

// SHA-1 十六进制小写（用于与表项的 sbootSha1 对照展示，facts §C9）
QString sha1Hex(const QByteArray &data);

} // namespace eub
```

`src/core/eub/eub_loadout.cpp`：8 张表的数值**逐条照抄 spec §5.4**，风格按 `spec §5.4` 的"风格"列
（8890/8895 → `zeroStyle()`，其余 6 个 → `dnwStyle()`），`sourceNote` 写 `reference/...:line`。
`sbootSha1` 三个有记录的值（十六进制小写文本，来源为脚本注释第 1 行；其余 5 张表留空）：

| SoC | sbootSha1 | 出处 |
|---|---|---|
| Exynos8890 | `9322ccb4e9b382b8cc67ff9ef989c459a763621f` | `reference/exynos-usbdl/scripts/split-sboot-8890.sh:1`（G930W8VLS6CSH1） |
| Exynos8895 | `648a3e2c4de149250c575b4f14de096e147cc799` | `reference/exynos-usbdl/scripts/split-sboot-8895.sh:1`（G950FXXU1AQJ5） |
| Exynos7580 | `466852d13fa02d51729d21633f47708308579f58` | `reference/exynos8890-exynos-usbdl-recovery/A510F/split-sboot-7580.sh:1`（A510FXXS8CTI7） |
查表用 `soc.compare(name, Qt::CaseInsensitive)`；`splitSboot` 先**全表校验**再切（两遍循环，
保证失败时 `out` 为空）；`detectSocFromImage` 用 `QRegularExpression("EXYNOS[0-9]+")` 取首个匹配后
转成 `Exynos` + 数字（对齐 hubble.py:202 的 `.title()` 结果）；`sha1Hex` 用
`QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex()`。

7580 的 `sourceNote` 必须**同时**写出两源与采信理由，例如：
`"reference/hubble/ExynosData/Exynos7580.json（采信：bl2 长度 0x8000 与下一段起点连续）；"
 "另一源 reference/exynos8890-exynos-usbdl-recovery/A510F/split-sboot-7580.sh:4 作 0x7D10 —— facts §C4 记录该分歧"`。

CMakeLists：测试源清单加 `tests/test_eub_loadout.cpp`；extra sources 分支 =
`src/core/eub/eub_loadout.cpp` + `src/core/eub/eub_protocol.cpp`。

- [ ] **Step 4: 跑测试确认 GREEN**

Run: `cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_eub_loadout && ./build/image_engine_tests_test_eub_loadout`
Expected: `Totals: 15 passed, 0 failed, 0 skipped`（本任务用例 slots 13 + 2）

- [ ] **Step 5: 提交**

```bash
git add src/core/eub/eub_loadout.h src/core/eub/eub_loadout.cpp tests/test_eub_loadout.cpp CMakeLists.txt
git commit -m "feat(eub): 8 个 SoC 的 sboot 布局表 + 查表/切段/摘要（数值逐值钉死，来源带 file:line）"
```

---

### Task 5: 载荷来源解析（eub_payload）

**Files:**
- Create: `src/core/eub/eub_payload.h`
- Create: `src/core/eub/eub_payload.cpp`
- Test: `tests/test_eub_payload.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `imgtar::indexTarStream`（`src/image_engine/tar_image.h:34`）、`imgcomp::lz4Decompress`
  （`src/image_engine/compression/lz4_wrapper.h:6`）、`imgtar::buildTar` / `appendMd5Footer`（测试用）
- Produces: `eub::SbootSource{ QString description; bool wasCompressed; }`、
  `eub::loadSbootBytes(path, out, source, error)`、`eub::looksLikeLz4Frame(data)`

> CMake 备注：`tar_image.cpp` / `lz4_wrapper.cpp` 在 `image_engine` 静态库里，而**每个** image_engine
> 测试目标都已 `target_link_libraries(... PRIVATE image_engine Qt6::Test)`（`CMakeLists.txt:644`），
> 所以本任务**不需要**往 `_test_extra_sources` 里补这两个源，liblz4 也随 `image_engine` 的 PUBLIC
> 链接传递（`CMakeLists.txt:211-213`）。

- [ ] **Step 1: 写失败测试**

创建 `tests/test_eub_payload.cpp`：

```cpp
// tests/test_eub_payload.cpp
//
// 载荷来源三条路径（spec §D8）：裸镜像 / LZ4 frame / BL tar 内查找。
// 本线**无真样本**（facts §F1）：用例自造合成 sboot（确定性图案），用 imgtar::buildTar 造 tar。
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

#include "core/eub/eub_payload.h"
#include "image_engine/compression/lz4_wrapper.h"
#include "image_engine/tar_image.h"

class TestEubPayload : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString writeFile(const QString &name, const QByteArray &bytes)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) qFatal("无法写临时文件");
        f.write(bytes);
        f.close();
        return path;
    }
    static QByteArray syntheticSboot()
    {
        // 非周期填充（xorshift32）：`(i * k) & 0xFF` 仍是 256 周期图案，会让"偏移错 256 的整数倍"漏检
        //（T4 实现者的变异证据：brief 原夹具下 offset 错 0x100 时 split 槽仍全绿）
        QByteArray b(0x8000, '\0');
        quint32 x = 0x12345678u;
        for (int i = 0; i < b.size(); ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b[i] = char(x & 0xFF); }
        return b;
    }

private slots:
    void rawImageIsLoadedVerbatim()
    {
        const QByteArray img = syntheticSboot();
        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("sboot.bin", img), out, &src, &err), qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(!src.wasCompressed);
        QVERIFY(src.description.contains(QStringLiteral("sboot.bin")));
    }

    void lz4FrameImageIsDecompressed()
    {
        const QByteArray img = syntheticSboot();
        const QByteArray packed = imgcomp::lz4Compress(img);
        QVERIFY(!packed.isEmpty());
        QVERIFY(eub::looksLikeLz4Frame(packed));

        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("sboot.bin.lz4", packed), out, &src, &err), qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(src.wasCompressed);
    }

    void tarMd5WithPlainSbootIsExtracted()
    {
        const QByteArray img = syntheticSboot();
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry e; e.name = QStringLiteral("sboot.bin"); e.data = img;
        imgtar::TarEntry other; other.name = QStringLiteral("param.bin"); other.data = QByteArray(16, '\x11');
        entries << e << other;
        const QByteArray tar = imgtar::appendMd5Footer(imgtar::buildTar(entries));

        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("BL_TEST.tar.md5", tar), out, &src, &err), qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(!src.wasCompressed);
        QVERIFY(src.description.contains(QStringLiteral("sboot.bin")));
        QVERIFY(src.description.contains(QStringLiteral("tar")));
    }

    void tarWithLz4SbootIsExtractedThenDecompressed()
    {
        const QByteArray img = syntheticSboot();
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry e; e.name = QStringLiteral("sboot.bin.lz4"); e.data = imgcomp::lz4Compress(img);
        entries << e;
        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("BL_LZ4.tar", imgtar::buildTar(entries)), out, &src, &err),
                 qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(src.wasCompressed);
    }

    void tarWithoutSbootFailsAndListsWhatItHas()
    {
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry a; a.name = QStringLiteral("boot.img");   a.data = QByteArray(8, '\x22');
        imgtar::TarEntry b; b.name = QStringLiteral("tzsw.img");   b.data = QByteArray(8, '\x33');
        entries << a << b;

        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY(!eub::loadSbootBytes(writeFile("BL_NO.tar", imgtar::buildTar(entries)), out, &src, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(out.isEmpty());
        QVERIFY(err.contains(QStringLiteral("boot.img")));   // 文案列出包内有什么（可行动）
    }

    void corruptLz4Fails()
    {
        QByteArray junk = QByteArray::fromHex("04224d18") + QByteArray(64, '\x77');  // 魔数对、内容坏
        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY(!eub::loadSbootBytes(writeFile("bad.lz4", junk), out, &src, &err));
        QVERIFY(!err.isEmpty());
    }

    void emptyInputFails()
    {
        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY(!eub::loadSbootBytes(writeFile("empty.bin", QByteArray()), out, &src, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(!eub::loadSbootBytes(m_dir.filePath("does-not-exist.bin"), out, &src, &err));
    }

    void lz4MagicDetection()
    {
        QVERIFY(!eub::looksLikeLz4Frame(QByteArray::fromHex("04224d")));    // 太短
        QVERIFY(!eub::looksLikeLz4Frame(QByteArray::fromHex("00000000")));
        QVERIFY(eub::looksLikeLz4Frame(QByteArray::fromHex("04224d18" "00")));
    }
};

QTEST_APPLESS_MAIN(TestEubPayload)
#include "test_eub_payload.moc"
```

- [ ] **Step 2: 跑测试确认 RED**

Run: `cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_eub_payload`
Expected: 失败（`core/eub/eub_payload.h` 不存在）

- [ ] **Step 3: 实现**

`src/core/eub/eub_payload.h`：

```cpp
// src/core/eub/eub_payload.h
//
// 载荷来源解析（设计 spec §D8）：把用户指定的**一个文件**变成 sboot.bin 字节。
//   ① 裸镜像（非 tar 的任何文件）
//   ② LZ4 frame 压缩的镜像（.lz4；三星 BL 包内即此格式 —— hubble.py:174 用 lz4.frame.decompress，
//      本仓 imgcomp::lz4Decompress 同为 LZ4 **frame** 格式，见 lz4_wrapper.cpp:21-）
//   ③ BL_*.tar.md5：用 imgtar::indexTarStream 找 sboot.bin / sboot.bin.lz4 条目（流式，整包不进内存），
//      读出条目字节后若是 LZ4 再解压
// **不做**：从 AP 包找 boot.img（spec §D8）、分发任何三星签名二进制（facts §F8）。
#pragma once
#include <QByteArray>
#include <QString>

namespace eub {

struct SbootSource {
    QString description;        // 展示用，例如 "BL_G930F.tar.md5 内的 sboot.bin.lz4（已解压）"
    bool    wasCompressed = false;
};

// 成功：out 非空。失败：false + 中文 *error（含"包里有什么"或"该文件不是有效的 LZ4/镜像"）。
bool loadSbootBytes(const QString &path, QByteArray &out, SbootSource *source, QString *error);

// LZ4 frame 魔数 04 22 4D 18（纯判据，测试可直接调）
bool looksLikeLz4Frame(const QByteArray &data);

} // namespace eub
```

`src/core/eub/eub_payload.cpp` 要点（顺序即实现顺序）：

1. `QFile` 打不开 → 失败（含路径）。
2. 读前 512 字节：若 `hdr.mid(257, 5) == "ustar"`（**与 `tar_image.cpp:631` 的判据逐字一致**）
   或文件名以 `.tar` / `.tar.md5` 结尾 → **按 tar 处理**：
   `imgtar::indexTarStream(path, idx, &tarEnd, &err)`（失败 → 转发 err）；
   在 `idx` 里找 `basename` 大小写不敏感等于 `sboot.bin` 的条目，找不到再找 `sboot.bin.lz4`；
   都找不到 → 失败，错误里**列出包内条目名**（前若干个，避免超长）；
   命中后 `QFile::seek(entry.offset)` 读 `entry.size` 字节。
3. 否则（或 tar 内条目读完之后）：若 `size >= 4 && looksLikeLz4Frame(bytes)` → `imgcomp::lz4Decompress`；
   返回空 → 失败（"LZ4 解压失败"）；`wasCompressed = true`。
4. 最终 `out` 为空 → 失败（空文件/空条目都没意义）。
5. `description` 形如：`"<文件名>（裸镜像）"` / `"<文件名>（LZ4 解压后）"` / `"<tar 名> 内的 <条目名>（已解压）"`。

CMakeLists：测试源清单加 `tests/test_eub_payload.cpp`；extra sources 分支 =
`src/core/eub/eub_payload.cpp`（**不**补 tar_image.cpp / lz4_wrapper.cpp：随 `image_engine` 链接而来，
见本任务开头的 CMake 备注）。

- [ ] **Step 4: 跑测试确认 GREEN**

Run: `cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_eub_payload && ./build/image_engine_tests_test_eub_payload`
Expected: `Totals: 10 passed, 0 failed, 0 skipped`（本任务用例 slots 8 + 2）

- [ ] **Step 5: 提交**

```bash
git add src/core/eub/eub_payload.h src/core/eub/eub_payload.cpp tests/test_eub_payload.cpp CMakeLists.txt
git commit -m "feat(eub): 载荷来源解析（裸镜像 / LZ4 frame / BL tar 内查找）+ 用例"
```

---

### Task 6: 会话编排（eub_session）

**Files:**
- Create: `src/core/eub/eub_session.h`
- Create: `src/core/eub/eub_session.cpp`
- Test: `tests/test_eub_session.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `eub::IEubTransport`（Task 1）、`eub::EubLoadout`/`splitSboot`/`detectSocFromImage`（Task 4）、
  `eub::sendSegment`（Task 3）、`tests/mock_eub_transport.h`（Task 3 建）
- Produces: `eub::EubOptions`、`eub::EubProgress`、`eub::EubSession`（`identify` / `run`）

**行为契约（实现按此，不得增删步骤）：**

1. `identify(lo, err)`：`openWithRetry` → `readDeviceInfo` → SoC 名（空 → 记日志并返回失败，
   由调用方决定是否用 `detectSocFromImage` 兜底）→ `eubLoadoutFor` → **close**（不在两次调用之间
   持有句柄：设备可能瞬态消失，facts §A6）→ 转发 `transport.notes()` 到日志。
2. `run(lo, sboot, err)`：`splitSboot`（失败即返回，**不写任何字节**）→ 逐段：
   `openWithRetry` → （仅第 1 段）`readDeviceInfo` 并**核对 SoC 与 `lo.soc` 一致**（不一致 → 失败：
   用户在识别与开始之间换了设备）→ `sendSegment(lo.style)` → 若 `lo.responseSupport && opt.readResponse`
   读一次回显（`readBulk(512, kReadTimeoutMs)`，空/失败**不判失败**，只落日志）→ `close` →
   `sleep(segmentGapMs)`（最后一段后不睡）。
3. `openWithRetry`：最多 `opt.revolveAttempts` 次，每次失败后 `sleep(opt.revolvePollMs)`。
   **用次数不用墙钟**：可注入 sleep 后仍确定，测试不依赖真实时间。
4. 失败语义（spec §7）：错误文案**固定格式** ——
   `QStringLiteral("第 %1 段 \"%2\"（0x%3/0x%4）写入失败：%5").arg(序号).arg(段名).arg(偏移, 0, 16).arg(长度, 0, 16).arg(原因)`
   （序号**从 1 起**，测试按 `第 2 段` 断言）；**不支持从中间续传**（引导链必须从第一段起）；
   失败时 `close()` 已调用，句柄不泄漏。
5. 进度：`stage` 取值 `"identify"` / `"send"` / `"done"`，`percent` 单次 `run` 内**单调不减**。

- [ ] **Step 1: 写失败测试**

创建 `tests/test_eub_session.cpp`：

```cpp
// tests/test_eub_session.cpp
//
// 会话编排：段序、每段重开、重试、失败语义、进度单调 —— 全部经 MockEubTransport（无真机，facts §F1）。
#include <QtTest>

#include "core/eub/eub_session.h"
#include "mock_eub_transport.h"

namespace {

// 合成 sboot：够 9610 全表（最大段到 0x1DA000 + 0x40000 = 0x21A000）
// 非周期填充（xorshift32）：`(i * k) & 0xFF` 是 256 周期图案，会让"偏移错 256 的整数倍"漏检
//（T4 实现者的变异证据：brief 原夹具下 offset 错 0x100 时 split 槽仍全绿）
QByteArray syntheticSboot(quint64 bytes = 0x220000)
{
    QByteArray b(int(bytes), '\0');
    quint32 x = 0x12345678u;
    for (int i = 0; i < b.size(); ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b[i] = char(x & 0xFF); }
    return b;
}

eub::EubOptions fastOptions()
{
    eub::EubOptions o;
    o.segmentGapMs = 0;         // 用例里不等真时间
    o.revolvePollMs = 0;
    o.revolveAttempts = 3;
    o.sleepFn = [](int) {};     // 注入空实现
    return o;
}

} // namespace

class TestEubSession : public QObject
{
    Q_OBJECT

private:
    static eub::EubLoadout loadout9610()
    {
        eub::EubLoadout lo; QString err;
        if (!eub::eubLoadoutFor(QStringLiteral("Exynos9610"), lo, &err)) qFatal("查表失败");
        return lo;
    }
    static eub::EubDeviceInfo info9610()
    {
        eub::EubDeviceInfo i;
        i.socName = QStringLiteral("Exynos9610");
        i.socId = QStringLiteral("0123456789abcde");
        i.chipId = QStringLiteral("0123456789abcdef");
        i.usbBootVersion = QStringLiteral("1.0");
        return i;
    }

private slots:
    void sendsEverySegmentAsOneFrameInOrder()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        eub::EubSession s(t, fastOptions());
        const QByteArray sboot = syntheticSboot();
        QString err;
        QVERIFY2(s.run(loadout9610(), sboot, &err), qPrintable(err));

        const eub::EubLoadout lo = loadout9610();
        QCOMPARE(t.writes.size(), lo.segments.size());
        for (int i = 0; i < lo.segments.size(); ++i) {
            const eub::EubSegment &seg = lo.segments[i];
            QString ferr;
            const QByteArray expect = eub::buildEubFrame(sboot.mid(int(seg.offset), int(seg.length)),
                                                         lo.style, &ferr);
            QVERIFY2(!expect.isEmpty(), qPrintable(ferr));
            QCOMPARE(t.writes[i], expect);
        }
    }

    void closesAndReopensForEverySegment()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY(s.run(loadout9610(), syntheticSboot(), &err));
        QCOMPARE(t.calls.count(QStringLiteral("open")), 6);     // 9610 是 6 段
        QCOMPARE(t.calls.count(QStringLiteral("write")), 6);
        QCOMPARE(t.calls.count(QStringLiteral("close")), 6);
        QCOMPARE(t.calls.first(), QStringLiteral("open"));
        QCOMPARE(t.calls.last(), QStringLiteral("close"));
    }

    void retriesOpenUntilDeviceAppears()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        t.openFailures = 2;                     // 头两次"设备还没出现"
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY2(s.run(loadout9610(), syntheticSboot(), &err), qPrintable(err));
        QCOMPARE(t.writes.size(), 6);
    }

    void failsWhenDeviceNeverAppears()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        t.openFailures = 99;                    // 一直没出现
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY(!s.run(loadout9610(), syntheticSboot(), &err));
        QVERIFY(!err.isEmpty());
        QCOMPARE(t.calls.count(QStringLiteral("open")), 3);     // revolveAttempts
        QCOMPARE(t.writes.size(), 0);
    }

    void writeFailureNamesTheSegmentAndStops()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        t.failWriteAt = 1;                      // 第 2 段（epbl）写失败
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY(!s.run(loadout9610(), syntheticSboot(), &err));
        QCOMPARE(t.writes.size(), 1);           // 出错即停，不再发后续段
        QVERIFY(err.contains(QStringLiteral("epbl")));
        QVERIFY(err.contains(QStringLiteral("0x2000")));       // 偏移可复盘
        QVERIFY(err.contains(QStringLiteral("第 2 段")));       // 段序号（文案格式见 eub_session.h）
        QCOMPARE(t.calls.last(), QStringLiteral("close"));     // 句柄收干净
    }

    void socMismatchBetweenIdentifyAndRunFails()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        t.info.socName = QStringLiteral("Exynos8890");         // 用户中途换了设备
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY(!s.run(loadout9610(), syntheticSboot(), &err));
        QVERIFY(err.contains(QStringLiteral("Exynos8890")));
        QCOMPARE(t.writes.size(), 0);
    }

    void echoIsReadOnlyWhenResponseSupport()
    {
        // 9610：不读回显
        {
            eub::MockEubTransport t; t.info = info9610();
            eub::EubSession s(t, fastOptions());
            QString err;
            QVERIFY(s.run(loadout9610(), syntheticSboot(), &err));
            QCOMPARE(t.calls.count(QStringLiteral("read")), 0);
        }
        // 9830：responseSupport = true → 每段读一次
        {
            eub::EubLoadout lo; QString lerr;
            QVERIFY(eub::eubLoadoutFor(QStringLiteral("Exynos9830"), lo, &lerr));
            eub::MockEubTransport t;
            t.info = info9610(); t.info.socName = QStringLiteral("Exynos9830");
            t.response = QByteArray("boot ok");
            eub::EubSession s(t, fastOptions());
            QString err;
            QVERIFY2(s.run(lo, syntheticSboot(0x400000), &err), qPrintable(err));
            QCOMPARE(t.calls.count(QStringLiteral("read")), lo.segments.size());
        }
    }

    void identifyReportsUnknownSocWithoutWriting()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        t.info.socName = QStringLiteral("Exynos9999");
        eub::EubSession s(t, fastOptions());
        eub::EubLoadout lo;
        QString err;
        QVERIFY(!s.identify(lo, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(err.contains(QStringLiteral("Exynos9610")));   // 列出支持项
        QCOMPARE(t.writes.size(), 0);
        QCOMPARE(t.calls.last(), QStringLiteral("close"));     // identify 也不留句柄
    }

    void progressIsMonotonicAndEndsAtDone()
    {
        QList<eub::EubProgress> seen;
        eub::MockEubTransport t;
        t.info = info9610();
        eub::EubSession s(t, fastOptions(), [&](const eub::EubProgress &p) { seen.append(p); });
        QString err;
        QVERIFY(s.run(loadout9610(), syntheticSboot(), &err));
        QVERIFY(!seen.isEmpty());
        QCOMPARE(seen.last().stage, QStringLiteral("done"));
        QCOMPARE(seen.last().percent, 100);
        for (int i = 1; i < seen.size(); ++i)
            QVERIFY(seen[i].percent >= seen[i - 1].percent);
    }

    void splitFailureStopsBeforeAnyWrite()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY(!s.run(loadout9610(), QByteArray(0x1000, '\xAB'), &err));   // 镜像太短
        QVERIFY(!err.isEmpty());
        QCOMPARE(t.writes.size(), 0);
    }
};

QTEST_APPLESS_MAIN(TestEubSession)
#include "test_eub_session.moc"
```

- [ ] **Step 2: 跑测试确认 RED**

Run: `cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_eub_session`
Expected: 失败（`core/eub/eub_session.h` 不存在）

- [ ] **Step 3: 实现**

`src/core/eub/eub_session.h`：

```cpp
// src/core/eub/eub_session.h
//
// EUB 救援编排：识别 → 查表 → 切段 → 逐段(重开设备 → 发送 →（可选）读回显) → 收尾。
// 设备访问只经 IEubTransport —— 本文件不碰 libusb、不构造帧（帧在 eub_protocol）、不切段
// （切段在 eub_loadout）。
//
// 段间"重开设备"是参照的实测路径（facts §B8：shell 脚本每段都是一次全新的 exynos-usbdl 调用 +
// sleep 1；cfg 注释写作 "Wait Re-Enumeration"）。关闭再打开天然容忍设备重枚举与地址变化
// （facts §B9 明说重连会失败，故本层带**重试**，比参照脚本更稳）。
#pragma once
#include <QByteArray>
#include <QString>
#include <functional>

#include "eub_transport.h"
#include "eub_loadout.h"

namespace eub {

struct EubOptions {
    int  segmentGapMs     = 1000;   // 段间基础等待（参照脚本用 sleep 1，facts §B8）
    int  revolveAttempts  = 20;     // open 重试次数；总上限 ≈ revolveAttempts × revolvePollMs
    int  revolvePollMs    = 500;    // 重试间隔
    bool readResponse     = true;   // responseSupport 的表项：每段后读一次回显（facts §C7/§C8）
    // 注入用：空 = QThread::msleep。用例注入空实现 → 重试/等待逻辑**不依赖真实时间**
    // （超时用**次数**而非墙钟，见 revolveAttempts）。
    std::function<void(int)> sleepFn;
};

// stage 取值（UI 依赖这组字符串，不要改名/新增而不通知 UI 层）：
//   "identify" / "send" / "done"
struct EubProgress { QString stage; QString detail; int percent = 0; };
using EubProgressFn = std::function<void(const EubProgress &)>;

class EubSession
{
public:
    explicit EubSession(IEubTransport &t, const EubOptions &opt = {}, EubProgressFn progress = {});

    // 打开设备 → 读自述 → 查表 → 关闭（不持有句柄：设备可能瞬态消失，facts §A6）。
    // SoC 名为空（极老 SoC 只报 "SEC S5PC210 Test B/D"，facts §A4）→ 失败，
    // 由调用方用 detectSocFromImage(sboot) 兜底后自行查表。
    bool identify(EubLoadout &out, QString *error);

    // 完整救援：切段（失败即中止，**不写任何字节**）→ 逐段 open(带重试) → 发送 → 可选读回显 → close。
    // 第 1 段发送前核对设备 SoC 与 lo.soc 一致（防"识别与开始之间换了设备"）。
    // 失败文案含段序号/段名/偏移长度；**不支持从中间续传**（引导链必须从第一段起，spec §7）。
    bool run(const EubLoadout &lo, const QByteArray &sboot, QString *error);

private:
    bool openWithRetry(QString *error);
    void report(const QString &stage, const QString &detail, int percent);

    IEubTransport &m_t;
    EubOptions     m_opt;
    EubProgressFn  m_progress;
};

} // namespace eub
```

`src/core/eub/eub_session.cpp` 要点：
- `openWithRetry`：`for (int i = 0; i < m_opt.revolveAttempts; ++i) { if (m_t.open(error)) return true; if (i + 1 < attempts) sleep(revolvePollMs); }` → 返回 false（error 保留最后一次的）。
- `identify`：`openWithRetry` → `readDeviceInfo` → `openAndForget`（close）→ `eubLoadoutFor`。
  注意顺序：**先 close 再查表**（查表失败也要关句柄；用一个小 RAII 或显式 close + 早退前 close）。
- `run`：`splitSboot` 先做（失败即 return，句柄未打开）→ 循环段（含 `report("send", ..., percent)`）
  → 结束后 `report("done", ..., 100)`。
- percent：`int((i + 1) * 100 / segments.size())`，最后一段给 100。
- 读回显：`m_t.readBulk(512, LibusbEubTransport::kReadTimeoutMs ...)` — ⚠️ 但 session 不应依赖
  libusb 头。**定义一个会话内的常量** `kReadEchoTimeoutMs = 50`（facts §B2/hubble.py:115），
  并在注释里指向 `eub_libusb_transport.h` 的同名常量。**不要** include libusb 相关头。

CMakeLists：测试源清单加 `tests/test_eub_session.cpp`；extra sources 分支 =
`src/core/eub/eub_session.cpp` + `eub_loadout.cpp` + `eub_protocol.cpp`（**不含**任何 libusb 源 ——
会话只经 IEubTransport，这是本层的依赖契约，也让构建图可见）。

- [ ] **Step 4: 跑测试确认 GREEN**

Run: `cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_eub_session && ./build/image_engine_tests_test_eub_session`
Expected: `Totals: 12 passed, 0 failed, 0 skipped`（本任务用例 slots 10 + 2）

- [ ] **Step 5: 提交**

```bash
git add src/core/eub/eub_session.h src/core/eub/eub_session.cpp tests/test_eub_session.cpp CMakeLists.txt
git commit -m "feat(eub): 会话编排（逐段重开+重试、SoC 核对、失败语义、进度）"
```

---

### Task 7: 救援对话框 + FlashPanel 入口（UI）

**Files:**
- Create: `src/ui/eub_recovery_dialog.h`
- Create: `src/ui/eub_recovery_dialog.cpp`
- Test: `tests/test_eub_recovery_dialog.cpp`
- Modify: `src/ui/flash_panel.cpp`（模式感知块 + 点击分支 + include）
- Modify: `CMakeLists.txt`（测试源 + extra sources + Widgets 列表 + offscreen ENVIRONMENT 列表）

**Interfaces:**
- Consumes: `eub::IEubTransport` / `eub::LibusbEubTransport`（Task 1）、`eub::EubSession`（Task 6）、
  `eub::EubLoadout`（Task 4）、`eub::loadSbootBytes`（Task 5）、`tests/mock_eub_transport.h`（Task 3）
- Produces: `EubRecoveryDialog`（**传输必须注入**：对话框不 new 真机传输，UI 层只依赖抽象）

- [ ] **Step 1: 写失败测试**

创建 `tests/test_eub_recovery_dialog.cpp`（Widgets 测试，需 offscreen，见 CMake 段）：

```cpp
// tests/test_eub_recovery_dialog.cpp
//
// 对话框的**离线可测面**：段表/摘要两个纯函数，以及"注入 mock 传输后的识别 + 载荷预检"路径。
// 真机流程（选择文件对话框 / 真机发送）本机无设备，facts §F1。
#include <QtTest>
#include <QCheckBox>

#include "ui/eub_recovery_dialog.h"
#include "mock_eub_transport.h"

namespace {
QByteArray syntheticSboot(quint64 bytes = 0x220000)
{
    QByteArray b(int(bytes), '\0');
    for (int i = 0; i < b.size(); ++i) b[i] = char((i * 5) & 0xFF);
    return b;
}
} // namespace

class TestEubRecoveryDialog : public QObject
{
    Q_OBJECT

private slots:
    void segmentTableListsEverySegmentWithHexRanges()
    {
        eub::EubLoadout lo; QString err;
        QVERIFY(eub::eubLoadoutFor(QStringLiteral("Exynos9610"), lo, &err));
        const QString text = EubRecoveryDialog::segmentTableText(lo);
        for (const eub::EubSegment &s : lo.segments)
            QVERIFY2(text.contains(s.name), qPrintable(s.name));
        QVERIFY(text.contains(QStringLiteral("0x2000")));       // fwbl1 的长度
        QVERIFY(text.contains(lo.evidence));                    // 证据等级随表展示（facts §F3）
        QVERIFY(text.contains(lo.sourceNote));
    }

    void sha1CompareTextDistinguishesMatchAndMismatch()
    {
        const QByteArray a = QByteArray("a9993e364706816aba3e25717850c26c9cd0d89d");
        const QByteArray b = QByteArray("0000000000000000000000000000000000000000");
        QVERIFY(EubRecoveryDialog::sha1CompareText(a, a).contains(QStringLiteral("一致")));
        const QString diff = EubRecoveryDialog::sha1CompareText(a, b);
        QVERIFY(diff.contains(QStringLiteral("不一致")));
        QVERIFY(diff.contains(QStringLiteral("a9993e36")));     // 给出前 8 位便于人工核对（spec §D6）
        // 表未记录修订（hubble 系）→ 明说不知道，而不是假装一致
        QVERIFY(EubRecoveryDialog::sha1CompareText(QByteArray(), b).contains(QStringLiteral("未记录")));
    }

    void prepareIdentifiesDeviceAndPreviewsSegments()
    {
        eub::MockEubTransport mock;
        mock.info.socName = QStringLiteral("Exynos9610");
        mock.info.socId = QStringLiteral("0123456789abcde");
        EubRecoveryDialog dlg(mock, nullptr);

        QString err;
        QVERIFY2(dlg.prepare(syntheticSboot(), QStringLiteral("合成 sboot（用例）"), &err), qPrintable(err));
        QCOMPARE(dlg.loadout().soc, QStringLiteral("Exynos9610"));
        QVERIFY(dlg.statusText().contains(QStringLiteral("Exynos9610")));
        QVERIFY(dlg.statusText().contains(QStringLiteral("双源一致")));
    }

    void prepareRejectsUnsupportedSoc()
    {
        eub::MockEubTransport mock;
        mock.info.socName = QStringLiteral("Exynos9999");
        EubRecoveryDialog dlg(mock, nullptr);
        QString err;
        QVERIFY(!dlg.prepare(syntheticSboot(), QString(), &err));
        QVERIFY(!err.isEmpty());
    }

    void prepareRejectsTooShortImage()
    {
        eub::MockEubTransport mock;
        mock.info.socName = QStringLiteral("Exynos9610");
        EubRecoveryDialog dlg(mock, nullptr);
        QString err;
        QVERIFY(!dlg.prepare(QByteArray(0x1000, '\xAB'), QString(), &err));
        QVERIFY(!err.isEmpty());
    }

    void startButtonIsGatedByConfirmationCheckbox()
    {
        eub::MockEubTransport mock;
        mock.info.socName = QStringLiteral("Exynos9610");
        EubRecoveryDialog dlg(mock, nullptr);
        QString err;
        QVERIFY(dlg.prepare(syntheticSboot(), QString(), &err));
        QCheckBox *box = dlg.findChild<QCheckBox *>();
        QVERIFY(box != nullptr);
        QVERIFY(!dlg.isStartEnabledForTest());
        box->setChecked(true);
        QVERIFY(dlg.isStartEnabledForTest());      // 未验证勾选门控（沿用 Phase C 惯例，spec §D6）
    }
};

QTEST_MAIN(TestEubRecoveryDialog)
#include "test_eub_recovery_dialog.moc"
```

- [ ] **Step 2: 跑测试确认 RED**

Run: `cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_eub_recovery_dialog`
Expected: 失败（`ui/eub_recovery_dialog.h` 不存在）

- [ ] **Step 3: 实现**

`src/ui/eub_recovery_dialog.h`（**传输注入，不 new 真机传输** —— UI 层只依赖抽象，真机实现在
FlashPanel 侧构造）：

```cpp
// src/ui/eub_recovery_dialog.h
//
// EUB 救援对话框（设计 spec §D9/§D10）：识别设备 → 选 sboot 来源 → 段表 + sha1 对照预览 →
// 勾选"未验证"确认 → 开始 → 进度日志 → 交接提示（设备应已进入 Download 模式，请继续刷写）。
// 载荷一律由用户自备（facts §F8），本对话框不内置任何三星二进制。
#pragma once
#include <QDialog>
#include <QString>
#include <functional>

#include "core/eub/eub_loadout.h"
#include "core/eub/eub_session.h"
#include "core/eub/eub_transport.h"

class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;

class EubRecoveryDialog : public QDialog
{
    Q_OBJECT

public:
    // transport 由调用方持有（真机 = LibusbEubTransport；用例 = MockEubTransport）
    explicit EubRecoveryDialog(eub::IEubTransport &transport, QWidget *parent = nullptr);

    // 主窗口日志汇（FlashPanel 接到 outputMessage）
    void setLogSink(const std::function<void(const QString &, bool)> &sink) { m_logSink = sink; }

    // 识别 + 载荷预检 + 查表 + 切段试算（"选择文件之后"的同一段逻辑；用例直接调它）
    bool prepare(const QByteArray &sboot, const QString &sourceDescription, QString *error);

    const eub::EubLoadout &loadout() const { return m_loadout; }
    QString statusText() const;
    bool isStartEnabledForTest() const;     // 仅用例用：门控状态

    // 纯函数（离线可测）
    static QString segmentTableText(const eub::EubLoadout &lo);
    static QString sha1CompareText(const QByteArray &tableSha1, const QByteArray &fileSha1);

private slots:
    void onPickSource();
    void onStart();

private:
    void log(const QString &line, bool isError = false);
    void refreshStartEnabled();

    eub::IEubTransport &m_transport;
    eub::EubOptions      m_opt;
    eub::EubSession      m_session;
    std::function<void(const QString &, bool)> m_logSink;

    QLabel         *m_deviceLabel  = nullptr;
    QLabel         *m_sourceLabel  = nullptr;
    QLabel         *m_shaLabel     = nullptr;
    QPlainTextEdit *m_segmentView  = nullptr;
    QPlainTextEdit *m_logView      = nullptr;
    QCheckBox      *m_confirmBox   = nullptr;
    QPushButton    *m_startBtn     = nullptr;

    QByteArray       m_sboot;
    eub::EubLoadout  m_loadout;
    bool             m_prepared = false;
};

```

`src/ui/eub_recovery_dialog.cpp` 要点：
- 构造：`m_session(m_transport, m_opt, progress)`（progress 回调 → `log()` 逐行 + 更新窗口标题/状态）。
  布局用 `QVBoxLayout`：设备信息 QLabel → 「选择 sboot 来源…」按钮 → 来源 QLabel → sha1 QLabel →
  段表 `QPlainTextEdit`（只读）→ 确认 `QCheckBox("我理解该布局表绑定特定固件修订，分段偏移可能与我的固件不符")`
  → 日志 `QPlainTextEdit`（只读）→ 底部 `QDialogButtonBox`（`m_startBtn` = "开始救援" + Close）。
- `onPickSource`：`QFileDialog::getOpenFileName(this, "选择 sboot 来源", QString(), "sboot 镜像 (*.bin *.lz4);;"
  "三星 BL 包 (*.tar.md5 *.tar);;所有文件 (*)")` → `eub::loadSbootBytes` → `prepare(...)`；
  失败 → `QMessageBox::warning` + `log(err, true)`。
- `prepare`：`m_session.identify(m_loadout, error)`（失败 → false）→ `eub::splitSboot(sboot, m_loadout, parts, error)`
  （**试算**：太短当场拒绝，不等到点开始）→ 填 `m_segmentView`（`segmentTableText`）与 `m_shaLabel`
  （`sha1CompareText(m_loadout.sbootSha1, sha1Hex(sboot).toLatin1())`）→ `m_prepared = true` →
  `refreshStartEnabled()`。
  SoC 名为空时的兜底（facts §A4）：`eub::detectSocFromImage(sboot)` 得到名字后**再查一次表**；
  两次都失败才报错，且错误里说明"设备未报 SoC 名，也无法从镜像识别"。
- `onStart`：`m_session.run(m_loadout, m_sboot, &err)`；成功 →
  `QMessageBox::information(this, "EUB 救援", "分段已全部发送。设备应已进入 Download 模式，"
  "请继续用三星刷写（BL/AP/CP/CSC）刷入固件。\n\n注意：本流程只向设备 RAM 发送了引导镜像，"
  "未写任何存储，设备仍需正常刷写。")`；失败 → `QMessageBox::critical` + `log(err, true)`。
  **文案必须含"未写存储/仍需刷写"**（facts §F5/§D1）。
- `segmentTableText`：逐段一行 `"%1  offset 0x%2  length 0x%3（%4 字节）"`，末尾附
  `证据：<evidence>` 与 `来源：<sourceNote>`，再附 `extraFiles`（非空时）与 `responseSupport` 说明，
  以及**帧风格出处**（facts §B6 要求"UI 必须说明照抄自哪个实现"，本任务承接 —— T3 无 UI 改动的遗留）：
  风格头为 `1b444e57` → 显示"帧风格：照抄 hubble（头 `1B 44 4E 57` / 尾 `FF FF`）；该两字段语义未定"；
  为 `00000000` → 显示"帧风格：照抄 exynos-usbdl（头 `00 00 00 00` / 尾 `00 00`）；该两字段语义未定"。
- `sha1CompareText`：表为空 → `"该表未记录固件修订（无法对照）；你的文件 sha1 前 8 位 <x>"`；
  相同 → `"sha1 一致"`；不同 → `"sha1 不一致：表 <前8位> / 你的文件 <前8位>（布局偏移可能不匹配）"`。
- `refreshStartEnabled`：`m_startBtn->setEnabled(m_prepared && m_confirmBox->isChecked())`；
  `m_confirmBox` 的 `toggled` 也接 `refreshStartEnabled`。

`src/ui/flash_panel.cpp` 三处改动：

1. include 区（与 `#include "samsung_plan_dialog.h"` 同处）加：
```cpp
#include "eub_recovery_dialog.h"                     // EUB 救援（Exynos USB Boot）
#include "core/eub/eub_libusb_transport.h"           // 真机传输在本面板构造，对话框只认抽象
```
2. 模式感知块（`:355-372`）：条件里加 `|| m_deviceInfo.mode == DeviceDetector::MODE_SAMSUNG_EUB`，
   并把 `setToolTip` 那段（含 MTK 三元表达式）替换为：
```cpp
        m_flashBtn->setEnabled(true);
        if (m_deviceInfo.mode == DeviceDetector::MODE_SAMSUNG_EUB) {
            m_flashBtn->setText(QStringLiteral("EUB 救援…"));
            m_flashBtn->setToolTip(QStringLiteral(
                "EUB 救援：用你自备的原厂 BL（sboot.bin / BL_*.tar.md5）按 SoC 布局表分段注入设备 RAM，"
                "把设备引导进 Download 模式；本流程只发 RAM 镜像、不写存储，完成后请继续用三星刷写"));
        } else {
            m_flashBtn->setText(QStringLiteral("刷入"));
            m_flashBtn->setToolTip(m_deviceInfo.mode == DeviceDetector::MODE_MTK_BROM
                ? QStringLiteral("协议通道按计划刷写：DA + 镜像 → 计划预览 → 按设备代际自动选择 "
                                 "LEGACY / XFLASH / XML 链（三代均已实现，实际链路见日志「代际判定」）")
                : QStringLiteral("协议通道整包/按计划刷写（按模式选择 update.app / pac+FDL / DA+镜像 / 三星 tar.md5）"));
        }
        return;
```
3. `onFlashClicked()`（`:664`）在 `const DeviceDetector::DeviceMode deviceMode = ...;` 之后插入：
```cpp
    // EUB 不是刷写模式：本按钮在 EUB 设备上是「EUB 救援…」入口（先把设备引导进 Download 模式，
    // 之后才谈得上刷写）；不进入通道分派，也不走分区列表路径（对本模式无意义）。
    if (deviceMode == DeviceDetector::MODE_SAMSUNG_EUB) {
        eub::LibusbEubTransport transport;
        EubRecoveryDialog dlg(transport, this);
        dlg.setLogSink([this](const QString &m, bool isErr) { emit outputMessage(m, isErr); });
        dlg.exec();
        return;
    }
```

CMakeLists：测试源清单加 `tests/test_eub_recovery_dialog.cpp`；extra sources 分支 =
`src/ui/eub_recovery_dialog.cpp` + `src/core/eub/eub_session.cpp` + `eub_loadout.cpp` +
`eub_protocol.cpp` + `eub_payload.cpp`；Widgets 列表（`test_flash_plan_dialog` 那一段）加本 stem；
offscreen ENVIRONMENT 列表（同段）加本 stem。**本目标不需要 libusb**（对话框只认抽象）。

- [ ] **Step 4: 跑测试确认 GREEN**

Run: `cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_eub_recovery_dialog && ./build/image_engine_tests_test_eub_recovery_dialog`
Expected: `Totals: 8 passed, 0 failed, 0 skipped`（本任务用例 slots 6 + 2；无 DISPLAY 环境靠 `QT_QPA_PLATFORM=offscreen`）

- [ ] **Step 5: 提交**

```bash
git add src/ui/eub_recovery_dialog.h src/ui/eub_recovery_dialog.cpp tests/test_eub_recovery_dialog.cpp src/ui/flash_panel.cpp CMakeLists.txt
git commit -m "feat(eub): 救援对话框（段表/sha1 对照/未验证勾选/交接提示）+ FlashPanel 三星 EUB 入口"
```

---

### Task 8: 文档、清单与终验

**Files:**
- Modify: `功能清单.txt`
- Modify: `README.md`
- Modify: `docs/superpowers/specs/2026-09-17-exynos-eub-design.md`（状态行）
- Modify: `docs/superpowers/specs/exynos-eub-facts.md`（若实施期发现新事实）

**Interfaces:** 无代码接口（纯文档与验证）

- [ ] **Step 1: 更新 `功能清单.txt`**

把「排队中 (特殊模式)」里的 `[ ] 三星 Exynos EUB 救援链 …` 移入已交付区（与 Phase C 三星条目相邻），
写成 `[√]` 行，**必须包含**：
- 能力：检测 `0x04E8:0x1234` / 读 SoC 自述 / 8 个 SoC 布局表 / 分段注入 RAM / 交给 Odin 链；
- 证据：mock + 数值断言（**本线无真样本**）；
- 边界（**逐条写，不得省略**）：真机未验证；帧头/尾两字段语义未定（三实现矛盾）；
  4 个 SoC 单源、7580 第 3 段两源分歧已采信 hubble；布局表绑固件修订；
  EUB 需引导失败/测试点且 2025-04 起可能被 eFuse 封堵；只发 RAM 不写存储；
  **不做**未签名代码加载/签名绕过/eFuse/内置三星二进制。

- [ ] **Step 2: 更新 `README.md`**

- 设备模式表/亮点行：加入「三星 Exynos EUB 救援（EUB → Download 模式）」一行，注明载荷自备与边界；
- 测试目标计数：**以实测为准** —— 跑 `ctest -N | tail -1`（或统计 ctest 目标数）后填真实数字，
  **不要**照抄旧值或估算；
- 许可证段：补 `reference/exynos-usbdl`（GPL-3.0-or-later）、`reference/hubble`（GPL-2.0）、
  两个救援包（未声明许可）—— 措辞沿用既有口径："仅作事实参照、gitignored、不分发、不构建、
  非自有设备"。

- [ ] **Step 3: 更新设计 spec 状态行**

`docs/superpowers/specs/2026-09-17-exynos-eub-design.md` 顶部 `> **状态**：待用户评审…`
→ `> **状态**：已评审通过（2026-09-17）；实施见 docs/superpowers/plans/2026-09-17-exynos-eub-recovery.md`。

- [ ] **Step 4: 终验（原始输出）

```bash
# ① 全新目录构建（/tmp 满，必须用 /home）——"0 警告"的**唯一**合格证据
rm -rf /home/DslsDZC/eub-final && cmake -B /home/DslsDZC/eub-final -G Ninja -DMTK_SAMPLES_REQUIRED=ON \
      -DCMAKE_CXX_FLAGS="-Wall -Wextra -Wsign-compare"   # 见 Global Constraints 第 6 条：裸构建的 0 警告无甄别力
cmake --build /home/DslsDZC/eub-final 2>&1 | tee /home/DslsDZC/eub-final-build.log | tail -5
grep -ci warning /home/DslsDZC/eub-final-build.log     # 期望 0

# ② 全量测试（真样本目标必须 0 skipped —— 本线新目标无真样本，如实标注）
ctest --test-dir /home/DslsDZC/eub-final -j4

# ③ 逐个新目标报告（0 skipped 是判据）
for t in test_eub_transport test_eub_samsung_mode test_eub_protocol test_eub_loadout \
         test_eub_payload test_eub_session test_eub_recovery_dialog; do
  ./home/DslsDZC/eub-final/image_engine_tests_$t 2>&1 | tail -1
done
```

Expected：构建 `BUILD_RC=0` 且 warning 计数 0；`ctest` 100% 通过；7 个新目标各自
`Totals: N passed, 0 failed, 0 skipped`。

- [ ] **Step 5: 提交**

```bash
git add 功能清单.txt README.md docs/superpowers/specs/2026-09-17-exynos-eub-design.md
git commit -m "docs(eub): 功能清单/README/spec 状态 —— 交付与边界（真机未验证，无真样本）"
```

---

## 计划自查（写计划时已跑）

**1. spec 覆盖**：spec §9 的 8 条交付清单 ↔ 本计划任务映射 ——
① 传输层+检测 → T1+T2；② 协议层 → T3；③ 布局层 → T4；④ 会话层 → T6；⑤ 载荷获取 → T5；
⑥ UI → T7；⑦ 文案/文档/清单 → T2（检测显示名）+ T7（按钮文案）+ T8；⑧ 终验 → T8。
spec §10 的 Q1–Q7 决策已全部落进对应任务（Q1 → T4 的 8 张表；Q2 → T4 逐表风格；
Q3 → T4 的 7580 采信值 + 注释；Q4 → T7 的勾选门控；Q5 → T6 的回显只读一次；
Q6 → T7 的 FlashPanel 入口；Q7 → 参照仓库已在 `reference/`）。

**2. 占位扫描**：无 TBD/TODO；每个代码步骤给出可照抄的完整片段；数值全部来自 spec §5.4 与 facts。

**3. 类型一致性**：跨任务引用的名字已对齐 ——
`eub::IEubTransport`（T1）→ T3/T6/T7 消费；`eub::EubFrameStyle`/`zeroStyle`/`dnwStyle`（T3）→ T4 消费；
`eub::EubLoadout`/`EubSegment`/`eubLoadoutFor`/`splitSboot`/`sha1Hex`/`detectSocFromImage`（T4）→ T6/T7 消费；
`eub::loadSbootBytes`/`SbootSource`（T5）→ T7 消费；`EubSession::identify/run`（T6）→ T7 消费；
`MockEubTransport`（T3 建）→ T6/T7 复用。字段名以各任务 Step 3 的头文件为准（`sbootSha1`，非
`sboutSha1` 等异体）。

**4. 已知风险（实施期注意）**：
- 每个新测试目标都要在 `CMakeLists.txt` 的三个位置登记（测试源清单 / extra sources / 按名的
  libusb 或 Widgets 列表）；漏一处是**链接期**报错而非编译错（本仓踩过）。
- 改 `CMakeLists.txt` 后**先重配再 build**。
- 本线**没有真样本**：任何"看起来像真样本验证"的措辞都是越界（Global Constraints 第 4 条）。

