#include "core/modes/spd_flash.h"

#include <QDateTime>
#include <libusb.h>

// 实现对照（核实记录见头文件与计划文档"协议核实记录"）
namespace spd {
namespace {

constexpr quint16 kHdlcHeader = 0x7E;
constexpr quint16 kHdlcEscape = 0x7D;
constexpr quint16 kSpdVid = 0x1782;
constexpr int kMaxLogFrames = 16; // 连续 BSL_REP_LOG 上限（防设备刷屏死循环）

const char *usbErrName(int ret) { return libusb_error_name(ret); }

void putBe16(QByteArray &out, quint16 v)
{
    out.append(char((v >> 8) & 0xFF));
    out.append(char(v & 0xFF));
}

quint16 getBe16(const QByteArray &b, int off)
{
    return off + 2 <= b.size()
        ? (quint16(quint8(b[off])) << 8) | quint16(quint8(b[off + 1])) : 0;
}

// libusb 通道
class SpdUsbLibusb final : public IUsbChannel {
public:
    SpdUsbLibusb(int vid, int pid) : m_vid(vid), m_pid(pid) {}
    ~SpdUsbLibusb() override { close(); }

    bool open(QString *error) override
    {
        // 幂等守卫：openLibusbUsb 后 SpdSession::connect 再次调用 open——
        // 二次 libusb_init/claim 真机返回 BUSY 且首 ctx/handle 泄漏
        if (m_handle)
            return true; // 已打开（幂等：openLibusbUsb 后 connect 再次调用）
        int ret = libusb_init(&m_ctx);
        if (ret != LIBUSB_SUCCESS) {
            if (error) *error = QStringLiteral("libusb_init 失败: %1").arg(QLatin1String(usbErrName(ret)));
            return false;
        }
        libusb_set_option(m_ctx, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
        m_handle = libusb_open_device_with_vid_pid(m_ctx, m_vid, m_pid);
        if (!m_handle) {
            if (error) *error = QStringLiteral("打开设备 %1:%2 失败（未连接或无权限）")
                                    .arg(m_vid, 4, 16, QLatin1Char('0'))
                                    .arg(m_pid, 4, 16, QLatin1Char('0'));
            libusb_exit(m_ctx); m_ctx = nullptr;
            return false;
        }
        libusb_detach_kernel_driver(m_handle, 0);
        // claim 固定接口 0（参照 claim 端点所在接口）；典型展锐设备为 0，真机鲁棒性项
        ret = libusb_claim_interface(m_handle, 0);
        if (ret != LIBUSB_SUCCESS) {
            if (error) *error = QStringLiteral("claim 接口失败: %1").arg(QLatin1String(usbErrName(ret)));
            close();
            return false;
        }
        return discoverEndpoints(error);
    }

    bool write(const QByteArray &data, QString *error) override
    {
        if (!m_handle || !m_epOut) {
            if (error) *error = QStringLiteral("USB 通道未打开");
            return false;
        }
        int transferred = 0;
        const int ret = libusb_bulk_transfer(
            m_handle, m_epOut,
            reinterpret_cast<unsigned char *>(const_cast<char *>(data.constData())),
            data.size(), &transferred, 2000);
        if (ret != LIBUSB_SUCCESS || transferred != data.size()) {
            if (error) *error = QStringLiteral("USB 写失败: %1（%2/%3 字节）")
                                    .arg(QLatin1String(usbErrName(ret)))
                                    .arg(transferred).arg(data.size());
            return false;
        }
        // ZLP：512 字节块倍数时补空包（行为观察，UMS9117 兼容）
        if (m_epOutBlk == 512 && data.size() % 512 == 0) {
            libusb_bulk_transfer(m_handle, m_epOut, nullptr, 0, &transferred, 2000);
        }
        return true;
    }

    bool read(QByteArray &out, int maxLen, int timeoutMs, QString *error) override
    {
        out.clear();
        if (!m_handle || !m_epIn) {
            if (error) *error = QStringLiteral("USB 通道未打开");
            return false;
        }
        QByteArray buf(maxLen, Qt::Uninitialized);
        int transferred = 0;
        const int ret = libusb_bulk_transfer(m_handle, m_epIn,
                                             reinterpret_cast<unsigned char *>(buf.data()),
                                             maxLen, &transferred, timeoutMs);
        if (ret != LIBUSB_SUCCESS) {
            if (error) *error = QStringLiteral("USB 读失败: %1").arg(QLatin1String(usbErrName(ret)));
            return false;
        }
        out = buf.left(transferred);
        return true;
    }

    bool close() override
    {
        if (m_handle) {
            libusb_release_interface(m_handle, 0);
            libusb_close(m_handle);
            m_handle = nullptr;
        }
        if (m_ctx) { libusb_exit(m_ctx); m_ctx = nullptr; }
        return true;
    }

private:
    bool discoverEndpoints(QString *error)
    {
        libusb_config_descriptor *cfg = nullptr;
        if (libusb_get_active_config_descriptor(libusb_get_device(m_handle), &cfg) != LIBUSB_SUCCESS) {
            if (error) *error = QStringLiteral("读取配置描述符失败");
            return false;
        }
        m_epOutBlk = 0x400;
        for (int ic = 0; ic < int(cfg->bNumInterfaces); ++ic) {
            for (int a = 0; a < int(cfg->interface[ic].num_altsetting); ++a) {
                const libusb_interface_descriptor *alt = &cfg->interface[ic].altsetting[a];
                for (int e = 0; e < alt->bNumEndpoints; ++e) {
                    const quint8 addr = alt->endpoint[e].bEndpointAddress;
                    if (addr & LIBUSB_ENDPOINT_IN) {
                        if (!m_epIn) m_epIn = addr;
                    } else if (!m_epOut) {
                        m_epOut = addr;
                        m_epOutBlk = int(alt->endpoint[e].wMaxPacketSize);
                    }
                }
                if (m_epIn && m_epOut) break;
            }
            if (m_epIn && m_epOut) break;
        }
        libusb_free_config_descriptor(cfg);
        if (!m_epIn || !m_epOut) {
            if (error) *error = QStringLiteral("未找到 IN/OUT 端点");
            close();
            return false;
        }
        return true;
    }

    const int m_vid;
    const int m_pid;
    libusb_context *m_ctx = nullptr;
    libusb_device_handle *m_handle = nullptr;
    quint8 m_epIn = 0;
    quint8 m_epOut = 0;
    int m_epOutBlk = 0x400;
};

} // namespace

bool enumerateUsb(QList<QPair<int, int>> &out, QString *error)
{
    out.clear();
    libusb_context *ctx = nullptr;
    int ret = libusb_init(&ctx);
    if (ret != LIBUSB_SUCCESS) {
        if (error) *error = QStringLiteral("libusb_init 失败: %1").arg(QLatin1String(usbErrName(ret)));
        return false;
    }
    libusb_set_option(ctx, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    libusb_device **list = nullptr;
    const ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) {
        if (error) *error = QStringLiteral("libusb_get_device_list 失败");
        libusb_exit(ctx);
        return false;
    }
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS)
            continue;
        if (desc.idVendor == kSpdVid)
            out.append({ desc.idVendor, desc.idProduct });
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return true;
}

bool openLibusbUsb(int vid, int pid, std::unique_ptr<IUsbChannel> &ch, QString *error)
{
    auto usb = std::make_unique<SpdUsbLibusb>(vid, pid);
    QString oerr;
    if (!usb->open(&oerr)) {
        if (error) *error = oerr;
        return false;
    }
    ch = std::move(usb);
    return true;
}

quint16 sumChecksum(const QByteArray &data)
{
    // 每 2 字节小端字累加 + 进位折叠 + 取反 + BE 交换。
    // 奇长 body 恒交换；参照发送侧仅偶长交换（接收侧一致），真机奇长末块待验证
    quint32 sum = 0;
    int i = 0;
    for (; i + 1 < data.size(); i += 2)
        sum += quint16(quint8(data[i])) | (quint16(quint8(data[i + 1])) << 8);
    if (i < data.size())
        sum += quint8(data[i]);
    while (sum >> 16)
        sum = (sum >> 16) + (sum & 0xFFFF);
    const quint16 neg = quint16(~sum & 0xFFFF);
    return quint16((neg >> 8) | (neg << 8)); // BE 交换
}

quint16 crc16(const QByteArray &data)
{
    // CRC-16（poly 0x11021 非反射，init 0；行为观察核实 init 0）
    quint16 crc = 0;
    for (char c : data) {
        crc ^= quint16(quint8(c)) << 8;
        for (int i = 0; i < 8; ++i) {
            if (crc & 0x8000)
                crc = quint16((crc << 1) ^ 0x1021);
            else
                crc <<= 1;
        }
    }
    return crc;
}

SpdSession::SpdSession(std::unique_ptr<IUsbChannel> usb, int vid, int pid, bool transcode)
    : m_usb(std::move(usb)), m_vid(vid), m_pid(pid), m_transcode(transcode)
{
}

SpdSession::~SpdSession()
{
    QString err;
    close(&err);
}

bool SpdSession::connect(QString *error)
{
    if (m_closed) {
        if (error) *error = QStringLiteral("会话已关闭");
        return false;
    }
    if (!m_usb) {
        if (error) *error = QStringLiteral("未打开 USB 通道");
        return false;
    }
    QString oerr;
    if (!m_usb->open(&oerr)) {
        if (error) *error = oerr;
        return false;
    }
    m_connected = true;
    return true;
}

bool SpdSession::handshake(int checkBaudLen, int maxAttempts, QString *error)
{
    if (m_closed) {
        if (error) *error = QStringLiteral("会话已关闭");
        return false;
    }
    if (!m_usb) {
        if (error) *error = QStringLiteral("未打开 USB 通道");
        return false;
    }
    // FDL1：单次 CHECK_BAUD → REP_VER；FDL2 就绪等待：CHECK_BAUD 重试 ≤maxAttempts
    // （参照语义：仅无数据/超时重试，其余失败立即报错）
    const QByteArray baud(QByteArray(checkBaudLen, '\0')); // buildFrame 只取长度
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        QByteArray reply;
        quint16 rtype = 0;
        QString err;
        if (!sendCommand(BSL_CMD_CHECK_BAUD, baud, reply, 64, &err, &rtype)) {
            if (attempt < maxAttempts
                && (err.contains(QStringLiteral("超时")) || err.contains(QStringLiteral("TIMEOUT"))))
                continue; // 设备未就绪（无响应）——重试
            if (error) *error = err;
            return false;
        }
        if (rtype != BSL_REP_VER) {
            if (error) *error = QStringLiteral("CHECK_BAUD 未获 REP_VER（响应 0x%1）")
                                    .arg(rtype, 4, 16, QLatin1Char('0'));
            return false;
        }
        // CONNECT → 强制 ACK（行为观察：发后强制校验 ACK 响应）
        if (!sendCommand(BSL_CMD_CONNECT, QByteArray(), reply, 64, error, &rtype))
            return false;
        if (rtype != BSL_REP_ACK) {
            if (error) *error = QStringLiteral("CONNECT 未获 ACK（响应 0x%1）")
                                    .arg(rtype, 4, 16, QLatin1Char('0'));
            return false;
        }
        return true;
    }
    return false; // maxAttempts ≥ 1 时不可达
}

