#include "bspatch_image.h"
#include <bzlib.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace imgbspatch {

namespace {

quint64 readU64(const QByteArray &d, int off)
{
    quint64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= static_cast<quint64>(static_cast<uchar>(d[off + i])) << (i * 8);
    return v;
}

// qint64 检查加法：溢出返回 false（防回绕 UB）
bool addChecked(qint64 a, qint64 b, qint64 &sum)
{
    if (b > 0 && a > std::numeric_limits<qint64>::max() - b)
        return false;
    if (b < 0 && a < std::numeric_limits<qint64>::min() - b)
        return false;
    sum = a + b;
    return true;
}

// bzip2 流式解压器（单次读完输入）。read 逐块解压，多余输出缓存到 cache，
// 直到凑满 n 字节或流结束（BZ_STREAM_END）；返回实际读到的字节数。
// 调用方把 got < 请求数视为失败（流截断/解压错误）。
struct BzStream {
    bz_stream s{};
    QByteArray input;
    QByteArray cache;   // 已解压、尚未取走的字节
    size_t cachePos = 0;
    bool ended = false;

    bool init(const QByteArray &compressed)
    {
        input = compressed;
        s.next_in = const_cast<char *>(input.constData());
        s.avail_in = static_cast<unsigned>(input.size());
        return BZ2_bzDecompressInit(&s, 0, 0) == BZ_OK;
    }

    size_t read(char *out, size_t n)
    {
        size_t got = 0;
        while (got < n) {
            // 1) 先取走缓存
            if (cachePos < static_cast<size_t>(cache.size())) {
                const size_t take = std::min<size_t>(n - got,
                                                     static_cast<size_t>(cache.size()) - cachePos);
                memcpy(out + got, cache.constData() + cachePos, take);
                cachePos += take;
                got += take;
                continue;
            }
            if (ended)
                break;
            // 2) 解压下一块到临时缓冲，多余字节进缓存
            char buf[8192];
            s.next_out = buf;
            s.avail_out = sizeof(buf);
            const int r = BZ2_bzDecompress(&s);
            if (r != BZ_OK && r != BZ_STREAM_END)
                return got; // 解压错误
            const size_t produced = sizeof(buf) - s.avail_out;
            cache = QByteArray(buf, static_cast<int>(produced));
            cachePos = 0;
            if (r == BZ_STREAM_END) {
                ended = true;
                continue; // 先把本次缓存取走，取完后由 ended 分支退出
            }
            if (produced == 0)
                return got; // 无进展（输入耗尽 → 截断）
        }
        return got;
    }

    void finish() { BZ2_bzDecompressEnd(&s); }
};

} // namespace

