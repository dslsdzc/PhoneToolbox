#include "core/modes/mtk_brom.h"

#include <libusb.h>

// 实现对照（核实记录见计划文档）：枚举 usb_ids.py；握手 Port.py run_handshake()；
// echo Port.py echo()；帧结构 mtk_preloader.py 各函数；大端字段 pack(">I"/">H")。

namespace mtkbrom {
namespace {

struct UsbIdEntry {
    quint16 vid;
    quint16 pid;
};

// 对照 usb_ids.py default_ids（BROM 全部 + Preloader 主要）
constexpr UsbIdEntry kUsbIds[] = {
    {0x0E8D, 0x0003}, {0x0E8D, 0x6000}, {0x0E8D, 0x2000}, {0x0E8D, 0x2001},
    {0x0E8D, 0x20FF}, {0x0E8D, 0x3000},
    {0x1004, 0x6000}, {0x22D9, 0x0006},
    {0x0FCE, 0xF200}, {0x0FCE, 0xD1E9}, {0x0FCE, 0xD1E2},
    {0x0FCE, 0xD1EC}, {0x0FCE, 0xD1DD},
};

// 对照 run_handshake() brom_pids：这些 PID 不需预发 0xA0
bool isBromPid(quint16 pid)
{
    switch (pid) {
    case 0x0003: case 0xF200: case 0xD1E9: case 0xD1E2: case 0xD1EC: case 0xD1DD:
        return true;
    default:
        return false;
    }
}

void putBe32(QByteArray &out, quint32 v)
{
    out.append(char((v >> 24) & 0xFF));
    out.append(char((v >> 16) & 0xFF));
    out.append(char((v >> 8) & 0xFF));
    out.append(char(v & 0xFF));
}

quint16 getBe16(const QByteArray &b, int off)
{
    return off + 2 <= b.size()
        ? (quint16(quint8(b[off])) << 8) | quint16(quint8(b[off + 1])) : 0;
}

quint32 getBe32(const QByteArray &b, int off)
{
    return off + 4 <= b.size()
        ? (quint32(quint8(b[off])) << 24) | (quint32(quint8(b[off + 1])) << 16)
            | (quint32(quint8(b[off + 2])) << 8) | quint32(quint8(b[off + 3])) : 0;
}

const char *usbErrName(int ret) { return libusb_error_name(ret); }

// libusb 通道（对照 usblib.py connect()：detach kernel driver → claim → 找端点）
class BromUsbLibusb final : public IBromUsb {
public:
    explicit BromUsbLibusb(const BromDevice &dev) : m_dev(dev) {}
    ~BromUsbLibusb() override { close(); }

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
        libusb_detach_kernel_driver(m_handle, 0); // 失败可忽略（无驱动时）
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
            data.size(), &transferred, 1000);
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

    int maxPacketSize() const override { return m_wMaxPacketSize; }

