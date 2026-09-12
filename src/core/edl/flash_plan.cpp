#include "flash_plan.h"

#include "image_engine/sparse_image.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QXmlStreamReader>
#include <algorithm>

namespace edl {

namespace {

// ---- 属性读取：沿用 qdl "取不到即记错、该条目丢弃" 的语义（reference/qdl/src/util.c:74-125）----

// 原始字符串属性；属性缺失 → *ok=false。
// 参照 reference/qdl/src/util.c:90-105（attr_as_string：xmlGetProp 取不到 → 记错）
QString attrRaw(QXmlStreamReader &reader, const char *name, bool *ok)
{
    const QXmlStreamAttributes attrs = reader.attributes();
    if (!attrs.hasAttribute(QLatin1String(name))) {
        if (ok) *ok = false;
        return QString();
    }
    if (ok) *ok = true;
    return attrs.value(QLatin1String(name)).toString();
}

// 数值属性：base 0 解析（识别 0x 前缀），与 qdl strtoul(value, NULL, 0) 一致（reference/qdl/src/util.c:80）。
//
// 注意 start_sector：qdl 把它读成**字符串**（reference/qdl/src/program.c:261、src/patch.c:46），
// 因为真机 XML 里它可能是 firehose 表达式（如 "NUM_DISK_SECTORS-5."，src/firehose.c:874-879
// 明确注释"解析它会把写入地址搞错"）。本项目模型按 spec §3.3 用 quint64，表达式无法表示，
// 故此处把"存在但不可解析"与"缺失"同等处理（丢弃条目 + warning）——
// 宁可少条目并告警，也不把表达式静默当成 0 而写到扇区 0。
quint64 attrU64(QXmlStreamReader &reader, const char *name, bool *ok)
{
    bool present = false;
    const QString raw = attrRaw(reader, name, &present);
    bool numOk = false;
    const quint64 value = raw.toULongLong(&numOk, 0);
    if (ok) *ok = present && numOk;
    return numOk ? value : 0;
}

// 必需属性收集器：任一必需属性缺失/空/不可解析 → 该条目被丢弃并记 warning。
// qdl 的 attr_as_* 取不到即 errors++，load_*_tag 随即丢弃整条（reference/qdl/src/program.c:254-280、
// src/patch.c:41-56）；本类只保留"第一个"出问题的属性名，语义与 errors 计数等价（都只导致丢弃该条目）。
class RequiredAttrs
{
public:
    explicit RequiredAttrs(QXmlStreamReader &reader) : m_reader(reader) {}

    // 字符串属性；空串按缺失处理（qdl attr_as_string 遇空串返回 NULL，reference/qdl/src/util.c:101-102）
    QString str(const char *name)
    {
        bool ok = false;
        const QString value = attrRaw(m_reader, name, &ok);
        note(ok && !value.isEmpty(), name);
        return value;
    }

    quint64 u64(const char *name)
    {
        bool ok = false;
        const quint64 value = attrU64(m_reader, name, &ok);
        note(ok, name);
        return value;
    }

    quint32 u32(const char *name) { return quint32(u64(name)); }

    bool    ok() const { return m_missing.isEmpty(); }
    QString missing() const { return m_missing; }

    // 供"非通用规则"的属性（如 start_sector：表达式合法、只认缺失）手工登记缺失
    void noteMissing(const char *name) { note(false, name); }

private:
    void note(bool ok, const char *name)
    {
        if (!ok && m_missing.isEmpty())
            m_missing = QLatin1String(name);
    }

