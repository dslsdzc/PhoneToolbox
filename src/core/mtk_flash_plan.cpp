#include "core/mtk_flash_plan.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
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

// 规则中文名：**未知规则原样输出**（可见即正确）—— 回落成"推导"会在将来加规则（D2/D3）时误标
QString ruleLabel(const QString &rule)
{
    if (rule == QLatin1String("exact"))
        return QStringLiteral("精确");
    if (rule == QLatin1String("prefix"))
        return QStringLiteral("前缀");
    if (rule == QLatin1String("derived"))
        return QStringLiteral("推导");
    return rule;
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
                            ruleLabel(e.matchRule)};
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
            // 只有**当前已有 name** 时 size 才归属该分区：散落在 name 之前的 size 一律忽略，
            // 否则会与上一个分区的 size 争夺同一个槽位（真 scatter 里顺序固定，纯防御）。
            if (name.isEmpty())
                continue;
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

// —— 现代 XML 方言 scatter + GPT 适配（Phase D2 T7）——

bool parseScatterXml(const QString &text, ScatterStorage want, QList<PartitionRef> &out, QString *error)
{
    out.clear();
    const QString wantStorage = (want == ScatterStorage::Emmc) ? QStringLiteral("HW_STORAGE_EMMC")
                                                               : QStringLiteral("HW_STORAGE_UFS");
    // 以 <partition_index ...>…</partition_index> 为块切分（真样本的块标签带 name 属性）
    static const QRegularExpression blockRe(QStringLiteral("<partition_index[^>]*>(.*?)</partition_index>"),
                                            QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression nameRe(QStringLiteral("<partition_name>([^<]*)</partition_name>"));
    static const QRegularExpression sizeRe(QStringLiteral("<partition_size>([^<]*)</partition_size>"));
    static const QRegularExpression storageRe(QStringLiteral("<storage>([^<]*)</storage>"));

    auto it = blockRe.globalMatch(text);
    while (it.hasNext()) {
        const QString body = it.next().captured(1);
        const QString storage = storageRe.match(body).captured(1).trimmed();
        if (storage != wantStorage)
            continue;                                  // **按 storage 过滤**（EMMC/UFS 两份副本）
        const QString name = nameRe.match(body).captured(1).trimmed();
        if (name.isEmpty())
            continue;
        const QString sizeText = sizeRe.match(body).captured(1).trimmed();
        bool ok = false;
        const quint64 size = sizeText.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)
                                 ? sizeText.mid(2).toULongLong(&ok, 16)
                                 : sizeText.toULongLong(&ok, 10);
        PartitionRef r;
        r.name = name;
        r.sizeBytes = ok ? size : 0;                   // 解析不出 → 0（= 未知，不参与大小校验）
        out << r;
    }
    if (out.isEmpty()) {
        if (error) *error = QStringLiteral("scatter XML 里没有解析出任何 %1 分区（partition_name/partition_size）")
                                .arg(wantStorage);
        return false;
    }
    return true;
}

bool parseScatterAnyDialect(const QString &text, QList<PartitionRef> &out, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    out.clear();                                    // 与 parseScatter / parseScatterXml 一致（失败路径不留半截表）
    if (!text.contains(QStringLiteral("<partition_index")))
        return parseScatter(text, out, error);            // 文本方言：D1 既有实现原样
    QList<PartitionRef> emmc, ufs;
    QString emmcErr, ufsErr;
    const bool okE = parseScatterXml(text, ScatterStorage::Emmc, emmc, &emmcErr);
    const bool okU = parseScatterXml(text, ScatterStorage::Ufs, ufs, &ufsErr);
    if (!okE && !okU) {
        if (error) *error = QStringLiteral("XML scatter 两份副本都解析失败：EMMC（%1）；UFS（%2）").arg(emmcErr, ufsErr);
        return false;
    }
    if (!okE || !okU) {                                   // 只有一份可用 → 用它，并说明另一份为何不可用
        say(QStringLiteral("XML scatter：只有 %1 副本可用（另一份：%2）")
                .arg(okE ? QStringLiteral("EMMC") : QStringLiteral("UFS"), okE ? ufsErr : emmcErr));
        out = okE ? emmc : ufs;
        return true;
    }
    if (emmc.size() == ufs.size()) {                      // 两份都成功 → 名字→大小完全一致才采信
        bool same = true;
        for (int i = 0; i < emmc.size() && same; ++i)
            same = (emmc.at(i).name == ufs.at(i).name && emmc.at(i).sizeBytes == ufs.at(i).sizeBytes);
        if (same) {
            out = emmc;
            return true;
        }
    }
    say(QStringLiteral("XML scatter：EMMC 与 UFS 两份副本不一致（%1 vs %2 条）—— 预览采用 EMMC 副本，"
                       "**写入判据仍以设备实读的分区表为准**").arg(emmc.size()).arg(ufs.size()));
    out = emmc;
    return true;
}

QList<PartitionRef> toPartitionRefs(const QList<mtkgpt::Partition> &parts, quint32 sectorSize)
{
    QList<PartitionRef> out;
    out.reserve(parts.size());
    for (const mtkgpt::Partition &p : std::as_const(parts)) {
        PartitionRef r;
        r.name = p.name;
        r.sizeBytes = mtkgpt::sizeBytes(p, sectorSize);
        out << r;
    }
    return out;
}

} // namespace mtkplan
