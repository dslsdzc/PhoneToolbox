#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

namespace imgsin {

// 索尼 SIN v3 块描述（BlockInfoHeader 解析结果; 布局三源确认: flashtool S1ParseLib /
// ROMExplorer / munjeni sin2raw, 字段全为大端 BE）
//   dataStart : 块数据在 sin 文件中的绝对偏移
//               （parse 时由 数据基址 = headerLen + mmcfLen + 8 折算 dataOffset 得到）
//   dataLength: ADDR = dataLen（原始字节数）; LZ4A = compDataLen（磁盘压缩字节数）
//   dataDest  : fileOffset —— 输出 raw 镜像中的落位偏移
//   blockSize : LZ4A = uncompDataLen（解压后字节数）; ADDR 为 0
//   compressed: true = LZ4A 块（LZ4 raw-block 格式, 无 frame 头）; false = ADDR 原始块
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
