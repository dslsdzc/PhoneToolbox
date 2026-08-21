#include "core/modes/spd_storage.h"

#include <QFile>

// 实现对照（核实记录见头文件与计划文档"协议核实记录"）
namespace spd {
namespace {

void putBe32(QByteArray &out, quint32 v)
{
    out.append(char((v >> 24) & 0xFF));
    out.append(char((v >> 16) & 0xFF));
    out.append(char((v >> 8) & 0xFF));
    out.append(char(v & 0xFF));
}

constexpr int kFdlBlock = 2048; // MIDST_DATA 块上限（行为观察）

} // namespace

bool SpdFlasher::uploadFdl(const QString &fdlPath, quint32 loadAddr, bool execAfter,
                           QString *error)
{
    QFile f(fdlPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 FDL 文件: %1").arg(fdlPath);
        return false;
    }
    const QByteArray data = f.readAll();
    f.close();
    if (data.isEmpty()) {
        if (error) *error = QStringLiteral("FDL 文件为空");
        return false;
    }
    // START_DATA：addr BE32 + size BE32
    QByteArray payload;
    putBe32(payload, loadAddr);
    putBe32(payload, quint32(data.size()));
    QByteArray reply;
    if (!m_session.sendCommand(BSL_CMD_START_DATA, payload, reply, 64, error))
        return false;
    // MIDST_DATA×N（块 ≤2048，每步响应确认）
    for (int off = 0; off < data.size(); off += kFdlBlock) {
        const QByteArray block = data.mid(off, kFdlBlock);
        if (!m_session.sendCommand(BSL_CMD_MIDST_DATA, block, reply, 64, error))
            return false;
    }
    // END_DATA
    if (!m_session.sendCommand(BSL_CMD_END_DATA, QByteArray(), reply, 64, error))
        return false;
    // EXEC_DATA（execAfter 时）
    if (execAfter
        && !m_session.sendCommand(BSL_CMD_EXEC_DATA, QByteArray(), reply, 64, error))
        return false;
    return true;
}

bool SpdFlasher::eraseFlash(quint32 addr, quint32 size, QString *error)
{
    QByteArray payload;
    putBe32(payload, addr);
    putBe32(payload, size);
    QByteArray reply;
    return m_session.sendCommand(BSL_CMD_ERASE_FLASH, payload, reply, 64, error);
}

bool SpdFlasher::readFlash(quint32 addr, quint32 len, quint32 offset, QByteArray &out,
                           QString *error)
{
    QByteArray payload;
    putBe32(payload, addr);
    putBe32(payload, len);
    putBe32(payload, offset);
    QByteArray reply;
    quint16 rtype = 0;
    if (!m_session.sendCommand(BSL_CMD_READ_FLASH, payload, reply, int(len) + 16, error,
                               &rtype))
        return false;
    // sendCommand 不校验响应 type——此处断言 BSL_REP_READ_FLASH(0x93)（行为观察）
    if (rtype != BSL_REP_READ_FLASH) {
        if (error) *error = QStringLiteral("READ_FLASH 响应类型不符 (0x%1)")
                                .arg(rtype, 4, 16, QLatin1Char('0'));
        return false;
    }
    // 响应数据区（sendCommand 已剥离 type/len/checksum）
    out = reply;
    return true;
}

bool SpdFlasher::resetDevice(QString *error)
{
    QByteArray reply;
    return m_session.sendCommand(BSL_CMD_NORMAL_RESET, QByteArray(), reply, 64, error);
}

} // namespace spd
