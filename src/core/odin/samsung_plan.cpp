// src/core/odin/samsung_plan.cpp
//
// 计划层：把「选中的 .tar.md5 集合 + PIT」变成可刷写清单。**只读文件**（流式索引 + 校验行），
// 不解包、不落盘。匹配规则见头文件；真包依据见 docs/superpowers/specs/samsung-odin-facts.md §3。
#include "samsung_plan.h"

#include <QFile>
#include <QFileInfo>
#include <utility>

#include "image_engine/tar_image.h"

namespace odin {
namespace {

void setErr(QString *error, const QString &msg)
{
    if (error) *error = msg;
}

bool endsWithPit(const QString &name)
{
    return name.endsWith(QLatin1String(".pit"), Qt::CaseInsensitive);
}

struct IndexedImage {
    QString name;          // tar 内条目名（已去尾 '/'）
    quint64 offset = 0;
    quint64 size = 0;
    int fileIndex = -1;
};

} // namespace

bool loadPitFromPackage(const QStringList &tarMd5Files, PitTable &out,
                        QString *pitPathOut, QString *error)
{
    // 在所选包内找 .pit 条目。**唯一**才取 —— 多个则拒（不同 CSC 的 PIT 可能不同，不擅自选一个）。
    struct Hit { QString path; QString entryName; quint64 offset = 0; quint64 size = 0; };
    QList<Hit> hits;
    for (const QString &path : tarMd5Files) {
        QList<imgtar::TarIndexEntry> idx;
        QString ierr;
        if (!imgtar::indexTarStream(path, idx, nullptr, &ierr)) {
            setErr(error, QStringLiteral("无法索引固件包 %1：%2").arg(QFileInfo(path).fileName(), ierr));
            return false;
        }
        for (const imgtar::TarIndexEntry &e : std::as_const(idx))
            if (!e.isDir && endsWithPit(e.name))
                hits.append({path, e.name, e.offset, e.size});
    }
    if (hits.isEmpty()) {
        setErr(error, QStringLiteral("所选包内未找到 .pit（请显式指定 PIT 文件）"));
        return false;
    }
    if (hits.size() > 1) {
        QStringList names;
        for (const Hit &h : std::as_const(hits))
            names << QStringLiteral("%1（%2）").arg(h.entryName, QFileInfo(h.path).fileName());
        setErr(error, QStringLiteral("所选包内有 %1 个 .pit，无法确定用哪个：%2")
                          .arg(hits.size()).arg(names.join(QStringLiteral("、"))));
        return false;
    }
    const Hit &hit = hits.first();
    if (hit.size == 0 || hit.size > 1024 * 1024) {      // 真样本 PIT 2924..18492 B；1 MiB 上限防呆
        setErr(error, QStringLiteral("包内 PIT 大小异常（%1 字节）：%2").arg(hit.size).arg(hit.entryName));
        return false;
    }
    QFile f(hit.path);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(qint64(hit.offset))) {
        setErr(error, QStringLiteral("无法读取包内 PIT：%1").arg(hit.entryName));
        return false;
    }
    const QByteArray data = f.read(qint64(hit.size));
    if (quint64(data.size()) != hit.size) {
        setErr(error, QStringLiteral("包内 PIT 数据不完整（%1：期望 %2 字节，读到 %3）")
                          .arg(hit.entryName).arg(hit.size).arg(data.size()));
        return false;
    }
    if (!parsePit(data, out, error))
        return false;
    if (pitPathOut)
        *pitPathOut = hit.path + QStringLiteral("（包内 %1）").arg(hit.entryName);
    return true;
}

