#include "dat_image.h"
#include <QFile>
#include <QTextStream>
#include <QStringList>

namespace imgdat {

namespace {
constexpr int kBlockSize = 4096;

struct Range { qint64 start, end; };

bool parseRanges(const QString &spec, QList<Range> &out)
{
    out.clear();
    const QStringList parts = spec.split(',');
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
} // namespace

bool sdat2img(const QString &transferListPath, const QString &datPath,
              QByteArray &outRaw, QString *error)
{
    QFile tl(transferListPath);
    if (!tl.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = "无法打开 transfer.list";
        return false;
    }
    const QStringList lines = QString::fromUtf8(tl.readAll()).split('\n', Qt::SkipEmptyParts);
    if (lines.size() < 2) {
        if (error) *error = "transfer.list 内容不足";
        return false;
    }
    const int version = lines[0].trimmed().toInt();
    if (version < 1 || version > 4) {
        if (error) *error = QString("不支持的 transfer.list 版本 %1").arg(version);
        return false;
    }
    const qint64 totalBlocks = lines[1].trimmed().toLongLong();
    outRaw.fill('\0', static_cast<int>(totalBlocks * kBlockSize));
    QFile dat(datPath);
    if (!dat.open(QIODevice::ReadOnly)) {
        if (error) *error = "无法打开 .dat 文件";
        return false;
    }
    for (int li = 2; li < lines.size(); ++li) {
        const QStringList parts = lines[li].trimmed().split(' ');
        if (parts.size() < 2)
            continue;
        const QString cmd = parts[0];
        const int count = parts[1].toInt();
        QList<Range> ranges;
        if (parts.size() >= 3 && !parseRanges(parts[2], ranges))
            continue;
        if (cmd == "new" || cmd == "data") {
            for (const Range &r : ranges) {
                const qint64 len = (r.end - r.start) * kBlockSize;
                QByteArray block = dat.read(static_cast<qint64>(len));
                if (block.size() != static_cast<int>(len)) {
                    if (error) *error = ".dat 数据不足";
                    return false;
                }
                outRaw.replace(static_cast<int>(r.start * kBlockSize), static_cast<int>(len), block);
            }
        } else if (cmd == "zero") {
            // outRaw 已零初始化
        }
        // erase/free: 忽略
        // range_: 版本>=2，从 outRaw 已有区域复制（复制后处理，本任务先支持 new/data/zero）
        if (cmd == "range_" && version >= 2) {
            for (int i = 0; i + 1 < ranges.size(); i += 2) {
                const Range &src = ranges[i];
                const Range &dst = ranges[i + 1];
                const qint64 len = (src.end - src.start) * kBlockSize;
                if (dst.end - dst.start != src.end - src.start)
                    continue;
                outRaw.replace(static_cast<int>(dst.start * kBlockSize), static_cast<int>(len),
                               outRaw.mid(static_cast<int>(src.start * kBlockSize), static_cast<int>(len)));
            }
        }
    }
    return true;
}

} // namespace imgdat
