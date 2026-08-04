#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

namespace imgkdz {

struct DzFile { QString name; QByteArray data; };
struct DzChunk { QString partition; quint64 offset; QByteArray data; };

// KDZ v3 容器解析: 头 + 文件记录表 → 文件列表
// (字节布局对照 IOMonster kdztools unkdz.py + libexec/kdz.py, 写布局见 mkkdz.py)
bool parseKdz(const QByteArray &kdz, QList<DzFile> &out, QString *error);
// DZ 解析: 分区 chunk 表 + 数据 → chunk 列表（offset = eMMC 偏移, 数据已解压）
// (字节布局对照 IOMonster kdztools undz.py + libexec/dz.py)
bool parseDz(const QByteArray &dz, QList<DzChunk> &out, QString *error);
// 按 eMMC 偏移合并 chunk 为单个分区镜像（空洞填零, 上限 2^33 = 8GB）
QByteArray mergeChunks(const QList<DzChunk> &chunks, QString *error);

} // namespace imgkdz
