#include "sparse_image.h"
#include <QList>
#include <QtEndian>

namespace imgsparse {

namespace {
constexpr quint32 kSparseMagic = 0xED26FF3A;
constexpr quint16 kChunkRaw = 0xCAC1, kChunkFill = 0xCAC2, kChunkDontCare = 0xCAC3, kChunkCrc32 = 0xCAC4;
constexpr int kSparseHeaderSize = 28, kChunkHeaderSize = 12;

struct SparseHeader {
    quint32 magic, blkSz, totalBlks, totalChunks;
    quint16 fileHdrSz, chunkHdrSz;
};
struct ChunkHeader { quint16 type; quint32 chunkSz, totalSz; };

bool parseHeader(const QByteArray &d, SparseHeader &h)
{
    if (d.size() < kSparseHeaderSize) return false;
    // AOSP sparse_format.h: magic(u32@0) major(u16@4) minor(u16@6)
    // file_hdr_sz(u16@8) chunk_hdr_sz(u16@10) blk_sz(u32@12)
    // total_blks(u32@16) total_chunks(u32@20) image_checksum(u32@24)
    h.magic = qFromLittleEndian<quint32>(d.constData());
    h.fileHdrSz = qFromLittleEndian<quint16>(d.constData() + 8);
    h.chunkHdrSz = qFromLittleEndian<quint16>(d.constData() + 10);
    h.blkSz = qFromLittleEndian<quint32>(d.constData() + 12);
    h.totalBlks = qFromLittleEndian<quint32>(d.constData() + 16);
    h.totalChunks = qFromLittleEndian<quint32>(d.constData() + 20);
    return h.magic == kSparseMagic && h.blkSz > 0 && h.chunkHdrSz >= 12;
}
} // namespace

bool isSparse(const QByteArray &header)
{
    return header.size() >= 4 && qFromLittleEndian<quint32>(header.constData()) == kSparseMagic;
}

QByteArray simg2img(const QByteArray &sparse)
{
    SparseHeader h;
    if (!parseHeader(sparse, h)) return {};
    QByteArray out(static_cast<int>(static_cast<quint64>(h.totalBlks) * h.blkSz), Qt::Uninitialized);
    out.fill('\0'); // DONTCARE 块依赖零填充（Qt::Uninitialized 不会清零）
    qint64 pos = h.fileHdrSz;
    quint64 written = 0;
    for (quint32 i = 0; i < h.totalChunks; ++i) {
        if (pos + h.chunkHdrSz > sparse.size()) return {};
        ChunkHeader ch;
        ch.type = qFromLittleEndian<quint16>(sparse.constData() + pos);
        ch.chunkSz = qFromLittleEndian<quint32>(sparse.constData() + pos + 4);
        ch.totalSz = qFromLittleEndian<quint32>(sparse.constData() + pos + 8);
        const quint64 chunkBytes = static_cast<quint64>(ch.chunkSz) * h.blkSz;
        if (written + chunkBytes > static_cast<quint64>(out.size())) return {};
        if (ch.totalSz < h.chunkHdrSz) return {}; // 非法 total_sz，防止 pos 不推进导致自旋
        switch (ch.type) {
        case kChunkRaw: {
            const qint64 dataSz = ch.totalSz - h.chunkHdrSz;
            if (dataSz < 0 || dataSz != static_cast<qint64>(chunkBytes)) return {};
            if (pos + h.chunkHdrSz + dataSz > sparse.size()) return {};
            out.replace(static_cast<int>(written), static_cast<int>(chunkBytes),
                        sparse.mid(pos + h.chunkHdrSz, static_cast<int>(dataSz)));
            break;
        }
        case kChunkFill: {
            const qint64 dataSz = ch.totalSz - h.chunkHdrSz;
            if (dataSz != 4) return {};
            const QByteArray fillByte = sparse.mid(pos + h.chunkHdrSz, 4);
            if (fillByte.size() != 4) return {};
            // 4 字节 pattern 按 pattern[i % 4] 重复填充整块（Android libsparse 语义）；
            // 每块内容相同，可复用同一 block
            QByteArray block(static_cast<int>(h.blkSz), Qt::Uninitialized);
            for (int j = 0; j < block.size(); ++j)
                block[j] = fillByte[j % 4];
            for (quint64 b = 0; b < ch.chunkSz; ++b)
                out.replace(static_cast<int>(written + b * h.blkSz), static_cast<int>(h.blkSz), block);
            break;
        }
        case kChunkDontCare:
            // 保持零（out 已零初始化）；0x00 填充已在初始化完成
            break;
        case kChunkCrc32:
            return {}; // Android 实际不用，遇 CRC 视为不支持
        default:
            return {};
        }
        written += chunkBytes;
        pos += ch.totalSz;
    }
    return written == static_cast<quint64>(out.size()) ? out : QByteArray();
}

QByteArray img2simg(const QByteArray &raw, quint32 blockSize)
{
    if (raw.isEmpty() || blockSize == 0) return {};
    // 连续全同块 → FILL，其余 → RAW
    const quint64 totalBlks = (static_cast<quint64>(raw.size()) + blockSize - 1) / blockSize;
    QByteArray out;
    out.reserve(static_cast<int>(raw.size() + 32 + totalBlks * 16));
    QByteArray hdr(28, Qt::Uninitialized);
    auto put16 = [&](QByteArray &d, int off, quint16 v) {
        d[off] = char(v); d[off + 1] = char(v >> 8);
    };
    auto put32 = [&](QByteArray &d, int off, quint32 v) {
        d[off] = char(v); d[off + 1] = char(v >> 8); d[off + 2] = char(v >> 16); d[off + 3] = char(v >> 24);
    };
    // 先收集 chunk 描述，再写头
    struct C { quint16 type; quint32 blocks; quint32 total; QByteArray data; };
    QList<C> chunks;
    quint64 i = 0;
    while (i < totalBlks) {
        const int off = static_cast<int>(i * blockSize);
        const int len = qMin(static_cast<int>(blockSize), raw.size() - off);
        const QByteArray block = raw.mid(off, len);
        if (block.size() == static_cast<int>(blockSize) && block == QByteArray(blockSize, block[0])) {
            // 合并连续 FILL 块
            quint32 n = 1;
            while (i + n < totalBlks && raw.mid(static_cast<int>((i + n) * blockSize), blockSize) == block)
                ++n;
            chunks.append({kChunkFill, n, 12 + 4, QByteArray(4, block[0])});
            i += n;
        } else {
            // RAW 块（最后一块可能不足块大小）
            quint64 n = 1;
            quint64 bytes = len;
            while (i + n < totalBlks) {
                const int no = static_cast<int>((i + n) * blockSize);
                const int nl = qMin(static_cast<int>(blockSize), raw.size() - no);
                QByteArray nb = raw.mid(no, nl);
                if (nb.size() == static_cast<int>(blockSize) && nb == QByteArray(blockSize, nb[0]))
                    break; // 下一块是 FILL
                bytes += nl;
                ++n;
            }
            QByteArray data = raw.mid(off, static_cast<int>(bytes));
            if (static_cast<quint64>(data.size()) < n * blockSize) // 末块不足块大小：补零 pad 到整块
                data.append(QByteArray(static_cast<int>(n * blockSize - static_cast<quint64>(data.size())), '\0'));
            chunks.append({kChunkRaw, static_cast<quint32>(n), static_cast<quint32>(12 + data.size()), data});
            i += n;
        }
    }
    put32(hdr, 0, kSparseMagic);
    hdr[4] = 1; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;
    // AOSP 布局: file_hdr_sz/chunk_hdr_sz 为 u16（偏移 8/10），blk_sz 等从偏移 12 起
    put16(hdr, 8, 28); put16(hdr, 10, 12);
    put32(hdr, 12, blockSize);
    put32(hdr, 16, static_cast<quint32>(totalBlks));
    put32(hdr, 20, static_cast<quint32>(chunks.size()));
    out.append(hdr);
    for (const C &c : chunks) {
        QByteArray ch(12, Qt::Uninitialized);
        ch[0] = char(c.type); ch[1] = char(c.type >> 8);
        ch[2] = 0; ch[3] = 0;
        put32(ch, 4, c.blocks); put32(ch, 8, c.total);
        out.append(ch).append(c.data);
    }
    return out;
}

} // namespace imgsparse