bool buildSamsungPlan(const QStringList &tarMd5Files, const PitTable &pit,
                      SamsungPlan &plan, QString *error, const QString &pitSource)
{
    plan = SamsungPlan{};
    plan.pitSource = pitSource;
    if (tarMd5Files.isEmpty()) {
        setErr(error, QStringLiteral("未选择固件包（.tar.md5）"));
        return false;
    }
    if (pit.entries.isEmpty()) {
        setErr(error, QStringLiteral("PIT 无条目，无法构建刷写计划"));
        return false;
    }

    // ① 逐包：校验行状态 + 流式索引（重名首个为准 + 告警，不静默丢）
    QList<IndexedImage> images;
    for (int fi = 0; fi < tarMd5Files.size(); ++fi) {
        const QString path = tarMd5Files.at(fi);
        const QString base = QFileInfo(path).fileName();

        SamsungPlanFile f;
        f.path = path;
        f.sizeBytes = quint64(QFileInfo(path).size());

        bool hasFooter = false;
        QString verr;
        const bool verified = imgtar::verifyMd5FooterStream(path, &hasFooter, &verr);
        f.md5HasFooter = hasFooter;
        f.verifyOk = verified && hasFooter;
        if (!verified)
            plan.warnings << QStringLiteral("包校验失败（%1）：%2").arg(base, verr);
        else if (!hasFooter)
            plan.warnings << QStringLiteral("包内无 MD5 校验行，未做完整性校验：%1").arg(base);

        QList<imgtar::TarIndexEntry> idx;
        QString ierr;
        if (!imgtar::indexTarStream(path, idx, nullptr, &ierr)) {
            setErr(error, QStringLiteral("无法索引固件包 %1：%2").arg(base, ierr));
            return false;
        }
        for (const imgtar::TarIndexEntry &e : std::as_const(idx)) {
            if (e.isDir)
                continue;
            f.entryNames << e.name;
            bool dup = false;
            for (const IndexedImage &im : std::as_const(images))
                dup = dup || im.name.compare(e.name, Qt::CaseInsensitive) == 0;
            if (dup) {
                plan.warnings << QStringLiteral("包内条目重名，已忽略后者：%1（%2）").arg(e.name, base);
                continue;
            }
            images.append({e.name, e.offset, e.size, fi});
        }
        plan.files.append(f);
    }

    // ② 逐 PIT 条目匹配（规则 1-4）+ 大小核对（规则 7）
    QList<bool> claimed(images.size(), false);
    int smallImages = 0;
    int noImageName = 0;
    for (const PitEntry &pe : pit.entries) {
        if (!pe.isFlashable())
            continue;                                   // libpit.h:107-110
        if (!pe.hasImageName()) {                       // 规则 2：空 / 字面 "-" → 不是"缺"，是没声明
            ++noImageName;
            continue;
        }
        int hit = -1;
        QString rule;
        for (int i = 0; i < images.size(); ++i) {
            if (images.at(i).name.compare(pe.flashFilename, Qt::CaseInsensitive) == 0) {
                hit = i;
                rule = QStringLiteral("文件名精确匹配");
                break;
            }
        }
        if (hit < 0 && endsWithPit(pe.flashFilename)) {  // 规则 4：已知反例的回退
            QList<int> candidates;
            for (int i = 0; i < images.size(); ++i)
                if (endsWithPit(images.at(i).name))
                    candidates << i;
            if (candidates.size() == 1) {
                hit = candidates.first();
                rule = QStringLiteral(".pit 唯一性回退");
                plan.warnings << QStringLiteral("PIT 条目 %1 声明的文件名（%2）与包内不一致，"
                                                "已按 .pit 唯一性回退匹配到 %3（请核对包与机型是否配套）")
                                     .arg(pe.partitionName, pe.flashFilename, images.at(hit).name);
            } else {
                plan.warnings << QStringLiteral("PIT 条目 %1 声明的镜像 %2 不在所选包内"
                                                "（包内有 %3 个 .pit，无法按唯一性回退）")
                                     .arg(pe.partitionName, pe.flashFilename).arg(candidates.size());
                continue;
            }
        }
        if (hit < 0) {                                  // 规则 5
            plan.warnings << QStringLiteral("PIT 条目 %1 声明的镜像 %2 不在所选包内（跳过）")
                                 .arg(pe.partitionName, pe.flashFilename);
            continue;
        }
        claimed[hit] = true;

        SamsungPlanEntry e;
        e.partition = pe.partitionName;
        e.imageFile = images.at(hit).name;
        e.sizeBytes = images.at(hit).size;
        e.sourceOffset = images.at(hit).offset;
        e.fileIndex = images.at(hit).fileIndex;
        e.matchRule = rule;
        e.pit = pe;

        const quint64 partBytes = pe.partitionBytes();
        if (partBytes == 0) {
            plan.warnings << QStringLiteral("分区 %1 未声明大小（blockCount=0），无法核对镜像 %2 是否放得下")
                                 .arg(pe.partitionName, e.imageFile);
        } else if (e.sizeBytes > partBytes) {
            plan.warnings << QStringLiteral("镜像 %1（%2 字节）大于分区 %3（%4 字节），放不下 —— 请核对包与机型")
                                 .arg(e.imageFile).arg(e.sizeBytes).arg(pe.partitionName).arg(partBytes);
        } else if (e.sizeBytes < partBytes) {
            ++smallImages;                              // 规则 7 的降噪：逐条太吵，最后出一条汇总
        }

        plan.entries.append(e);
        plan.totalBytes += e.sizeBytes;
    }

    // ③ 包内未被认领的条目（规则 6）
    for (int i = 0; i < images.size(); ++i)
        if (!claimed.at(i))
            plan.warnings << QStringLiteral("包内镜像 %1 未出现在 PIT 中（跳过）").arg(images.at(i).name);

    // ④ 汇总告警（规则 2/7 的降噪版；条目顺序仍按 PIT，规则 8）
    if (noImageName > 0)
        plan.warnings << QStringLiteral("PIT 中有 %1 个条目未声明镜像文件名，已跳过").arg(noImageName);
    if (smallImages > 0)
        plan.warnings << QStringLiteral("%1 个条目的镜像小于分区（正常：剩余区域保持原样）").arg(smallImages);

    if (plan.entries.isEmpty()) {                       // 规则 9：fail-closed
        setErr(error, QStringLiteral("所选包内没有任何镜像能在 PIT 中找到对应分区（%1）")
                          .arg(pitSource.isEmpty() ? QStringLiteral("PIT") : pitSource));
        return false;
    }
    return true;
}

} // namespace odin
