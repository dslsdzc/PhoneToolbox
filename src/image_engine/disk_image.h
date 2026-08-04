#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

namespace imgdisk {

struct Partition { QString name; quint64 startSector = 0; quint64 numSectors = 0; QString typeGuid; };
struct DiskInfo { quint64 sectorSize = 512; quint64 totalSectors = 0; QList<Partition> partitions; };

bool isGpt(const QByteArray &lba1Header);               // "EFI PART"
bool parseGpt(const QByteArray &disk, DiskInfo &out);
bool extractPartition(const QByteArray &disk, const Partition &part, QByteArray &outRaw);

} // namespace imgdisk
