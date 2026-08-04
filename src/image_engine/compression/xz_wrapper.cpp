#include "xz_wrapper.h"
#include <limits>
#include <lzma.h>

namespace imgcomp {

QByteArray xzCompress(const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    size_t bound = lzma_stream_buffer_bound(static_cast<size_t>(data.size()));
    QByteArray out(static_cast<int>(bound), Qt::Uninitialized);
    size_t outPos = 0;
    lzma_ret r = lzma_easy_buffer_encode(6, LZMA_CHECK_CRC64, nullptr,
                                         reinterpret_cast<const uint8_t *>(data.constData()),
                                         static_cast<size_t>(data.size()),
                                         reinterpret_cast<uint8_t *>(out.data()), &outPos, bound);
    if (r != LZMA_OK)
        return {};
    out.truncate(static_cast<int>(outPos));
    return out;
}

QByteArray xzDecompress(const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    // 轻量 footer 结构预检（标准 xz 流尾部 12 字节，布局自流尾）：
    //   [CRC32(4)][Backward Size(4)][Stream Flags: 0x00+check(2)]["YZ"(2)]
    // 截断帧（footer 被切/缺失）在此直接拒绝，避免无谓的缓冲倍增。
    if (data.size() < 12)
        return {};
    const uchar *ft = reinterpret_cast<const uchar *>(data.constData()) + data.size() - 12;
    if (ft[8] != 0x00 || ft[9] > 0x0F || ft[10] != 0x59 || ft[11] != 0x5A)
        return {};
    // 输出缓冲从输入规模驱动起步（最低 64KB），LZMA_BUF_ERROR 时倍增重试。
    // 注意：本系统 lzma_stream_buffer_decode 失败时不更新 in_pos，因此
    // "输出不足"与"输入截断"无法靠 in_pos 区分 —— 截断由上方 footer 预检兜底。
    const size_t cap = static_cast<size_t>(std::numeric_limits<int>::max()); // ~2 GiB（QByteArray 上限）
    size_t outSize = static_cast<size_t>(data.size()) * 4;
    if (outSize < 64 * 1024)
        outSize = 64 * 1024;
    if (outSize > cap)
        outSize = cap;
    const size_t inSize = static_cast<size_t>(data.size());
    uint64_t memlimit = 1024ULL * 1024 * 1024; // 1 GiB 真实内存限额（非 UINT64_MAX）
    for (;;) {
        QByteArray out(static_cast<int>(outSize), Qt::Uninitialized);
        size_t inPos = 0, outPos = 0;
        lzma_ret r = lzma_stream_buffer_decode(&memlimit, 0, nullptr,
                                               reinterpret_cast<const uint8_t *>(data.constData()),
                                               &inPos, inSize,
                                               reinterpret_cast<uint8_t *>(out.data()), &outPos,
                                               outSize);
        if (r == LZMA_OK) {
            // 完整流必须恰好消费整个输入；未消费完 = 截断/尾随垃圾 → 失败
            if (inPos != inSize)
                return {};
            out.truncate(static_cast<int>(outPos));
            return out;
        }
        if (r == LZMA_BUF_ERROR) {
            if (inPos >= inSize || outSize >= cap)
                return {};
            outSize *= 2;
            if (outSize > cap)
                outSize = cap;
            continue;
        }
        return {};
    }
}

} // namespace imgcomp
