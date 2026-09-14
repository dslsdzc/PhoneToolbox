// src/core/odin/odin_libusb_transport.cpp
//
// 结构照抄 Phase B 的 src/core/edl/edl_libusb_transport.cpp（本仓已验证的同款真机实现：
// open/枚举/打开/认领/端点发现/短写拒绝/超时语义），差异只有三处：
//   ① 身份判据换成本任务的 isOdinDevice（接口类 0x0A + 批量端点，老 PID 兜底；三方对照见头文件）；
//   ② 接口号与端点**不写死**：Thor/odin4/samloader-rs 都是遍历描述符取，Heimdall 的固定端点值
//      只适用于 2011 年的 3 个老机型（docs/superpowers/specs/samsung-odin-facts.md §1.5）；
//   ③ 空写 = 真的发一个 0 长度 bulk 传输（ZLP，见 odin_transport.h:23-24），不按包长自行补包。
// 真机行为（枚举/claim 顺序/时序/ZLP）**离线无法验证**：本阶段无真机，见 tests 头注释与
// phaseC-task-7-report.md §边界。
#include "odin_libusb_transport.h"

#include <libusb.h>

#include <QStringList>
#include <limits>   // std::numeric_limits（write 的 int 长度上限判定）

namespace odin {
namespace {

constexpr quint16 kSamsungVid   = 0x04e8;   // Thor Communication/USB.cs:6 / Heimdall BridgeManager.h:71
constexpr quint8  kCdcDataClass = 0x0a;     // USB_CLASS_CDC_DATA（Thor Linux.cs:134；samloader-rs mod.rs:59）

QString hex4(quint16 v)
{
    return QString::number(v, 16).rightJustified(4, QLatin1Char('0'));
}

void setErr(QString *error, const QString &msg)
{
    if (error) *error = msg;
}

// 一次描述符巡检的结果。
// 候选接口的选择口径（三家一致）：优先 **class 0x0A(CDC_DATA) 且同时具备批量 IN/OUT** 的接口；
// 没有这样的接口时退回第一个同时具备批量 in/out 的接口 —— 老 PID 兜底的机型可能不按 CDC 类
// 枚举（Heimdall 时代的主机侧连端点都是写死的，正说明那批设备的描述符不遵循 CDC 布局）。
struct DeviceInspection {
    QList<quint8> classes;          // 全部接口（含各 altsetting）的 bInterfaceClass → isOdinDevice
    bool hasBulkInOut = false;      // 存在某个接口同时具备批量 IN 与批量 OUT（设备级事实，同上）
    int  iface  = -1;               // 候选接口号（-1 = 没有任何接口带批量 in/out）
    int  epIn   = 0;
    int  epOut  = 0;
};

DeviceInspection inspectDevice(libusb_device *dev)
{
    DeviceInspection out;
    libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != LIBUSB_SUCCESS || !cfg)
        return out;   // 描述符读不到：类表为空、无批量端点 → 只剩老 PID 兜底（isOdinDevice）

