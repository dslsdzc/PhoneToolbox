#include "brotli_wrapper.h"
#include <brotli/decode.h>
#include <brotli/encode.h>
#include <limits>

namespace imgcomp {

QByteArray brotliCompress(const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    size_t bound = BrotliEncoderMaxCompressedSize(static_cast<size_t>(data.size()));
    if (bound == 0)
        return {};
    QByteArray out(static_cast<int>(bound), Qt::Uninitialized);
    size_t outSize = bound;
    if (!BrotliEncoderCompress(5, BROTLI_DEFAULT_WINDOW, BROTLI_MODE_GENERIC,
                               static_cast<size_t>(data.size()),
                               reinterpret_cast<const uint8_t *>(data.constData()),
                               &outSize, reinterpret_cast<uint8_t *>(out.data())))
        return {};
    out.truncate(static_cast<int>(outSize));
    return out;
}

QByteArray brotliDecompress(const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    // 未知原始大小：倍增缓冲。截断输入返回 NEEDS_MORE_INPUT → 直接失败；
    // 仅 NEEDS_MORE_OUTPUT 时倍增重试。上限 INT_MAX（Qt5 QByteArray int 上限，
    // 与 xz/lz4 封装一致），且在任何分配前检查 —— 超限即失败，
    // 避免恶意流触发 >2GiB 分配或 int 溢出为负。
    const size_t maxCap = static_cast<size_t>(std::numeric_limits<int>::max());
    size_t cap = 64 * 1024;
    for (;;) {
        if (cap > maxCap)
            return {};
        QByteArray out(static_cast<int>(cap), Qt::Uninitialized);
        size_t outSize = cap;
        BrotliDecoderResult r = BrotliDecoderDecompress(static_cast<size_t>(data.size()),
                                                        reinterpret_cast<const uint8_t *>(data.constData()),
                                                        &outSize, reinterpret_cast<uint8_t *>(out.data()));
        if (r == BROTLI_DECODER_RESULT_SUCCESS) {
            out.truncate(static_cast<int>(outSize));
            return out;
        }
        if (r != BROTLI_DECODER_RESULT_NEEDS_MORE_OUTPUT)
            return {};
        cap = (cap > maxCap / 2) ? maxCap : cap * 2;
    }
}

} // namespace imgcomp