bool SpdSession::buildFrame(quint16 type, const QByteArray &payload, QByteArray &frame) const
{
    // CHECK_BAUD 特判：整帧为 len 个 0x7E，无 header/len/checksum 结构（行为观察）
    if (type == BSL_CMD_CHECK_BAUD) {
        frame = QByteArray(payload.size(), char(0x7E));
        return true;
    }
    // 0x7E | type(2B BE) + len(2B BE) + data + checksum(2B BE) | 0x7E
    if (payload.size() > 0xFFFF)
        return false;
    QByteArray body;
    putBe16(body, type);
    putBe16(body, quint16(payload.size()));
    body += payload;
    putBe16(body, sumChecksum(body));
    frame.clear();
    frame.append(char(0x7E));
    if (m_transcode) {
        for (char c : body) {
            const quint8 b = quint8(c);
            if (b == 0x7E || b == 0x7D) {
                frame.append(char(0x7D));
                frame.append(char(b == 0x7E ? 0x5E : 0x5D));
            } else {
                frame.append(c);
            }
        }
    } else {
        frame += body;
    }
    frame.append(char(0x7E));
    return true;
}

bool SpdSession::sendCommand(quint16 type, const QByteArray &payload, QByteArray &reply,
                             int replyMaxLen, QString *error, quint16 *replyType,
                             int timeoutMs)
{
    if (!m_usb) {
        if (error) *error = QStringLiteral("未打开 USB 通道");
        return false;
    }
    QByteArray frame;
    if (!buildFrame(type, payload, frame)) {
        if (error) *error = QStringLiteral("消息过长（> 0xFFFF）");
        return false;
    }
    if (!m_usb->write(frame, error))
        return false;
    // 响应帧解析（行为观察 recv 侧）：
    //   • 帧头 0x7E 之前的杂散字节跳过；起始 0x7E 后累计 body（type+len+data+checksum）
    //   • 帧长由 len 字段闭合：期望总长 = len + 6（type 2 + len 2 + data + checksum 2；
    //     len 为 16 位，帧长天然上限 0x10005）——数据区含 0x7E 不提前截断
    //   • 转义模式（TRANSCODE）：接收侧反转义 7D 5E→7E、7D 5D→7D
    //   • BSL_REP_LOG(0xFF) log 帧跳过继续读（行为观察：接收循环跳过 log 帧），
    //     连续 log 帧上限 kMaxLogFrames，防设备刷屏死循环
    //   • 帧间残留：LOG 帧闭合后同 chunk 残留字节丢弃（参照有残量缓冲）；
    //     失败形态为明确报错
    int logFrames = 0;
    quint16 frameType = 0;
    for (;;) {
        QByteArray raw;
        QByteArray chunk;
        bool headFound = false;
        bool escaped = false;
        int expected = 6; // 最小帧长（type 2 + len 2 + checksum 2）
        const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
        bool frameDone = false;
        while (!frameDone) {
            const int remaining = int(deadline - QDateTime::currentMSecsSinceEpoch());
            if (remaining <= 0) {
                if (error && error->isEmpty())
                    *error = QStringLiteral("响应超时");
                return false;
            }
            if (!m_usb->read(chunk, 256, qMin(remaining, 256), error))
                return false;
            for (char c : chunk) {
                const quint8 b = quint8(c);
                if (m_transcode) {
                    // 转义后字节必须是 5E/5D（行为观察）
                    if (escaped && b != 0x5E && b != 0x5D) {
                        if (error) *error = QStringLiteral("响应转义字节异常 (0x%1)")
                                                .arg(b, 2, 16, QLatin1Char('0'));
                        return false;
                    }
                    if (b == kHdlcHeader) {
                        if (!headFound) { headFound = true; continue; }
                        if (raw.isEmpty()) continue;
                        if (raw.size() < expected) {
                            if (error) *error = QStringLiteral("响应帧过短");
                            return false;
                        }
                        frameDone = true; // 闭合 0x7E
                        break;
                    }
                    if (b == kHdlcEscape) { escaped = true; continue; }
                    if (!headFound) continue; // 帧前杂散字节跳过
                    if (raw.size() >= expected) {
                        if (error) *error = QStringLiteral("响应帧过长");
                        return false;
                    }
                    raw.append(char(b ^ (escaped ? 0x20 : 0)));
                    escaped = false;
                } else {
                    if (!headFound) {
                        if (b == kHdlcHeader) headFound = true;
                        continue; // 帧前杂散字节跳过
                    }
                    if (raw.size() == expected) {
                        if (b != kHdlcHeader) {
                            if (error) *error = QStringLiteral("响应帧尾缺失 0x7E");
                            return false;
                        }
                        frameDone = true;
                        break;
                    }
                    raw.append(c);
                }
                if (raw.size() == 4)
                    expected = getBe16(raw, 2) + 6; // len 字段声明期望帧长
            }
        }
        // 校验：长度 + checksum（type/len 不校验，调用方按需断言）
        if (raw.size() < 6) {
            if (error) *error = QStringLiteral("响应帧过短");
            return false;
        }
        if (raw.size() != expected) {
            if (error) *error = QStringLiteral("响应长度不符 (%1, 期望 %2)")
                                    .arg(raw.size()).arg(expected);
            return false;
        }
        const quint16 expChk = getBe16(raw, raw.size() - 2);
        const quint16 actChk = sumChecksum(raw.left(raw.size() - 2));
        if (expChk != actChk) {
            if (error) *error = QStringLiteral("响应 checksum 不符");
            return false;
        }
        frameType = getBe16(raw, 0);
        // 跳过 log 帧（0xFF）：继续读下一帧直至真实响应（行为观察：接收循环跳过 log 帧）
        if (frameType == BSL_REP_LOG) {
            if (++logFrames > kMaxLogFrames) {
                if (error) *error = QStringLiteral("响应 log 帧过多");
                return false;
            }
            continue;
        }
        const int dataLen = getBe16(raw, 2);
        reply = raw.mid(4, qMin(dataLen, replyMaxLen));
        if (replyType) *replyType = frameType;
        return true;
    }
}

bool SpdSession::close(QString *error)
{
    Q_UNUSED(error)
    if (m_usb)
        m_usb->close();
    m_connected = false;
    m_closed = true;
    return true;
}

} // namespace spd
