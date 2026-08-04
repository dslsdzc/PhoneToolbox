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
    // ~INT_MAX 输入时 bound 可能超 int 上限，static_cast<int> 溢出为负 → qBadAlloc 崩溃
    if (bound == 0 || bound > static_cast<size_t>(std::numeric_limits<int>::max()))
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
    // 不能用一次性 BrotliDecoderDecompress + 倍增缓冲：brotli 1.2.0 的一次性
    // 封装在输出缓冲不足时返回 ERROR 而非 NEEDS_MORE_OUTPUT（实测 64K/1M/4M
    // 缓冲解 5MB 流均 r=0），倍增循环对 >64K 输出完全无效。改用流式 API
    // （与 zstd 流式路径一致）：增量输出、总量封顶 INT_MAX、截断（输入耗尽
    // 未达流尾 → NEEDS_MORE_INPUT）→ 失败。无倍增循环，死循环问题不复存在。
    BrotliDecoderState *st = BrotliDecoderCreateInstance(nullptr, nullptr, nullptr);
    if (!st)
        return {};
    const size_t maxOut = static_cast<size_t>(std::numeric_limits<int>::max());
    size_t availIn = static_cast<size_t>(data.size());
    const uint8_t *nextIn = reinterpret_cast<const uint8_t *>(data.constData());
    QByteArray out;
    QByteArray chunk(64 * 1024, Qt::Uninitialized);
    bool done = false;
    for (;;) {
        size_t availOut = static_cast<size_t>(chunk.size());
        uint8_t *nextOut = reinterpret_cast<uint8_t *>(chunk.data());
        BrotliDecoderResult r = BrotliDecoderDecompressStream(st, &availIn, &nextIn,
                                                              &availOut, &nextOut, nullptr);
        const int produced = static_cast<int>(chunk.size() - availOut);
        if (produced > 0) {
            out.append(chunk.constData(), produced);
            if (static_cast<size_t>(out.size()) > maxOut) {
                BrotliDecoderDestroyInstance(st);
                return {};
            }
        }
        if (r == BROTLI_DECODER_RESULT_SUCCESS) {
            done = true;
            break;
        }
        if (r == BROTLI_DECODER_RESULT_ERROR) {
            BrotliDecoderDestroyInstance(st);
            return {};
        }
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && availIn == 0) {
            // 输入耗尽但未到流尾 → 截断
            BrotliDecoderDestroyInstance(st);
            return {};
        }
        // NEEDS_MORE_OUTPUT（缓冲满，继续下一块）或尚余输入 → 继续
    }
    BrotliDecoderDestroyInstance(st);
    return done ? out : QByteArray();
}

} // namespace imgcomp
