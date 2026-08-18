#include "hisi_flash.h"

#include <QFile>

#include <cstring>

#include <zlib.h>

// 实现对照（核实记录见头文件与计划文档"协议核实记录"）
namespace hisi {

QByteArray zlibCompress(const QByteArray &data)
{
    // 协议要求 zlib 头 0x78 0x01（Deflate 级别 1 = Fastest）
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (deflateInit2(&strm, 1, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return QByteArray();
    // 压缩（流式，防御性倍增缓冲）
    QByteArray out;
    const uLong bound = deflateBound(&strm, uLong(data.size()));
    out.resize(int(bound));
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    strm.avail_in = uInt(data.size());
    strm.next_out = reinterpret_cast<Bytef *>(out.data());
    strm.avail_out = uInt(out.size());
    const int ret = deflate(&strm, Z_FINISH);
    const int used = int(strm.total_out);
    deflateEnd(&strm);
    if (ret != Z_STREAM_END)
        return QByteArray();
    out.truncate(used);
    // 头应为 0x78 0x01（deflateInit2 level 1 生成 0x78 0x01）
    return out;
}

bool HisiFlasher::unlock(const QByteArray &unlockCode, QString *error)
{
    // UNLOCK：0x0B + unlockcode（帧封装由 buildFrame 完成）
    return m_session.sendCommand(FRAME_UNLOCK, unlockCode, 1.0, error);
}

bool HisiFlasher::flashPartition(const QString &name, const QByteArray &header,
                                 const QString &imagePath,
                                 std::function<void(qint64)> progress, QString *error)
{
    // HEAD：0x41 + 分区头
    if (!m_session.sendCommand(FRAME_HEAD, header, 2.0, error))
        return false;
    // DATA 块
    if (!sendDataBlocks(name, header, imagePath, progress, error))
        return false;
    // TAIL：0x43 + 分区头
    if (!m_session.sendCommand(FRAME_TAIL, header, 8.0, error))
        return false;
    return true;
}

bool HisiFlasher::sendDataBlocks(const QString &name, const QByteArray &header,
                                 const QString &imagePath,
                                 std::function<void(qint64)> progress, QString *error)
{
    QFile f(imagePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开分区 %1 镜像: %2").arg(name, imagePath);
        return false;
    }
    // fileSeq = 分区头偏移 20 的 4 字节（大端）
    quint32 fileSeqInt = 0;
    if (header.size() >= 24) {
        fileSeqInt = (quint32(quint8(header[20])) << 24)
                   | (quint32(quint8(header[21])) << 16)
                   | (quint32(quint8(header[22])) << 8)
                   | quint32(quint8(header[23]));
    }
    const qint64 fileSize = f.size();
    quint64 addr = 0;
    qint64 sent = 0;
    QByteArray buf(0x20000, Qt::Uninitialized);
    while (sent < fileSize) {
        const int toRead = int(qMin<qint64>(0x20000, fileSize - sent));
        const qint64 n = f.read(buf.data(), toRead);
        if (n <= 0) {
            // 中途读失败（EOF 不会触发：toRead 已钳制到剩余长度）：
            // 不得静默退出继续发 TAIL，否则截断分区被报告为刷写成功
            if (error) *error = QStringLiteral("读取分区 %1 镜像失败（偏移 %2）: %3")
                                    .arg(name).arg(sent).arg(f.errorString());
            return false;
        }
        const QByteArray raw = buf.left(int(n));
        const QByteArray comp = zlibCompress(raw);
        if (comp.isEmpty()) {
            if (error) *error = QStringLiteral("zlib 压缩失败");
            return false;
        }
        // DATA 帧体：0x0F + (fileSeqInt+addr) BE32 + origLen BE32 + 压缩数据
        //（0x0F 命令字节由 sendCommand/buildFrame 前置，payload 只含字段）
        QByteArray payload;
        const quint32 combined = fileSeqInt + quint32(addr);
        payload.append(char((combined >> 24) & 0xFF));
        payload.append(char((combined >> 16) & 0xFF));
        payload.append(char((combined >> 8) & 0xFF));
        payload.append(char(combined & 0xFF));
        const quint32 origLen = quint32(n);
        payload.append(char((origLen >> 24) & 0xFF));
        payload.append(char((origLen >> 16) & 0xFF));
        payload.append(char((origLen >> 8) & 0xFF));
        payload.append(char(origLen & 0xFF));
        payload += comp;
        // 超时 = max(1, min(8, 压缩后 MB × 1.5))
        const double mb = comp.size() / 1024.0 / 1024.0;
        const double timeout = qBound(1.0, mb * 1.5, 8.0);
        if (!m_session.sendCommand(FRAME_DATA, payload, timeout, error))
            return false;
        addr += quint64(n);
        sent += n;
        if (progress) progress(sent);
    }
    return true;
}

bool HisiFlasher::reboot(QString *error)
{
    // REBOOT → FORCE_REBOOT（先普通后强制，行为观察时序）
    if (!m_session.sendCommand(FRAME_REBOOT, QByteArray(), 0.3, error))
        return false;
    return m_session.sendCommand(FRAME_FORCE_REBOOT, QByteArray(), 0.3, error);
}

} // namespace hisi
