#include "lz4_wrapper.h"
#include <lz4frame.h>

namespace imgcomp {

QByteArray lz4Compress(const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    LZ4F_preferences_t prefs = {};
    prefs.frameInfo.contentSize = static_cast<size_t>(data.size());
    size_t bound = LZ4F_compressFrameBound(static_cast<size_t>(data.size()), &prefs);
    QByteArray out(static_cast<int>(bound), Qt::Uninitialized);
    size_t r = LZ4F_compressFrame(out.data(), bound, data.constData(), data.size(), &prefs);
    if (LZ4F_isError(r))
        return {};
    out.truncate(static_cast<int>(r));
    return out;
}

QByteArray lz4Decompress(const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    LZ4F_decompressionContext_t ctx = nullptr;
    if (LZ4F_isError(LZ4F_createDecompressionContext(&ctx, LZ4F_VERSION)))
        return {};
    LZ4F_frameInfo_t fi{};
    size_t consumed = data.size();
    size_t err = LZ4F_getFrameInfo(ctx, &fi, data.constData(), &consumed);
    if (LZ4F_isError(err)) {
        LZ4F_freeDecompressionContext(ctx);
        return {};
    }
    size_t remaining = static_cast<size_t>(data.size()) - consumed;
    QByteArray out;
    out.reserve(fi.contentSize ? static_cast<int>(fi.contentSize) : data.size() * 4);
    QByteArray chunk(64 * 1024, Qt::Uninitialized);
    const char *src = data.constData() + consumed;
    while (remaining > 0) {
        size_t srcSize = remaining;
        size_t dstSize = chunk.size();
        err = LZ4F_decompress(ctx, chunk.data(), &dstSize, src, &srcSize, nullptr);
        if (LZ4F_isError(err)) {
            LZ4F_freeDecompressionContext(ctx);
            return {};
        }
        out.append(chunk.constData(), static_cast<int>(dstSize));
        src += srcSize;
        remaining -= srcSize;
        if (srcSize == 0 && dstSize == 0)
            break;
    }
    LZ4F_freeDecompressionContext(ctx);
    return out;
}

} // namespace imgcomp
