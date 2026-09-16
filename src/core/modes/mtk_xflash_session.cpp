#include "core/modes/mtk_xflash_session.h"

namespace mtkbrom {
namespace {

constexpr int kParamChunk = 0x200;      // XFL:172（send_param 的载荷分块）
constexpr int kStatusTimeoutMs = 3000;  // 传输层约定（上游 usbread 由传输层决定超时，无此常量）

QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
    return b;
}

QByteArray le64(quint64 v) { return le32(quint32(v & 0xFFFFFFFFu)) + le32(quint32(v >> 32)); }

quint32 leToU32(const QByteArray &b, int off)
{
    return quint32(quint8(b.at(off))) | (quint32(quint8(b.at(off + 1))) << 8)
         | (quint32(quint8(b.at(off + 2))) << 16) | (quint32(quint8(b.at(off + 3))) << 24);
}

// 错误码统一以**大写**十六进制出现（`0xC0040050` 等按字面可诊断 —— 用例按字面断言）
QString hexCode(quint32 v)
{
    QString hex = QString::number(v, 16).toUpper();
    while (hex.size() < 8)
        hex.prepend(QLatin1Char('0'));
    return QStringLiteral("0x") + hex;
}

// 通道未设置（构造时传 nullptr）—— 唯一文案来源；所有接触 m_usb 的入口都先过这里
bool noUsb(QString *error)
{
    if (error) *error = QStringLiteral("XFlash：USB 通道未设置");
    return false;
}

} // namespace

// 铁律 3：帧头 pack("<III", magic, datatype, length)，头一次写、载荷第二次写（XFL:112-115）
bool XFlashSession::xsendHeader(quint32 length, QString *error)
{
    if (!m_usb)
        return noUsb(error);
    return m_usb->write(le32(kXMagic) + le32(kXDataProtocolFlow) + le32(length), error);
}

bool XFlashSession::xsend(const QByteArray &payload, QString *error)
{
    if (!xsendHeader(quint32(payload.size()), error))   // 头一次写
        return false;
    if (!payload.isEmpty() && !m_usb->write(payload, error))   // 载荷第二次写
        return false;
    return true;
}

bool XFlashSession::xsendInt(quint32 v, QString *error) { return xsend(le32(v), error); }
bool XFlashSession::xsendInt64(quint64 v, QString *error) { return xsend(le64(v), error); }

// 应答帧 = 12B 头 + 载荷（XFL:117-128）
bool XFlashSession::xread(QByteArray &payload, quint32 *datatype, QString *error)
{
    if (!m_usb)
        return noUsb(error);
    QByteArray header;
    if (!m_usb->readExact(header, 12, kStatusTimeoutMs, error))
        return false;
    if (leToU32(header, 0) != kXMagic) {
        if (error) *error = QStringLiteral("XFlash：应答帧 magic 不符（读到 0x%1）")
                                .arg(leToU32(header, 0), 8, 16, QLatin1Char('0'));
        return false;
    }
    const quint32 dt = leToU32(header, 4);
    const quint32 len = leToU32(header, 8);
    if (len > (1u << 20)) {                      // 防御：单帧不超过 1 MB
        if (error) *error = QStringLiteral("XFlash：应答帧长度异常（%1）").arg(len);
        return false;
    }
    payload.clear();
    if (len > 0 && !m_usb->readExact(payload, int(len), kStatusTimeoutMs, error))
        return false;
    if (datatype) *datatype = dt;
    return true;
}

// 铁律 6：判据见头文件注释（XFL:138-158）
bool XFlashSession::readStatus(quint32 &code, QString *error)
{
    QByteArray payload;
    if (!xread(payload, nullptr, error))
        return false;
    if (payload.size() == 2) {
        const quint16 v = quint16(quint8(payload.at(0))) | (quint16(quint8(payload.at(1))) << 8);
        code = v;                                // 为 0 才算成功（由调用方/checkStatus 判定）
        return true;
    }
    if (payload.size() == 4) {
        const quint32 v = leToU32(payload, 0);
        code = (v == kXMagic) ? 0u : v;          // == magic 视为成功
        return true;
    }
    if (payload.size() < 4) {
        if (error) *error = QStringLiteral("XFlash：状态帧长度异常（%1）").arg(payload.size());
        return false;
    }
    code = leToU32(payload, 0);                  // 其它长度：取载荷首个 u32
    return true;
}

