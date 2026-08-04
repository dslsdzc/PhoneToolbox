#include "sparse_image.h"
#include <QList>
#include <QtEndian>

namespace imgsparse {

namespace {
constexpr quint32 kSparseMagic = 0xED26FF3A;
constexpr quint16 kChunkRaw = 0xCAC1, kChunkFill = 0xCAC2, kChunkDontCare = 0xCAC3, kChunkCrc32 = 0xCAC4;
constexpr int kSparseHeaderSize = 28, kChunkHeaderSize = 12;

struct SparseHeader {
    quint32 magic, fileHdrSz, chunkHdrSz, blkSz, totalBlks, totalChunks;
};
struct ChunkHeader { quint16 type; quint32 chunkSz, totalSz; };

bool parseHeader(const QByteArray &d, SparseHeader &h)
{
    if (d.size() < kSparseHeaderSize) return false;
    h.magic = qFromLittleEndian<quint32>(d.constData());
    h.fileHdrSz = qFromLittleEndian<quint32>(d.constData() + 8);
    h.chunkHdrSz = qFromLittleEndian<quint32>(d.constData() + 12);
    h.blkSz = qFromLittleEndian<quint32>(d.constData() + 16);
    h.totalBlks = qFromLittleEndian<quint32>(d.constData() + 20);
    h.totalChunks = qFromLittleEndian<quint32>(d.constData() + 24);
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
            QByteArray block(static_cast<int>(h.blkSz), fillByte[0]);
            // 校验四个字节一致，不一致则逐块填充
            bool uniform = fillByte[0] == fillByte[1] && fillByte[1] == fillByte[2] && fillByte[2] == fillByte[3];
            for (quint64 b = 0; b < ch.chunkSz; ++b) {
                if (!uniform)
                    block = QByteArray(static_cast<int>(h.blkSz), fillByte[b % 4]);
                out.replace(static_cast<int>(written + b * h.blkSz), static_cast<int>(h.blkSz), block);
            }
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
            chunks.append({kChunkRaw, static_cast<quint32>(n), static_cast<quint32>(12 + bytes),
                           raw.mid(off, static_cast<int>(bytes))});
            i += n;
        }
    }
    put32(hdr, 0, kSparseMagic);
    hdr[4] = 1; hdr[5] = 0; hdr[6] = 0; hdr[7] = 0;
    put32(hdr, 8, 28); put32(hdr, 12, 12);
    put32(hdr, 16, blockSize);
    put32(hdr, 20, static_cast<quint32>(totalBlks));
    put32(hdr, 24, static_cast<quint32>(chunks.size()));
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
