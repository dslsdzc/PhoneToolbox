// src/core/edl/edl_libusb_transport.cpp
//
// 真机传输实现（Phase B Task 7）。本文件与 src/core/modes/edl_handler.cpp 是全仓仅有的两处
// libusb 依赖；协议模块只见 IEdlTransport（见头文件顶部与本文件末尾的对照说明）。
//
// 搬来的既有逻辑（src/core/modes/edl_handler.cpp，**重写前**版本 = 提交 515cc57；改动点写在每处）：
//   * 设备身份表（:9-21 的 EDL_SAHARA_VID/PID + EDL_900E + EDL_FIREHOSE_PID）
//   * 接口/端点常量（:16-21 的 EDP_OUT/EDP_IN/FH_OUT/FH_IN + :111-129 claimInterface 的 iface 0/1）
//   * open/枚举/打开/认领/超时（:45-129）
//   * 重枚举轮询（:589-614：初等 msleep(3000) + 15 次重试、每次 msleep(2000) ⇒ 上限 33s，
//     本项目参数化为"总预算 + 3s 间隔"）
// 改动（有意为之，逐条已由离线用例或注释钉住）：
//   1. 端点与 OUT 包长改为从**活动配置描述符**取（qdl 同款：reference/qdl/src/usb.c:190-210），
//      取不到才退回阶段常量 —— 既有实现把端点号写死，且拿不到 wMaxPacketSize（ZLP 判据缺失）。
//   2. maxPacketSize() 未打开时返回 0（既有实现无此方法；0 = 包长未知，会话据此不发 ZLP）。
//   3. 未打开句柄时：write/read 报中文错误而不是静默失败（read(timeoutMs==0) 的 drain 语义见下）。
//   4. resetDevice() 只做句柄退场，不调用 libusb_reset_device（理由见实现处注释）。
#include "edl_libusb_transport.h"

#include <QElapsedTimer>
#include <QThread>
#include <libusb.h>

#include <limits>       // std::numeric_limits（write 的 int 长度上限判定）

namespace edl {
namespace {

// 设备身份（搬自 edl_handler.cpp（重写前 515cc57）:10-13）
constexpr quint16 kQcVid       = 0x05c6;
constexpr quint16 kPidSahara   = 0x9008;   // Sahara（9008 主形态）
constexpr quint16 kPidBothStages = 0x900e; // 两阶段都出现（部分机型不换 PID）
constexpr quint16 kPidFirehose = 0x9025;   // programmer 载入后的 Firehose

QString stageName(EdlUsbStage stage)
{
    return stage == EdlUsbStage::Sahara ? QStringLiteral("Sahara") : QStringLiteral("Firehose");
}

QString hex4(quint16 v)
{
    return QString::number(v, 16).rightJustified(4, QLatin1Char('0'));
}

void setErr(QString *error, const QString &msg)
{
    if (error) *error = msg;
}

} // namespace

// ---------------------------------------------------------------------------
// 纯函数：身份表与端点表
// ---------------------------------------------------------------------------

bool LibusbEdlTransport::isEdlId(quint16 vid, quint16 pid)
{
    if (vid != kQcVid) return false;
    return pid == kPidSahara || pid == kPidBothStages || pid == kPidFirehose;
}

bool LibusbEdlTransport::matchesStage(EdlUsbStage stage, quint16 vid, quint16 pid)
{
    if (vid != kQcVid) return false;
    if (pid == kPidBothStages) return true;    // 0x900E：两阶段都认
    return stage == EdlUsbStage::Sahara ? pid == kPidSahara : pid == kPidFirehose;
}

int LibusbEdlTransport::interfaceNumber(EdlUsbStage stage)
{
    return stage == EdlUsbStage::Sahara ? 0 : 1;
}

int LibusbEdlTransport::outEndpoint(EdlUsbStage stage)
{
    return stage == EdlUsbStage::Sahara ? 0x01 : 0x02;
}

int LibusbEdlTransport::inEndpoint(EdlUsbStage stage)
{
    return stage == EdlUsbStage::Sahara ? 0x82 : 0x83;
}

QString LibusbEdlTransport::noDeviceError(EdlUsbStage stage)
{
    const quint16 primary = stage == EdlUsbStage::Sahara ? kPidSahara : kPidFirehose;
    return QStringLiteral("未找到 %1 EDL 设备（期望 VID:PID %2:%3 或 %2:%4）")
        .arg(stageName(stage), hex4(kQcVid), hex4(primary), hex4(kPidBothStages));
}

// read() 的超时换算：IEdlTransport 的 0 = **非阻塞轮询**，而 libusb 的 0 = **无限等待**
// （libusb sync.c "For an unlimited timeout, use value 0"；`struct libusb_transfer::timeout`
// 字段注释同款："A value of 0 indicates no timeout"）。照搬传 0 → 会话每笔写前的 drain
// （edl_session.cpp 的 drainResidual）在真机永久阻塞；mock 把 0 建模成轮询，离线用例全绿也看不见。
// 取 1 ms：轮询只问"端点里现在有没有字节"，1 ms 对任何 USB 速率都够一次往返；
// bkerler 同款（edl/edlclient/Library/Connection/usblib.py:377-378 的 `if timeout == 0: timeout = 1`）。
int LibusbEdlTransport::effectiveTimeoutMs(int requested)
{
    return requested > 0 ? requested : 1;
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

LibusbEdlTransport::LibusbEdlTransport() = default;

LibusbEdlTransport::~LibusbEdlTransport()
{
    close();
}

void LibusbEdlTransport::setStage(EdlUsbStage stage)
{
    m_stage = stage;
}

EdlUsbStage LibusbEdlTransport::stage() const
{
    return m_stage;
}

bool LibusbEdlTransport::isOpen() const
{
    return m_dev != nullptr;
}

bool LibusbEdlTransport::open(QString *error)
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
        if (!matchesStage(m_stage, desc.idVendor, desc.idProduct))
            continue;
        if (libusb_open(list[i], &m_dev) == LIBUSB_SUCCESS)
            found = true;
        else
            m_dev = nullptr;      // 该设备打不开（权限/占用）→ 继续找下一个
    }
    libusb_free_device_list(list, 1);

