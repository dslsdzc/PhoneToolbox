#include "dat_image.h"
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <limits>

namespace imgdat {

namespace {
constexpr int kBlockSize = 4096;

struct Range { qint64 start, end; };

// 解析 "start,end start,end..."（项目格式: count 与 range 串空格分隔）;
// 奇数个逗号 token（如 "0,2,5"）、非数字、end<=start → false
bool parseRanges(const QString &spec, QList<Range> &out)
{
    out.clear();
    const QStringList parts = spec.split(',');
    if (parts.size() % 2 != 0)
        return false;
    for (int i = 0; i + 1 < parts.size(); i += 2) {
        bool a = false, b = false;
        const qint64 s = parts[i].toLongLong(&a);
        const qint64 e = parts[i + 1].toLongLong(&b);
        if (!a || !b || e <= s)
            return false;
        out.append({s, e});
    }
    return true;
}

bool fail(QString *error, const QString &msg)
{
    if (error) *error = msg;
    return false;
}
} // namespace

bool sdat2img(const QString &transferListPath, const QString &datPath,
              QByteArray &outRaw, QString *error)
{
    QFile tl(transferListPath);
    if (!tl.open(QIODevice::ReadOnly | QIODevice::Text))
        return fail(error, "无法打开 transfer.list");
    const QStringList lines = QString::fromUtf8(tl.readAll()).split('\n', Qt::SkipEmptyParts);
    if (lines.size() < 2)
        return fail(error, "transfer.list 内容不足");
    bool vOk = false;
    const int version = lines[0].trimmed().toInt(&vOk);
    if (!vOk || version < 1 || version > 4)
        return fail(error, QString("不支持的 transfer.list 版本 %1").arg(lines[0].trimmed()));
    bool tbOk = false;
    const qint64 totalBlocks = lines[1].trimmed().toLongLong(&tbOk);
    // 分配前校验: >0 且不超过 int 上限（真实 3GB+ 镜像在 qint64*4096 上会负回绕，
    // 恶意 totalBlocks 会导致巨量分配 terminate；先打开 .dat 再分配缓冲）
    if (!tbOk || totalBlocks <= 0
        || totalBlocks > static_cast<qint64>(std::numeric_limits<int>::max()) / kBlockSize)
        return fail(error, "transfer.list 总块数非法（非数字/<=0/过大）");
    QFile dat(datPath);
    if (!dat.open(QIODevice::ReadOnly))
        return fail(error, "无法打开 .dat 文件");
    outRaw.fill('\0', static_cast<int>(totalBlocks * kBlockSize));
    for (int li = 2; li < lines.size(); ++li) {
        const QString line = lines[li].trimmed();
        if (line.isEmpty())
            continue;
        static const QRegularExpression re("\\s+");
        const QStringList parts = line.split(re, Qt::SkipEmptyParts);
        if (parts.isEmpty())
            continue;
        const QString cmd = parts[0];
        if (cmd == "free") // 真实文件为 "free <stash_id>"（无 range）；stash 数据无法重建，忽略
            continue;
        if (parts.size() != 3)
            return fail(error, "transfer.list 行格式错误: " + line);
        bool countOk = false;
        const qint64 count = parts[1].toLongLong(&countOk);
        QList<Range> ranges;
        if (!countOk || count < 0 || !parseRanges(parts[2], ranges))
            return fail(error, "transfer.list 命令行非法: " + line);
        for (const Range &r : ranges) {
            if (r.end > totalBlocks)
                return fail(error, "transfer.list range 越界: " + line);
        }
        // move/range_ 的 count 语义不参与校验（src+dst 成对出现，count 信息性）
        if (cmd != "move" && cmd != "range_") {
            qint64 rangeBlocks = 0;
            for (const Range &r : ranges)
                rangeBlocks += r.end - r.start;
            if (rangeBlocks != count)
                return fail(error, "transfer.list 块数与 range 不一致: " + line);
        }
        if (cmd == "new" || cmd == "data") {
            for (const Range &r : ranges) {
                const qint64 len = (r.end - r.start) * kBlockSize;
                QByteArray block = dat.read(static_cast<qint64>(len));
                if (block.size() != static_cast<int>(len))
                    return fail(error, ".dat 数据不足");
                outRaw.replace(static_cast<int>(r.start * kBlockSize), static_cast<int>(len), block);
            }
        } else if (cmd == "zero") {
            // outRaw 已零初始化
        } else if (cmd == "erase") {
            // 忽略
        } else if (cmd == "range_" || cmd == "move") {
            // range_ (v3+) / move (v2): 从 outRaw 已有区域复制。
            // range 串为 src/dst 交错的平坦对（如 "0,1,1,2" = src(0,1) dst(1,2)）;
            // mid() 复制语义，src/dst 重叠安全。
            if (version < 2)
                return fail(error, QString("transfer.list 版本 %1 不支持 %2").arg(version).arg(cmd));
            if (ranges.size() % 2 != 0)
                return fail(error, QString("transfer.list %1 需 src/dst 成对: %2").arg(cmd, line));
            for (int i = 0; i + 1 < ranges.size(); i += 2) {
                const Range &src = ranges[i];
                const Range &dst = ranges[i + 1];
                const qint64 len = (src.end - src.start) * kBlockSize;
                if (dst.end - dst.start != src.end - src.start)
                    return fail(error, QString("transfer.list %1 src/dst 长度不一致: %2").arg(cmd, line));
                outRaw.replace(static_cast<int>(dst.start * kBlockSize), static_cast<int>(len),
                               outRaw.mid(static_cast<int>(src.start * kBlockSize), static_cast<int>(len)));
            }
        } else {
            return fail(error, QString("未知命令 %1: %2").arg(cmd, line));
        }
    }
    return true;
}

} // namespace imgdat