QByteArray applyBsdiff(const QByteArray &oldData, const QByteArray &patch, bool *ok)
{
    if (ok) *ok = false;
    if (patch.size() < 32 || patch.left(8) != "BSDIFF40")
        return {};
    const quint64 ctrlLen = readU64(patch, 8);
    const quint64 diffLen = readU64(patch, 16);
    const quint64 newLen = readU64(patch, 24);
    if (ctrlLen > static_cast<quint64>(patch.size()) - 32
        || diffLen > static_cast<quint64>(patch.size()) - 32 - ctrlLen)
        return {};
    if (newLen > static_cast<quint64>(std::numeric_limits<int>::max()))
        return {};
    const int extraStart = static_cast<int>(32 + ctrlLen + diffLen);

    BzStream ctrl, diff, extra;
    if (!ctrl.init(patch.mid(32, static_cast<int>(ctrlLen))))
        return {};
    if (!diff.init(patch.mid(32 + static_cast<int>(ctrlLen), static_cast<int>(diffLen)))) {
        ctrl.finish();
        return {};
    }
    if (!extra.init(patch.mid(extraStart))) {
        ctrl.finish();
        diff.finish();
        return {};
    }

    QByteArray out(static_cast<int>(newLen), Qt::Uninitialized);
    quint64 newPos = 0;
    qint64 oldPos = 0;
    QByteArray ctrlBuf(24, Qt::Uninitialized);
    while (newPos < newLen) {
        // ctrl 三元组 (diff_len, extra_len, old_offset)，各 8B LE
        if (ctrl.read(ctrlBuf.data(), 24) != 24) {
            ctrl.finish(); diff.finish(); extra.finish();
            return {};
        }
        const qint64 diffLenThis = static_cast<qint64>(readU64(ctrlBuf, 0));
        const qint64 extraLen = static_cast<qint64>(readU64(ctrlBuf, 8));
        const qint64 oldOffset = static_cast<qint64>(readU64(ctrlBuf, 16));
        // 边界（对照 FreeBSD bspatch.c）:
        // 1) 各段长度先按值限制在 newLen 内（newLen ≤ INT_MAX → 天然排除 INT 级回绕/负尺寸分配）；
        // 2) 再分段检查 newPos 推进不越界（此时 diffLenThis/extraLen ≤ newLen、newPos ≤ newLen，
        //    和 ≤ 3*newLen 无回绕）。
        if (diffLenThis < 0 || extraLen < 0
            || diffLenThis > newLen || extraLen > newLen
            || newPos + static_cast<quint64>(diffLenThis) > newLen
            || newPos + static_cast<quint64>(diffLenThis) + static_cast<quint64>(extraLen) > newLen) {
            ctrl.finish(); diff.finish(); extra.finish();
            return {};
        }
        // oldPos 不符号检查（真实 bsdiff 允许负 old_offset；越界由 diff 循环 0 填充守卫兜底），
        // 但加减用检查算术防 qint64 回绕 UB。diff 段读取用推进前的 oldPos（bspatch.c 语义）。
        QByteArray diffBuf(static_cast<int>(diffLenThis), Qt::Uninitialized);
        if (diff.read(diffBuf.data(), static_cast<size_t>(diffLenThis)) != static_cast<size_t>(diffLenThis)) {
            ctrl.finish(); diff.finish(); extra.finish();
            return {};
        }
        for (qint64 i = 0; i < diffLenThis; ++i) {
            char oldc = 0;
            qint64 idx = 0;
            if (addChecked(oldPos, i, idx)
                && idx >= 0 && idx < static_cast<qint64>(oldData.size()))
                oldc = oldData[static_cast<int>(idx)];
            out[static_cast<int>(newPos + i)] =
                char((static_cast<uchar>(oldc) + static_cast<uchar>(diffBuf[static_cast<int>(i)])) & 0xFF);
        }
        qint64 nextOldPos;
        if (!addChecked(oldPos, diffLenThis, nextOldPos)) {
            ctrl.finish(); diff.finish(); extra.finish();
            return {};
        }
        oldPos = nextOldPos;
        newPos += static_cast<quint64>(diffLenThis);
        // extra 段: 直接复制
        QByteArray extraBuf(static_cast<int>(extraLen), Qt::Uninitialized);
        if (extra.read(extraBuf.data(), static_cast<size_t>(extraLen)) != static_cast<size_t>(extraLen)) {
            ctrl.finish(); diff.finish(); extra.finish();
            return {};
        }
        out.replace(static_cast<int>(newPos), static_cast<int>(extraLen), extraBuf);
        newPos += static_cast<quint64>(extraLen);
        if (!addChecked(oldPos, oldOffset, nextOldPos)) {
            ctrl.finish(); diff.finish(); extra.finish();
            return {};
        }
        oldPos = nextOldPos;
    }
    ctrl.finish(); diff.finish(); extra.finish();
    if (ok) *ok = true;
    return out;
}

QByteArray applyPuffdiff(const QByteArray &oldData, const QByteArray &patch)
{
    // Task 15 骨架: 仅校验魔数。完整 puffin 解压（PUFFDIFF 头 + gzip 流）留 Task 16。
    Q_UNUSED(oldData);
    if (patch.size() < 8 || patch.left(8) != "PUFFDIFF")
        return {};
    return {};
}

} // namespace imgbspatch
