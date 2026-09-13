#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

namespace imgdisk {

struct Partition { QString name; quint64 startSector = 0; quint64 numSectors = 0; QString typeGuid; };
// sectorSize = 本盘的逻辑块大小（512 或 4096，由 parseGpt 按头所在偏移识别），也是
// startSector/numSectors 的字节单位 —— extractPartition 要按它换算，别写死 512。
struct DiskInfo { quint64 sectorSize = 512; quint64 totalSectors = 0; QList<Partition> partitions; };

bool isGpt(const QByteArray &lba1Header);               // "EFI PART"
bool parseGpt(const QByteArray &disk, DiskInfo &out);
// GPT 布局探测（签名在 0x200 → 512，在 0x1000 → 4096；顺序与理由见 disk_image.cpp）。
// parseGpt 内部也走它；对外的用途只有一个：让调用方在 parseGpt 失败后区分
// "头都不在/布局不认识"、"文件被截断"、"表项数组坏了"三种诊断（Phase B flash_plan.cpp 的
// readGptPartitions 文案分层）。它不做解析、不校验校验和。
bool detectGptLayout(const QByteArray &disk, quint64 &lbaSize);
// lbaSize 传 DiskInfo::sectorSize（默认 512 仅为兼容既有调用点；传错会静默取错区段）
bool extractPartition(const QByteArray &disk, const Partition &part, QByteArray &outRaw,
                      quint64 lbaSize = 512);

} // namespace imgdisk
