#include "disk_image.h"
#include <QtEndian>
#include <algorithm>

namespace imgdisk {

namespace {
constexpr quint64 kSector = 512;

// UEFI 规范: GPT 分区项的 type_guid 为混合字节序存储
// Data1(4B LE) - Data2(2B LE) - Data3(2B LE) - Data4(2B BE) - Data5(6B 原序)
// 前三个字段需反转字节才能得到规范文本表示 (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)
QString formatTypeGuid(const char *e)
{
    auto hexUp = [](const QByteArray &b) {
        return QString::fromLatin1(b.toHex()).toUpper();
    };
    QByteArray d1(e, 4);
    QByteArray d2(e + 4, 2);
    QByteArray d3(e + 6, 2);
    std::reverse(d1.begin(), d1.end());
    std::reverse(d2.begin(), d2.end());
    std::reverse(d3.begin(), d3.end());
    return QString("%1-%2-%3-%4-%5")
        .arg(hexUp(d1))
        .arg(hexUp(d2))
        .arg(hexUp(d3))
        .arg(hexUp(QByteArray(e + 8, 2)))
        .arg(hexUp(QByteArray(e + 10, 6)));
}

bool isNullGuid(const char *e)
{
    for (int i = 0; i < 16; ++i)
        if (e[i] != 0)
            return false;
    return true;
}
} // namespace

bool isGpt(const QByteArray &header)
{
    return header.size() >= 8 && header.left(8) == "EFI PART";
}

bool parseGpt(const QByteArray &disk, DiskInfo &out)
{
    if (disk.size() < 2 * static_cast<qint64>(kSector))
        return false;
    if (!isGpt(disk.mid(static_cast<int>(kSector), 8)))
        return false;
    const char *h = disk.constData() + kSector;
    const quint64 tableLba = qFromLittleEndian<quint64>(h + 72);
    const quint32 numEntries = qFromLittleEndian<quint32>(h + 80);
    const quint32 entrySize = qFromLittleEndian<quint32>(h + 84);
    if (entrySize < 128)
        return false;
    // 分区表 LBA 超出磁盘范围则失败（同时避免 tableLba * kSector 溢出）
    if (tableLba > static_cast<quint64>(disk.size()) / kSector)
        return false;
    out.sectorSize = kSector;
    out.totalSectors = static_cast<quint64>(disk.size()) / kSector;
    const quint64 tableOff = tableLba * kSector;
    out.partitions.clear();
    // 用 quint64 累加偏移，避免 i * entrySize 有符号溢出
    for (quint32 i = 0; i < numEntries; ++i) {
        const quint64 off = tableOff + static_cast<quint64>(i) * entrySize;
        if (off + 128 > static_cast<quint64>(disk.size()))
            break;
        const char *e = disk.constData() + off;
        const quint64 first = qFromLittleEndian<quint64>(e + 32);
        const quint64 last = qFromLittleEndian<quint64>(e + 40);
        // 空项: 全零 type GUID 且无扇区范围（真实分区 first_lba 远大于 0，
        // 故单看 type GUID 全零不足以判定空项——测试用全零 GUID 的合法分区须保留）
        if (isNullGuid(e) && first == 0 && last == 0)
            continue;
        Partition p;
        p.typeGuid = formatTypeGuid(e);
        p.startSector = first;
        p.numSectors = last >= first ? last - first + 1 : 0;
        QByteArray nameRaw(e + 56, 72);
        // UTF-16LE → UTF-8
        p.name = QString::fromUtf16(reinterpret_cast<const char16_t *>(nameRaw.constData()),
                                    nameRaw.size() / 2).split(QChar(0)).first();
        out.partitions.append(p);
    }
    return true;
}

bool extractPartition(const QByteArray &disk, const Partition &part, QByteArray &outRaw)
{
    if (part.numSectors == 0)
        return false;
    const quint64 diskSize = static_cast<quint64>(disk.size());
    // 除零检查 + 溢出防护: 先按扇区数比较再乘
    if (part.startSector > diskSize / kSector)
        return false;
    const quint64 off = part.startSector * kSector;
    if (part.numSectors > (diskSize - off) / kSector)
        return false;
    const quint64 len = part.numSectors * kSector;
    outRaw = disk.mid(static_cast<int>(off), static_cast<int>(len));
    return true;
}

} // namespace imgdisk
