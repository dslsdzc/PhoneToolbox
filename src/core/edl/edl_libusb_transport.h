// src/core/edl/edl_libusb_transport.h
//
// 真机传输：`IEdlTransport` 的 libusb 实现（Phase B Task 7）。协议模块（sahara/firehose/
// edl_session）只看见 `IEdlTransport`，libusb 只出现在本文件的 .cpp 里（design §3.1 依赖方向）。
//
// 设备阶段（`EdlUsbStage`）是**本类唯一的模式状态**：Sahara（programmer 载入前）与 Firehose
// （programmer 载入后、设备重枚举回来）的 **PID 表、接口号、端点号都不同**。这三张表搬自既有
// 实现（src/core/modes/edl_handler.cpp:9-21 的 PID/端点常量 + :111-129 的 claimInterface），
// 做成静态纯函数以便离线钉住 —— 重构中改错它们，真机上只表现为"设备没反应"。
//
// 本类**不补 ZLP**（一条规则一处责任，控制方裁定）：write() 是纯字节管道，无法区分"命令帧"与
// "数据块"，若在此处按包长整数倍补 ZLP，数据块会被会话与传输各补一次（多一个 ZLP，离线用例
// 看不见）。数据块的 ZLP 由会话数据面负责（edl_session.cpp 的 writeRaw），命令帧的 ZLP 由
// firehoseSendCommand 负责（firehose.cpp）——两者都是"发起该笔写的上层"。
//
// ⚠️ libusb 的 `timeout=0` 是**无限等待**，不是"立即返回"（sync.c 的 "For an unlimited timeout,
// use value 0"）。而 IEdlTransport 的 `read(timeoutMs=0)` 契约是**非阻塞轮询**（drain）——本类的
// read() 必须经 effectiveTimeoutMs() 换算（<=0 → 1 ms），否则会话每笔写前的 drain 会在真机上
// **永久阻塞**，而 mock 把 0 当轮询、离线用例全绿也发现不了。
#pragma once
#include <QByteArray>
#include <QString>
#include "edl_transport.h"

struct libusb_context;
struct libusb_device_handle;

namespace edl {

// 设备阶段：两阶段的身份与端点都不同（见上）
enum class EdlUsbStage { Sahara, Firehose };

class LibusbEdlTransport : public IEdlTransport
{
public:
    LibusbEdlTransport();
    ~LibusbEdlTransport() override;

    // 构造、析构与 close() **不触碰 USB 栈**（未打开时是纯内存对象）—— 离线用例可直接构造与断言。

    // ---- 纯函数：设备身份与端点表（离线可测；常量搬自 edl_handler.cpp:9-21,111-129）----
    // 9008 家族身份：VID 0x05C6 + PID {0x9008(Sahara), 0x900E(两阶段), 0x9025(Firehose)}
    static bool isEdlId(quint16 vid, quint16 pid);
    // 分阶段 PID 表（**不对称**：Sahara 不认 0x9025，Firehose 不认 0x9008；0x900E 两阶段都认
    // —— 部分机型载入 programmer 后不换 PID，原地切换模式）
    static bool matchesStage(EdlUsbStage stage, quint16 vid, quint16 pid);
    static int  interfaceNumber(EdlUsbStage stage);   // Sahara 0 / Firehose 1
    static int  outEndpoint(EdlUsbStage stage);       // Sahara 0x01 / Firehose 0x02
    static int  inEndpoint(EdlUsbStage stage);        // Sahara 0x82 / Firehose 0x83
    // 找不到设备时的中文文案（带阶段名与该阶段期望的 PID，直接落 UI 便于诊断"插错了模式"）
    static QString noDeviceError(EdlUsbStage stage);
    // read() 的超时换算（libusb 语义 vs IEdlTransport 契约，见文件头）：<=0（轮询/drain）→ 1 ms，
    // 正值原样。**绝不把 0 交给 libusb**（那是无限等待）。抽成纯函数是为了让这条修正在离线侧
    // 有回归保护（真机行为无法离线验证，但"换算存在且正确"可以）。
    static int  effectiveTimeoutMs(int requested);

    // 阶段状态。waitReenumerate 成功后自动进入 Firehose；**重新开始一次刷机前**（如 connectSahara
    // 重试）调用方须显式 setStage(Sahara)，否则 open() 会拿 Firehose 的 PID 表去找设备。
    void        setStage(EdlUsbStage stage);
    EdlUsbStage stage() const;

    bool isOpen() const;

    // ---- IEdlTransport ----
    bool open(QString *error) override;        // close 后可再 open（契约，edl_transport.h 顶部）
    void close() override;                     // 幂等；未打开时是 no-op
    bool write(const QByteArray &data, QString *error) override;   // 裸写 OUT，不补 ZLP（见文件头）
    QByteArray read(int maxBytes, int timeoutMs, QString *error) override;  // 0 = 轮询（经换算，见文件头）
    bool resetDevice(QString *error) override; // best-effort：未打开 = 无事可做 → true
    // 重枚举（Sahara→Firehose）：**调用前须已 close()**（未 close 直接失败，不替调用方 close ——
    // 契约 edl_transport.h:14-19 要求"设备消失又回来"由调用方显式表达）；**返回 true ⇒ 设备已
    // 重新 open()**，调用方不要再 open。timeoutMs = 总预算，轮询间隔 3s（先等一个间隔再首试）。
    bool waitReenumerate(int timeoutMs, QString *error) override;
    int  maxPacketSize() const override;       // 未打开 → 0（包长未知；会话据此不发 ZLP）

    static constexpr int kReenumPollIntervalMs = 3000;  // 既有 3s 轮询（edl_handler.cpp:589-614）
    static constexpr int kWriteTimeoutMs       = 10000; // 既有 TIMEOUT_MS（edl_handler.cpp:23）
    static constexpr int kFallbackMaxPacket    = 512;   // 描述符未给出包长时的兜底（USB 2.0 bulk）

private:
    // 认领接口 + 从活动配置描述符取该接口的批量端点与 OUT 包长（qdl 同款做法：
    // reference/qdl/src/usb.c:175-215 的接口/端点遍历）。失败 → false + 中文 error。
    bool claimAndDiscoverEndpoints(QString *error);

    libusb_context       *m_ctx = nullptr;
    libusb_device_handle *m_dev = nullptr;
    EdlUsbStage m_stage = EdlUsbStage::Sahara;
    int m_iface = -1;
    int m_outEp = 0;
    int m_inEp = 0;
    int m_outMaxPacket = 0;    // 0 = 包长未知（未打开）→ maxPacketSize() 返回 0
};

} // namespace edl