    if (!found) {
        setErr(error, noDeviceError(m_stage));
        libusb_exit(m_ctx);
        m_ctx = nullptr;
        return false;
    }

    if (!claimAndDiscoverEndpoints(error)) {
        close();                  // 半开状态不留：失败即回到"未打开"
        return false;
    }
    return true;
}

bool LibusbEdlTransport::claimAndDiscoverEndpoints(QString *error)
{
    const int iface = interfaceNumber(m_stage);

    // 既有顺序（edl_handler.cpp（重写前 515cc57）:111-129）：claim → 失败则 detach 内核驱动 → 再 claim
    int ret = libusb_claim_interface(m_dev, iface);
    if (ret != LIBUSB_SUCCESS) {
        const int det = libusb_detach_kernel_driver(m_dev, iface);
        if (det == LIBUSB_SUCCESS || det == LIBUSB_ERROR_NOT_FOUND)
            ret = libusb_claim_interface(m_dev, iface);
    }
    if (ret != LIBUSB_SUCCESS) {
        setErr(error, QStringLiteral("认领 EDL 接口 %1 失败：%2")
                          .arg(iface).arg(QString::fromLatin1(libusb_error_name(ret))));
        return false;
    }
    m_iface = iface;

    // 端点与 OUT 包长：阶段接口内的批量端点（qdl 同款遍历：reference/qdl/src/usb.c:190-210）。
    // 描述符读不到时退回阶段常量（端点号）与兜底包长 —— 设备身份已匹配，不因描述符异常而拒连。
    m_outEp = outEndpoint(m_stage);
    m_inEp  = inEndpoint(m_stage);
    m_outMaxPacket = 0;

    libusb_config_descriptor *cfg = nullptr;
    if (libusb_get_active_config_descriptor(libusb_get_device(m_dev), &cfg) == LIBUSB_SUCCESS && cfg) {
        for (int ic = 0; ic < int(cfg->bNumInterfaces); ++ic) {
            for (int a = 0; a < int(cfg->interface[ic].num_altsetting); ++a) {
                const libusb_interface_descriptor *alt = &cfg->interface[ic].altsetting[a];
                if (alt->bInterfaceNumber != iface)
                    continue;
                for (int e = 0; e < int(alt->bNumEndpoints); ++e) {
                    const libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                    if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                        continue;   // 只看批量端点（EDL 的控制/中断端点不走这里）
                    if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN)
                        m_inEp = ep->bEndpointAddress;
                    else {
                        m_outEp = ep->bEndpointAddress;
                        m_outMaxPacket = int(ep->wMaxPacketSize);   // ZLP 判据
                    }
                }
            }
        }
        libusb_free_config_descriptor(cfg);
    }
    if (m_outMaxPacket <= 0)
        m_outMaxPacket = kFallbackMaxPacket;
    return true;
}

void LibusbEdlTransport::close()
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
    // 端点/包长回到"未知"：maxPacketSize() 必须如实反映"未打开"（0），不能留着上次的值
    m_iface = -1;
    m_outEp = 0;
    m_inEp = 0;
    m_outMaxPacket = 0;
}

// ---------------------------------------------------------------------------
// 数据面
// ---------------------------------------------------------------------------

