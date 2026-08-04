#include "pac_image.h"
#include <QtEndian>

namespace imgpac {

namespace {

// ==================== PAC 容器结构（两源独立确认的参考实现） ====================
// 说明: .pac 是 Spreadtrum/Unisoc（SPD ResearchDownload）固件容器（社区工具
// divinebird/pacextractor C 版、HemanthJabalapuri/pacextractor Java/Python 版均针对
// SPD; MTK SP Flash Tool 使用 scatter 文件, 不用 pac）。本模块按社区参考实现实现,
// 字段级偏移逐一对注释与参考源码。
//
// ---- 新格式（BP_R1.0.0 / BP_R2.0.1, Java/Python 版, 全部小端 LE）----
// PAC 头 2124B（Java: SIZE_OF_PAC_HEADER; Python: '44s I I 512s 512s I I I I I I I 200s I I I 800s I H H'）:
//   0    version u16[22] UTF-16LE（须为 "BP_R1.0.0"/"BP_R2.0.1", 官方版本门禁）
//   44   dwHiSize u32    总大小高 32 位
//   48   dwLoSize u32    总大小低 32 位（dwSize = hi<<32|lo, 参考实现要求 == 文件长度）
//   52   productName u16[256] UTF-16LE
//   564  firmwareName u16[256] UTF-16LE
//   1076 partitionCount u32        分区/文件条目数
//   1080 partitionsListStart u32   分区表起始偏移（紧跟头, >= 2124）
//   1084 dwMode / 1088 dwFlashType / 1092 dwNandStrategy / 1096 dwIsNvBackup / 1100 dwNandPageType
//   1104 szPrdAlias u16[100] UTF-16LE
//   1304 dwOmaDmProductFlag / 1308 dwIsOmaDM / 1312 dwIsPreload
//   1316 dwReserved[200]（800B）
//   2116 dwMagic u32 0xfffafffa（参考实现仅在 CRC 校验时使用, 不做解析门禁）
//   2120 wCRC1 u16 / 2122 wCRC2 u16
// 分区项固定 2580B, 自 partitionsListStart 连续排布
// （Java: SIZE_OF_PARTITION_HEADER; Python: 'I 512s 512s 504s I I I I I I I I 5I 996s'）:
//   0    length u32（参考实现硬校验 == 2580）
//   4    partitionName u16[256] UTF-16LE（文件 ID: FDL1/FDL2/NV/...）
//   516  fileName u16[256] UTF-16LE（容器内文件名, 如 boot.img）
//   1028 szFileName u16[252]（保留）
//   1532 hiPartitionSize u32
//   1536 hiDataOffset u32
//   1540 loPartitionSize u32（partitionSize = hi<<32|lo, 64 位）
//   1544 nFileFlag u32（0=仅操作无文件, 如 "FLASH"; 1=需要文件）
//   1548 nCheckFlag u32
//   1552 loDataOffset u32（partitionAddrInPac = hiDataOffset<<32|loDataOffset, 64 位）
//   1556 dwCanOmitFlag u32 / 1560 dwAddrNum u32 / 1564 dwAddr[5] / 1584 dwReserved[249]
//
// ---- 旧格式（divinebird C 版, 全部小端 LE）----
// PAC 头 1220B 无魔数（C: sizeof(PacHeader), 唯一校验是文件长度 >= 1220）:
//   0    someField u16[24]（48B, 未知）
//   48   someInt u32
//   52   productName u16[256] UTF-16LE
//   564  firmwareName u16[256] UTF-16LE
//   1076 partitionCount u32
//   1080 partitionsListStart u32
//   1084 someIntFields1[5] / 1104 productName2 u16[50] / 1204 someIntFields2[6] / 1216 someIntFields3[2]
// 分区项变长（C: 固定部分 sizeof(PartitionHeader)=1568B + 尾随 dataArray[]）,
// 自 partitionsListStart 起按每项 length 链式步进（C: 先读 length, curPos += length）:
//   0    length u32（该项总长, 固定部分为 1568B, 可更大）
//   4    partitionName u16[256] UTF-16LE
//   516  fileName u16[512] UTF-16LE（C 版 getString 截断 bug 仅取 256 单元, 本实现取满 512）
//   1540 partitionSize u32
//   1544 someFileds1[2]（8B, 未知）
//   1552 partitionAddrInPac u32
//   1556 someFileds2[3]（12B, 未知）
// ================================================================================

constexpr int kNewHdrSize    = 2124;   // 新格式 PAC 头
constexpr int kNewEntrySize  = 2580;   // 新格式分区项固定长
constexpr int kOldHdrSize    = 1220;   // 旧格式 PAC 头
constexpr int kOldEntryMin   = 1568;   // 旧格式分区项固定部分长
constexpr quint32 kNewMagic  = 0xFFFAFFFA; // 新格式头魔数（仅 CRC 用途, 不做门禁）
constexpr int kMaxPartitions = 4096;   // 分区数上限（防呆, 真实固件远小于此）

inline quint32 rd32(const QByteArray &d, qsizetype off)
{
    return qFromLittleEndian<quint32>(d.constData() + off);
}

// 字符串字段: 社区工具按 UTF-16LE 单元数组读取（旧格式 getString 只取低字节,
// 对 ASCII 名等价）。遇 NUL 单元终止, 至多 units 个单元。
QString sprdString(const QByteArray &d, qsizetype off, int units)
{
    QString s;
    s.reserve(units);
    for (int i = 0; i < units; ++i) {
        const qsizetype p = off + qsizetype(i) * 2;
        if (p + 2 > d.size())
            break;
        const quint16 u = quint16(static_cast<uchar>(d[p]))
                        | quint16(quint16(static_cast<uchar>(d[p + 1])) << 8);
        if (u == 0)
            break;
        s.append(QChar(u));
    }
    return s;
}

// 分区数据范围校验: size==0 为操作型条目（无数据, 偏移无意义, 放行）;
// 其余要求 addr 及其后 size 字节全部落在文件内（防越界, 失败返回 false 不崩溃）。
bool validDataRange(qsizetype fileSize, quint64 addr, quint64 len)
{
    if (len == 0)
        return true;
    if (addr > static_cast<quint64>(fileSize))
        return false;
    return len <= static_cast<quint64>(fileSize) - addr;
}

// 新格式（BP_R1.0.0 / BP_R2.0.1）: 头 2124B + 分区项固定 2580B 连续
bool parseNewFormat(const QByteArray &pac, QList<PacPartition> &out, QString *error)
{
    const quint64 dwSize = (quint64(rd32(pac, 44)) << 32) | rd32(pac, 48);
    if (dwSize != static_cast<quint64>(pac.size())) {
        if (error) *error = QStringLiteral("PAC 头声明的文件长度与实际不符");
        return false;
    }
    const quint32 count = rd32(pac, 1076);
    if (count == 0 || count > kMaxPartitions) {
        if (error) *error = QStringLiteral("PAC 分区数异常");
        return false;
    }
    const quint32 start = rd32(pac, 1080);
    if (start < kNewHdrSize || static_cast<qsizetype>(start) > pac.size()) {
        if (error) *error = QStringLiteral("PAC 分区表偏移越界");
        return false;
    }
    const quint64 tableLen = static_cast<quint64>(count) * kNewEntrySize;
    if (tableLen > static_cast<quint64>(pac.size()) - start) {
        if (error) *error = QStringLiteral("PAC 分区表越界");
        return false;
    }
    for (quint32 i = 0; i < count; ++i) {
        const qsizetype off = start + qsizetype(i) * kNewEntrySize;
        if (rd32(pac, off) != kNewEntrySize) {
            if (error) *error = QStringLiteral("PAC 分区项长度异常");
            return false;
        }
        PacPartition p;
        p.name     = sprdString(pac, off + 4, 256);
        p.fileName = sprdString(pac, off + 516, 256);
        const quint64 size =
            (quint64(rd32(pac, off + 1532)) << 32) | rd32(pac, off + 1540);
        const quint64 addr =
            (quint64(rd32(pac, off + 1536)) << 32) | rd32(pac, off + 1552);
        if (!validDataRange(pac.size(), addr, size)) {
            if (error) *error = QStringLiteral("PAC 分区数据越界");
            return false;
        }
        p.offset = addr;
        out.append(p);
    }
    return true;
}

// 旧格式（divinebird C）: 头 1220B 无魔数, 分区项变长按 length 链式步进
bool parseLegacyFormat(const QByteArray &pac, QList<PacPartition> &out, QString *error)
{
    const quint32 count = rd32(pac, 1076);
    if (count == 0 || count > kMaxPartitions) {
        if (error) *error = QStringLiteral("PAC 分区数异常");
        return false;
    }
    const quint32 start = rd32(pac, 1080);
    if (start < kOldHdrSize || static_cast<qsizetype>(start) > pac.size()) {
        if (error) *error = QStringLiteral("PAC 分区表偏移越界");
        return false;
    }
    quint64 cur = start;
    for (quint32 i = 0; i < count; ++i) {
        if (cur + 4 > static_cast<quint64>(pac.size())) {
            if (error) *error = QStringLiteral("PAC 分区项越界");
            return false;
        }
        const quint32 len = rd32(pac, static_cast<qsizetype>(cur));
        if (len < kOldEntryMin || cur + len > static_cast<quint64>(pac.size())) {
            if (error) *error = QStringLiteral("PAC 分区项长度越界");
            return false;
        }
        const qsizetype off = static_cast<qsizetype>(cur);
        PacPartition p;
        p.name     = sprdString(pac, off + 4, 256);
        p.fileName = sprdString(pac, off + 516, 512);
        const quint64 size = rd32(pac, off + 1540);
        const quint64 addr = rd32(pac, off + 1552);
        if (!validDataRange(pac.size(), addr, size)) {
            if (error) *error = QStringLiteral("PAC 分区数据越界");
            return false;
        }
        p.offset = addr;
        out.append(p);
        cur += len;
    }
    return true;
}

} // namespace

bool isPac(const QByteArray &pac)
{
    QList<PacPartition> scratch;
    return parsePac(pac, scratch, nullptr);
}

bool parsePac(const QByteArray &pac, QList<PacPartition> &out, QString *error)
{
    out.clear();
    // 新格式: 头 2124B 且版本串（UTF-16LE）命中 —— 官方版本门禁
    if (pac.size() >= kNewHdrSize) {
        const QString ver = sprdString(pac, 0, 22);
        if (ver == QLatin1String("BP_R1.0.0") || ver == QLatin1String("BP_R2.0.1"))
            return parseNewFormat(pac, out, error);
    }
    // 旧格式: 无魔数/版本, 仅结构校验（分区数/表偏移/条目链/数据范围全通过才认可）
    if (pac.size() >= kOldHdrSize)
        return parseLegacyFormat(pac, out, error);
    if (error) *error = QStringLiteral("不是 PAC 固件（头长度不足）");
    return false;
}

} // namespace imgpac
