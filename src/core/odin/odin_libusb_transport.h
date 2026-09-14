// src/core/odin/odin_libusb_transport.h
//
// 真机传输：`IOdinTransport` 的 libusb 实现（Phase C Task 7）。会话（odin_session）只看见
// `IOdinTransport`；odin 侧的 libusb 依赖只出现在本文件的 .cpp 里（设备缝，odin_transport.h:8-11）。
//
// 设备身份判据（**纯函数** `isOdinDevice` / `fallbackPids`，与 Task 8 的设备检测**共用同一判据**）
// —— 三方对照（三处参照都在 reference/，gitignored，只编路径不编内容）：
//   ① Thor（`reference/thor/TheAirBlow.Thor.Library/`，社区维护的官方续作；facts §2.4）：`Communication/USB.cs:6`
//      只有 `Vendor = 0x04E8`、**无 PID 白名单**；`Platform/Linux.cs:120-134` 遍历接口描述符，
//      命中条件 = `clss == 0x0a`(CDC_DATA) **且**同一接口里同时有合法的批量 in/out
//      （端点须 `(bmAttributes & 0x03) == 0x02`，否则该接口作废）。
//   ② odin4-llucs（`reference/odin4-llucs/`，现代 C++ 重写；facts §2.1）：`src/usb/usb_device.cpp:177` 给
//      `bInterfaceClass == 0x0A && bNumEndpoints == 2` 的接口满分（`:257,472` 的
//      `cdc_data = interface_class == 0x0A` 是同一判据在传输路径的复用）；PID 只作 +20 分加分
//      （`:250-253`），**不是**准入条件。
//   ③ samloader-rs（`reference/samloader-rs/`，Heimdall libpit 的 Rust 直系移植；facts §2.2）：同口径
//      —— "select the class USB_CLASS_CDC_DATA interface with 2 endpoints"
//      （`odin/src/usb/nusb.rs:57,79`；`odin/src/usb/mod.rs:59` 的 `USB_CLASS_CDC_DATA = 0x0A`）。
//   **为什么必须带类判据**：正常开机/充电的三星手机也是 VID 0x04E8（MTP 0x06 类 / ADB 0xFF 类）
//   —— 只按 VID 会把它们误报成"下载模式"。Heimdall 的 3 个老 PID 对现代机型完全不适用
//   （docs/superpowers/specs/samsung-odin-facts.md §1.5 的结论）。
//   → 判据 = `vid == 0x04E8` 且（接口类含 0x0A **且有**批量 in/out **或** pid ∈ fallbackPids()）。
//     老 PID 兜底表 = Heimdall `reference/heimdall/heimdall/source/BridgeManager.h:71-78`
//     （`kVidSamsung = 0x04E8`；`kPidGalaxyS 0x6601` / `kPidGalaxyS2 0x685D` /
//     `kPidDroidCharge 0x68C3`）—— 描述符读不到类信息（老机型不带 CDC 描述符、或被系统按别的类
//     枚举）时仍认这三个 PID：兜底表的语义就是"认这个 PID"，不再看类。
//   ⚠️ 纯函数的 `interfaceClasses` 与 `hasBulkInOut` 是**扁平的设备级事实**（Task 8 从它自己
//      枚举到的描述符喂进来，签名如此）："类与批量端点必须落在同一个接口上"这层配对由调用方在
//      open() 的候选选择里落实（优先 class 0x0A 且同时带批量 in/out 的接口，见 .cpp 的
//      inspectDevice）——纯函数只回答"这个 VID/PID/描述符组合算不算 Odin 设备"。
//
// 超时语义（Phase B 同款红线）：libusb 的 `timeout=0` 是**无限等待**（sync.c "For an unlimited
// timeout, use value 0"），而 `IOdinTransport::read(timeoutMs=0)` 的契约是**非阻塞轮询**
// （清端点语义，odin_transport.h:25-28）—— 必须经 effectiveTimeoutMs() 换算（<=0 → 1 ms），
// 否则会话的握手清端点（odin_session.cpp 的 `m_t.read(64, 0, nullptr)`）会在真机上**永久阻塞**，
// 而 mock 把 0 当轮询、离线用例全绿也发现不了。
//
// 本类**不补 ZLP**（一条规则一处责任）：`write(QByteArray())` 的语义就是"真发一个 0 长度 bulk
// 传输"（odin_transport.h:23-24），由会话在结束序列前显式发起（odin_session.cpp 的 D7 空写）；
// 本层不按包长自行补包 —— 与 Phase B 的 LibusbEdlTransport 同款分工。
//
// 本类**不持有 wMaxPacketSize**：收窄后的接口没有 maxPacketSize()（odin_transport.h 头注释：
// "ZLP 由会话的显式空写表达，不靠包长判定"），存下来无人消费即死状态 —— 任务书 Step 3 的
// "取 wMaxPacketSize"一条按此收窄执行，见 phaseC-task-7-report.md §自查。
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

