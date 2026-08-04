#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

// super 动态分区镜像：lp metadata 解析与逻辑分区提取。
// 布局（对照 AOSP metadata_format.h，已由 spec 验证）：
//   geometry 主副本区 [0, 4096) → 几何块 "gDla" [4096, 8192) → metadata 头+表 [8192, ...)
//   LpMetadataPartition 52B / LpMetadataExtent 24B / 逻辑扇区 512B。
namespace imgsuper {

struct Extent { quint64 numSectors = 0; quint32 targetType = 0; quint64 targetData = 0; };
struct Partition {
    QString name;
    quint32 attrs = 0;
    quint64 firstExtentIndex = 0;
    quint32 numExtents = 0;
    quint64 groupIndex = 0;
    QList<Extent> extents;
};
struct SuperInfo { quint32 slotCount = 1; quint32 logicalBlockSize = 4096; QList<Partition> partitions; };

bool isSuper(const QByteArray &geometryHeader);          // 几何块头 4B == 0x616c4467 ("gDla")
bool parseSuper(const QByteArray &image, SuperInfo &out);
QList<QByteArray> extractPartitions(const QByteArray &image, const SuperInfo &info, QString *error);

} // namespace imgsuper