    bool haveCdc = false;
    for (int ic = 0; ic < int(cfg->bNumInterfaces); ++ic) {
        for (int a = 0; a < int(cfg->interface[ic].num_altsetting); ++a) {
            const libusb_interface_descriptor *alt = &cfg->interface[ic].altsetting[a];
            out.classes.append(quint8(alt->bInterfaceClass));

            int epIn = 0;
            int epOut = 0;
            for (int e = 0; e < int(alt->bNumEndpoints); ++e) {
                const libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                    continue;   // 只看批量端点（CDC 的控制/中断端点不走这里，Thor 同款：非批量即作废）
                if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN)
                    epIn = ep->bEndpointAddress;
                else
                    epOut = ep->bEndpointAddress;
            }
            if (epIn == 0 || epOut == 0)
                continue;   // 只带单方向的接口不是下载接口

            out.hasBulkInOut = true;
            if (out.iface < 0) {            // 第一个"带批量 in/out 的接口"作为退路
                out.iface = alt->bInterfaceNumber;
                out.epIn  = epIn;
                out.epOut = epOut;
            }
            if (alt->bInterfaceClass == kCdcDataClass && !haveCdc) {
                // CDC_DATA 命中即覆盖退路（且不被后续的非 CDC 接口覆盖：iface<0 的退路只走一次）
                out.iface = alt->bInterfaceNumber;
                out.epIn  = epIn;
                out.epOut = epOut;
                haveCdc = true;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// 纯函数：设备身份与超时换算
// ---------------------------------------------------------------------------

bool LibusbOdinTransport::isOdinDevice(quint16 vid, quint16 pid,
                                       const QList<quint8> &interfaceClasses, bool hasBulkInOut)
{
    if (vid != kSamsungVid)
        return false;    // 非三星一律不认：高通 EDL（0x05C6）/Google（0x18D1）都不是 Odin
    if (fallbackPids().contains(pid))
        return true;     // 老 PID 兜底：PID 命中即认，不再看描述符（Heimdall 口径，头文件 :35-38）
    return interfaceClasses.contains(kCdcDataClass) && hasBulkInOut;
}

QList<quint16> LibusbOdinTransport::fallbackPids()
{
    // Heimdall BridgeManager.h:76-78 的三个老 PID（顺序同上游声明序：Galaxy S / Galaxy S2 / Droid Charge）
    return {0x6601, 0x685D, 0x68C3};
}

QString LibusbOdinTransport::noDeviceError()
{
    QStringList pids;
    for (quint16 pid : fallbackPids())
        pids << hex4(pid);
    return QStringLiteral("未找到三星 Odin 下载模式设备（期望 VID %1：接口类 0a/CDC_DATA + 批量 in/out 端点，"
                          "或老机型 PID %2）")
        .arg(hex4(kSamsungVid), pids.join(QLatin1Char('/')));
}

int LibusbOdinTransport::effectiveTimeoutMs(int requested)
{
    // libusb 的 0 = **无限等待**（sync.c "For an unlimited timeout, use value 0"），而本接口的
    // 0 = 非阻塞轮询 —— 照搬传 0 会让会话的握手清端点在真机永久阻塞（头文件 :40-46）。
    // 取 1 ms：轮询只问"端点里现在有没有字节"，1 ms 对任何 USB 速率都够一次往返；
    // bkerler 同款（edl/edlclient/Library/Connection/usblib.py:380-381 的 `if timeout == 0: timeout = 1`）。
    return requested > 0 ? requested : 1;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

LibusbOdinTransport::LibusbOdinTransport() = default;

LibusbOdinTransport::~LibusbOdinTransport()
{
    close();
}

bool LibusbOdinTransport::open(QString *error)
{
    if (m_dev) close();     // 契约：close 后可再 open（重复 open 先收尾，避免拿两个句柄）

    int ret = libusb_init(&m_ctx);
    if (ret != LIBUSB_SUCCESS) {
        setErr(error, QStringLiteral("libusb_init 失败：%1").arg(QString::fromLatin1(libusb_error_name(ret))));
        m_ctx = nullptr;
        return false;
    }

    libusb_device **list = nullptr;
    const ssize_t count = libusb_get_device_list(m_ctx, &list);
    if (count < 0) {
        setErr(error, QStringLiteral("枚举 USB 设备失败：%1")
                          .arg(QString::fromLatin1(libusb_error_name(int(count)))));
        libusb_exit(m_ctx);
        m_ctx = nullptr;
        return false;
    }

    bool found = false;
    for (ssize_t i = 0; i < count && !found; ++i) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS)
            continue;
        if (desc.idVendor != kSamsungVid)
            continue;    // VID 不符：连描述符都不必细看
        const DeviceInspection insp = inspectDevice(list[i]);
        if (!isOdinDevice(desc.idVendor, desc.idProduct, insp.classes, insp.hasBulkInOut))
            continue;
        if (insp.iface < 0)
            continue;    // 身份认得出但没有可用的批量接口（传不了）→ 试下一个候选
        if (libusb_open(list[i], &m_dev) == LIBUSB_SUCCESS) {
            m_iface = insp.iface;
            m_inEp  = insp.epIn;
            m_outEp = insp.epOut;
            found = true;
        } else {
            m_dev = nullptr;   // 该设备打不开（权限/被占用）→ 继续找下一个
        }
    }
    libusb_free_device_list(list, 1);

    if (!found) {
        setErr(error, noDeviceError());
        libusb_exit(m_ctx);
        m_ctx = nullptr;
        return false;
    }

    if (!claimInterface(error)) {
        close();                  // 半开状态不留：失败即回到"未打开"
        return false;
    }
    return true;
}

bool LibusbOdinTransport::claimInterface(QString *error)
{
    // claim → 失败则 detach 内核驱动 → 再 claim（既有顺序，edl_libusb_transport.cpp:182-193 同款）：
    // Odin 设备的 CDC_DATA 接口在 Linux 上通常被 cdc_acm 占用，第一次 claim 会 LIBUSB_ERROR_BUSY。
    int ret = libusb_claim_interface(m_dev, m_iface);
    if (ret != LIBUSB_SUCCESS) {
        const int det = libusb_detach_kernel_driver(m_dev, m_iface);
        if (det == LIBUSB_SUCCESS || det == LIBUSB_ERROR_NOT_FOUND)
            ret = libusb_claim_interface(m_dev, m_iface);
    }
    if (ret != LIBUSB_SUCCESS) {
        setErr(error, QStringLiteral("认领 Odin 接口 %1 失败：%2")
                          .arg(m_iface).arg(QString::fromLatin1(libusb_error_name(ret))));
        return false;
    }
    return true;
}

void LibusbOdinTransport::close()
{
    if (m_dev) {
        if (m_iface >= 0)
            libusb_release_interface(m_dev, m_iface);
        libusb_close(m_dev);
        m_dev = nullptr;
    }
    if (m_ctx) {
        libusb_exit(m_ctx);
        m_ctx = nullptr;
    }
    // 接口/端点回到"未知"：open() 的候选选择必须每次重新从描述符取，不能留着上次的值
    m_iface = -1;
    m_inEp  = 0;
    m_outEp = 0;
}

// ---------------------------------------------------------------------------
// 数据面
// ---------------------------------------------------------------------------

bool LibusbOdinTransport::write(const QByteArray &data, QString *error)
{
    if (!m_dev) {
        setErr(error, QStringLiteral("写入失败：Odin 设备未打开"));
        return false;
    }
    if (data.isEmpty()) {
        // 空写 = ZLP：**真的**发一个 0 长度 bulk 传输（odin_transport.h:23-24）。odin4 同款：
        // send_empty_transfer()（odin_protocol.cpp:294-302）用 100 ms 超时；失败**在此返回 false**，
        // "失败是否致命"由发起方裁定（会话按 odin4 口径忽略它，见 odin_session.cpp 的 D7 空写）。
        int transferred = 0;
        const int ret = libusb_bulk_transfer(m_dev, m_outEp, nullptr, 0, &transferred, kZlpTimeoutMs);
        if (ret != LIBUSB_SUCCESS) {
            setErr(error, QStringLiteral("发送 ZLP 失败：%1")
                              .arg(QString::fromLatin1(libusb_error_name(ret))));
            return false;
        }
        return true;
    }

    // 单块长度必须塞得进 libusb 的 `int length`。今天这条**不可达**的真实理由是**上游已有定表**：
    // 单块尺寸来自 `profileForVersion()` 的固定档（最大 kPartSizeDefaultLarge = 1 MiB，odin_protocol.h），
    // **不是**设备回报值。留着是因为将来若改成更大的块/64 位缓冲，这里必须**拒绝**而不是窄化。
    // （注意：Qt6 的 `QByteArray::size()` 返回 `qsizetype`（64 位），不能拿它当"塞不进 int"的论据。）
    if (data.size() > std::numeric_limits<int>::max()) {
        setErr(error, QStringLiteral("写入失败：单块 %1 字节超过 libusb 的 int 长度上限")
                          .arg(data.size()));
        return false;
    }

    int transferred = 0;
    const int ret = libusb_bulk_transfer(m_dev, m_outEp,
                                         reinterpret_cast<unsigned char *>(const_cast<char *>(data.constData())),
                                         static_cast<int>(data.size()), &transferred, kWriteTimeoutMs);
    if (ret != LIBUSB_SUCCESS) {
        setErr(error, QStringLiteral("写入 OUT 端点失败：%1")
                          .arg(QString::fromLatin1(libusb_error_name(ret))));
        return false;
    }
    if (transferred != data.size()) {
        // 短写：bulk 传输在超时时可能少发 —— 绝不能当成功（会话会继续发下一片，设备侧错位）
        setErr(error, QStringLiteral("写入 OUT 端点短写：已发 %1/%2 字节")
                          .arg(transferred).arg(data.size()));
        return false;
    }
    return true;
}

QByteArray LibusbOdinTransport::read(int maxBytes, int timeoutMs, QString *error)
{
    if (!m_dev) {
        // 未打开时**连轮询也报错**（调用方拿它诊断"谁没 open"，而不是把空返回当"端点静默"）
        // —— 与 LibusbEdlTransport 同款（edl_libusb_transport.cpp:298-303）。
        setErr(error, QStringLiteral("读取失败：Odin 设备未打开"));
        return {};
    }
    if (maxBytes <= 0)
        return {};

    QByteArray buf(maxBytes, Qt::Uninitialized);
    int transferred = 0;
    // ⚠️ 绝不把 0 直接交给 libusb（0 = 无限等待，见 effectiveTimeoutMs 的注释）
    const int ret = libusb_bulk_transfer(m_dev, m_inEp,
                                         reinterpret_cast<unsigned char *>(buf.data()),
                                         maxBytes, &transferred,
                                         effectiveTimeoutMs(timeoutMs));
    if (ret == LIBUSB_ERROR_TIMEOUT) {
        // 轮询（timeoutMs <= 0，已被换算成 1 ms）：端点静默不是错误 —— 清端点语义，
        // 见 odin_transport.h:25-28（"立即返回端点里已有的字节，没有则空返回且不置 error"）
        if (timeoutMs <= 0)
            return {};
        setErr(error, QStringLiteral("读取 IN 端点超时（%1 ms）").arg(timeoutMs));
        return {};
    }
    if (ret != LIBUSB_SUCCESS) {
        setErr(error, QStringLiteral("读取 IN 端点失败：%1")
                          .arg(QString::fromLatin1(libusb_error_name(ret))));
        return {};
    }
    buf.truncate(transferred);
    return buf;
}

} // namespace odin
