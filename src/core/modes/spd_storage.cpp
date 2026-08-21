#include "core/modes/spd_storage.h"

#include <QFile>

#include <climits>

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
constexpr int kExecTimeoutMs = 15000; // EXEC_DATA 设备执行耗时可达 15s（行为观察 recv_msg_timeout(15000)）

} // namespace

bool SpdFlasher::sendAndExpectAck(quint16 type, const QByteArray &payload, QString *error,
                                  int timeoutMs)
{
    QByteArray reply;
    quint16 replyType = 0;
    if (!m_session.sendCommand(type, payload, reply, 64, error, &replyType, timeoutMs))
        return false;
    // 强制校验响应 type == BSL_REP_ACK（行为观察 send_and_check 语义）：
    // 设备错误响应（如 0x84 OPERATION_FAILED）不得被当作成功
    if (replyType != BSL_REP_ACK) {
        if (error) *error = QStringLiteral("命令 0x%1 未获 ACK（响应 0x%2）")
                                .arg(type, 4, 16, QLatin1Char('0'))
                                .arg(replyType, 4, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

bool SpdFlasher::uploadFdl(const QString &fdlPath, quint32 loadAddr, bool execAfter,
                           QString *error)
{
    QFile f(fdlPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 FDL 文件: %1").arg(fdlPath);
        return false;
    }
    // 明确上限：块偏移循环仅支持 < 2GiB（qsizetype 比较，先于 readAll 读入；
    // 项目先例 update_app.cpp 的 INT_MAX 守卫，7ad80f8）
    if (f.size() > qsizetype(INT_MAX)) {
        if (error) *error = QStringLiteral("FDL 文件过大（≥2GiB 暂不支持）");
        return false;
    }
    const QByteArray data = f.readAll();
    f.close();
    if (data.isEmpty()) {
        if (error) *error = QStringLiteral("FDL 文件为空");
        return false;
    }
    // START_DATA：addr BE32 + size BE32（每步响应强制 ACK，行为观察 send_and_check）
    QByteArray payload;
    putBe32(payload, loadAddr);
    putBe32(payload, quint32(data.size()));
    if (!sendAndExpectAck(BSL_CMD_START_DATA, payload, error))
        return false;
    // MIDST_DATA×N（块 ≤2048，每步响应强制 ACK）
    for (qsizetype off = 0; off < data.size(); off += kFdlBlock) {
        const QByteArray block = data.mid(off, kFdlBlock);
        if (!sendAndExpectAck(BSL_CMD_MIDST_DATA, block, error))
            return false;
    }
    // END_DATA
    if (!sendAndExpectAck(BSL_CMD_END_DATA, QByteArray(), error))
        return false;
    // EXEC_DATA（execAfter 时；设备执行耗时可达 15s，行为观察 recv_msg_timeout(15000)）
    if (execAfter
        && !sendAndExpectAck(BSL_CMD_EXEC_DATA, QByteArray(), error, kExecTimeoutMs))
        return false;
    return true;
}

bool SpdFlasher::eraseFlash(quint32 addr, quint32 size, QString *error)
{
    QByteArray payload;
    putBe32(payload, addr);
    putBe32(payload, size);
    return sendAndExpectAck(BSL_CMD_ERASE_FLASH, payload, error);
}

bool SpdFlasher::readFlash(quint32 addr, quint32 len, quint32 offset, QByteArray &out,
                           QString *error)
{
    // 单次读取上限 0xFFFF（响应 len 字段 16 位，行为观察）；更大须分块
    if (len > 0xFFFF) {
        if (error) *error = QStringLiteral("单次读取超过 0xFFFF 字节，需分块");
        return false;
    }
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
    out = reply;
    // 短读报错而非静默截断：返回数据量须与请求一致（0xFFFF 上限内可比）
    if (out.size() != int(len)) {
        if (error) *error = QStringLiteral("READ_FLASH 响应数据量不符（%1/请求 %2 字节）")
                                .arg(out.size()).arg(len);
        return false;
    }
    return true;
}

bool SpdFlasher::resetDevice(QString *error)
{
    return sendAndExpectAck(BSL_CMD_NORMAL_RESET, QByteArray(), error);
}

} // namespace spd
