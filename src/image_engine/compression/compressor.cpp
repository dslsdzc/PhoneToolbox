#include "compressor.h"
#include "brotli_wrapper.h"
#include "bzip2_wrapper.h"
#include "lz4_wrapper.h"
#include "xz_wrapper.h"
#include "zstd_wrapper.h"

#include <limits>
#include <zlib.h>

namespace imgcomp {
namespace {

// 完整标准 gzip 帧。注意：不能用 compress2/qCompress —— 它们产出 zlib 流
// （自身 2 字节 0x78 头 + Adler-32 尾），与 gzip 帧（0x1f 0x8b 头 + 裸 deflate
// + CRC32/ISIZE 尾）互不兼容，解压侧按标准 gzip 布局解析会失败。
// 用 deflateInit2(windowBits=15+16) 让 zlib 自动写 gzip 头与 CRC32/ISIZE 尾，
// 产出与 gzip -d 互操作的标准 gzip 文件。
QByteArray gzipCompressImpl(const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    z_stream strm{};
    if (deflateInit2(&strm, 6, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return {};
    uLongf bound = deflateBound(&strm, static_cast<uLong>(data.size()));
    QByteArray out(static_cast<int>(bound) + 18, Qt::Uninitialized); // +18 兜底
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    strm.avail_in = static_cast<uInt>(data.size());
    strm.next_out = reinterpret_cast<Bytef *>(out.data());
    strm.avail_out = static_cast<uInt>(out.size());
    int r = deflate(&strm, Z_FINISH);
    const uLongf totalOut = strm.total_out;
    deflateEnd(&strm);
    if (r != Z_STREAM_END)
        return {};
    out.truncate(static_cast<int>(totalOut));
    return out;
}

// 流式 inflate 兜底：不信任尾部 ISIZE 预分配，增量解压、总量封顶 INT_MAX。
// 成功须到达 Z_STREAM_END（截断 → 失败）。注意 windowBits=15+16 时 inflate
// 期望输入自带 gzip 头（自行跳过并校验），须从 data[0] 喂入，不可跳过头部。
// inflate 内部还会校验 trailer CRC32 与 ISIZE：伪造/损坏 → Z_DATA_ERROR → 空。
QByteArray gzipInflateStreaming(const QByteArray &data)
{
    z_stream strm{};
    // 15 + 16: 解码 gzip 格式，strm.adler 报告 CRC32
    if (inflateInit2(&strm, 15 + 16) != Z_OK)
        return {};
    const size_t maxOut = static_cast<size_t>(std::numeric_limits<int>::max());
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    strm.avail_in = static_cast<uInt>(data.size());
    QByteArray out;
    QByteArray chunk(64 * 1024, Qt::Uninitialized);
    int r;
    for (;;) {
        strm.next_out = reinterpret_cast<Bytef *>(chunk.data());
        strm.avail_out = static_cast<uInt>(chunk.size());
        r = inflate(&strm, Z_NO_FLUSH);
        const int produced = static_cast<int>(chunk.size()) - static_cast<int>(strm.avail_out);
        if (produced > 0) {
            out.append(chunk.constData(), produced);
            if (static_cast<size_t>(out.size()) > maxOut) {
                inflateEnd(&strm);
                return {};
            }
        }
        if (r == Z_STREAM_END)
            break;
        if (r != Z_OK && r != Z_BUF_ERROR) { // Z_DATA_ERROR 等 → 损坏
            inflateEnd(&strm);
            return {};
        }
        if (produced == 0 && strm.avail_in == 0) {
            // 输入耗尽但流未结束 → 截断
            inflateEnd(&strm);
            return {};
        }
    }
    inflateEnd(&strm);
    // 校验尾部 CRC32（gzip trailer 倒数第 8 字节起，小端）
    if (static_cast<size_t>(data.size()) < 8)
        return {};
    quint32 crc = 0;
    const int cpos = data.size() - 8;
    for (int i = 0; i < 4; ++i)
        crc |= static_cast<quint32>(static_cast<uchar>(data[cpos + i])) << (i * 8);
    if (crc != static_cast<quint32>(strm.adler))
        return {};
    return out;
}

QByteArray gzipDecompressImpl(const QByteArray &data)
{
    if (data.size() < 10 || (static_cast<uchar>(data[0]) != 0x1f) ||
        (static_cast<uchar>(data[1]) != 0x8b))
        return {};
    // 跳过 gzip 头（10 字节起，含可选 FEXTRA/FNAME/FCOMMENT）
    size_t pos = 10;
    uchar flg = static_cast<uchar>(data[3]);
    if (flg & 0x04) { // FEXTRA
        if (pos + 2 > static_cast<size_t>(data.size())) return {};
        pos += 2 + ((static_cast<uchar>(data[pos]) |
                     (static_cast<uchar>(data[pos + 1]) << 8)));
    }
    if (flg & 0x08) { while (pos < static_cast<size_t>(data.size()) && data[pos++] != 0) {} }
    if (flg & 0x10) { while (pos < static_cast<size_t>(data.size()) && data[pos++] != 0) {} }
    if (flg & 0x02) { pos += 2; }
    if (pos >= static_cast<size_t>(data.size()))
        return {};
    // 尾部 ISIZE（最后 4 字节）是原始大小 mod 2^32 —— 仅作预分配提示，绝不可信：
    // 伪造 ISIZE 大值不得引发大分配（Task 5 同款问题）。直接分配仅当
    // ISIZE ∈ [1, INT_MAX] 且不超过输入 1024 倍（deflate 理论最大压缩比约
    // 1032:1，留余量）；其余情况（ISIZE==0 / 超 2GiB / 远超输入）回退流式。
    quint32 isize = 0;
    const int tail = data.size() - 4;
    for (int i = 0; i < 4; ++i)
        isize |= static_cast<quint32>(static_cast<uchar>(data[tail + i])) << (i * 8);
    const quint64 maxDirect = static_cast<quint64>(std::numeric_limits<int>::max());
    const bool plausible = isize >= 1 &&
                           static_cast<quint64>(isize) <= maxDirect &&
                           static_cast<quint64>(isize) <= static_cast<quint64>(data.size()) * 1024ULL;
    if (!plausible)
        return gzipInflateStreaming(data);
    // 一次性 inflate（windowBits=15+16 期望输入含 gzip 头，从 data[0] 喂入，
    // 头由 inflate 自行跳过并校验）。注意不能再用 uncompress —— 它只认
    // zlib 流，不认 gzip 帧；且 uncompress 不校验 gzip trailer。
    QByteArray out(static_cast<int>(isize), Qt::Uninitialized);
    z_stream strm{};
    if (inflateInit2(&strm, 15 + 16) != Z_OK)
        return {};
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    strm.avail_in = static_cast<uInt>(data.size());
    strm.next_out = reinterpret_cast<Bytef *>(out.data());
    strm.avail_out = static_cast<uInt>(out.size());
    int r = inflate(&strm, Z_FINISH);
    const uLongf totalOut = strm.total_out;
    const uLong streamCrc = strm.adler; // gzip 模式下为 CRC32，须在 inflateEnd 前读
    inflateEnd(&strm);
    if (r != Z_STREAM_END)
        return gzipInflateStreaming(data); // 缓冲不足/截断/伪造 → 流式兜底（亦失败）
    // inflate 已校验 trailer CRC32 与 ISIZE（不通过返回 Z_DATA_ERROR 到上面分支）；
    // 此处再比对一次 CRC32 作双保险：trailer 被截/损坏 → 失败
    quint32 crc = 0;
    const int cpos = data.size() - 8;
    for (int i = 0; i < 4; ++i)
        crc |= static_cast<quint32>(static_cast<uchar>(data[cpos + i])) << (i * 8);
    if (crc != static_cast<quint32>(streamCrc))
        return {};
    out.truncate(static_cast<int>(totalOut));
    return out;
}

} // namespace

QByteArray compress(Type t, const QByteArray &data)
{
    switch (t) {
    case Type::Zstd: return zstdCompress(data);
    case Type::Lz4: return lz4Compress(data);
    case Type::Bzip2: return bzip2Compress(data);
    case Type::Xz: return xzCompress(data);
    case Type::Brotli: return brotliCompress(data);
    case Type::Gzip: return gzipCompressImpl(data);
    default: return {};
    }
}

QByteArray decompress(Type t, const QByteArray &data)
{
    switch (t) {
    case Type::Zstd: return zstdDecompress(data);
    case Type::Lz4: return lz4Decompress(data);
    case Type::Bzip2: return bzip2Decompress(data);
    case Type::Xz: return xzDecompress(data);
    case Type::Brotli: return brotliDecompress(data);
    case Type::Gzip: return gzipDecompressImpl(data);
    default: return {};
    }
}

QByteArray gzipCompress(const QByteArray &data) { return gzipCompressImpl(data); }
QByteArray gzipDecompress(const QByteArray &data) { return gzipDecompressImpl(data); }

} // namespace imgcomp