    bool close() override
    {
        if (m_handle) {
            libusb_release_interface(m_handle, 0);
            libusb_close(m_handle);
            m_handle = nullptr;
        }
        if (m_ctx) {
            libusb_exit(m_ctx);
            m_ctx = nullptr;
        }
        return true;
    }

private:
    bool discoverEndpoints(QString *error)
    {
        // 对照 usblib.py connect()：接口内第一个 IN/OUT 批量端点
        libusb_config_descriptor *cfg = nullptr;
        if (libusb_get_active_config_descriptor(libusb_get_device(m_handle), &cfg) != LIBUSB_SUCCESS) {
            if (error) *error = QStringLiteral("读取配置描述符失败");
            return false;
        }
        m_wMaxPacketSize = 0x400;
        for (int ic = 0; ic < int(cfg->bNumInterfaces); ++ic) {
            for (int a = 0; a < int(cfg->interface[ic].num_altsetting); ++a) {
                const libusb_interface_descriptor *alt = &cfg->interface[ic].altsetting[a];
                for (int e = 0; e < alt->bNumEndpoints; ++e) {
                    const quint8 addr = alt->endpoint[e].bEndpointAddress;
                    if (addr & LIBUSB_ENDPOINT_IN) {
                        if (!m_epIn) {
                            m_epIn = addr;
                            m_wMaxPacketSize = int(alt->endpoint[e].wMaxPacketSize);
                        }
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

    const BromDevice m_dev;
    libusb_context *m_ctx = nullptr;
    libusb_device_handle *m_handle = nullptr;
    quint8 m_epIn = 0;
    quint8 m_epOut = 0;
    int m_wMaxPacketSize = 0x400;
};

} // namespace

bool enumerateUsb(QList<BromDevice> &out, QString *error)
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
        for (const UsbIdEntry &id : kUsbIds) {
            if (desc.idVendor == id.vid && desc.idProduct == id.pid) {
                BromDevice d;
                d.vid = id.vid;
                d.pid = id.pid;
                d.portName = QStringLiteral("usb-%1-%2")
                                 .arg(libusb_get_bus_number(list[i]))
                                 .arg(libusb_get_device_address(list[i]));
                out.append(d);
                break;
            }
        }
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return true;
}

bool openLibusbUsb(const BromDevice &dev, std::unique_ptr<IBromUsb> &ch, QString *error)
{
    auto usb = std::make_unique<BromUsbLibusb>(dev);
    QString oerr;
    if (!usb->open(&oerr)) {
        if (error) *error = oerr;
        return false;
    }
    ch = std::move(usb);
    return true;
}

BromSession::BromSession(std::unique_ptr<IBromUsb> usb, const BromDevice &dev)
    : m_usb(std::move(usb)), m_dev(dev)
{
}

BromSession::~BromSession()
{
    QString err;
    close(&err);
}

bool BromSession::connect(QString *error)
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

bool BromSession::handshake(QString *error)
{
    // 对照 Port.py run_handshake()：非 BROM PID 预发 0xA0；逐字节发 A0 0A 50 05，
    // 每字节读回显须 == 取反；任一字节不符回到开头重试（源码 5 次，本实现 3 次），
    // 重试前读清残留输入
    const QByteArray start("\xA0\x0A\x50\x05", 4);
    for (int attempt = 0; attempt < 3; ++attempt) {
        bool ok = true;
        if (!isBromPid(quint16(m_dev.pid))) {
            if (!m_usb->write(QByteArray(1, '\xA0'), error))
                ok = false;
        }
        if (ok) {
            for (quint8 b : start) {
                const QByteArray out(1, char(b));
                if (!m_usb->write(out, error)) {
                    ok = false;
                    break;
                }
                QByteArray echo;
                if (!m_usb->read(echo, 1, 500, error)) {
                    ok = false;
                    break;
                }
                if (echo.size() != 1 || quint8(echo[0]) != quint8(~b)) {
                    if (error) *error = QStringLiteral("握手回显不符（期望 0x%1 收到 0x%2）")
                                            .arg(quint8(~b), 2, 16, QLatin1Char('0'))
                                            .arg(echo.isEmpty() ? QStringLiteral("--")
                                                                : QStringLiteral("%1").arg(quint8(echo[0]), 2, 16, QLatin1Char('0')));
                    ok = false;
                    break;
                }
            }
        }
        if (ok)
            return true;
        if (attempt + 1 < 3) {
            // 残留输入清空：失败后重试前，短超时读一次（结果忽略）
            QByteArray stale;
            m_usb->read(stale, 0x400, 50, nullptr);
        }
    }
    return false;
}

bool BromSession::echoCmd(quint8 cmd, QString *error)
{
    if (!m_usb) {
        if (error) *error = QStringLiteral("未打开 USB 通道");
        return false;
    }
    const QByteArray out(1, char(cmd));
    if (!m_usb->write(out, error))
        return false;
    QByteArray echo;
    if (!m_usb->read(echo, 1, 500, error))
        return false;
    if (echo.size() != 1 || quint8(echo[0]) != cmd) {
        if (error) *error = QStringLiteral("命令回显不符: 发送 0x%1 收到 0x%2")
                                .arg(cmd, 2, 16, QLatin1Char('0'))
                                .arg(echo.isEmpty() ? QStringLiteral("--")
                                                    : QStringLiteral("%1").arg(quint8(echo[0]), 2, 16, QLatin1Char('0')));
        return false;
    }
    return true;
}

bool BromSession::echoBe32(quint32 v, QString *error)
{
    QByteArray p;
    putBe32(p, v);
    if (!m_usb->write(p, error))
        return false;
    QByteArray echo;
    if (!m_usb->read(echo, 4, 1000, error))
        return false;
    if (echo != p) {
        if (error) *error = QStringLiteral("参数回显不符: 0x%1").arg(v, 8, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

bool BromSession::readStatus(quint16 &status, QString *error)
{
    QByteArray b;
    if (!m_usb->read(b, 2, 1000, error))
        return false;
    status = getBe16(b, 0);
    return true;
}

bool BromSession::getTargetConfig(TargetConfig &out, QString *error)
{
    // 对照 get_target_config()：echo 0xD8 → 读 6B（config 4B BE + status 2B BE）
    if (!echoCmd(CMD_GET_TARGET_CONFIG, error))
        return false;
    QByteArray b;
    if (!m_usb->read(b, 6, 1000, error))
        return false;
    if (b.size() != 6) {
        if (error) *error = QStringLiteral("get_target_config 响应长度不符");
        return false;
    }
    const quint32 cfg = getBe32(b, 0);
    const quint16 status = getBe16(b, 4);
    if (status > 0xFF) {
        if (error) *error = QStringLiteral("get_target_config 状态错误: 0x%1")
                                .arg(status, 4, 16, QLatin1Char('0'));
        return false;
    }
    out.sbc = (cfg & 0x1) != 0;
    out.sla = (cfg & 0x2) != 0;
    out.daa = (cfg & 0x4) != 0;
    out.epp = (cfg & 0x8) != 0;
    out.cert = (cfg & 0x10) != 0;
    out.memread = (cfg & 0x20) != 0;
    out.memwrite = (cfg & 0x40) != 0;
    out.cmdC8 = (cfg & 0x80) != 0;
    return true;
}

bool BromSession::getHwCode(quint16 &hwCode, quint16 &hwVer, QString *error)
{
    // 对照 mtk_preloader.py get_hwcode()（0xFD）：echo → 读 4B → regular mode 拆分（:190-191）
    // ⚠️ 4B 之后**没有**尾随状态字（D1-T4 实施期裁决，三源一致，勿"补"）：
    //   • 上游 :880-882 `sendcmd(GET_HW_CODE, 4)` → `unpack(">HH")`，恰好 4B；
    //   • `Port.mtk_cmd()`（Port.py:210-226）= 写命令 → 读回显 1B → 读 bytestoread，无 status 段；
    //   • lx 文档《02-联发科-BROM-DA-协议》表在有状态字的命令上会明写（0xD8 回 6B `>IH`、
    //     0xD0/0xD1 有 status）—— 0xFD/0xFC 没写。
    //   若按"回 4B + 2B 状态"实现：真机上 libusb 读超时 → 每台设备第一条命令就失败
    //   （单测测不出来：mock 的读队列是预置的）。
    if (!echoCmd(CMD_GET_HW_CODE, error))
        return false;
    QByteArray b;
    if (!m_usb->read(b, 4, 1000, error))
        return false;
    if (b.size() != 4) { // 校验通过后才写出参：失败不留半成品
        if (error) *error = QStringLiteral("get_hw_code 响应长度不符（%1）").arg(b.size());
        return false;
    }
    const quint32 val = getBe32(b, 0);
    hwCode = quint16((val >> 16) & 0xFFFF);
    hwVer  = quint16(val & 0xFFFF);
    return true;
}

bool BromSession::getHwSwVer(HwSwVer &out, QString *error)
{
    // 对照 mtk_preloader.py get_hw_sw_ver()（0xFC）：sendcmd(0xFC, 8) → unpack(">HHHH")（:928-930）
    // 同样无尾随状态字（三源同上）。IoT 芯片上游不走这条路径（:182-187 改读 A2 寄存器），
    // D1 由调用方按芯片表 iot 位明确拒绝。
    if (!echoCmd(CMD_GET_HW_SW_VER, error))
        return false;
    QByteArray b;
    if (!m_usb->read(b, 8, 1000, error))
        return false;
    if (b.size() != 8) { // 校验通过后才写出参：失败不留半成品
        if (error) *error = QStringLiteral("get_hw_sw_ver 响应长度不符（%1）").arg(b.size());
        return false;
    }
    out.hwSubCode = getBe16(b, 0);
    out.hwVer     = getBe16(b, 2);
    out.swVer     = getBe16(b, 4);
    return true;
}

bool BromSession::sendCommand(quint8 cmd, const QByteArray &payload, QByteArray &reply,
                              int replyMaxLen, QString *error)
{
    if (!echoCmd(cmd, error))
        return false;
    if (!payload.isEmpty()) {
        if (!m_usb->write(payload, error))
            return false;
        QByteArray echo;
        if (!m_usb->read(echo, payload.size(), 1000, error) || echo != payload) {
            if (error && error->isEmpty())
                *error = QStringLiteral("参数回显不符");
            return false;
        }
    }
    if (replyMaxLen > 0 && !m_usb->read(reply, replyMaxLen, 1000, error))
        return false;
    return true;
}

bool BromSession::sendDa(quint32 address, quint32 length, quint32 sigLen,
                         const QByteArray &data, QString *error)
{
    // 对照 send_da()：echo 0xD7 → echo addr → echo len → echo sig_len → 读状态
    if (!echoCmd(CMD_SEND_DA, error))
        return false;
    if (!echoBe32(address, error) || !echoBe32(length, error) || !echoBe32(sigLen, error))
        return false;
    quint16 status = 0;
    if (!readStatus(status, error))
        return false;
    if (status == kStatusSlaRequired) {
        if (error) *error = QStringLiteral("SEND_DA 需要 SLA 认证（0x1D0D；RSA 响应生成自研为后续任务）");
        return false;
    }
    if (status > 0xFF) {
        if (error) *error = QStringLiteral("SEND_DA 状态错误: 0x%1").arg(status, 4, 16, QLatin1Char('0'));
        return false;
    }
    // 对照 upload_data()：分块写；每 0x2000 字节补空包；收尾补空包
    const int chunk = m_usb->maxPacketSize();
    int pos = 0;
    while (pos < data.size()) {
        const QByteArray piece = data.mid(pos, chunk);
        if (!m_usb->write(piece, error))
            return false;
        pos += piece.size();
        if (pos % 0x2000 == 0)
            m_usb->write(QByteArray(), error); // 空包失败可忽略（对照源码不检查）
    }
    m_usb->write(QByteArray(), error); // 收尾空包
    // 对照 upload_data()：rword(2) 读 4B unpack ">HH" —— u16 checksum BE + u16 status BE
    QByteArray resp;
    if (!m_usb->read(resp, 4, 2000, error))
        return false;
    if (resp.size() != 4) {
        if (error) *error = QStringLiteral("SEND_DA 上传响应长度不符");
        return false;
    }
    const quint16 checksum = getBe16(resp, 0);
    const quint16 upStatus = getBe16(resp, 2);
    if (checksum != 0 && checksum != calcDaChecksum(data)) {
        // 对照 upload_data()：checksum 不符为警告级，不终止（以状态为准）
        if (error && error->isEmpty())
            *error = QStringLiteral("SEND_DA 校验和不符（已忽略）");
    }
    if (upStatus > 0xFF) {
        if (error) *error = QStringLiteral("SEND_DA 上传状态错误: 0x%1")
                                .arg(upStatus, 4, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

bool BromSession::jumpDa(quint32 addr, QString *error)
{
    // 对照 jump_da()：echo 0xD5 → 写 addr → 读 4B 回显 == addr → 读 2B 状态 == 0
    if (!echoCmd(CMD_JUMP_DA, error))
        return false;
    QByteArray p;
    putBe32(p, addr);
    if (!m_usb->write(p, error))
        return false;
    QByteArray echo;
    if (!m_usb->read(echo, 4, 1000, error))
        return false;
    if (getBe32(echo, 0) != addr) {
        if (error) *error = QStringLiteral("JUMP_DA 地址回显不符");
        return false;
    }
    quint16 status = 0;
    if (!readStatus(status, error))
        return false;
    if (status != 0) {
        if (error) *error = QStringLiteral("JUMP_DA 状态错误: 0x%1").arg(status, 4, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

bool BromSession::jumpDa64(quint32 addr, QString *error)
{
    // 对照 jump_da64()：echo 0xDE → 写 addr → 读回显 → echo 0x01（64 位，读回显校验）→ 读状态 == 0
    if (!echoCmd(CMD_JUMP_DA64, error))
        return false;
    QByteArray p;
    putBe32(p, addr);
    if (!m_usb->write(p, error))
        return false;
    QByteArray echo;
    if (!m_usb->read(echo, 4, 1000, error))
        return false;
    if (getBe32(echo, 0) != addr) {
        if (error) *error = QStringLiteral("JUMP_DA64 地址回显不符");
        return false;
    }
    // 对照 jump_da64()：echo(b"\x01") —— 写 0x01 后须读 1B 回显并校验 == 0x01
    if (!m_usb->write(QByteArray(1, char(0x01)), error))
        return false;
    QByteArray marker;
    if (!m_usb->read(marker, 1, 1000, error))
        return false;
    if (marker.size() != 1 || quint8(marker[0]) != 0x01) {
        if (error) *error = QStringLiteral("JUMP_DA64 标记回显不符");
        return false;
    }
    quint16 status = 0;
    if (!readStatus(status, error))
        return false;
    if (status != 0) {
        if (error) *error = QStringLiteral("JUMP_DA64 状态错误: 0x%1").arg(status, 4, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

bool BromSession::close(QString *error)
{
    Q_UNUSED(error)
    if (m_usb)
        m_usb->close();
    m_connected = false;
    m_closed = true;
    return true;
}

quint32 BromSession::calcDaChecksum(const QByteArray &data)
{
    // 对照 prepare_data()：逐 2 字节小端 u16 异或累加；奇数尾字节补零参与
    quint32 sum = 0;
    int i = 0;
    for (; i + 1 < data.size(); i += 2) {
        const quint16 w = quint16(quint8(data[i])) | (quint16(quint8(data[i + 1])) << 8);
        sum ^= w;
    }
    if (i < data.size())
        sum ^= quint16(quint8(data[i]));
    return sum;
}

QByteArray BromSession::expectedHandshakeEcho()
{
    QByteArray out;
    for (quint8 b : QByteArray("\xA0\x0A\x50\x05", 4))
        out.append(char(quint8(~b)));
    return out;
}

} // namespace mtkbrom
