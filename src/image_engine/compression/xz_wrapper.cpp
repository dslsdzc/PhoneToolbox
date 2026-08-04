#include "xz_wrapper.h"
#include <lzma.h>

namespace imgcomp {

QByteArray xzCompress(const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    size_t bound = lzma_stream_buffer_bound(static_cast<size_t>(data.size()));
    // +8：v1 容器，尾部追加 8 字节小端 uncompressed size
    QByteArray out(static_cast<int>(bound) + 8, Qt::Uninitialized);
    size_t outPos = 0;
    lzma_ret r = lzma_easy_buffer_encode(6, LZMA_CHECK_CRC64, nullptr,
                                         reinterpret_cast<const uint8_t *>(data.constData()),
                                         static_cast<size_t>(data.size()),
                                         reinterpret_cast<uint8_t *>(out.data()), &outPos, bound);
    if (r != LZMA_OK)
        return {};
    const quint64 sz = static_cast<quint64>(data.size());
    for (int i = 0; i < 8; ++i)
        out[static_cast<int>(outPos) + i] = static_cast<char>((sz >> (i * 8)) & 0xff);
    out.truncate(static_cast<int>(outPos + 8));
    return out;
}

QByteArray xzDecompress(const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    // 先读 uncompr_size（xz 尾 8 字节，v1 容器）
    if (data.size() < 8)
        return {};
    quint64 rawSize = 0;
    for (int i = 0; i < 8; ++i)
        rawSize |= static_cast<quint64>(static_cast<uchar>(data[data.size() - 8 + i])) << (i * 8);
    if (rawSize == 0 || rawSize > (1ULL << 32) * 8)
        return {};
    QByteArray out(static_cast<int>(rawSize), Qt::Uninitialized);
    size_t inPos = 0, outPos = 0;
    uint64_t memlimit = UINT64_MAX; // 本系统 liblzma 要求有效的 memlimit 指针；UINT64_MAX 等效无限制
    lzma_ret r = lzma_stream_buffer_decode(&memlimit, 0, nullptr,
                                           reinterpret_cast<const uint8_t *>(data.constData()),
                                           &inPos, static_cast<size_t>(data.size() - 8),
                                           reinterpret_cast<uint8_t *>(out.data()), &outPos,
                                           static_cast<size_t>(rawSize));
    if (r != LZMA_OK)
        return {};
    out.truncate(static_cast<int>(outPos));
    return out;
}

} // namespace imgcomp
