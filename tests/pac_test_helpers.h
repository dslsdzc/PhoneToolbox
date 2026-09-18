// tests/pac_test_helpers.h
//
// SPD（展锐）`.pac` 合成夹具（共享：test_pac / test_spd_pac）。
//
// 布局按社区参考实现构造（两源独立确认）:
//   divinebird/pacextractor (C)                   —— 旧格式, 头 1220B 无魔数
//   HemanthJabalapuri/pacextractor (Java/Python)  —— 新格式 BP_R1.0.0 / BP_R2.0.1
// 注: .pac 实为 Spreadtrum/Unisoc (SPD ResearchDownload) 容器; MTK 侧 SP Flash Tool
//     使用 scatter 文件而非 pac。社区所有 pac 解包工具均针对 SPD, 字段级布局以其为准。
//
// 新格式 (BP_R1.0.0 / BP_R2.0.1), 头 2124B, 全部小端:
//   version u16[22]@0(UTF-16LE) + hiSize@44 + loSize@48 + productName@52(512B)
//   + firmwareName@564(512B) + partitionCount@1076 + partitionsListStart@1080
//   + mode/flashType/nandStrategy/isNvBackup/nandPageType@1084..1100 + prdAlias@1104(200B)
//   + omaDmPrdFlag@1304 + isOmaDM@1308 + isPreload@1312 + reserved@1316(800B)
//   + magic 0xfffafffa@2116 + CRC1@2120 + CRC2@2122
//   分区项固定 2580B, 从 partitionsListStart 连续排布:
//   length@0(==2580) + partitionName@4(512B) + fileName@516(512B) + szFileName@1028(504B)
//   + hiPartitionSize@1532 + hiDataOffset@1536 + loPartitionSize@1540 + nFileFlag@1544
//   + nCheckFlag@1548 + loDataOffset@1552 + dwCanOmitFlag@1556 + dwAddrNum@1560
//   + dwAddr[5]@1564 + dwReserved[249]@1584
//   partitionSize  = hiPartitionSize<<32 | loPartitionSize（64 位）
//   partitionAddrInPac = hiDataOffset<<32 | loDataOffset（64 位）
//
// 旧格式 (divinebird C), 头 1220B 无魔数:
//   someField u16[24]@0 + someInt@48 + productName@52(512B) + firmwareName@564(512B)
//   + partitionCount@1076 + partitionsListStart@1080 + 其余保留
//   分区项变长, 以 length@0 链式步进（固定部分 1568B, 尾随 dataArray[]）:
//   length(4) + partitionName@4(512B) + fileName@516(1024B) + partitionSize@1540
//   + 保留@1544(8B) + partitionAddrInPac@1552 + 保留@1556(12B)
//   字符串为 UTF-16LE 单元数组（旧工具 getString 只取低字节, 对 ASCII 等价）
//
// 夹具**只写原始字段**（utf16le / put32 / putEntry*），不复用被测模块 `imgpac` 的任何
// 读写原语 —— 夹具与解析器共用同一原语时，方向性错误会自洽通过（先例：tests/mtk_test_helpers.h）。
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace pactest {

// 一条分区：数据按 payload 原样附在分区表之后（偏移由构造器算出，见 PacSample::offsets）
struct Entry {
    QString name;        // 分区 ID（写 partitionName）
    QString fileName;    // 容器内文件名（写 fileName）
    QByteArray payload;  // 分区数据；空 = 操作型条目（size 0，不占数据区）
};

// 构造结果。offsets 与 entries 一一对应，是**构造器自算**的数据偏移：
// 用例拿它（而不是回头问 parsePac）对拍提取结果，否则解析/提取双向错误会自洽通过。
struct PacSample {
    QByteArray data;
    QList<quint64> offsets;
};

inline QByteArray utf16le(const QString &s)
{
    QByteArray out;
    out.reserve(s.size() * 2);
    for (const QChar c : s) {
        out.append(char(c.unicode() & 0xFF));
        out.append(char((c.unicode() >> 8) & 0xFF));
    }
    return out;
}

inline void put32(QByteArray &d, int off, quint32 v)
{
    for (int i = 0; i < 4; ++i) d[off + i] = char((v >> (i * 8)) & 0xFF);
}

// 新格式分区项（2580B 固定）。注意: 字符串以实际 UTF-16LE 字节写入定宽字段
// （QByteArray::replace 遇更短数据会收缩数组, 必须只替换实际字节数）
inline void putEntryNew(QByteArray &d, int off, const QString &name, const QString &file,
                        quint64 size, quint64 dataOffset, quint32 fileFlag)
{
    put32(d, off + 0, 2580);                       // length == SIZE_OF_PARTITION_HEADER
    const QByteArray n = utf16le(name), f = utf16le(file);
    d.replace(off + 4, n.size(), n);
    d.replace(off + 516, f.size(), f);
    put32(d, off + 1532, quint32(size >> 32));     // hiPartitionSize
    put32(d, off + 1536, quint32(dataOffset >> 32)); // hiDataOffset
    put32(d, off + 1540, quint32(size & 0xFFFFFFFF)); // loPartitionSize
    put32(d, off + 1544, fileFlag);                // nFileFlag: 1 = 需要文件
    put32(d, off + 1552, quint32(dataOffset & 0xFFFFFFFF)); // loDataOffset
}

// 旧格式分区项（最小 1568B）。size/offset 只有 32 位字段 —— 夹具样本远小于 4GiB。
inline void putEntryOld(QByteArray &d, int off, const QString &name, const QString &file,
                        quint32 size, quint32 dataOffset)
{
    put32(d, off + 0, 1568);                       // length
    const QByteArray n = utf16le(name), f = utf16le(file);
    d.replace(off + 4, n.size(), n);
    d.replace(off + 516, f.size(), f);
    put32(d, off + 1540, size);                    // partitionSize
    put32(d, off + 1552, dataOffset);              // partitionAddrInPac
}

// 新格式样本: 头 2124B + N×2580B 分区表 + 各条目 payload 顺序追加。
// dwLoSize 写实际文件长度（parseNewFormat 要求两者相符）。
inline PacSample buildNew(const QList<Entry> &entries,
                          const QString &version = QStringLiteral("BP_R1.0.0"))
{
    constexpr int kHdr = 2124;
    constexpr int kEntry = 2580;
    const quint64 dataStart = quint64(kHdr) + quint64(entries.size()) * kEntry;
    PacSample s;
    s.data = QByteArray(qsizetype(dataStart), '\0');
    s.data.replace(0, utf16le(version).size(), utf16le(version));
    QByteArray blob;
    for (const Entry &e : entries) {
        s.offsets.append(dataStart + quint64(blob.size()));
        blob.append(e.payload);
    }
    put32(s.data, 44, 0);                                     // dwHiSize
    put32(s.data, 48, quint32(s.data.size() + blob.size()));  // dwLoSize
    put32(s.data, 1076, quint32(entries.size()));             // partitionCount
    put32(s.data, 1080, quint32(kHdr));                       // partitionsListStart
    put32(s.data, 2116, 0xfffafffa);                          // dwMagic
    for (int i = 0; i < entries.size(); ++i)
        putEntryNew(s.data, kHdr + i * kEntry, entries[i].name, entries[i].fileName,
                    quint64(entries[i].payload.size()), s.offsets[i], 1);
    s.data.append(blob);
    return s;
}

// 旧格式样本: 头 1220B + N×1568B 分区表（定长条目，不造变长尾随 dataArray[]）+ payload。
// 版本字段保持全零 —— 否则会命中新格式的版本门禁（parsePac 按版本串识别）。
inline PacSample buildLegacy(const QList<Entry> &entries)
{
    constexpr int kHdr = 1220;
    constexpr int kEntry = 1568;
    const quint64 dataStart = quint64(kHdr) + quint64(entries.size()) * kEntry;
    PacSample s;
    s.data = QByteArray(qsizetype(dataStart), '\0');
    QByteArray blob;
    for (const Entry &e : entries) {
        s.offsets.append(dataStart + quint64(blob.size()));
        blob.append(e.payload);
    }
    put32(s.data, 1076, quint32(entries.size()));  // partitionCount
    put32(s.data, 1080, quint32(kHdr));            // partitionsListStart
    for (int i = 0; i < entries.size(); ++i)
        putEntryOld(s.data, kHdr + i * kEntry, entries[i].name, entries[i].fileName,
                    quint32(entries[i].payload.size()), quint32(s.offsets[i]));
    s.data.append(blob);
    return s;
}

// 单分区样本（test_pac.cpp 原样沿用）: 1 分区 boot/boot.img, 数据 4096B。
// 新格式数据位于 2124+2580=4704, dwSize = 8800；旧格式数据位于 1220+1568=2788。
inline QByteArray buildPacNew(const QString &version = QStringLiteral("BP_R1.0.0"))
{
    return buildNew({Entry{QStringLiteral("boot"), QStringLiteral("boot.img"),
                           QByteArray(4096, '\x5a')}}, version).data;
}

inline QByteArray buildPacLegacy()
{
    return buildLegacy({Entry{QStringLiteral("boot"), QStringLiteral("boot.img"),
                              QByteArray(4096, '\x5a')}}).data;
}

} // namespace pactest
