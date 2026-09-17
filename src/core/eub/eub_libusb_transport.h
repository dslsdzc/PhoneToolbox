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
// （dltool.c:305）。但**不能无条件补设**：libusb 文档明说对**已配置**设备再设一次会"act as a
// lightweight device reset"（重发 SET_CONFIGURATION，altsetting 归零、端点 halt 清除、toggle 复位），
// 且"cannot change/reset configuration if your application has claimed interfaces"（BUSY 的确切
// 含义是**接口已被认领**，不是"配置已生效"；见 libusb api-1.0 `libusb_set_configuration` 文档）。
// 故只在"读不到活动配置"时才补设，且补设必须发生在 claim **之前**（文档同句："It is advised to
// set the desired configuration before claiming interfaces"）。
//
// 超时：写 50 s（hubble `timeout=50000` hubble.py:110；dltool 50*1000*1000 µs dltool.c:339
// —— 两家一致）；读 50 ms（hubble.py:115 的 `timeout=50`，仅用于段后回显）。
// libusb 的 timeout=0 = **无限等待**，故经 effectiveTimeoutMs() 换算（<=0 → 1 ms）。
//
// ⚠️ 真机路径未验证：本机无任何 Exynos 设备（facts §F1）。枚举/claim/端点发现/短写/超时的
// **真机时序**留持机人；离线用例只覆盖纯函数（tests/test_eub_transport.cpp）。
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

    // 构造、析构与 close() **不触碰 USB 栈**（未打开时是纯内存对象）—— 离线用例可直接构造。

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