bool LibusbEdlTransport::write(const QByteArray &data, QString *error)
{
    if (!m_dev) {
        setErr(error, QStringLiteral("写入失败：EDL 设备未打开"));
        return false;
    }
    if (data.isEmpty()) {
        // 0 字节写 = ZLP（会话数据面在"数据块长度恰为包长整数倍"时补发，edl_session.cpp 的
        // writeRaw）。本层不自行补 ZLP（见头文件顶部：一条规则一处责任）。
        int transferred = 0;
        const int ret = libusb_bulk_transfer(m_dev, m_outEp, nullptr, 0, &transferred, kWriteTimeoutMs);
        if (ret != LIBUSB_SUCCESS) {
            setErr(error, QStringLiteral("发送 ZLP 失败：%1")
                              .arg(QString::fromLatin1(libusb_error_name(ret))));
            return false;
        }
        return true;
    }

    // 单块长度必须塞得进 libusb 的 `int length`。今天 QByteArray 自己就以 int 计长（上限 INT_MAX），
    // 所以这条**目前不可达**；留着是因为上游的单块尺寸来自设备协商值（firehoseConfigure 已钳制，
    // 见 kMaxNegotiatedPayloadBytes），一旦将来数据面改成 64 位缓冲，这里必须**拒绝**而不是窄化。
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
        // 短写：bulk 传输在超时时可能少发 —— 绝不能当成功（数据面会继续发下一块，设备侧错位）
        setErr(error, QStringLiteral("写入 OUT 端点短写：已发 %1/%2 字节")
                          .arg(transferred).arg(data.size()));
        return false;
    }
    return true;
}

QByteArray LibusbEdlTransport::read(int maxBytes, int timeoutMs, QString *error)
{
    if (!m_dev) {
        // 未打开时**连 drain 轮询也报错**（调用方拿它诊断"谁没 open"，而不是把空返回当"端点静默"）
        setErr(error, QStringLiteral("读取失败：EDL 设备未打开"));
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
        // 轮询（timeoutMs <= 0，已被换算成 1 ms）：端点静默不是错误 —— drain 语义，
        // 见 edl_transport.h:20-22（"立即返回端点里已有的字节，没有则空返回且不置 error"）
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

bool LibusbEdlTransport::resetDevice(QString *error)
{
    // 契约（edl_transport.h:31）：退出 EDL / 复位。**真正的重启**由会话的 `<power value="reset"/>`
    // 命令触发；本层只做句柄退场。有意**不**调用 libusb_reset_device：设备此刻正在重启，端口复位
    // 可能长时间阻塞或返回错误，且它并不比"关掉句柄"多做什么。
    // best-effort（会话只把它当"报告"用，不判失败）：未打开 = 无事可做 → true。
    Q_UNUSED(error);
    if (m_dev || m_ctx)
        close();
    return true;
}

bool LibusbEdlTransport::waitReenumerate(int timeoutMs, QString *error)
{
    // 契约（edl_transport.h:14-19）：调用前须已 close()；返回 true ⇒ 设备**已重新 open()**，
    // 调用方不再 open。**fail-closed**：忘了 close 直接报错，不替调用方收尾 —— "设备消失又回来"
    // 这个硬件事件必须由调用方显式表达（正是 mock 里 close→wait 时序断言要钉住的东西）。
    if (m_dev) {
        setErr(error, QStringLiteral("等待重枚举失败：调用前必须先关闭设备句柄（close()）"
                                     "—— 拿着旧句柄等新设备，真机上永远等不到"));
        return false;
    }

    // 重枚举后的设备以 Firehose 身份回来（PID 表随之切换）
    m_stage = EdlUsbStage::Firehose;

    QElapsedTimer clock;
    clock.start();
    QString lastErr = noDeviceError(m_stage);

    // 3s 间隔轮询（既有实现：初等 msleep(3000) + 每次重试 msleep(2000) —— 重写前的
    // edl_handler.cpp，提交 515cc57 的 `connectSahara()` 段），
    // 预算耗尽即收手（默认 45s = FlashOptions::reenumerateTimeoutMs）。
    // **首试前也等一个间隔**：此刻设备正在掉线/重枚举（Sahara DONE 后立刻枚举必然落空），
    // 而 0x900E 这类"不换 PID"的机型尤其危险 —— 它能被立刻开出来，但模式切换尚未完成，
    // 紧接着的 configure 会打在还没切完的设备上（既有实现同样先 msleep(3000) 再试）。
    // 最后一次尝试可能略微超出预算（open() 自身耗时不计入 sleep 预算）——可接受。
    for (;;) {
        const qint64 left = qint64(timeoutMs) - clock.elapsed();
        if (left <= 0)
            break;
        QThread::msleep(unsigned(qMin<qint64>(qint64(kReenumPollIntervalMs), left)));
        if (open(&lastErr))
            return true;
    }

    setErr(error, QStringLiteral("等待设备重枚举为 Firehose 超时（预算 %1 ms）：%2")
                      .arg(timeoutMs).arg(lastErr));
    return false;
}

int LibusbEdlTransport::maxPacketSize() const
{
    return m_outMaxPacket;   // 未打开 → 0（包长未知；会话据此不发 ZLP，edl_session.cpp 的 writeRaw）
}

} // namespace edl