    QXmlStreamReader &m_reader;
    QString m_missing;
};

// start_sector 取值（program / patch / erase 同款规则）：
//   纯十进制 → startSector=N；否则**原样**存入 startSectorExpr 且 startSector=0。
//   表达式**不是错误**：不记 warning、不丢条目（qdl 把它当字符串读并原样下发：
//   reference/qdl/src/program.c:261、src/patch.c:46；src/firehose.c:874-879 注释明确
//   "解析它会把写入地址搞错"）。真机样本里 patch0.xml 的 Backup-GPT 头修补、
//   rawprogram0.xml 的 label=BackupGPT 都是这一类 —— 丢掉即漏掉备份 GPT 头修补。
//   只有属性**本身缺失**（或空串）才算错 → 记缺失（条目丢弃 + warning）。
// 注：其它数值属性走 attrU64 的 base 0（同 qdl strtoul(...,0)）；start_sector 只认纯十进制，
// 0x/表达式一律进原样透传分支，避免主机擅自改写下发形态。
// required=false：属性可缺省（erase 标签缺省 = 整 LUN 擦，见 loadEraseTag），缺省不记错。
void readStartSector(QXmlStreamReader &reader, PlanEntry &e, RequiredAttrs &attrs, bool required = true)
{
    bool present = false;
    const QString raw = attrRaw(reader, "start_sector", &present);
    if (!present || raw.isEmpty()) {
        if (required)
            attrs.noteMissing("start_sector");
        return;
    }
    bool numOk = false;
    const quint64 value = raw.toULongLong(&numOk, 10);
    if (numOk) {
        e.startSector = value;
    } else {
        e.startSectorExpr = raw;   // 表达式原样保留；startSector 保持 0（模型契约）
    }
}

// 条目 LUN（XML 属性）与文件序号（调用方给出）不一致 → 告警但不改值：
// 两个参照的刷写实现都**只读属性、不读文件名**（reference/qdl/src/program.c:259、
// edl/edlclient/Library/firehose_client.py:950-962），属性值才是下发值；
// 协议速查 §4 的约定是 rawprogramN.xml 的序号 == 条目 physical_partition_number，不一致即文件异常。
void warnLunMismatch(const QString &tag, const PlanEntry &e, quint32 fileLun, QStringList &warnings)
{
    if (e.lun == fileLun)
        return;
    warnings << QStringLiteral("%1 条目 lun=%2 与文件序号 %3 不一致（下发以 XML 属性为准）")
                    .arg(tag, QString::number(e.lun), QString::number(fileLun));
}

// rawprogram 的 <program> → PlanEntry{Program}
// 必需属性集（缺一即丢条目）：SECTOR_SIZE_IN_BYTES / filename / label / num_partition_sectors /
// physical_partition_number / start_sector / file_sector_offset
// （reference/qdl/src/program.c:254-271 + src/util.c:74-105；NAND 分支改读 PAGES_PER_BLOCK/last_sector，
//  本项目不覆盖 NAND，见 spec §8）
void loadProgramTag(QXmlStreamReader &reader, quint32 fileLun, const QString &xmlDir,
                    QList<PlanEntry> &out, QStringList &warnings)
{
    RequiredAttrs attrs(reader);
    PlanEntry e;
    e.action = PlanEntry::Action::Program;
    e.sectorSize = attrs.u32("SECTOR_SIZE_IN_BYTES");
    const QString filename = attrs.str("filename");
    e.partitionName = attrs.str("label");               // label → partitionName
    e.numSectors = attrs.u64("num_partition_sectors");  // 直接取 XML 值；sparse 展开由 Task 2 校验步骤做
    e.lun = attrs.u32("physical_partition_number");
    readStartSector(reader, e, attrs);                  // 十进制 → startSector；表达式 → startSectorExpr 原样保留
    attrs.u64("file_sector_offset");                    // 必需属性，但 spec §3.3 模型无对应字段：读出即弃
    if (!attrs.ok()) {
        warnings << QStringLiteral("rawprogram 条目被跳过：缺属性 %1（label=%2）")
                        .arg(attrs.missing(), e.partitionName);
        return;
    }
    // sparse 缺失不算错；仅字面 "true" 为真（reference/qdl/src/util.c:108-125 attr_as_bool）
    e.sparse = attrRaw(reader, "sparse", nullptr) == QLatin1String("true");
    // imageFile = XML 所在目录 + filename（绝对化）
    e.imageFile = QDir(xmlDir).filePath(filename);
    // rawBytes：sparse 展开后的实际字节数，由 Task 2 校验步骤填充（本函数只做 XML → 模型映射）
    warnLunMismatch(QStringLiteral("program"), e, fileLun, warnings);
    out << e;
}

// rawprogram 的 <erase> → PlanEntry{Erase}
// 必需：SECTOR_SIZE_IN_BYTES / physical_partition_number（reference/qdl/src/program.c:39-42）；
// start_sector / num_partition_sectors **可缺省 = 整 LUN 擦**（<erase> 省略 start/count 即整 LUN，
// reference/qdl/src/firehose.c:611-628），缺省置 0，由会话层按"整 LUN"解释。
// 注：qdl 的 load_erase_tag 把这四项都当必需且拒绝 num_sectors=0（src/program.c:39-59）——
// 本项目按 spec §3.4/§4 允许整 LUN 擦，故此处放宽为可缺省。
//
// **安全语义（Task 1 审查发现、Task 2 修复）**：num_partition_sectors 属性**存在但不可解析**
// （"abc"、firehose 表达式）曾被静默当 0，而 0 在本模型里 ="整 LUN 擦" ⇒ 定点擦被静默放大成
// 整盘擦。qdl 对这类输入是直接拒绝的（src/program.c:56-62 "erase tag with num_sectors=0 not
// allowed"）。现在只有**属性确实缺失**才保留整 LUN 语义；其余（不可解析 / 表达式 / 显式 0）
// 一律丢弃该条目 + 中文 warning —— 与本文件"宁可少条目并告警，也不猜"的原则一致。
void loadEraseTag(QXmlStreamReader &reader, quint32 fileLun,
                  QList<PlanEntry> &out, QStringList &warnings)
{
    RequiredAttrs attrs(reader);
    PlanEntry e;
    e.action = PlanEntry::Action::Erase;
    e.sectorSize = attrs.u32("SECTOR_SIZE_IN_BYTES");
    e.lun = attrs.u32("physical_partition_number");
    if (!attrs.ok()) {
        warnings << QStringLiteral("rawprogram erase 条目被跳过：缺属性 %1（lun=%2）")
                        .arg(attrs.missing()).arg(fileLun);
        return;
    }
    // 缺省 0 = 整 LUN；表达式同样原样保留。
    // 注意（Task 5）：startSectorExpr 非空时不能按"numSectors==0 = 整 LUN"处理。
    readStartSector(reader, e, attrs, /*required=*/false);

    bool countPresent = false;
    const QString countRaw = attrRaw(reader, "num_partition_sectors", &countPresent);
    if (!countPresent) {
        // 属性缺失 = 整 LUN 擦（唯一保留该语义的形态）
        e.numSectors = 0;
    } else {
        bool countOk = false;
        const quint64 count = countRaw.toULongLong(&countOk, 0);
        if (!countOk || count == 0) {
            warnings << QStringLiteral("rawprogram erase 条目被跳过：num_partition_sectors=\"%1\" %2"
                                       "（lun=%3，start_sector=%4）—— 按 0 处理会把定点擦放大成整 LUN 擦")
                            .arg(countRaw,
                                 countOk ? QStringLiteral("不是有效的定点擦范围（整 LUN 擦请省略该属性）")
                                         : QStringLiteral("不可解析"))
                            .arg(e.lun)
                            .arg(e.startSectorExpr.isEmpty() ? QString::number(e.startSector)
                                                             : e.startSectorExpr);
            return;
        }
        e.numSectors = count;
    }
    warnLunMismatch(QStringLiteral("erase"), e, fileLun, warnings);
    out << e;
}

// patch 文件的 <patch> → PlanEntry{Patch}
// 8 属性全部必需（reference/qdl/src/patch.c:41-48，两实现一致，协议速查 §2）
void loadPatchTag(QXmlStreamReader &reader, quint32 fileLun,
                  QList<PlanEntry> &out, QStringList &warnings)
{
    RequiredAttrs attrs(reader);
    PlanEntry e;
    e.action = PlanEntry::Action::Patch;
    readStartSector(reader, e, attrs);  // 十进制 → startSector；表达式 → startSectorExpr 原样保留
    e.byteOffset = attrs.u64("byte_offset");
    e.lun = attrs.u32("physical_partition_number");
    e.sizeInBytes = attrs.u32("size_in_bytes");
    // **原样保留**：NUM_DISK_SECTORS-6. / CRC32(2,4096) 这类表达式主机不解释
    // （reference/qdl/src/firehose.c:1420-1421 原样下发；协议速查 §2）
    e.value = attrs.str("value");
    const QString filename = attrs.str("filename");
    e.sectorSize = attrs.u32("SECTOR_SIZE_IN_BYTES");
    e.what = attrs.str("what");
    if (!attrs.ok()) {
        warnings << QStringLiteral("patch 条目被跳过：属性 %1 缺失或不可解析（filename=%2）")
                        .arg(attrs.missing(), filename);
        return;
    }
    e.partitionName = filename;  // patch 无 label，用 filename 作条目标识（供日志）
    // filename == "DISK" 才是"下发设备"：真实文件名（gpt_main0.bin / gpt_backup0.bin 等）是给
    // QFIL/Trace32 离线改 bin 用的，qdl 与 bkerler 都跳过
    // （reference/qdl/src/firehose.c:1405-1406；edl/edlclient/Library/firehose_client.py:977-978）
    if (filename != QLatin1String("DISK")) {
        warnings << QStringLiteral("patch 条目被跳过：filename=%1 非 DISK（离线改 bin 用，不下发设备）")
                        .arg(filename);
        return;
    }
    e.imageFile = filename;  // "DISK" 哨兵：打设备磁盘偏移，不是本地文件（模型注释）
    // what 存进模型但**只进日志**：出站 <patch> 不发 what
    // （reference/qdl/src/firehose.c:1408,1410-1424；edl/edlclient/Library/firehose.py:427-443）
    warnLunMismatch(QStringLiteral("patch"), e, fileLun, warnings);
    out << e;
}

bool openXml(const QString &xmlPath, const QString &kind, QFile &file, QString *error)
{
    file.setFileName(xmlPath);
    if (file.open(QIODevice::ReadOnly))
        return true;
    if (error)
        *error = QStringLiteral("无法打开 %1 XML：%2（%3）").arg(kind, xmlPath, file.errorString());
    return false;
}

bool xmlParseError(const QString &xmlPath, const QString &kind, QXmlStreamReader &reader, QString *error)
{
    if (!reader.hasError())
        return false;
    if (error)
        *error = QStringLiteral("%1 XML 解析失败：%2（%3）").arg(kind, xmlPath, reader.errorString());
    return true;
}

} // namespace

bool parseRawprogramXml(const QString &xmlPath, quint32 lun,
                        QList<PlanEntry> &out, QStringList &warnings, QString *error)
{
    QFile file;
    if (!openXml(xmlPath, QStringLiteral("rawprogram"), file, error))
        return false;

    // filename 是包内相对名 → imageFile 以 XML 所在目录为基准绝对化
    const QString xmlDir = QFileInfo(xmlPath).absolutePath();

    QXmlStreamReader reader(&file);
    while (!reader.atEnd()) {
        if (reader.readNext() != QXmlStreamReader::StartElement)
            continue;
        const QString tag = reader.name().toString();
        if (tag == QLatin1String("program"))
            loadProgramTag(reader, lun, xmlDir, out, warnings);
        else if (tag == QLatin1String("erase"))
            loadEraseTag(reader, lun, out, warnings);
        // 其它标签忽略（qdl 对未识别标签报错中止：reference/qdl/src/program.c:348-351；
        // 本实现按 brief 忽略以容忍真机 XML 的扩展标签 —— 未知标签不产生条目，不静默丢条目数据）
    }
    if (xmlParseError(xmlPath, QStringLiteral("rawprogram"), reader, error))
        return false;
    // 注：文件为空/根元素不匹配 → 不算错也不新增条目（out 只追加）；
    // 调用方按"本次新增条目数"判断该文件是否真的提供了计划（Task 3）。
    return true;
}

bool parsePatchXml(const QString &xmlPath, quint32 lun,
                   QList<PlanEntry> &out, QStringList &warnings, QString *error)
{
    QFile file;
    if (!openXml(xmlPath, QStringLiteral("patch"), file, error))
        return false;

    QXmlStreamReader reader(&file);
    while (!reader.atEnd()) {
        if (reader.readNext() != QXmlStreamReader::StartElement)
            continue;
        if (reader.name().toString() == QLatin1String("patch"))
            loadPatchTag(reader, lun, out, warnings);
        // 其它标签忽略；qdl 对 patch 文件的未识别标签是"告警后继续"（reference/qdl/src/patch.c:33-35）
    }
    if (xmlParseError(xmlPath, QStringLiteral("patch"), reader, error))
        return false;
    return true;
}

// ================= Task 2：排序/统计 + 校验 =================

namespace {

// 排序分组：Erase 一律先于 Program（整 LUN 擦要覆盖掉旧数据后再写，spec §3.5），
// Patch 一律最后（打的是 GPT 头等任意磁盘偏移，必须在 program 之后）。
int actionRank(PlanEntry::Action a)
{
    switch (a) {
    case PlanEntry::Action::Erase:   return 0;
    case PlanEntry::Action::Program: return 1;
    case PlanEntry::Action::Patch:   return 2;
    }
    return 3;
}

// 报错/告警里的条目标识：program/patch 用 label（patch 无 label，解析层已用 filename 填
// partitionName），erase 在 XML 里没有名字 → 用动作名。保证非空，文案里永远能定位到条目。
QString entryName(const PlanEntry &e)
{
    if (!e.partitionName.isEmpty())
        return e.partitionName;
    if (!e.imageFile.isEmpty())
        return e.imageFile;
    return e.action == PlanEntry::Action::Erase ? QStringLiteral("erase") : QStringLiteral("(未命名)");
}

// 规则 4/5/6 的 Program 侧：镜像在不在、sparse 头算出的扇区数是多少、sha256 记不记 warning。
// **规则 5 会就地修正条目**（numSectors / rawBytes / sparse 标记）—— 见 validatePlan 注释。
void checkProgramImage(PlanEntry &e, PlanCheck &chk)
{
    if (e.imageFile.isEmpty()) {
        chk.errors << QStringLiteral("条目 %1 未指定镜像文件（imageFile 为空）").arg(entryName(e));
        return;
    }
    QFile f(e.imageFile);
    if (!f.open(QIODevice::ReadOnly)) {
        chk.errors << QStringLiteral("条目 %1 的镜像文件无法打开：%2（%3）")
                          .arg(entryName(e), e.imageFile, f.errorString());
        return;
    }
    if (!e.sparse)
        return;   // 非 sparse：存在且可读即可（不校验文件大小 —— 下发的 num_partition_sectors 来自 XML）

    // 规则 5：读 sparse 头 → 去 sparse 后的 raw 字节数（协议速查 §1：bkerler 按去 sparse 后大小
    // 算 num_partition_sectors，Library/sparse.py:53-76 + firehose.py:475-486）
    const QByteArray head = f.read(28);
    quint64 rawBytes = 0;
    if (!imgsparse::sparseRawSizeFromHeader(head, rawBytes)) {
        // 标了 sparse="true" 但文件里没有 sparse 头。qdl 先例（reference/qdl/src/program.c:79-93）：
        // 若 文件大小 == SECTOR_SIZE_IN_BYTES × num_partition_sectors，判为"标记写错"，改按非 sparse
        // 处理并告警；对不上则报错 —— 宁可拒刷，也不猜文件结构。
        const quint64 declared = e.numSectors * e.sectorSize;
        if (declared != 0 && static_cast<quint64>(f.size()) == declared) {
            e.sparse = false;
            chk.warnings << QStringLiteral("条目 %1 标记 sparse=\"true\" 但文件头不是 sparse（文件 %2 字节"
                                           " == 声明 %3 字节）—— 按非 sparse 处理（参照 reference/qdl/src/program.c:79-93）")
                                .arg(entryName(e)).arg(f.size()).arg(declared);
        } else {
            chk.errors << QStringLiteral("条目 %1 标记 sparse 但文件头不是 sparse 格式：%2（文件 %3 字节，"
                                         "声明 %4 扇区）")
                              .arg(entryName(e), e.imageFile).arg(f.size()).arg(e.numSectors);
        }
        return;
    }
    if (e.sectorSize == 0) {
        // qdl 对 sparse 且 SECTOR_SIZE_IN_BYTES=0 直接报错（reference/qdl/src/program.c:98-101）：
        // 没有扇区大小就无法把字节数换算成扇区数。
        chk.errors << QStringLiteral("条目 %1 的 SECTOR_SIZE_IN_BYTES 为 0，无法换算 sparse 扇区数")
                          .arg(entryName(e));
        return;
    }
    const quint64 declared = e.numSectors * e.sectorSize;
    if (declared != rawBytes) {
        // 不足整扇区的尾巴向上取整：firehose 只能按扇区下发（protocol facts §1）
        const quint64 corrected = (rawBytes + e.sectorSize - 1) / e.sectorSize;
        chk.warnings << QStringLiteral("条目 %1 的 sparse 头声明 %2 字节（%3 扇区），与 XML 的 %4 扇区"
                                       "（%5 字节）不符 —— 以文件头为准修正 numSectors")
                            .arg(entryName(e)).arg(rawBytes).arg(corrected)
                            .arg(e.numSectors).arg(declared);
        e.numSectors = corrected;
    }
    e.rawBytes = rawBytes;   // Task 1 模型契约：rawBytes（去 sparse 后字节数）由校验步骤回填
}

} // namespace

void finalizePlan(FlashPlan &plan)
{
    // stable_sort：同组同键（同 lun、同 startSector）保持解析顺序。表达式条目的 startSector 恒为 0
    // （模型契约），会排到本 LUN 的 Program 组最前 —— 无副作用：每个区间的写入彼此独立，
    // 表达式由设备侧求值（决策见 Task 1 报告；不是按地址排序，不改变写入结果）。
    std::stable_sort(plan.entries.begin(), plan.entries.end(),
                     [](const PlanEntry &a, const PlanEntry &b) {
                         const int ra = actionRank(a.action), rb = actionRank(b.action);
                         if (ra != rb) return ra < rb;
                         if (a.lun != b.lun) return a.lun < b.lun;
                         return a.startSector < b.startSector;
                     });

    quint64 total = 0;
    for (const PlanEntry &e : plan.entries) {
        if (e.action != PlanEntry::Action::Program)
            continue;                                    // Erase/Patch 不进进度分母
        total += e.rawBytes ? e.rawBytes : e.numSectors * e.sectorSize;
    }
    plan.totalBytes = total;
}

PlanCheck validatePlan(FlashPlan &plan, const QList<StorageInfo> &device)
{
    PlanCheck chk;

    QHash<quint32, StorageInfo> geo;                     // LUN → 设备几何（getstorageinfo 逐 LUN）
    for (const StorageInfo &s : device)
        geo.insert(s.lun, s);

    // ---- 第一遍：逐条目的文件与 sparse 换算（规则 3/4/5/6）+ 就地修正 ----
    // 顺序说明：**先**按文件头校正扇区数，**再**做几何校验（第二遍）—— 下发放多少扇区就按多少校验；
    // 否则 XML 少报的扇区数会让越界条目蒙混过关（见 validateSparseCorrectionFeedsBoundsCheck）。
    for (PlanEntry &e : plan.entries) {
        // 规则 3：逐条目 SECTOR_SIZE_IN_BYTES 覆盖全局是参照允许的（qdl 每个 op 用自己的值，
        // reference/qdl/src/firehose.c:1022），与设备 block_size 不一致只记 warning、以条目值为准。
        const auto it = geo.constFind(e.lun);
        if (it != geo.constEnd() && e.sectorSize != it->blockSize) {
            chk.warnings << QStringLiteral("条目 %1 的 sectorSize %2 与设备 blockSize %3 不一致（以条目值为准）")
                                .arg(entryName(e)).arg(e.sectorSize).arg(it->blockSize);
        }
        // 规则 6：有 sha256 即记 warning。此处**不读整个文件**——"边写边算"是默认路径，
        // "刷前完整校验"是 Task 8 的可选开关。
        if (!e.sha256.isEmpty()) {
            chk.warnings << QStringLiteral("条目 %1 携带 sha256（%2…）：默认边写边校验，"
                                           "刷前完整校验为可选开关（Task 8）")
                                .arg(entryName(e), e.sha256.left(12));
        }
        if (e.action == PlanEntry::Action::Program)
            checkProgramImage(e, chk);
        // Patch：imageFile=="DISK" 是"打设备磁盘偏移"哨兵，不是本地文件，不查（解析层已把
        // 非 DISK 的 patch 条目丢弃，flash_plan.cpp loadPatchTag）；Erase 没有镜像文件。
    }

    // ---- 第二遍：设备几何（规则 1/2/7）----
    int exprSkipped = 0;
    QSet<quint32> missingLun;
    QHash<quint32, QList<int>> programsByLun;             // 已见的 Program 条目下标（解析顺序）
    for (int i = 0; i < plan.entries.size(); ++i) {
        const PlanEntry &e = plan.entries[i];
        // 规则 7：表达式条目主机侧无法求值（参照也不解释，reference/qdl/src/firehose.c:874-879），
        // 跳过规则 1/2，最后汇总成一条 warning。
        if (!e.startSectorExpr.isEmpty()) {
            ++exprSkipped;
            continue;
        }
        const auto it = geo.constFind(e.lun);
        if (it == geo.constEnd()) {
            // 规则 1 前置：没有该 LUN 的几何信息就无法判越界 → error（逐 LUN 只报一次）
            if (!missingLun.contains(e.lun)) {
                missingLun.insert(e.lun);
                chk.errors << QStringLiteral("LUN %1 无设备几何信息（getstorageinfo 未返回）").arg(e.lun);
            }
            continue;
        }
        const quint64 end = e.startSector + e.numSectors;      // 半开区间 [start, end)
        if (end > it->totalBlocks) {
            chk.errors << QStringLiteral("条目 %1 越界：LUN %2 需要扇区 %3..%4，设备仅 %5")
                              .arg(entryName(e)).arg(e.lun).arg(e.startSector).arg(end).arg(it->totalBlocks);
        }
        // 规则 2：重叠只在 Program 条目之间判（同 LUN 区间相交）。整 LUN 擦本来就会覆盖后续 program
        // 的范围，patch 打的是任意磁盘偏移 —— 二者都不参与（spec §3.5）。
        if (e.action != PlanEntry::Action::Program)
            continue;
        for (int j : programsByLun.value(e.lun)) {
            const PlanEntry &prev = plan.entries[j];
            const quint64 prevEnd = prev.startSector + prev.numSectors;
            if (e.startSector < prevEnd && prev.startSector < end) {
                chk.errors << QStringLiteral("条目 %1 与 %2 重叠：LUN %3 区间 [%4,%5) 与 [%6,%7)")
                                  .arg(entryName(e), entryName(prev)).arg(e.lun)
                                  .arg(e.startSector).arg(end).arg(prev.startSector).arg(prevEnd);
            }
        }
        programsByLun[e.lun].append(i);
    }
    if (exprSkipped > 0) {
        chk.warnings << QStringLiteral("%1 个条目的 start_sector 为表达式，未参与设备几何校验（按参照原样下发）")
                            .arg(exprSkipped);
    }

    chk.ok = chk.errors.isEmpty();                        // 规则 8：任何 error → 拒刷
    return chk;
}

} // namespace edl
