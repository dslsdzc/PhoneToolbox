#include "zstd_wrapper.h"
#include <zstd.h>

namespace imgcomp {

QByteArray zstdCompress(const QByteArray &data, int level)
{
    if (data.isEmpty())
        return {};
    size_t bound = ZSTD_compressBound(static_cast<size_t>(data.size()));
    QByteArray out(static_cast<int>(bound), Qt::Uninitialized);
    size_t r = ZSTD_compress(out.data(), bound, data.constData(), data.size(), level);
    if (ZSTD_isError(r))
        return {};
    out.truncate(static_cast<int>(r));
    return out;
}

QByteArray zstdDecompress(const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    size_t sz = ZSTD_getFrameContentSize(data.constData(), data.size());
    if (sz != ZSTD_CONTENTSIZE_UNKNOWN && sz != ZSTD_CONTENTSIZE_ERROR) {
        QByteArray out(static_cast<int>(sz), Qt::Uninitialized);
        size_t r = ZSTD_decompress(out.data(), sz, data.constData(), data.size());
        if (ZSTD_isError(r))
            return {};
        out.truncate(static_cast<int>(r));
        return out;
    }
    // 流式解压（未知大小）
    ZSTD_DStream *ds = ZSTD_createDStream();
    if (!ds)
        return {};
    ZSTD_initDStream(ds);
    QByteArray out;
    out.reserve(data.size() * 4);
    QByteArray buf(64 * 1024, Qt::Uninitialized);
    ZSTD_inBuffer in{data.constData(), static_cast<size_t>(data.size()), 0};
    bool done = false;
    while (in.pos < in.size) {
        ZSTD_outBuffer ob{buf.data(), static_cast<size_t>(buf.size()), 0};
        size_t ret = ZSTD_decompressStream(ds, &ob, &in);
        if (ZSTD_isError(ret)) {
            ZSTD_freeDStream(ds);
            return {};
        }
        out.append(buf.constData(), static_cast<int>(ob.pos));
        if (ret == 0) {
            done = true;
            break;
        }
    }
    ZSTD_freeDStream(ds);
    return done ? out : QByteArray();
}

} // namespace imgcomp
