#include "super_image.h"
#include <QtEndian>

namespace imgsuper {

namespace {
constexpr int kReserved = 4096;          // geometry 主副本区（首 4KB 块，AOSP LP_PARTITION_RESERVED_BYTES）
constexpr int kGeometrySize = 4096;      // 几何块大小（lpmake 固定 4KB）
constexpr quint32 kGeomMagic = 0x616c4467;
constexpr quint32 kMetaMagic = 0x414C5030;
constexpr quint64 kSector = 512;         // 逻辑扇区 512B
constexpr quint32 kPartitionEntrySize = 52;  // LpMetadataPartition 定长（name36+attrs4+first_ext4+num4+group4）
constexpr quint32 kExtentEntrySize = 24;     // LpMetadataExtent 定长（num_sectors8+target_type4+target_data8+source4）
// numSectors * 512 不产生 quint64 回绕的最大扇区数（2^55 - 1）
constexpr quint64 kMaxSectors = (Q_UINT64_C(1) << 55) - 1;
// ZERO extent 单段分配硬上限：真实 lpmake --sparse 产物的 ZERO extent 远小于此。
// ZERO extent 长度可以与镜像文件本身无关（sparse 产物中零填充区间通常比文件大），
// 故不按文件大小闸门，而用固定分配上限防御恶意/损坏元数据声明的巨量零填充导致 OOM。
constexpr quint64 kMaxZeroExtentBytes = Q_UINT64_C(1) << 30;  // 1 GiB

struct TableDesc { quint32 offset, numEntries, entrySize; };

// 表描述符前置校验：条目定长（AOSP 布局强制）、表完整落在 tables 区域内。
// 防恶意 numEntries/entrySize 导致长循环或 OOB 读。
bool checkTableDesc(const TableDesc &desc, quint32 tablesSize, quint32 expectedEntrySize)
{
    if (desc.entrySize != expectedEntrySize)
        return false;
    const quint64 bytes = static_cast<quint64>(desc.numEntries) * desc.entrySize;
    return desc.offset <= tablesSize && bytes <= tablesSize - desc.offset;
}
} // namespace

bool isSuper(const QByteArray &header)
{
    return header.size() >= 4 && qFromLittleEndian<quint32>(header.constData()) == kGeomMagic;
}

bool parseSuper(const QByteArray &image, SuperInfo &out)
{
    // geometry 主副本区 [0,4096) → 几何块
    if (image.size() < kReserved + kGeometrySize)
        return false;
    const char *geom = image.constData() + kReserved;
    if (qFromLittleEndian<quint32>(geom) != kGeomMagic)
        return false;
    out.slotCount = qFromLittleEndian<quint32>(geom + 44);
    out.logicalBlockSize = qFromLittleEndian<quint32>(geom + 48);
    if (out.slotCount == 0 || out.logicalBlockSize == 0)
        return false;
    // metadata slot 0（紧随 geometry）
    const int metaOff = kReserved + kGeometrySize;
    // 读取 meta+0..128（头字段 + 4 个表描述符 @80/92/104/116，最末读至 meta+128）
    if (metaOff + 128 > image.size())
        return false;
    const char *meta = image.constData() + metaOff;
    if (qFromLittleEndian<quint32>(meta) != kMetaMagic)
        return false;
    const quint32 headerSize = qFromLittleEndian<quint32>(meta + 8);
    const quint32 tablesSize = qFromLittleEndian<quint32>(meta + 44);
    if (headerSize < 128 || headerSize > 256)
        return false;
    // 表描述符: partitions=80, extents=92, groups=104, block_devices=116
    TableDesc partsDesc{ qFromLittleEndian<quint32>(meta + 80), qFromLittleEndian<quint32>(meta + 84),
                         qFromLittleEndian<quint32>(meta + 88) };
    TableDesc extDesc{ qFromLittleEndian<quint32>(meta + 92), qFromLittleEndian<quint32>(meta + 96),
                       qFromLittleEndian<quint32>(meta + 100) };
    // 前置校验：条目定长 + 表区域边界（防恶意条目数/条目大小导致长循环或 OOB 读）
    if (!checkTableDesc(partsDesc, tablesSize, kPartitionEntrySize) ||
        !checkTableDesc(extDesc, tablesSize, kExtentEntrySize))
        return false;
    const qint64 tablesBase = metaOff + headerSize;
    if (tablesBase + tablesSize > image.size())
        return false;
    out.partitions.clear();
    for (quint32 i = 0; i < partsDesc.numEntries; ++i) {
        const qint64 off = tablesBase + partsDesc.offset + static_cast<qint64>(i) * partsDesc.entrySize;
        if (off + partsDesc.entrySize > image.size())
            return false;
        const char *p = image.constData() + off;
        Partition part;
        part.name = QString::fromLatin1(p, 36).split('\0').first();
        part.attrs = qFromLittleEndian<quint32>(p + 36);
        part.firstExtentIndex = qFromLittleEndian<quint32>(p + 40);
        part.numExtents = qFromLittleEndian<quint32>(p + 44);
        part.groupIndex = qFromLittleEndian<quint32>(p + 48);
        for (quint32 e = 0; e < part.numExtents; ++e) {
            const qint64 eoff = tablesBase + extDesc.offset +
                                static_cast<qint64>(part.firstExtentIndex + e) * extDesc.entrySize;
            if (eoff + extDesc.entrySize > image.size())
                return false;
            const char *ex = image.constData() + eoff;
            Extent ext;
            ext.numSectors = qFromLittleEndian<quint64>(ex);
            ext.targetType = qFromLittleEndian<quint32>(ex + 8);
            ext.targetData = qFromLittleEndian<quint64>(ex + 12);
            part.extents.append(ext);
        }
        out.partitions.append(part);
    }
    return true;
}

QList<QByteArray> extractPartitions(const QByteArray &image, const SuperInfo &info, QString *error)
{
    QList<QByteArray> out;
    for (const Partition &part : info.partitions) {
        QByteArray data;
        bool ok = true;
        for (const Extent &ext : part.extents) {
            // 乘法回绕校验：numSectors > 2^55 时 numSectors * 512 会回绕 quint64
            if (ext.numSectors > kMaxSectors) {
                ok = false;
                if (error) *error = QString("分区 %1 extent 扇区数溢出").arg(part.name);
                break;
            }
            const quint64 byteLen = ext.numSectors * kSector;
            if (ext.targetType == 0) { // LINEAR: 物理扇区号 → 字节偏移
                if (ext.targetData > kMaxSectors) {
                    ok = false;
                    if (error) *error = QString("分区 %1 extent 起始扇区溢出").arg(part.name);
                    break;
                }
                const quint64 byteOff = ext.targetData * kSector;
                if (byteOff + byteLen > static_cast<quint64>(image.size())) {
                    ok = false;
                    if (error) *error = QString("分区 %1 extent 越界").arg(part.name);
                    break;
                }
                // 经上界校验 byteOff/byteLen ≤ image.size()（qsizetype 上限），
                // 窄化为 qint64 无回绕风险。Qt6 mid 原生接受 qsizetype，
                // 不再用 int —— 旧实现 int(3GB) 回绕为负会静默产出错误内容。
                data.append(image.mid(static_cast<qint64>(byteOff), static_cast<qint64>(byteLen)));
            } else { // ZERO: 零填充；长度可与镜像文件大小无关（lpmake --sparse 产物），
                // 故用固定分配硬上限（而非文件大小）作闸门，防巨量分配与 int 负尺寸崩溃
                if (byteLen > kMaxZeroExtentBytes) {
                    ok = false;
                    if (error) *error = QString("分区 %1 ZERO extent 过长 (%2 字节，上限 %3)")
                                             .arg(part.name).arg(byteLen).arg(kMaxZeroExtentBytes);
                    break;
                }
                data.append(QByteArray(static_cast<int>(byteLen), 0));
            }
        }
        if (!ok)
            return {};
        out.append(data);
    }
    return out;
}

} // namespace imgsuper
