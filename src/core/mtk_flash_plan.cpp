#include "core/mtk_flash_plan.h"

#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <utility>   // std::as_const（遍历 Qt 容器，不得用 qAsConst）

#include "core/bytes_format.h"

namespace mtkplan {
namespace {

QString stemOf(const QString &path)
{
    return QFileInfo(path).baseName();     // 只剥最后一个扩展名（a.b.img → "a.b"）
}

QString fileNameOf(const QString &path)
{
    return QFileInfo(path).fileName();
}

bool fileSize(const QString &path, quint64 &out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    out = quint64(f.size());
    return true;
}

} // namespace

QStringList planHeaders()
{
    return {QStringLiteral("镜像文件"), QStringLiteral("目标分区"),
            QStringLiteral("镜像大小"), QStringLiteral("分区大小"), QStringLiteral("匹配规则")};
}

QList<QStringList> planRows(const MtkFlashPlan &plan)
{
    QList<QStringList> rows;
    for (const PlanEntry &e : std::as_const(plan.entries)) {
        rows << QStringList{fileNameOf(e.imagePath), e.partition,
                            humanBytes(e.imageSize),
                            e.partitionSize > 0 ? humanBytes(e.partitionSize) : QStringLiteral("未知"),
                            e.matchRule == QStringLiteral("exact") ? QStringLiteral("精确")
                            : e.matchRule == QStringLiteral("prefix") ? QStringLiteral("前缀")
                                                                     : QStringLiteral("推导")};
    }
    return rows;
}

QString planSummaryHtml(const MtkFlashPlan &plan)
{
    return QStringLiteral("<b>%1 个分区</b>，合计 <b>%2</b>（进度分母）%3")
        .arg(plan.entries.size())
        .arg(humanBytes(plan.totalBytes))
        .arg(plan.skippedOversize > 0
                 ? QStringLiteral("；<b>%1 个镜像因超过分区大小被跳过</b>").arg(plan.skippedOversize)
                 : QString());
}

bool buildMtkPlan(const QList<PartitionRef> &partitions, const QStringList &imagePaths,
                  MtkFlashPlan &out, QString *error)
{
    out = MtkFlashPlan{};
    if (imagePaths.isEmpty()) {
        if (error) *error = QStringLiteral("未选择任何镜像文件");
        return false;
    }

    // 去重：同一镜像被选择多次 → 只取首个 + 告警（不静默写两遍）
    QStringList images;
    QSet<QString> seen;
    for (const QString &p : std::as_const(imagePaths)) {
        const QString key = QFileInfo(p).absoluteFilePath();
        if (seen.contains(key)) {
            out.warnings << QStringLiteral("镜像被重复选择，只取首个：%1").arg(fileNameOf(p));
            continue;
        }
        seen.insert(key);
        images << p;
    }

    QList<quint64> sizes;
    for (const QString &p : std::as_const(images)) {
        quint64 sz = 0;
        if (!fileSize(p, sz)) {
            if (error) *error = QStringLiteral("无法读取镜像文件：%1").arg(p);
            return false;
        }
        sizes << sz;
    }

    // 参照表为空：目标分区名由镜像文件名推导（预览期无 scatter 的兜底；设备侧会再校验）
    if (partitions.isEmpty()) {
        out.warnings << QStringLiteral(
            "未提供分区参照表（scatter），目标分区名由镜像文件名推导；"
            "设备上是否存在该分区将在刷写时按设备分区表校验");
        for (int i = 0; i < images.size(); ++i) {
            PlanEntry e;
            e.partition = stemOf(images.at(i));
            e.imagePath = images.at(i);
            e.imageSize = sizes.at(i);
            e.matchRule = QStringLiteral("derived");
            out.entries << e;
            out.totalBytes += e.imageSize;
        }
        return true;
    }

    const int n = partitions.size();
    QList<int> matched(n, -1);
    QList<bool> used(images.size(), false);

    // 参照表重名：只有首个能拿到镜像（如实告警，不静默丢）
    QSet<QString> nameSeen;
    for (const PartitionRef &p : std::as_const(partitions)) {
        const QString key = p.name.toLower();
        if (key.isEmpty() || nameSeen.contains(key)) {
            out.warnings << QStringLiteral("参照分区表内有重名/空名分区：'%1'（只有首个能匹配到镜像）").arg(p.name);
            continue;
        }
        nameSeen.insert(key);
    }

    // 第一轮：精确（去扩展名的文件名 == 分区名，大小写不敏感）
    for (int pi = 0; pi < n; ++pi) {
        if (partitions.at(pi).name.isEmpty())
            continue;
        for (int ii = 0; ii < images.size(); ++ii) {
            if (used.at(ii))
                continue;
            if (stemOf(images.at(ii)).compare(partitions.at(pi).name, Qt::CaseInsensitive) == 0) {
                matched[pi] = ii;
                used[ii] = true;
                break;
            }
        }
    }
    // 第二轮：前缀（stem 以 "<分区名>_" 开头；唯一候选才用，多个 → 不猜）
    for (int pi = 0; pi < n; ++pi) {
        const QString pname = partitions.at(pi).name;
        if (matched.at(pi) >= 0 || pname.isEmpty())
            continue;
        QList<int> cand;
        for (int ii = 0; ii < images.size(); ++ii) {
            if (used.at(ii))
                continue;
            const QString stem = stemOf(images.at(ii));
            if (stem.size() > pname.size() && stem.left(pname.size()).compare(pname, Qt::CaseInsensitive) == 0
                && stem.at(pname.size()) == QLatin1Char('_'))
                cand << ii;
        }
        if (cand.size() == 1) {
            matched[pi] = cand.first();
            used[cand.first()] = true;
            out.warnings << QStringLiteral("镜像 %1 按前缀规则匹配到分区 %2（文件名带后缀）")
                                .arg(fileNameOf(images.at(cand.first())), pname);
        } else if (cand.size() > 1) {
            QStringList names;
            for (int ii : std::as_const(cand))
                names << fileNameOf(images.at(ii));
            out.warnings << QStringLiteral("分区 %1 有 %2 个候选镜像（%3）—— 不猜，跳过")
                                .arg(pname).arg(cand.size()).arg(names.join(QStringLiteral("、")));
        }
    }

    // 组装（参照表顺序）+ 大小校验（裁决 2：镜像 > 分区 = 跳过不写）
    int undersize = 0;
    for (int pi = 0; pi < n; ++pi) {
        const int ii = matched.at(pi);
        if (ii < 0)
            continue;
        const PartitionRef &ref = partitions.at(pi);
        const quint64 size = sizes.at(ii);
        if (ref.sizeBytes > 0 && size > ref.sizeBytes) {
            out.warnings << QStringLiteral("镜像 %1（%2）大于分区 %3（%4）—— 放不下，已跳过"
                                           "（写超分区会覆盖相邻分区数据）")
                                .arg(fileNameOf(images.at(ii)), humanBytes(size),
                                     ref.name, humanBytes(ref.sizeBytes));
            ++out.skippedOversize;
            continue;
        }
        if (ref.sizeBytes > 0 && size < ref.sizeBytes)
            ++undersize;
        PlanEntry e;
        e.partition = ref.name;
        e.imagePath = images.at(ii);
        e.imageSize = size;
        e.partitionSize = ref.sizeBytes;
        // 轮次即规则：去扩展名的文件名 == 分区名 = 精确，否则是第二轮的前缀命中
        e.matchRule = (stemOf(images.at(ii)).compare(ref.name, Qt::CaseInsensitive) == 0)
                          ? QStringLiteral("exact")
                          : QStringLiteral("prefix");
        out.entries << e;
        out.totalBytes += e.imageSize;
    }

    // 未匹配的镜像：逐条告警（跳过）
    for (int ii = 0; ii < images.size(); ++ii)
        if (!used.at(ii))
            out.warnings << QStringLiteral("镜像 %1 在参照分区表中没有对应分区 —— 已跳过")
                                .arg(fileNameOf(images.at(ii)));
    if (undersize > 0)
        out.warnings << QStringLiteral("其中 %1 个镜像小于分区容量（正常：分区尾部保持原样）").arg(undersize);
    if (out.entries.isEmpty())
        out.warnings << QStringLiteral("计划中没有任何可写入的分区");
    return true;
}

bool parseScatter(const QString &text, QList<PartitionRef> &out, QString *error)
{
    out.clear();
    QString name;
    quint64 size = 0;
    bool haveSize = false;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &raw : std::as_const(lines)) {
        QString line = raw.trimmed();
        // 真 scatter 里**只有** partition_index 行带前导 '-'，partition_name / partition_size
        // 是缩进的裸 "key: value"（实测 MT6765_Android_scatter.txt）—— 按 key 解析，
        // 带与不带前导 '-' 的两种写法都认。
        if (line.startsWith(QLatin1Char('-')))
            line = line.mid(1).trimmed();
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        const QString key = line.left(colon).trimmed();
        const QString value = line.mid(colon + 1).trimmed();
        if (key == QLatin1String("partition_name")) {
            if (!name.isEmpty() && haveSize)
                out.append(PartitionRef{name, size});
            name = value;
            size = 0;
            haveSize = false;
        } else if (key == QLatin1String("partition_size")) {
            bool ok = false;
            const quint64 v = value.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)
                                  ? value.mid(2).toULongLong(&ok, 16)
                                  : value.toULongLong(&ok, 10);
            if (ok) { size = v; haveSize = true; }
        }
    }
    if (!name.isEmpty() && haveSize)
        out.append(PartitionRef{name, size});
    if (out.isEmpty()) {
        if (error) *error = QStringLiteral("scatter 里没有解析出任何分区（partition_name/partition_size）");
        return false;
    }
    return true;
}

} // namespace mtkplan
