#include "hisi_update.h"

#include <QDateTime>

#include <libusb.h>

// 实现对照（核实记录见头文件与计划文档"协议核实记录"）
namespace hisi {
namespace {

// 华为 VID 白名单（DBAdapter / USB Update 设备）
constexpr quint16 kHuaweiVid = 0x12D1;

const char *usbErrName(int ret) { return libusb_error_name(ret); }

// libusb 通道：枚举端点 + CDC line coding(9600) 控制请求
class HisiUsbLibusb final : public IUsbChannel {
public:
    explicit HisiUsbLibusb(const HisiDevice &dev) : m_dev(dev) {}
    ~HisiUsbLibusb() override { close(); }

    bool open(QString *error) override
    {
        int ret = libusb_init(&m_ctx);
        if (ret != LIBUSB_SUCCESS) {
            if (error) *error = QStringLiteral("libusb_init 失败: %1").arg(QLatin1String(usbErrName(ret)));
            return false;
        }
        libusb_set_option(m_ctx, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
        m_handle = libusb_open_device_with_vid_pid(m_ctx, m_dev.vid, m_dev.pid);
        if (!m_handle) {
            if (error) *error = QStringLiteral("打开设备 %1:%2 失败（未连接或无权限）")
                                    .arg(m_dev.vid, 4, 16, QLatin1Char('0'))
                                    .arg(m_dev.pid, 4, 16, QLatin1Char('0'));
            libusb_exit(m_ctx); m_ctx = nullptr;
            return false;
        }
        libusb_detach_kernel_driver(m_handle, 0);
        ret = libusb_claim_interface(m_handle, 0);
        if (ret != LIBUSB_SUCCESS) {
            if (error) *error = QStringLiteral("claim 接口失败: %1").arg(QLatin1String(usbErrName(ret)));
            close();
            return false;
        }
        // CDC SET_LINE_CODING：9600 波特 8N1（行为一致性，bulk 传输不受影响）
        unsigned char lc[7] = { 0x80, 0x25, 0x00, 0x00, 0x00, 0x00, 0x08 };
        libusb_control_transfer(m_handle, 0x21, 0x20, 0, 0, lc, 7, 1000);
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
        for (int ic = 0; ic < int(cfg->bNumInterfaces); ++ic) {
            for (int a = 0; a < int(cfg->interface[ic].num_altsetting); ++a) {
                const libusb_interface_descriptor *alt = &cfg->interface[ic].altsetting[a];
                for (int e = 0; e < alt->bNumEndpoints; ++e) {
                    const quint8 addr = alt->endpoint[e].bEndpointAddress;
                    if (addr & LIBUSB_ENDPOINT_IN) {
                        if (!m_epIn) m_epIn = addr;
                    } else if (!m_epOut) {
                        m_epOut = addr;
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

    const HisiDevice m_dev;
    libusb_context *m_ctx = nullptr;
    libusb_device_handle *m_handle = nullptr;
    quint8 m_epIn = 0;
    quint8 m_epOut = 0;
};

} // namespace

const QByteArray HisiSession::kHandshakeCommand =
    QByteArray("\x26\x00\x00\x25\xA7\x00\x06\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00", 19);
const QByteArray HisiSession::kAckResponse = QByteArray("\x7E\x02\x6A\xD3\x7E", 5);
const QByteArray HisiSession::kHandshakePrefix = QByteArray("\x7E\x26\x00\x00\x25\xA7", 6);

bool enumerateUsb(QList<HisiDevice> &out, QString *error)
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
        if (desc.idVendor == kHuaweiVid) {
            HisiDevice d;
            d.vid = desc.idVendor;
            d.pid = desc.idProduct;
            d.portName = QStringLiteral("usb-%1-%2")
                             .arg(libusb_get_bus_number(list[i]))
                             .arg(libusb_get_device_address(list[i]));
            out.append(d);
        }
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return true;
}

bool openLibusbUsb(const HisiDevice &dev, std::unique_ptr<IUsbChannel> &ch, QString *error)
{
    auto usb = std::make_unique<HisiUsbLibusb>(dev);
    QString oerr;
    if (!usb->open(&oerr)) {
        if (error) *error = oerr;
        return false;
    }
    ch = std::move(usb);
    return true;
}

quint16 crc16X25(const QByteArray &data)
{
    // CRC16-X25：init 0xFFFF、反射多项式 0x8408、结果取反
    quint16 crc = 0xFFFF;
    for (char c : data) {
        crc ^= quint16(quint8(c));
        for (int i = 0; i < 8; ++i) {
            if (crc & 0x0001)
                crc = quint16((crc >> 1) ^ 0x8408);
            else
                crc >>= 1;
        }
    }
    return quint16(~crc);
}

QByteArray escapePayload(const QByteArray &data)
{
    QByteArray out;
    out.reserve(data.size() * 2);
    for (char c : data) {
        const quint8 b = quint8(c);
        if (b == 0x7E) {
            out.append(char(0x7D)); out.append(char(0x5E));
        } else if (b == 0x7D) {
            out.append(char(0x7D)); out.append(char(0x5D));
        } else {
            out.append(c);
        }
    }
    return out;
}

QByteArray buildFrame(quint8 cmd, const QByteArray &payload)
{
    // 0x7E | escaped(cmd + payload + CRC16-X25 LE) | 0x7E
    QByteArray body(1, char(cmd));
    body += payload;
    const quint16 crc = crc16X25(body);
    body.append(char(crc & 0xFF));
    body.append(char((crc >> 8) & 0xFF));
    QByteArray frame(1, '\x7E');
    frame += escapePayload(body);
    frame += QByteArray(1, '\x7E');
    return frame;
}

HisiSession::HisiSession(std::unique_ptr<IUsbChannel> usb, const HisiDevice &dev)
    : m_usb(std::move(usb)), m_dev(dev)
{
}

HisiSession::~HisiSession()
{
    QString err;
    close(&err);
}

bool HisiSession::connect(QString *error)
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
    if (!handshake(error)) {
        m_usb->close();
        return false;
    }
    m_connected = true;
    return true;
}

bool HisiSession::handshake(QString *error)
{
    // 发握手帧（命令 + CRC LE + 0x7E，无前导 0x7E），期望响应含前缀；
    // 3 次重试，间隔 200ms
    const quint16 crc = crc16X25(kHandshakeCommand);
    QByteArray frame = kHandshakeCommand;
    frame.append(char(crc & 0xFF)).append(char((crc >> 8) & 0xFF));
    frame.append('\x7E');
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!m_usb->write(frame, error))
            return false;
        QByteArray resp;
        if (m_usb->read(resp, 512, 500, error) && resp.contains(kHandshakePrefix))
            return true;
        // 不丢弃残留输入直接重试：残留由 readFrame/read 的前导字节过滤吸收；
        // 且 bulk 读超时即丢包，不会残留半帧
    }
    if (error && error->isEmpty())
        *error = QStringLiteral("握手失败（未收到 7E 26 00 00 25 A7 前缀响应）");
    return false;
}

bool HisiSession::sendCommand(quint8 cmd, const QByteArray &payload, double timeoutSec,
                              QString *error)
{
    // 帧分块 ≤0x10000 写入
    if (!m_usb) {
        if (error) *error = QStringLiteral("未打开 USB 通道");
        return false;
    }

    const QByteArray frame = buildFrame(cmd, payload);
    int offset = 0;
    while (offset < frame.size()) {
        const int chunk = qMin(0x10000, frame.size() - offset);
        if (!m_usb->write(frame.mid(offset, chunk), error))
            return false;
        offset += chunk;
    }

    // 读响应帧：丢弃非 0x7E 起始字节，收集至 0x7E…0x7E 闭合
    QByteArray resp;
    const int timeoutMs = int(timeoutSec * 1000);
    if (!readFrame(resp, timeoutMs, error))
        return false;
    // 解析：帧内 payload 首字节 = 0x02 成功 / 0x03 设备错误
    if (resp.size() >= 2 && quint8(resp[1]) == 0x02)
        return true;
    if (resp.size() >= 2 && quint8(resp[1]) == 0x03) {
        if (error) *error = QStringLiteral("设备错误 (0x03): %1")
                                .arg(QString::fromLatin1(resp.toHex(' ')));
        return false;
    }
    if (error) *error = QStringLiteral("意外响应: %1")
                            .arg(QString::fromLatin1(resp.toHex(' ')));
    return false;
}

bool HisiSession::readFrame(QByteArray &out, int timeoutMs, QString *error)
{
    out.clear();
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    bool seenStart = false;
    while (true) {
        const int remaining = int(deadline - QDateTime::currentMSecsSinceEpoch());
        if (remaining <= 0) {
            if (error && error->isEmpty())
                *error = QStringLiteral("响应超时");
            return false;
        }
        QByteArray chunk;
        if (!m_usb->read(chunk, 256, qMin(remaining, 256), error)) {
            if (error && error->isEmpty())
                *error = QStringLiteral("响应超时");
            return false;
        }
        for (char c : chunk) {
            const quint8 b = quint8(c);
            if (!seenStart) {
                if (b == 0x7E) { seenStart = true; out.append(c); }
                continue;
            }
            out.append(c);
            if (b == 0x7E && out.size() >= 2)
                return true; // 闭合
        }
    }
}

bool HisiSession::close(QString *error)
{
    Q_UNUSED(error)
    if (m_usb)
        m_usb->close();
    m_connected = false;
    m_closed = true;
    return true;
}

} // namespace hisi
