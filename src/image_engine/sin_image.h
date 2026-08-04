#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

namespace imgsin {

// 索尼 SIN v3 块描述（BlockInfoHeader 解析结果）
//   dataStart : 块数据相对文件起始的偏移（sin2raw 语义）
//   dataLength: ADDR = 原始数据字节数; LZ4A = 压缩数据字节数（磁盘上大小）
//   dataDest  : 输出 raw 镜像中的落位偏移（fileOffset）
//   blockSize : LZ4A = 解压后数据字节数（ADDR 为 0）
//   compressed: true = LZ4A 压缩块（lz4frame 解压）; false = ADDR 原始块
struct BlockDesc {
    QByteArray magic;        // "ADDR" / "LZ4A"
    quint64 dataStart = 0;
    quint64 blockSize = 0;
    quint64 dataLength = 0;
    quint64 dataDest = 0;
    bool compressed = false;
};

bool isSinV3(const QByteArray &header);                    // 0x03 "SIN"
bool parseSin(const QByteArray &sin, QList<BlockDesc> &blocks, QString *error);
QByteArray extractRaw(const QByteArray &sin, const QList<BlockDesc> &blocks, QString *error);

} // namespace imgsin
