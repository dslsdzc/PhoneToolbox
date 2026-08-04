#include "twrp_image.h"
#include <QFile>
#include <QFileInfo>
#include <QtEndian>

namespace imgtwrp {

namespace {
constexpr int kHeaderSize = 64;

struct WinHeader {
    quint8 version = 1;
    quint64 restoreSize = 0;
    quint64 packedSize = 0;
    quint64 restoreUsed = 0;
};

bool parseHeader(const QByteArray &hdr, WinHeader &out)
{
    if (hdr.size() < kHeaderSize || hdr.left(4) != "TWRP")
        return false;
    out.version = static_cast<quint8>(hdr[4]);
    out.restoreSize = qFromLittleEndian<quint64>(hdr.constData() + 5);
    out.packedSize = qFromLittleEndian<quint64>(hdr.constData() + 13);
    out.restoreUsed = qFromLittleEndian<quint64>(hdr.constData() + 21);
    return out.version >= 1 && out.version <= 3;
}

// 版本 2/3: 片段流。TWRP backup.cpp 的 twrpbackup 按版本选择 restore 数据格式：
// v2 为无压缩分片、v3 为压缩流（backup.cpp 中 twrp_backup v2/v3 每片段自带头部，
// 还原需按片段头解析、解压后拼接）——不能像 v1 那样按 packedSize 顺序拼接原始
// 字节。直接 readAll 会把压缩流/片段头原样当镜像内容返回（静默垃圾，比失败更
// 危险），且无长度校验。真实解压逻辑（对照 backup.cpp 的 twrpbackup 片段格式）
// 未实现前，显式拒绝。
bool extractV23(QFile &f, quint64 packedSize, QByteArray &out, QString *error)
{
    Q_UNUSED(f);
    Q_UNUSED(packedSize);
    Q_UNUSED(out);
    if (error)
        *error = QStringLiteral("v2/v3 备份解压未实现");
    return false;
}
} // namespace

bool isTwrpBackup(const QByteArray &header)
{
    return header.size() >= 4 && header.left(4) == "TWRP";
}

bool extractWin(const QString &winPath, QByteArray &outRaw, QString *error)
{
    QFile f(winPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = "无法打开 .win 文件";
        return false;
    }
    const QByteArray hdr = f.read(kHeaderSize);
    WinHeader wh;
    if (!parseHeader(hdr, wh)) {
        if (error) *error = "非法 TWRP 头";
        return false;
    }
    // .win001/.win002 等分段: 同目录按序号拼接（调用方已传入主文件，此处探测分段）
    const QFileInfo fi(winPath);
    QByteArray data;
    if (wh.version == 1) {
        const QByteArray body = f.read(static_cast<qint64>(wh.packedSize));
        data = body;
        // 检查分段文件
        for (int i = 1; ; ++i) {
            const QString seg = fi.absolutePath() + "/" + fi.completeBaseName() +
                                QString(".win%1").arg(i, 3, 10, QChar('0'));
            // 防呆: 调用方误传分段文件自身时（分段名恰好与 winPath 重合）避免重复拼接
            if (seg == winPath)
                break;
            QFile sf(seg);
            if (!sf.exists())
                break;
            if (!sf.open(QIODevice::ReadOnly))
                break;
            data.append(sf.readAll());
        }
        // packed_size 为跨分段总量: 主文件单独短读是合法场景（其余数据在 .win001 等分段），
        // 故必须在分段拼接完成之后校验总量 —— 主文件截断与分段链断裂
        // （如 .win001 缺失而 .win002 存在，拼接后总量仍不足）均在此暴露为失败。
        // v1 未压缩，restore_size == packed_size：两个声明长度都不得超出实际数据，
        // 否则调用方按 restore_size 消费会读到垃圾/越界。
        if (data.size() < static_cast<qint64>(wh.packedSize) ||
            data.size() < static_cast<qint64>(wh.restoreSize)) {
            if (error) *error = "备份数据不完整";
            return false;
        }
    } else {
        if (!extractV23(f, wh.packedSize, data, error))
            return false;
    }
    if (data.isEmpty()) {
        if (error) *error = "备份数据为空";
        return false;
    }
    outRaw = data;
    return true;
}

} // namespace imgtwrp
