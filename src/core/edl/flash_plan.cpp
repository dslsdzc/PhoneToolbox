#include "flash_plan.h"

#include "image_engine/disk_image.h"
#include "image_engine/sparse_image.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QXmlStreamReader>
#include <QtEndian>
#include <algorithm>
#include <limits>

namespace edl {

namespace {

// ---- 属性读取：沿用 qdl "取不到即记错、该条目丢弃" 的语义（reference/qdl/src/util.c:74-125）----

// 原始字符串属性；属性缺失 → *ok=false。
// 参照 reference/qdl/src/util.c:90-105（attr_as_string：xmlGetProp 取不到 → 记错）
// **按属性集合实现**（唯一实现）：OPS 元数据的"容器兜底"形态必须先把父子属性合并成一份集合
// 再读（见 mergeAttrs / appendOpsProgramEntry），reader 版只是"取当前元素属性"的薄包装。
QString attrRawFrom(const QXmlStreamAttributes &attrs, const char *name, bool *ok)
{
    if (!attrs.hasAttribute(QLatin1String(name))) {
        if (ok) *ok = false;
        return QString();
    }
    if (ok) *ok = true;
    return attrs.value(QLatin1String(name)).toString();
}

QString attrRaw(QXmlStreamReader &reader, const char *name, bool *ok)
{
    return attrRawFrom(reader.attributes(), name, ok);
}

// 数值属性：base 0 解析（识别 0x 前缀），与 qdl strtoul(value, NULL, 0) 一致（reference/qdl/src/util.c:80）。
// 缺失、空串、不可解析一律 *ok=false（同 qdl attr_as_* 的 errors++ → 丢弃该条目）。
//
// **适用范围（与 readSectorAttr 的分工，勿混用）**：只用于 program/patch 的**纯数值**属性
// （SECTOR_SIZE_IN_BYTES / num_partition_sectors / physical_partition_number / byte_offset /
// size_in_bytes）。可能承载 firehose 表达式的扇区属性（start_sector、erase 的
// num_partition_sectors）一律走 readSectorAttr —— 那里有"缺失 / 十进制 / 表达式"三态语义。
quint64 attrU64From(const QXmlStreamAttributes &attrs, const char *name, bool *ok)
{
    bool present = false;
    const QString raw = attrRawFrom(attrs, name, &present);
    bool numOk = false;
    const quint64 value = raw.toULongLong(&numOk, 0);
    if (ok) *ok = present && numOk;
    return numOk ? value : 0;
}

quint64 attrU64(QXmlStreamReader &reader, const char *name, bool *ok)
{
    return attrU64From(reader.attributes(), name, ok);
}

// ---- 扇区类属性的统一三态（start_sector 与 erase 的 num_partition_sectors 共用）----
//
// 为什么单独一套：真机 XML 里这些属性可能是 firehose 表达式（如 "NUM_DISK_SECTORS-5."），
// 参照实现把 start_sector 读成**字符串**原样下发（reference/qdl/src/program.c:261、src/patch.c:46；
// src/firehose.c:874-879 注释明确"解析它会把写入地址搞错"），而 attrU64 的 base 0 数值语义
// （识别 0x）与"原样透传"冲突 —— 合并成一套会让主机擅自改写 0x 形态的下发值。
//
// 三态与各调用点的处置（**逐态行为写在这里，两个调用点都不得自行分叉**）：
//   Missing     属性**不存在**：
//                 · start_sector（program/patch，required=true）→ 记缺失、丢弃该条目；
//                 · start_sector（erase，required=false）→ 保持 0；
//                 · erase 的 num_partition_sectors → 0 = **整 LUN 擦**（唯一保留该语义的形态）。
//   Empty       属性存在但为空串：qdl attr_as_string 遇空串返回 NULL（reference/qdl/src/util.c:101-102），
//                等同缺失处理 —— 但对**破坏性**的 erase 计数一律 fail-closed 丢弃（见 loadEraseTag）：
//                空串被当作"0 扇区"会静默放大成整 LUN 擦。
//   Decimal     纯十进制：取数值。
//   NonDecimal  存在但按该属性的 base 解析不出来（firehose 表达式如 "NUM_DISK_SECTORS-5."、
//                 base=10 时的 "0x800"、或 "abc"）：
//                 · start_sector（base=10）→ **原样**存 startSectorExpr，**不是错误**、不丢条目；
//                 · erase 的 num_partition_sectors（base=0）→ 丢弃条目 + warning（无法当定点擦范围）。
enum class SectorAttrKind { Missing, Empty, Decimal, NonDecimal };

struct SectorAttr {
    SectorAttrKind kind = SectorAttrKind::Missing;
    quint64 value = 0;   // Decimal 时有效
    QString raw;         // NonDecimal 时的原样串；Empty 时为空串
};

// 读取扇区类属性。**唯一的分类实现**（present/empty/解析三件事只写一遍），逐调用点只差一个 base：
//   base=10（start_sector）：只认纯十进制 —— 0x 与表达式一样进 NonDecimal 原样透传，
//          避免主机改写下发形态（spec §3.3 契约：startSector 只填"纯十进制"值）；
//   base=0 （erase 的 num_partition_sectors）：识别 0x/0 前缀，与 qdl 的数值属性
//          attr_as_unsigned → strtoul(value, NULL, 0) 一致（reference/qdl/src/util.c:80）——
//          它是纯数值属性，0x800 这种十六进制形态必须当数值接受，而不是当"表达式"丢弃。
SectorAttr readSectorAttrFrom(const QXmlStreamAttributes &attrs, const char *name, int base = 10)
{
    SectorAttr out;
    bool present = false;
    out.raw = attrRawFrom(attrs, name, &present);
    if (!present)
        return out;                                     // Missing（raw 为空）
    if (out.raw.isEmpty()) {
        out.kind = SectorAttrKind::Empty;
        return out;
    }
    bool numOk = false;
    const quint64 value = out.raw.toULongLong(&numOk, base);
    if (numOk) {
        out.kind = SectorAttrKind::Decimal;
        out.value = value;
        out.raw.clear();
    } else {
        out.kind = SectorAttrKind::NonDecimal;
    }
    return out;
}

SectorAttr readSectorAttr(QXmlStreamReader &reader, const char *name, int base = 10)
{
    return readSectorAttrFrom(reader.attributes(), name, base);
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

// start_sector 取值（program / patch / erase 同款策略，三态判定统一走 readSectorAttr）：
//   纯十进制 → startSector=N；表达式/0x → **原样**存入 startSectorExpr 且 startSector=0（模型契约）。
//   表达式**不是错误**：不记 warning、不丢条目（Backup-GPT 头修补条目就在这一类，丢掉即漏修补；
//   reference/qdl/src/program.c:261、src/patch.c:46、src/firehose.c:874-879）。
//   缺失或空串 → 必需时记缺失（条目丢弃 + warning）。
// required=false：属性可缺省（erase 标签缺省 = 整 LUN 擦，见 loadEraseTag），缺省不记错。
void readStartSector(QXmlStreamReader &reader, PlanEntry &e, RequiredAttrs &attrs, bool required = true)
{
    const SectorAttr a = readSectorAttr(reader, "start_sector");
    switch (a.kind) {
    case SectorAttrKind::Decimal:
        e.startSector = a.value;
        return;
    case SectorAttrKind::NonDecimal:
        e.startSectorExpr = a.raw;   // 表达式原样保留；startSector 保持 0（模型契约）
        return;
    case SectorAttrKind::Missing:
    case SectorAttrKind::Empty:
        if (required)
            attrs.noteMissing("start_sector");
        return;
    }
}

// start_sector 的**三态处置**（唯一实现；逐态语义见上面 readStartSector 的注释）。
// 单独拆出来是给 OPS 元数据用：那条路径没有 RequiredAttrs（OPS 条目不是"缺一即丢"的模型，
// 缺几何由 GPT 回填），但仍必须与 rawprogram/patch 走**同一套**状态机 —— 否则两处会各自漂移。
void applyStartSector(const SectorAttr &a, PlanEntry &e)
{
    switch (a.kind) {
    case SectorAttrKind::Decimal:
        e.startSector = a.value;
        return;
    case SectorAttrKind::NonDecimal:
        e.startSectorExpr = a.raw;
        return;
    case SectorAttrKind::Missing:
    case SectorAttrKind::Empty:
        return;                      // 保持 0：待 GPT 回填（调用方负责告警）
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
    e.numSectors = attrs.u64("num_partition_sectors");  // 直接取 XML 值；sparse 展开由 normalizePlan 做
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
    // rawBytes：sparse 展开后的实际字节数，由 normalizePlan 读文件头回填（本函数只做 XML → 模型映射）
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
// 整盘擦。qdl 对这类输入是直接拒绝的：`load_erase_tag` 的 `if (!program->num_sectors)` →
// ux_err("erase tag with num_sectors=0 not allowed") → -EINVAL（reference/qdl/src/program.c:54-59）。
// 现在只有**属性确实缺失**才保留整 LUN 语义；其余（不可解析 / 表达式 / **显式 0**）一律丢弃该条目
// + 中文 warning —— 与本文件"宁可少条目并告警，也不猜"的原则一致；显式 0 之所以也丢，正因为它与
// "整 LUN"哨兵同值（同 program.c:54-59 的拒绝理由）。
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

    // 三态判定统一走 readSectorAttr（与 start_sector 同源），处置按注释里的策略逐态分发：
    // 只有**属性缺失**才是整 LUN 擦；Empty/NonDecimal/显式 0 一律丢弃（fail-closed）
    const SectorAttr count = readSectorAttr(reader, "num_partition_sectors", /*base=*/0);
    if (count.kind == SectorAttrKind::Decimal && count.value > 0) {
        e.numSectors = count.value;
    } else if (count.kind == SectorAttrKind::Missing) {
        e.numSectors = 0;   // 属性缺失 = 整 LUN 擦（唯一保留该语义的形态）
    } else {
        const bool isZero = count.kind == SectorAttrKind::Decimal;   // 走到这里 Decimal 只剩 0
        const QString shown = isZero ? QString::number(count.value) : count.raw;
        warnings << QStringLiteral("rawprogram erase 条目被跳过：num_partition_sectors=\"%1\" %2"
                                   "（lun=%3，start_sector=%4）—— 按 0 处理会把定点擦放大成整 LUN 擦")
                        .arg(shown,
                             isZero ? QStringLiteral("不是有效的定点擦范围（整 LUN 擦请省略该属性）")
                                    : QStringLiteral("不可解析"))
                        .arg(e.lun)
                        .arg(e.startSectorExpr.isEmpty() ? QString::number(e.startSector)
                                                         : e.startSectorExpr);
        return;
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
// partitionName）；erase 在 XML 里没有名字 → 带 LUN 与起始扇区，多条 erase 才分得开
// （整 LUN 擦写"整 LUN"）。保证非空，文案里永远能定位到条目。
QString entryName(const PlanEntry &e)
{
    if (!e.partitionName.isEmpty())
        return e.partitionName;
    if (!e.imageFile.isEmpty())
        return e.imageFile;
    if (e.action != PlanEntry::Action::Erase)
        return QStringLiteral("(未命名)");
    if (e.startSector == 0 && e.numSectors == 0 && e.startSectorExpr.isEmpty())
        return QStringLiteral("erase(lun=%1, 整 LUN)").arg(e.lun);
    return QStringLiteral("erase(lun=%1, start=%2)")
            .arg(e.lun)
            .arg(e.startSectorExpr.isEmpty() ? QString::number(e.startSector) : e.startSectorExpr);
}

// normalizePlan 的 Program 侧：镜像在不在、sparse 头声明的扇区数是多少。
// **就地修正条目**（numSectors / rawBytes / sparse 标记）；无法确定真实大小 → 记进 failures。
void normalizeProgramImage(PlanEntry &e, QStringList &warnings, QStringList &failures)
{
    if (e.imageFile.isEmpty()) {
        failures << QStringLiteral("条目 %1 未指定镜像文件（imageFile 为空）").arg(entryName(e));
        return;
    }
    QFile f(e.imageFile);
    if (!f.open(QIODevice::ReadOnly)) {
        failures << QStringLiteral("条目 %1 的镜像文件无法打开：%2（%3）")
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
        // 处理并告警；对不上则失败 —— 宁可拒刷，也不猜文件结构。
        // 无回绕写法：乘法两边都是 u64 量级，改为"先除后比"（fileSize % sectorSize == 0 且商相等）
        const quint64 fileSize = static_cast<quint64>(f.size());
        const bool sizeMatches = e.sectorSize != 0 && fileSize != 0
                                 && fileSize % e.sectorSize == 0
                                 && e.numSectors == fileSize / e.sectorSize;
        if (sizeMatches) {
            e.sparse = false;
            warnings << QStringLiteral("条目 %1 标记 sparse=\"true\" 但文件头不是 sparse（文件 %2 字节"
                                       " == 声明 %3 扇区 × %4 字节）—— 按非 sparse 处理"
                                       "（参照 reference/qdl/src/program.c:79-93）")
                            .arg(entryName(e)).arg(fileSize).arg(e.numSectors).arg(e.sectorSize);
        } else {
            failures << QStringLiteral("条目 %1 标记 sparse 但文件头不是 sparse 格式：%2（文件 %3 字节，"
                                       "声明 %4 扇区）")
                            .arg(entryName(e), e.imageFile).arg(fileSize).arg(e.numSectors);
        }
        return;
    }
    if (e.sectorSize == 0) {
        // qdl 对 sparse 且 SECTOR_SIZE_IN_BYTES=0 直接报错（reference/qdl/src/program.c:98-101）：
        // 没有扇区大小就无法把字节数换算成扇区数。
        failures << QStringLiteral("条目 %1 的 SECTOR_SIZE_IN_BYTES 为 0，无法换算 sparse 扇区数")
                        .arg(entryName(e));
        return;
    }
    // 无回绕比较：不比 numSectors × sectorSize（u64 可能溢出而被小值蒙混），改比**扇区数** ——
    // 不足整扇区的尾巴向上取整（firehose 只能按扇区下发，协议速查 §1）
    const quint64 fromHeader = rawBytes / e.sectorSize + (rawBytes % e.sectorSize != 0 ? 1 : 0);
    if (e.numSectors != fromHeader) {
        warnings << QStringLiteral("条目 %1 的 sparse 头声明 %2 字节（%3 扇区），与 XML 的 %4 扇区"
                                   "不符 —— 以文件头为准修正 numSectors")
                            .arg(entryName(e)).arg(rawBytes).arg(fromHeader).arg(e.numSectors);
        e.numSectors = fromHeader;
    }
    e.rawBytes = rawBytes;   // Task 1 模型契约：rawBytes（去 sparse 后字节数）由归一化步骤回填
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

    // 无回绕累加（预览路径也会调本函数，此时未必过校验）：乘法饱和 + 求和饱和，
    // 保证 totalBytes 不会回绕成垃圾进度分母（这类条目会被 validatePlan 拒掉，见规则 1）。
    const quint64 maxU64 = std::numeric_limits<quint64>::max();
    quint64 total = 0;
    for (const PlanEntry &e : plan.entries) {
        if (e.action != PlanEntry::Action::Program)
            continue;                                    // Erase/Patch 不进进度分母
        quint64 bytes = e.rawBytes;
        if (bytes == 0) {
            const quint64 ss = e.sectorSize ? e.sectorSize : 1;
            bytes = e.numSectors > maxU64 / ss ? maxU64 : e.numSectors * ss;
        }
        total = bytes > maxU64 - total ? maxU64 : total + bytes;
    }
    plan.totalBytes = total;
}

bool normalizePlan(FlashPlan &plan, QStringList &warnings, QString *error)
{
    QStringList failures;
    for (PlanEntry &e : plan.entries) {
        // 只有 Program 条目有本地镜像文件可查（Patch 的 imageFile=="DISK" 是"打设备磁盘偏移"哨兵，
        // 不是本地文件 —— 解析层已把非 DISK 的 patch 条目丢弃，见 loadPatchTag；Erase 没有镜像）。
        if (e.action == PlanEntry::Action::Program)
            normalizeProgramImage(e, warnings, failures);
    }
    if (!failures.isEmpty()) {
        if (error)
            *error = failures.join(QLatin1Char('\n'));   // 逐行列出（每条带条目名与路径），便于一次性修包
        return false;
    }
    return true;
}

PlanCheck validatePlan(const FlashPlan &plan, const QList<StorageInfo> &device)
{
    PlanCheck chk;

    QHash<quint32, StorageInfo> geo;                     // LUN → 设备几何（getstorageinfo 逐 LUN）
    for (const StorageInfo &s : device)
        geo.insert(s.lun, s);

    int exprSkipped = 0;
    QSet<quint32> missingLun;
    QHash<quint32, QList<int>> programsByLun;             // 已见的 Program 条目下标（解析顺序）
    for (int i = 0; i < plan.entries.size(); ++i) {
        const PlanEntry &e = plan.entries[i];
        const auto it = geo.constFind(e.lun);
        // 规则 3：逐条目 SECTOR_SIZE_IN_BYTES 覆盖全局是参照允许的（qdl 每个 op 用自己的值，
        // reference/qdl/src/firehose.c:1022），与设备 block_size 不一致只记 warning、以条目值为准。
        if (it != geo.constEnd() && e.sectorSize != it->blockSize) {
            chk.warnings << QStringLiteral("条目 %1 的 sectorSize %2 与设备 blockSize %3 不一致（以条目值为准）")
                                .arg(entryName(e)).arg(e.sectorSize).arg(it->blockSize);
        }
        // 规则 6：有 sha256 即记 warning。此处**不读整个文件**——"边写边算"是默认路径，
        // "刷前完整校验"是 Task 8 的可选开关。（纯告警、无修正、与几何无关，故不进 normalizePlan。）
        if (!e.sha256.isEmpty()) {
            chk.warnings << QStringLiteral("条目 %1 携带 sha256（%2…）：默认边写边校验，"
                                           "刷前完整校验为可选开关（Task 8）")
                                .arg(entryName(e), e.sha256.left(12));
        }
        // 规则 7：表达式条目主机侧无法求值（参照也不解释，reference/qdl/src/firehose.c:874-879），
        // 跳过规则 1/2，最后汇总成一条 warning。规则 3/6 在上面，表达式条目照样适用。
        if (!e.startSectorExpr.isEmpty()) {
            ++exprSkipped;
            continue;
        }
        if (it == geo.constEnd()) {
            // 规则 1 前置：没有该 LUN 的几何信息就无法判越界 → error（逐 LUN 只报一次）
            if (!missingLun.contains(e.lun)) {
                missingLun.insert(e.lun);
                chk.errors << QStringLiteral("LUN %1 无设备几何信息（getstorageinfo 未返回）").arg(e.lun);
            }
            continue;
        }
        // 规则 1：半开区间 [startSector, startSector + numSectors) 必须落在设备容量内。
        // **无回绕写法**（参照式）：先判 startSector 本身，再用减法避免 startSector + numSectors 溢出
        // （损坏 XML 里 numSectors 取极大值时，回绕会让越界条目蒙混过关）。
        const bool oob = e.startSector > it->totalBlocks
                         || e.numSectors > it->totalBlocks - e.startSector;
        if (oob) {
            // 文案里的结束扇区只用于显示：溢出时给出 "start+num（溢出）" 而不是回绕后的垃圾值
            const bool endOverflows = e.numSectors > std::numeric_limits<quint64>::max() - e.startSector;
            const QString need = endOverflows
                    ? QStringLiteral("%1+%2（溢出 u64）").arg(e.startSector).arg(e.numSectors)
                    : QStringLiteral("%1..%2").arg(e.startSector).arg(e.startSector + e.numSectors);
            chk.errors << QStringLiteral("条目 %1 越界：LUN %2 需要扇区 %3，设备仅 %4")
                              .arg(entryName(e)).arg(e.lun).arg(need).arg(it->totalBlocks);
        }
        // 规则 2：重叠只在 Program 条目之间判（同 LUN 区间相交）。整 LUN 擦本来就会覆盖后续 program
        // 的范围，patch 打的是任意磁盘偏移 —— 二者都不参与（spec §3.5）。
        if (e.action != PlanEntry::Action::Program)
            continue;
        for (int j : programsByLun.value(e.lun)) {
            const PlanEntry &prev = plan.entries[j];
            // **无回绕判交**：较晚的起点 < 较早起点的区间长度（不计算 end，避免 u64 回绕）
            const quint64 s1 = e.startSector, n1 = e.numSectors;
            const quint64 s2 = prev.startSector, n2 = prev.numSectors;
            const bool overlap = s1 <= s2 ? (s2 - s1 < n1) : (s1 - s2 < n2);
            if (overlap) {
                // 区间终点仅用于显示：饱和加法，溢出时显示 u64 上限（越界条目另有 error）
                const auto satEnd = [](quint64 s, quint64 n) {
                    return n > std::numeric_limits<quint64>::max() - s
                               ? std::numeric_limits<quint64>::max() : s + n;
                };
                chk.errors << QStringLiteral("条目 %1 与 %2 重叠：LUN %3 区间 [%4,%5) 与 [%6,%7)")
                                  .arg(entryName(e), entryName(prev)).arg(e.lun)
                                  .arg(s1).arg(satEnd(s1, n1)).arg(s2).arg(satEnd(s2, n2));
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

// ================= Task 3：来源探测 + OPS 元数据 + GPT 回填对账 =================

namespace {

// ---- 序号工具 ----

// 取字符串末尾数字（"rawprogram10" → 10、"Program0" → 0、"UFS_PROVISION" → 无）。
// 末尾无数字或超出 u32 → 0 + warning（不静默）。文件名序号与组标签序号是**同一条约定**
// （协议速查 §4：rawprogramN.xml 的序号 == 条目 physical_partition_number；
// reference/qdl/tests/data/rawprogram1.xml:5-8），故共用本实现。
quint32 trailingNumber(const QString &text, const QString &label, QStringList &warnings)
{
    int i = text.size();
    while (i > 0 && text.at(i - 1).isDigit())
        --i;
    if (i == text.size()) {
        warnings << QStringLiteral("%1 末尾无 LUN 序号，按 lun=0 解析").arg(label);
        return 0;
    }
    bool ok = false;
    const uint n = text.mid(i).toUInt(&ok);
    if (!ok) {
        warnings << QStringLiteral("%1 末尾的 LUN 序号超出范围，按 lun=0 解析").arg(label);
        return 0;
    }
    return n;
}

// 目录内 `<prefix>*.xml` → (lun, 文件名) 列表。按 (lun, 文件名) **数值**排序：QDir::Name 是字典序
// （"rawprogram10.xml" 会排在 "rawprogram2.xml" 前面），直接用它会让多 LUN 包的计划顺序不稳定。
QList<QPair<quint32, QString>> numberedXmlFiles(const QString &dir, const QString &prefix,
                                                QStringList &warnings)
{
    QList<QPair<quint32, QString>> out;
    const QStringList names = QDir(dir).entryList(QStringList{prefix + QStringLiteral("*.xml")},
                                                 QDir::Files, QDir::Name);
    for (const QString &n : names)
        out.append({trailingNumber(QFileInfo(n).completeBaseName(),
                                   QStringLiteral("文件 %1").arg(n), warnings), n});
    std::sort(out.begin(), out.end(), [](const QPair<quint32, QString> &a, const QPair<quint32, QString> &b) {
        return a.first != b.first ? a.first < b.first : a.second < b.second;
    });
    return out;
}

// ---- storageType 探测（spec §3.4）----

// prog_ufs_firehose_* → "ufs"；prog_emmc_firehose_* → "emmc"；都无 → 默认 "ufs" + warning。
// 之所以必须告警：真包常见不带 ufs/emmc 标识的 `prog_firehose_ddr.elf`（reference/qdl/docs/nbd.md:43、
// src/qdl.c:398 的示例就是它），默认值只是猜测 —— 会话层按 spec §4 在 configure 报
// "Not support configure MemoryName" 时还有一次换类型重试，这里不猜死。
void detectStorageType(const QString &dir, FlashPlan &plan)
{
    const QStringList names = QDir(dir).entryList(QDir::Files);
    const auto anyContains = [&names](const QString &needle) {
        for (const QString &n : names)
            if (n.contains(needle, Qt::CaseInsensitive))
                return true;
        return false;
    };
    if (anyContains(QStringLiteral("ufs_firehose"))) {
        plan.storageType = QStringLiteral("ufs");
        return;
    }
    if (anyContains(QStringLiteral("emmc_firehose"))) {
        plan.storageType = QStringLiteral("emmc");
        return;
    }
    plan.storageType = QStringLiteral("ufs");
    plan.warnings << QStringLiteral("目录内未找到 prog_ufs_firehose_* / prog_emmc_firehose_*，"
                                    "存储类型按默认 \"ufs\" 处理（configure 报 Not support configure "
                                    "MemoryName 时会换类型重试一次）");
}

// ---- GPT：文件 → 分区名/LBA 表（复用既有解析器 + 布局适配）----

// GPT 分区项：只取对账需要的三个字段。用 numSectors 而不是 lastLba —— PlanEntry 按扇区数建模，
// 少一次 ±1 换算就少一处 off-by-one（last-first+1 已由解析层做，disk_image.cpp:80）。
struct GptPartition { QString name; quint64 firstLba = 0; quint64 numSectors = 0; };

// 单个 LUN 的 GPT 加载结果（含失败文本，供"未对账"告警引用）
struct GptTable {
    bool loaded = false;             // 本 LUN 是否已尝试加载（失败也只告警一次）
    bool ok = false;
    QString fileName;                // gpt_main{N}.bin
    QList<GptPartition> parts;
};

// GPT 文件 → 分区名/LBA 表。**复用既有解析器** imgdisk::parseGpt（src/image_engine/disk_image.cpp:46-88），
// 不重写解析逻辑（硬要求 2）：它的接口本就给出"分区名 → (startSector, numSectors)"（disk_image.h:8-9），
// 故本任务**未改** disk_image.{h,cpp}，只加了读取 + 布局适配这一层薄胶水。
//
// **布局适配（真包必需）**：parseGpt 硬编码 512 字节 LBA —— 在文件偏移 512 处找头、按 `表LBA × 512`
// 定位表项数组（disk_image.cpp:8,50-63）。而真实 EDL 包的 gpt_main{N}.bin 是 **4096 字节 LBA**：
//   * edl/edlclient/Library/TestFiles/gpt_sm8180x.bin 实测 24576 字节、`EFI PART` 在 0x1000、
//     part_entry_lba=2（本仓内的真实样本）；
//   * reference/qdl/tests/data/rawprogram1.xml:12 的 `gpt_main1.bin num_partition_sectors="6"`
//     ⇒ 6 × 4096 = 24576 字节，与上者吻合；
//   * bkerler 读盘时 512/4096 都试（edl/edlclient/Library/gpt.py:526-531）。
// 只支持 512 会让 OPS 路径在真机（UFS）上永远拿不到 GPT 几何 ⇒ 回填/对账整体失效。
// 适配只搬运字节布局、不碰解析语义：把 92 字节头块搬到偏移 512，并把头里的"表项数组 LBA"字段按
// (lbaSize/512) 放大 —— 表项数组的**字节偏移不变**，parseGpt 随后读到的 LBA 与分区名与文件完全一致。
// 头既不在 512 也不在 4096 → 直接失败（不猜布局，交给调用方记"未对账"告警）。
bool readGptPartitions(const QString &gptPath, QList<GptPartition> &out, QString *error)
{
    QFile f(gptPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开（%1）").arg(f.errorString());
        return false;
    }
    QByteArray bytes = f.readAll();
    f.close();

    constexpr int kHeaderBytes = 92;         // UEFI 头最小 92 字节（parseGpt 只读到头 +88）
    if (!imgdisk::isGpt(bytes.mid(512, 8))) {
        if (!imgdisk::isGpt(bytes.mid(4096, 8))) {
            if (error)
                *error = QStringLiteral("\"EFI PART\" 既不在 0x200 也不在 0x1000"
                                        "（本解析器只支持 512/4096 字节 LBA 两种布局）");
            return false;
        }
        constexpr quint64 kLbaSize = 4096;
        if (bytes.size() < qint64(kLbaSize + kHeaderBytes)) {
            if (error)
                *error = QStringLiteral("文件过短（%1 字节），容不下 0x1000 处的 GPT 头").arg(bytes.size());
            return false;
        }
        QByteArray head = bytes.mid(kLbaSize, kHeaderBytes);
        const quint64 tableLba = qFromLittleEndian<quint64>(head.constData() + 72);
        // 字节偏移 = tableLba × 4096 = (tableLba × 8) × 512 —— 换算成 512 单位后交回 parseGpt
        if (tableLba > std::numeric_limits<quint64>::max() / (kLbaSize / 512)) {
            if (error) *error = QStringLiteral("分区表 LBA 字段溢出（%1）").arg(tableLba);
            return false;
        }
        qToLittleEndian<quint64>(tableLba * (kLbaSize / 512), head.data() + 72);
        bytes.replace(512, kHeaderBytes, head);   // 头搬到 512：parseGpt 只在这里找头
    }

    imgdisk::DiskInfo info;
    if (!imgdisk::parseGpt(bytes, info)) {
        if (error) *error = QStringLiteral("GPT 解析失败（表项数组越界或表项大小 <128）");
        return false;
    }
    for (const imgdisk::Partition &p : info.partitions)
        out.append({p.name, p.startSector, p.numSectors});
    return true;
}

// 该 LUN 的 GPT 表（每个 LUN 只读一次；失败也只告警一次 —— 一个 LUN 上几十条分区，
// 逐条报"未对账"会把预览刷爆）。
const GptTable &gptTableForLun(quint32 lun, const QString &packageDir, QHash<quint32, GptTable> &cache,
                               QStringList &warnings)
{
    GptTable &t = cache[lun];
    if (t.loaded)
        return t;
    t.loaded = true;
    // 包内 GPT 文件名 = gpt_main{N}.bin（N = lun）：协议速查 §5 "社区一手清单确认 .ops 内含
    // gpt_main0-5.bin / gpt_backup0-5.bin"。同名的 gpt_backup{N}.bin 是**备份 GPT 头**，
    // 几何与主表一致且 LBA 语义不同于分区表 → 不用它做对账。
    t.fileName = QStringLiteral("gpt_main%1.bin").arg(lun);
    const QString path = QDir(packageDir).filePath(t.fileName);
    if (!QFile::exists(path)) {
        warnings << QStringLiteral("LUN %1 的 %2 不存在 —— 该 LUN 的几何仅来自元数据、未做 GPT 对账")
                        .arg(lun).arg(t.fileName);
        return t;
    }
    QString gptErr;
    if (!readGptPartitions(path, t.parts, &gptErr)) {
        warnings << QStringLiteral("LUN %1 的 %2 不可用：%3 —— 该 LUN 的几何仅来自元数据、未做 GPT 对账")
                        .arg(lun).arg(t.fileName, gptErr);
        return t;
    }
    t.ok = true;
    return t;
}

// 分区名匹配候选：label / 条目标识（= label，或文件名主干）+ 文件名主干 —— 两侧大小写不敏感比较
// （GPT 名与元数据名都是 ASCII 标识符，大小写不是协议语义；用不敏感比较避免无谓的"查不到"）。
// 同名多条 → 取第一条（GPT 里分区名唯一是 UEFI 的期望，但真包不保证）。
const GptPartition *matchGptPartition(const PlanEntry &e, const GptTable &gpt)
{
    QStringList candidates;
    if (!e.partitionName.isEmpty())
        candidates << e.partitionName;
    const QString stem = QFileInfo(e.imageFile).completeBaseName();
    if (!stem.isEmpty() && !candidates.contains(stem, Qt::CaseInsensitive))
        candidates << stem;
    for (const QString &c : candidates)
        for (const GptPartition &p : gpt.parts)
            if (p.name.compare(c, Qt::CaseInsensitive) == 0)
                return &p;
    return nullptr;
}

// 逐条目对账（spec §3.4 规则 2；协议速查 §5 的"唯一有开源实证的几何来源"）。四态：
//   * 该 LUN 的 GPT 不可用 → 静默返回（LUN 级告警已在 gptTableForLun 出过，见那里）；
//   * GPT 里查不到该分区名 → 保留元数据值 + warning；
//   * 元数据**没有**提供该几何值 → 用 GPT 回填 + warning（"值来自 GPT"必须可见）；
//   * 两边都有但不一致 → warning + **以 GPT 为准**（brief 明文）；
//   * 一致 → 静默（不制造告警噪音）。
// 例外：startSectorExpr 非空（firehose 表达式，如 "NUM_DISK_SECTORS-5."）的条目**不参与对账** ——
// 表达式由设备侧求值、主机不得改写（reference/qdl/src/firehose.c:874-879），拿包内 GPT 的具体数字
// 替换会把"盘尾"语义写死成这个包的大小；只记一条"未对账"warning。
void reconcileWithGpt(PlanEntry &e, bool haveStart, bool haveCount, const GptTable &gpt,
                      QStringList &warnings)
{
    if (!gpt.ok)
        return;
    if (!e.startSectorExpr.isEmpty()) {
        warnings << QStringLiteral("条目 %1（lun=%2）的 start_sector 是表达式 \"%3\"，按参照原样下发、"
                                   "不参与 GPT 对账（reference/qdl/src/firehose.c:874-879）")
                        .arg(entryName(e), QString::number(e.lun), e.startSectorExpr);
        return;
    }
    const GptPartition *hit = matchGptPartition(e, gpt);
    if (!hit) {
        warnings << QStringLiteral("条目 %1（lun=%2）在 %3 的分区表里查不到 —— 保留元数据几何"
                                   "（start=%4、num=%5），未经 GPT 核对")
                        .arg(entryName(e), QString::number(e.lun), gpt.fileName)
                        .arg(e.startSector).arg(e.numSectors);
        return;
    }
    if (haveStart && haveCount && hit->firstLba == e.startSector && hit->numSectors == e.numSectors)
        return;                                          // 一致 → 静默
    if (haveStart && haveCount) {
        warnings << QStringLiteral("条目 %1（lun=%2）几何与 %3 不一致（元数据 start=%4、num=%5；"
                                   "GPT start=%6、num=%7）—— 以 GPT 为准修正")
                        .arg(entryName(e), QString::number(e.lun), gpt.fileName)
                        .arg(e.startSector).arg(e.numSectors)
                        .arg(hit->firstLba).arg(hit->numSectors);
    } else {
        warnings << QStringLiteral("条目 %1（lun=%2）元数据未提供 %3，几何由 %4 回填："
                                   "start=%5、num=%6")
                        .arg(entryName(e), QString::number(e.lun),
                             haveStart ? QStringLiteral("num_partition_sectors")
                                       : (haveCount ? QStringLiteral("start_sector")
                                                    : QStringLiteral("start_sector/num_partition_sectors")),
                             gpt.fileName)
                        .arg(hit->firstLba).arg(hit->numSectors);
    }
    e.startSector = hit->firstLba;
    e.numSectors = hit->numSectors;
}

// ---- OPS 条目 ----

// 属性合并：子元素优先、容器兜底。真实 settings.xml 有两种形态（brief 的扁平形态 + FirmwareKit 样本的
// 容器形态 `<program label="persist" num_partition_sectors="…"><Image filename="persist.img" …/></program>`：
// reference/FirmwareKit.Oppo/FirmwareKit.Oppo.Tests/Parsers/OpsParserTests.cs:116-122）——文件名在子元素、
// 几何在容器的情况很常见，合并成一份集合后按同一套字段规则读一次即可，不必为两种形态写两套取值。
QXmlStreamAttributes mergeAttrs(const QXmlStreamAttributes &container, const QXmlStreamAttributes &child)
{
    QXmlStreamAttributes merged = container;
    for (const QXmlStreamAttribute &a : child)
        if (!merged.hasAttribute(a.name()))
            merged.append(a);
    return merged;
}

// 一个 OPS `<program>`/`<Image>` 条目 → PlanEntry{Program}。
//
// 属性语义与出处（逐字段）：
//   filename → imageFile（包内相对名，以 packageDir 绝对化）
//   label    → partitionName（缺失时用文件名主干兜底：条目标识必须非空，见 entryName()）
//   sparse   → sparse（"true"（不区分大小写）或 "1"；同 Phase A 清单口径 oppo_ops.cpp:157-159）
//   Sha256   → sha256（字段名见 opscrypto.py:614 的 `item.attrib["Sha256"]`；元数据有的原样带走）
//   SECTOR_SIZE_IN_BYTES → sectorSize（缺失保持 PlanEntry 默认值）
//   physical_partition_number → lun（**属性优先**：参照对每条带 filename 的 <program> 硬读该属性，
//       edl/edlclient/Library/firehose_client.py:950；缺失才用组序号 Program{N} → N）
//   start_sector / num_partition_sectors → startSector / numSectors（"强推断存在"，依据同上的硬读
//       firehose_client.py:950,952 + chayleaf 原样导出后 `edl qfil` 刷机成功；缺失/不可解析 → 0，
//       并按 haveStart/haveCount = false 交给 GPT 回填）
// **包内偏移字段一律不读**：FileOffsetInSrc / SizeInByteInSrc / SizeInSectorInSrc 是"在包里的位置/
//   长度"—— 打包器按包内累计位置重算它们（reference/oppo_decrypt/opscrypto.py:503-513），C# 模型
//   注释也写 "Start offset in the archive (bytes)"（reference/FirmwareKit.Oppo/.../OppEntry.cs:16-17）；
//   它们与设备扇区无关，当成 startSector/numSectors 会直接把镜像写到错误地址（协议速查 §5）。
void appendOpsProgramEntry(const QXmlStreamAttributes &attrs, quint32 groupLun, const QString &packageDir,
                           QHash<quint32, GptTable> &gptCache, QList<PlanEntry> &out,
                           QStringList &warnings)
{
    const QString filename = attrs.value(QStringLiteral("filename")).toString();
    if (filename.isEmpty())
        return;      // 空名跳过（同参照 opscrypto.py:614-615 的 `wfilename == "" → continue`）

    PlanEntry e;
    e.action = PlanEntry::Action::Program;
    e.imageFile = QDir(packageDir).filePath(filename);
    e.partitionName = attrs.value(QStringLiteral("label")).toString();
    if (e.partitionName.isEmpty())
        e.partitionName = QFileInfo(filename).completeBaseName();

    const QString sparse = attrs.value(QStringLiteral("sparse")).toString();
    e.sparse = sparse.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0
               || sparse == QLatin1String("1");
    e.sha256 = attrs.value(QStringLiteral("Sha256")).toString();

    bool ok = false;
    const quint64 sectorSize = attrU64From(attrs, "SECTOR_SIZE_IN_BYTES", &ok);
    if (ok && sectorSize > 0 && sectorSize <= std::numeric_limits<quint32>::max())
        e.sectorSize = quint32(sectorSize);

    bool lunOk = false;
    const quint64 attrLun = attrU64From(attrs, "physical_partition_number", &lunOk);
    e.lun = (lunOk && attrLun <= std::numeric_limits<quint32>::max()) ? quint32(attrLun) : groupLun;

    // 几何三态：与 rawprogram/patch 共用同一套状态机（readSectorAttrFrom + applyStartSector）
    const SectorAttr startAttr = readSectorAttrFrom(attrs, "start_sector");
    applyStartSector(startAttr, e);
    const SectorAttr countAttr = readSectorAttrFrom(attrs, "num_partition_sectors", /*base=*/0);
    if (countAttr.kind == SectorAttrKind::Decimal) {
        e.numSectors = countAttr.value;
    } else if (countAttr.kind != SectorAttrKind::Missing) {
        // 存在但空/不可解析：不静默当 0 —— 0 在本模型里会被当成"元数据没给"而由 GPT 回填，
        // 脏元数据就这样悄悄被掩盖过去了（同 rawprogram 侧"宁可少条目并告警"的原则）。
        warnings << QStringLiteral("OPS 条目 %1（lun=%2）的 num_partition_sectors=\"%3\" 不可解析"
                                   "—— 该值改由 %4 回填")
                        .arg(e.partitionName, QString::number(e.lun),
                             countAttr.kind == SectorAttrKind::Empty ? QStringLiteral("(空)") : countAttr.raw,
                             QStringLiteral("gpt_main%1.bin").arg(e.lun));
    }

    // GPT 回填 + 对账（brief 明文：不一致 → 以 GPT 为准）
    const GptTable &gpt = gptTableForLun(e.lun, packageDir, gptCache, warnings);
    reconcileWithGpt(e, startAttr.kind == SectorAttrKind::Decimal,
                     countAttr.kind == SectorAttrKind::Decimal, gpt, warnings);

    warnLunMismatch(QStringLiteral("OPS program"), e, groupLun, warnings);
    out << e;
}

} // namespace

bool parseOpsSettingsXml(const QString &settingsXmlPath, const QString &packageDir,
                         QList<PlanEntry> &out, QStringList &warnings, QString *error)
{
    QFile file;
    if (!openXml(settingsXmlPath, QStringLiteral("OPS settings.xml"), file, error))
        return false;

    QXmlStreamReader reader(&file);
    // 根元素名不做限制：样本/参照是 <Firehose>（FirmwareKit OpsParserTests.cs:113），Phase A 的清单
    // 解析同样只要求"有根元素"（src/image_engine/oppo_ops.cpp:176-177）—— 收窄会拒真包。
    if (!reader.readNextStartElement()) {
        if (error)
            *error = QStringLiteral("OPS settings.xml 不是有效的 XML（缺少根元素）：%1").arg(settingsXmlPath);
        return false;
    }

    const int firstIndex = out.size();      // 只统计本次新增（parse 约定：out 只追加）
    QHash<quint32, GptTable> gptCache;      // lun → GPT 表（每 LUN 只读一次）
    bool havePatchGroup = false;

    while (reader.readNextStartElement()) {              // 顶层：组
        const QString tag = reader.name().toString();
        if (tag.contains(QLatin1String("Patch"))) {
            havePatchGroup = true;
            const quint32 lun = trailingNumber(tag, QStringLiteral("组标签 %1").arg(tag), warnings);
            while (reader.readNextStartElement()) {      // 组内：<patch>（与 patch XML 逐属性同构）
                if (reader.name() == QLatin1String("patch"))
                    loadPatchTag(reader, lun, out, warnings);   // 复用：8 必需属性 + 非 DISK 跳过 + lun 告警
                reader.skipCurrentElement();
            }
            continue;
        }
        if (tag.contains(QLatin1String("Program")) || tag == QLatin1String("UFS_PROVISION")) {
            const quint32 lun = tag == QLatin1String("UFS_PROVISION")
                                    ? 0                       // brief/协议速查：UFS_PROVISION 组恒 lun=0
                                    : trailingNumber(tag, QStringLiteral("组标签 %1").arg(tag), warnings);
            while (reader.readNextStartElement()) {      // 组内：条目或容器
                if (reader.attributes().hasAttribute(QStringLiteral("filename"))) {
                    appendOpsProgramEntry(reader.attributes(), lun, packageDir, gptCache, out, warnings);
                    reader.skipCurrentElement();
                    continue;
                }
                // 容器形态（<program label="…">）：孙元素带 filename 者逐条产出，属性子优先、容器兜底
                const QXmlStreamAttributes container = reader.attributes();
                while (reader.readNextStartElement()) {
                    if (reader.attributes().hasAttribute(QStringLiteral("filename")))
                        appendOpsProgramEntry(mergeAttrs(container, reader.attributes()), lun, packageDir,
                                              gptCache, out, warnings);
                    reader.skipCurrentElement();
                }
            }
            continue;
        }
        reader.skipCurrentElement();                     // SAHARA/BasicInfo 等非分区组：忽略
    }
    if (reader.hasError()) {
        if (error)
            *error = QStringLiteral("OPS settings.xml 解析失败：%1（%2）")
                         .arg(settingsXmlPath, reader.errorString());
        return false;
    }

    int patchEntries = 0;
    for (int i = firstIndex; i < out.size(); ++i)
        if (out[i].action == PlanEntry::Action::Patch)
            ++patchEntries;
    // spec §3.4：patch 条目缺失必须明说（GPT 头定点修补 = 刷后能否引导）。
    // 分两种：整个 patch 分组不存在 vs 分组在但一条 DISK 条目都没产出（非 DISK 的会被 loadPatchTag 跳过）
    if (!havePatchGroup) {
        warnings << QStringLiteral("元数据中未找到 patch 分组：GPT 头定点修补缺失，刷后可能无法引导");
    } else if (patchEntries == 0) {
        warnings << QStringLiteral("patch 分组存在但未产出任何 DISK 条目：GPT 头定点修补缺失，刷后可能无法引导");
    }
    return true;
}

bool buildPlanFromDir(const QString &dir, FlashPlan &plan, QString *error)
{
    // 计划对象是本函数的**输出**：入口清空，重复调用不累积（parse*Xml 的契约是"只追加"，
    // 若这里不清，UI 换目录重建预览时会把上一次的条目/告警混进来）。
    plan.source.clear();
    plan.storageType.clear();
    plan.entries.clear();
    plan.warnings.clear();
    plan.totalBytes = 0;

    QDir d(dir);
    if (!d.exists()) {
        if (error)
            *error = QStringLiteral("目录不存在：%1").arg(dir);
        return false;
    }
    detectStorageType(dir, plan);

    // 目录内 XML 清单：诊断用（③ 失败时列给用户看），只列 .xml 文件名
    const QStringList xmlNames = d.entryList(QStringList{QStringLiteral("*.xml")}, QDir::Files, QDir::Name);
    const QString xmlList = xmlNames.isEmpty() ? QStringLiteral("（无）")
                                              : xmlNames.join(QStringLiteral("、"));

    // ① rawprogram*.xml（文件名末尾数字 = lun）+ patch*.xml
    const QList<QPair<quint32, QString>> rawprograms =
            numberedXmlFiles(dir, QStringLiteral("rawprogram"), plan.warnings);
    const QList<QPair<quint32, QString>> patches =
            numberedXmlFiles(dir, QStringLiteral("patch"), plan.warnings);

    bool built = false;
    if (!rawprograms.isEmpty()) {
        QStringList used;
        for (const QPair<quint32, QString> &rp : rawprograms) {
            if (!parseRawprogramXml(d.filePath(rp.second), rp.first, plan.entries, plan.warnings, error))
                return false;
            used << rp.second;
        }
        for (const QPair<quint32, QString> &pt : patches) {
            if (!parsePatchXml(d.filePath(pt.second), pt.first, plan.entries, plan.warnings, error))
                return false;
            used << pt.second;
        }
        if (!plan.entries.isEmpty()) {
            plan.source = QStringLiteral("rawprogram XML：%1").arg(used.join(QStringLiteral("、")));
            built = true;
        } else {
            // ①文件在但一条都没解析出来（空文件/标签不符）：不静默 —— 回退②并说明
            plan.warnings << QStringLiteral("rawprogram*.xml 存在但未产出任何条目，"
                                            "改用 OPS settings.xml 作为计划来源");
        }
    }

    // ② OPS settings.xml（.ops/.ofp 解包产物；几何走包内 gpt_main{N}.bin 回填对账）
    if (!built && QFile::exists(d.filePath(QStringLiteral("settings.xml")))) {
        if (!parseOpsSettingsXml(d.filePath(QStringLiteral("settings.xml")), d.absolutePath(),
                                 plan.entries, plan.warnings, error))
            return false;
        plan.source = QStringLiteral("OPS settings.xml 元数据 + gpt_main{N}.bin GPT 回填对账");
        built = true;
    }

    // ③ 两个来源都没有
    if (!built) {
        if (error) {
            *error = rawprograms.isEmpty()
                ? QStringLiteral("未找到 rawprogram*.xml 或 settings.xml，无法构建刷写计划。目录内 XML：%1")
                      .arg(xmlList)
                : QStringLiteral("rawprogram*.xml 存在但未产出任何条目，且目录内无 settings.xml 可回退。"
                                 "目录内 XML：%1").arg(xmlList);
        }
        return false;
    }
    // 选了来源却一条都没有 → 拒（fail-closed：空计划绝不能进预览/刷写）
    if (plan.entries.isEmpty()) {
        if (error)
            *error = QStringLiteral("%1 未产出任何刷写条目（XML 标签/属性与本项目解析器不符？）。"
                                    "目录内 XML：%2").arg(plan.source, xmlList);
        return false;
    }

    // 调用链：parse* → normalizePlan → finalizePlan（validatePlan 由会话层在 getstorageinfo 后做）
    QString normErr;
    if (!normalizePlan(plan, plan.warnings, &normErr)) {
        if (error) *error = normErr;                     // 逐行合并的失败清单，直接透传
        return false;
    }
    finalizePlan(plan);
    return true;
}

} // namespace edl
