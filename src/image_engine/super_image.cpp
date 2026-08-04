#include "super_image.h"
#include <QtEndian>

namespace imgsuper {

namespace {
constexpr int kReserved = 4096;
constexpr int kGeometrySize = 4096;
constexpr quint32 kGeomMagic = 0x616c4467;
constexpr quint32 kMetaMagic = 0x414C5030;
constexpr quint64 kSector = 512;

struct TableDesc { quint32 offset, numEntries, entrySize; };
} // namespace

bool isSuper(const QByteArray &header)
{
    return header.size() >= 4 && qFromLittleEndian<quint32>(header.constData()) == kGeomMagic;
}

bool parseSuper(const QByteArray &image, SuperInfo &out)
{
    // 保留区 4096 → geometry
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
    const qint64 tablesBase = metaOff + headerSize;
    if (tablesBase + tablesSize > image.size())
        return false;
    out.partitions.clear();
    for (quint32 i = 0; i < partsDesc.numEntries; ++i) {
        const qint64 off = tablesBase + partsDesc.offset + static_cast<qint64>(i) * partsDesc.entrySize;
        if (off + 52 > image.size())
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
            if (eoff + 24 > image.size())
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
            const quint64 byteOff = ext.targetData * kSector;
            const quint64 byteLen = ext.numSectors * kSector;
            if (ext.targetType != 0) { // ZERO
                // 防御: ZERO extent 无落盘数据可校验，拒绝大于整个镜像的声明长度，
                // 避免 QByteArray(int) 溢出 / 巨量内存分配
                if (byteLen > static_cast<quint64>(image.size())) {
                    ok = false;
                    if (error) *error = QString("分区 %1 ZERO extent 长度异常").arg(part.name);
                    break;
                }
                data.append(QByteArray(static_cast<int>(byteLen), 0));
                continue;
            }
            if (byteOff + byteLen > static_cast<quint64>(image.size())) {
                ok = false;
                if (error) *error = QString("分区 %1 extent 越界").arg(part.name);
                break;
            }
            data.append(image.mid(static_cast<int>(byteOff), static_cast<int>(byteLen)));
        }
        if (!ok)
            return {};
        out.append(data);
    }
    return out;
}

} // namespace imgsuper
