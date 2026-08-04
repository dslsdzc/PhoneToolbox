#include "ramdisk_utils.h"
#include "image_engine/compression/compressor.h"

#include <lz4.h>
#include <lzma.h>
#include <limits>

namespace patcher {
namespace {

// ---- 格式常量（全部经联网验证，见 task-C1-report.md）----
// lz4 legacy（Android ramdisk 常用，`lz4 -l` / magiskboot LZ4_LEGACY）：
//   [魔数 4B LE 0x184C2102][每块: compSize 4B LE + 压缩数据]...
//   每块无 uncompSize 字段（liblz4 legacy、magiskboot 新旧实现均如此）；
//   LG 变体（magiskboot LZ4_LG）在流尾追加 4B LE 总未压缩大小。
constexpr int kLegacyMagic = 0x184C2102;
constexpr int kLegacyBlockSize = 0x800000; // 8 MiB：legacy 每块最大未压缩输入

// lz4 frame 魔数：v2 0x184D2204；v1 0x184C2103（magiskboot LZ41_MAGIC/LZ42_MAGIC）
constexpr int kLz4FrameMagic = 0x184D2204;
constexpr int kLz4FrameMagicV1 = 0x184C2103;

quint32 readLE32(const QByteArray &d, int pos)
{
    return (static_cast<quint32>(static_cast<uchar>(d[pos])) |
            (static_cast<quint32>(static_cast<uchar>(d[pos + 1])) << 8) |
            (static_cast<quint32>(static_cast<uchar>(d[pos + 2])) << 16) |
            (static_cast<quint32>(static_cast<uchar>(d[pos + 3])) << 24));
}

void appendLE32(QByteArray &out, quint32 v)
{
    out.append(char(v & 0xFF));
    out.append(char((v >> 8) & 0xFF));
    out.append(char((v >> 16) & 0xFF));
    out.append(char((v >> 24) & 0xFF));
}

// lz4 legacy 块流解压。失败返回空并写入 error。
QByteArray lz4LegacyDecompress(const QByteArray &raw, QString *error)
{
    auto fail = [error](const char *msg) -> QByteArray {
        if (error)
            *error = QString::fromUtf8(msg);
        return {};
    };
    if (raw.size() < 4)
        return fail("lz4 legacy: 数据过短");
    if (readLE32(raw, 0) != static_cast<quint32>(kLegacyMagic))
        return fail("lz4 legacy: 魔数错误");
    // 单块压缩大小上限：compressBound(8MiB) 以内为正常块；超出视为 LG 尾部
    // （magiskboot LZ4BlockDecoder 同判据：block size > compress bound → 流尾）。
    const int maxBlock = LZ4_compressBound(kLegacyBlockSize);
    QByteArray out;
    QByteArray block(kLegacyBlockSize, Qt::Uninitialized);
    int pos = 4;
    while (pos < raw.size()) {
        if (pos + 4 > raw.size())
            return fail("lz4 legacy: 块头截断");
        const quint32 compSize = readLE32(raw, pos);
        pos += 4;
        if (compSize == 0)
            return fail("lz4 legacy: 空块");
        if (static_cast<int>(compSize) > maxBlock) {
            // LG 变体：流尾 4 字节为总未压缩大小，流结束
            break;
        }
        if (pos + static_cast<int>(compSize) > raw.size())
            return fail("lz4 legacy: 数据越界");
        const int r = LZ4_decompress_safe(raw.constData() + pos, block.data(),
                                          static_cast<int>(compSize), kLegacyBlockSize);
        if (r < 0)
            return fail("lz4 legacy: LZ4 解压失败");
        out.append(block.constData(), r);
        pos += static_cast<int>(compSize);
    }
    return out;
}

// lz4 legacy 块流压缩（8 MiB 分块，与 magiskboot/liblz4 一致）。
QByteArray lz4LegacyCompress(const QByteArray &data)
{
    QByteArray out;
    out.reserve(data.size() / 3 + 16);
    appendLE32(out, static_cast<quint32>(kLegacyMagic));
    int pos = 0;
    while (pos < data.size()) {
        const int len = qMin(kLegacyBlockSize, data.size() - pos);
        const int bound = LZ4_compressBound(len);
        QByteArray comp(bound, Qt::Uninitialized);
        const int compSize = LZ4_compress_default(data.constData() + pos, comp.data(), len, bound);
        if (compSize <= 0)
            return {};
        appendLE32(out, static_cast<quint32>(compSize));
        out.append(comp.constData(), compSize);
        pos += len;
    }
    return out;
}

// lzma-alone 容器解压（lzma_alone_decoder）。失败返回空并写入 error。
// 头布局：1B 属性(0x5D) + 4B LE 字典大小 + 8B LE 未压缩大小(UINT64_MAX=未知)。
QByteArray lzmaAloneDecompress(const QByteArray &raw, QString *error)
{
    auto fail = [error](const char *msg) -> QByteArray {
        if (error)
            *error = QString::fromUtf8(msg);
        return {};
    };
    if (raw.size() < 13)
        return fail("lzma: 数据过短");
    lzma_stream strm = LZMA_STREAM_INIT;
    // 512 MiB 内存限额：init 时校验字典大小，超限返回错误，不会崩溃
    if (lzma_alone_decoder(&strm, 512ULL * 1024 * 1024) != LZMA_OK)
        return fail("lzma: 解码器初始化失败");
    QByteArray out;
    QByteArray obuf(64 * 1024, Qt::Uninitialized);
    const size_t maxOut = static_cast<size_t>(std::numeric_limits<int>::max());
    strm.next_in = reinterpret_cast<const uint8_t *>(raw.constData());
    strm.avail_in = static_cast<size_t>(raw.size());
    for (;;) {
        strm.next_out = reinterpret_cast<uint8_t *>(obuf.data());
        strm.avail_out = static_cast<size_t>(obuf.size());
        const lzma_ret r = lzma_code(&strm, LZMA_FINISH);
        const size_t produced = obuf.size() - strm.avail_out;
        if (produced > 0) {
            if (static_cast<size_t>(out.size()) + produced > maxOut) {
                lzma_end(&strm);
                return fail("lzma: 输出超限");
            }
            out.append(obuf.constData(), static_cast<int>(produced));
        }
        if (r == LZMA_STREAM_END)
            break;
        if (r != LZMA_OK || strm.avail_in == 0) {
            lzma_end(&strm);
            return fail("lzma: 解压失败或数据截断");
        }
    }
    const size_t leftover = strm.avail_in;
    lzma_end(&strm);
    if (leftover != 0)
        return fail("lzma: 流后存在尾随数据");
    return out;
}

// lzma-alone 容器压缩（lzma_alone_encoder）。
QByteArray lzmaAloneCompress(const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    lzma_options_lzma opt;
    if (lzma_lzma_preset(&opt, 6))
        return {};
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_alone_encoder(&strm, &opt) != LZMA_OK)
        return {};
    QByteArray out;
    QByteArray obuf(64 * 1024, Qt::Uninitialized);
    strm.next_in = reinterpret_cast<const uint8_t *>(data.constData());
    strm.avail_in = static_cast<size_t>(data.size());
    for (;;) {
        strm.next_out = reinterpret_cast<uint8_t *>(obuf.data());
        strm.avail_out = static_cast<size_t>(obuf.size());
        const lzma_ret r = lzma_code(&strm, LZMA_FINISH);
        out.append(obuf.constData(), static_cast<int>(obuf.size() - strm.avail_out));
        if (r == LZMA_STREAM_END)
            break;
        if (r != LZMA_OK) {
            lzma_end(&strm);
            return {};
        }
    }
    lzma_end(&strm);
    return out;
}

} // namespace