// 0 = 成功；其余一律失败。0xC0040050（EMI 版本不匹配）有**独立**文案分支（三条分支各自可判别）：
// 上游 XFL:180-188 对该码只跳过错误打印与 sys.exit，**仍 `return False`**；显式 preloader 路径
// XFL:1147-1149 的 `if not self.send_emi(...): return False` 据此中止整链（自动搜索路径
// XFL:1131-1136 才是换候选继续）。本实现与之同判，仅补一条中文诊断（上游此处不打印）。
bool XFlashSession::checkStatus(QString *error)
{
    quint32 code = 0;
    if (!readStatus(code, error))
        return false;
    if (code == 0)
        return true;
    if (error) {
        *error = (code == kXEmitVersionMismatch)
                     ? QStringLiteral("XFlash：EMI 版本不匹配（%1）—— 按上游判失败").arg(hexCode(code))
                     : QStringLiteral("XFlash：设备返回错误码 %1").arg(hexCode(code));
    }
    return false;
}

// 铁律 4（XFL:85-100）
bool XFlashSession::ack(QString *error)
{
    if (m_dacode == 0x6781) {                    // 一次 16 字节（头+载荷合并，XFL:88）
        if (!m_usb)
            return noUsb(error);
        const QByteArray merged = le32(kXMagic) + le32(kXDataProtocolFlow) + le32(4) + le32(0);
        return m_usb->write(merged, error);
    }
    return xsendInt(0, error);                   // 其余芯片：两次写（XFL:90-94）
}

// 铁律 5：每个参数一个独立帧，载荷按 0x200 分块（XFL:163-177 —— 帧头 `usbwrite(pkt)` 一次，
// 随后 `while length > 0: dsize = min(length, 0x200)` 循环写，**从不整段写**），
// 全部写完读一次 status（XFL:177-188）
bool XFlashSession::sendParam(const QList<QByteArray> &params, QString *error)
{
    for (const QByteArray &p : params) {
        if (!xsendHeader(quint32(p.size()), error))
            return false;
        for (int off = 0; off < p.size(); off += kParamChunk) {
            const QByteArray chunk = p.mid(off, kParamChunk);
            if (!m_usb->write(chunk, error))
                return false;
        }
    }
    return checkStatus(error);                   // 全部写完读一次 status
}

// XFL:273-281：帧头一次写 → 载荷按 EP_OUT.wMaxPacketSize 循环分块（同样**不整段写**）→ 读一次 status
// （status 处理见 :282-285）
bool XFlashSession::sendData(const QByteArray &data, QString *error)
{
    if (!xsendHeader(quint32(data.size()), error))
        return false;
    const int chunk = m_usb->maxPacketSize() > 0 ? m_usb->maxPacketSize() : 0x400;
    for (int off = 0; off < data.size(); off += chunk) {
        if (!m_usb->write(data.mid(off, chunk), error))
            return false;
    }
    return checkStatus(error);
}

// XFL:190-204：DEVICE_CTRL → status → 子命令 → status →（无参：读回包 / 有参：send_param）
bool XFlashSession::sendDevCtrl(quint32 subcmd, const QByteArray &param, QByteArray *reply, QString *error)
{
    if (!xsendInt(X_CMD_DEVICE_CTRL, error))     // 0x010009
        return false;
    if (!checkStatus(error))
        return false;
    if (!xsendInt(subcmd, error))                // 子命令
        return false;
    if (!checkStatus(error))
        return false;
    if (param.isEmpty()) {
        QByteArray payload;
        if (!xread(payload, nullptr, error))     // 无参 → 读回包
            return false;
        if (reply) *reply = payload;
        return true;
    }
    return sendParam({param}, error);
}

} // namespace mtkbrom