#include "odin_transport.h"

struct libusb_context;
struct libusb_device_handle;

namespace odin {

class LibusbOdinTransport : public IOdinTransport
{
public:
    LibusbOdinTransport();
    ~LibusbOdinTransport() override;

    // 构造、析构与 close() **不触碰 USB 栈**（未打开时是纯内存对象）—— 离线用例可直接构造与断言。

    // ---- 纯函数：设备身份（离线可测；判据的三方出处见文件头）----
    // 命中 = vid == 0x04E8 且（接口类含 0x0A 且有批量 in/out 或 pid ∈ fallbackPids()）。
    static bool isOdinDevice(quint16 vid, quint16 pid,
                             const QList<quint8> &interfaceClasses, bool hasBulkInOut);
    // Heimdall 的 3 个老 PID（BridgeManager.h:76-78），顺序同上游声明序。
    static QList<quint16> fallbackPids();
    // 找不到设备时的中文文案（带 VID **与类判据**：只写 PID 会诱导用户把 MTP 模式当下载模式）。
    static QString noDeviceError();
    // read() 的超时换算（libusb 语义 vs IOdinTransport 契约，见文件头）：<=0（轮询）→ 1 ms，
    // 正值原样。**绝不把 0 交给 libusb**（那是无限等待）。抽成纯函数是为了让这条修正在离线侧
    // 有回归保护（真机行为无法离线验证，但"换算存在且正确"可以）。公开仅为单测（Phase B 同款）。
    static int effectiveTimeoutMs(int requested);

    // ---- IOdinTransport ----
    bool open(QString *error) override;        // close 后可再 open（契约，odin_transport.h:20-25）
    void close() override;                     // 幂等；未打开时是 no-op
    bool write(const QByteArray &data, QString *error) override;   // 裸写 OUT；**空数组 = ZLP**
    QByteArray read(int maxBytes, int timeoutMs, QString *error) override;  // 0 = 轮询（经换算，见文件头）

    // 写超时。⚠️ IOdinTransport::write 没有超时参数 —— 控制帧与数据片**共用**这一个常量，
    // 而 odin4 是分开的：控制帧 USB_TIMEOUT_CONTROL 10000 ms（src/usb/usb_device.h:16）、
    // 数据片 odin_flash_timeout_ms 30000/120000 ms（按协议版本，odin_protocol.cpp:400,404,535）。
    // 取 10000：= odin4 的控制值，且比另两家的**单一**写超时都宽（Heimdall kDefaultTimeoutSend
    // 3000，BridgeManager.h:83；Thor BulkWrite 5000，Platform/Linux.cs:188）。**代价**是 1 MiB
    // 数据片若被设备 NAK 超过 10 s 会被判失败 —— 真机未验证，留持机人（Task 7 报告 §疑虑）。
    static constexpr int kWriteTimeoutMs = 10000;
    // 空传输（ZLP）100 ms：odin4 `send_empty_transfer()`（odin_protocol.cpp:294-302）。
    static constexpr int kZlpTimeoutMs   = 100;

private:
    // 认领接口：claim → 失败则 detach 内核驱动 → 再 claim（既有顺序，见 .cpp 注释）。
    bool claimInterface(QString *error);

    libusb_context       *m_ctx = nullptr;
    libusb_device_handle *m_dev = nullptr;
    int m_iface = -1;          // 候选接口号（open() 里从描述符选定，-1 = 未打开）
    int m_outEp = 0;
    int m_inEp  = 0;
};

} // namespace odin