bool detectRamdiskFormat(const QByteArray &head, QString &format)
{
    if (head.size() >= 2 && static_cast<uchar>(head[0]) == 0x1f &&
        static_cast<uchar>(head[1]) == 0x8b) {
        format = QStringLiteral("gzip"); // RFC 1952
        return true;
    }
    if (head.size() >= 6 &&
        head.left(6) == QByteArray("\xfd\x37\x7a\x58\x5a\x00", 6)) {
        format = QStringLiteral("xz"); // XZ 规范: FD 37 7A 58 5A 00
        return true;
    }
    if (head.size() >= 14 && static_cast<uchar>(head[0]) == 0x5d) {
        // lzma-alone：1B 属性(0x5D=lc3/lp0/pb2) + 4B LE 字典大小 + 8B LE 未压缩大小。
        // 采用 magiskboot guess_lzma 严格判据（字典须为非零 2 的幂、大小须为未知
        // 0xFFFFFFFFFFFFFFFF），降低对任意二进制的误报。
        quint32 dict = 0;
        for (int i = 0; i < 4; ++i)
            dict |= static_cast<quint32>(static_cast<uchar>(head[1 + i])) << (i * 8);
        if (dict != 0 && (dict & (dict - 1)) == 0 &&
            head.mid(5, 8) == QByteArray("\xff\xff\xff\xff\xff\xff\xff\xff", 8)) {
            format = QStringLiteral("lzma");
            return true;
        }
    }
    if (head.size() >= 4) {
        const int magic = static_cast<int>(readLE32(head, 0));
        if (magic == kLegacyMagic) {
            format = QStringLiteral("lz4"); // legacy 块流（Android ramdisk 常用）
            return true;
        }
        if (magic == kLz4FrameMagic || magic == kLz4FrameMagicV1) {
            format = QStringLiteral("lz4"); // LZ4F frame（v2/v1）
            return true;
        }
    }
    return false;
}

bool decompressRamdisk(const QByteArray &raw, QByteArray &out, QString *error)
{
    if (error)
        error->clear();
    QString fmt;
    if (!detectRamdiskFormat(raw, fmt)) {
        out = raw; // 未压缩
        return true;
    }
    if (fmt == "gzip") {
        out = imgcomp::decompress(imgcomp::Type::Gzip, raw);
    } else if (fmt == "lz4") {
        // 同一 "lz4" 按魔数区分 legacy 块流与 LZ4F frame
        if (raw.size() >= 4 && readLE32(raw, 0) == static_cast<quint32>(kLegacyMagic))
            out = lz4LegacyDecompress(raw, error);
        else
            out = imgcomp::decompress(imgcomp::Type::Lz4, raw);
    } else if (fmt == "lzma") {
        out = lzmaAloneDecompress(raw, error);
    } else if (fmt == "xz") {
        out = imgcomp::decompress(imgcomp::Type::Xz, raw);
    } else {
        out.clear();
    }
    if (out.isEmpty()) {
        if (error && error->isEmpty())
            *error = QStringLiteral("ramdisk 解压失败");
        return false;
    }
    return true;
}

QByteArray compressRamdisk(const QByteArray &data, const QString &format)
{
    if (format == "gzip")
        return imgcomp::compress(imgcomp::Type::Gzip, data);
    if (format == "lz4")
        return lz4LegacyCompress(data);
    if (format == "lzma")
        return lzmaAloneCompress(data);
    if (format == "xz")
        return imgcomp::compress(imgcomp::Type::Xz, data);
    return data; // "raw" 与未知格式原样返回
}

} // namespace patcher
