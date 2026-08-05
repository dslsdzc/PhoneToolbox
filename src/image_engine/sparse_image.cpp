#include "sparse_image.h"
#include <QBuffer>
#include <QFile>
#include <QtEndian>
#include <cstring>
#include <limits>

namespace imgsparse {

namespace {
constexpr quint32 kSparseMagic = 0xED26FF3A;
constexpr quint16 kChunkRaw = 0xCAC1, kChunkFill = 0xCAC2, kChunkDontCare = 0xCAC3, kChunkCrc32 = 0xCAC4;
constexpr int kSparseHeaderSize = 28, kChunkHeaderSize = 12;
// blk_sz / blockSize 防护上限（AOSP 实际恒为 4096；防止恶意头触发 GB 级分配）
constexpr qint64 kMaxBlockSize = 64 * 1024 * 1024;

// AOSP sparse_format.h 头布局：magic(u32@0) major(u16@4) minor(u16@6)
// file_hdr_sz(u16@8) chunk_hdr_sz(u16@10) blk_sz(u32@12)
// total_blks(u32@16) total_chunks(u32@20) image_checksum(u32@24)
struct SparseHeader {
    quint32 magic = 0, blkSz = 0, totalBlks = 0, totalChunks = 0;
    quint16 fileHdrSz = 0, chunkHdrSz = 0;
};

void setErr(QString *error, const QString &msg)
{
    if (error) *error = msg;
}

bool readExact(QIODevice *d, char *buf, qint64 len)
{
    qint64 got = 0;
    while (got < len) {
        const qint64 n = d->read(buf + got, len - got);
        if (n <= 0) return false;
        got += n;
    }
    return true;
}

bool writeAll(QIODevice *d, const char *buf, qint64 len)
{
    qint64 done = 0;
    while (done < len) {
        const qint64 n = d->write(buf + done, len - done);
        if (n <= 0) return false;
        done += n;
    }
    return true;
}

bool parseSparseHeader(QIODevice *in, SparseHeader &h, QString *error)
{
    QByteArray d = in->read(kSparseHeaderSize);
    if (d.size() < kSparseHeaderSize) {
        setErr(error, QStringLiteral("sparse 文件过小，无法读取 %1 字节头").arg(kSparseHeaderSize));
        return false;
    }
    h.magic = qFromLittleEndian<quint32>(d.constData());
    const quint16 majorVer = qFromLittleEndian<quint16>(d.constData() + 4);
    h.fileHdrSz = qFromLittleEndian<quint16>(d.constData() + 8);
    h.chunkHdrSz = qFromLittleEndian<quint16>(d.constData() + 10);
    h.blkSz = qFromLittleEndian<quint32>(d.constData() + 12);
    h.totalBlks = qFromLittleEndian<quint32>(d.constData() + 16);
    h.totalChunks = qFromLittleEndian<quint32>(d.constData() + 20);
    if (h.magic != kSparseMagic) { setErr(error, QStringLiteral("不是 sparse 镜像（magic 不匹配）")); return false; }
    if (majorVer > 1) { setErr(error, QStringLiteral("sparse 主版本 %1 不受支持").arg(majorVer)); return false; }
    if (h.blkSz == 0 || h.blkSz > kMaxBlockSize) { setErr(error, QStringLiteral("非法 block size %1").arg(h.blkSz)); return false; }
    if (h.fileHdrSz < kSparseHeaderSize || h.chunkHdrSz < kChunkHeaderSize) {
        setErr(error, QStringLiteral("非法头字段 file_hdr_sz=%1 chunk_hdr_sz=%2").arg(h.fileHdrSz).arg(h.chunkHdrSz));
        return false;
    }
    return true;
}

// totalBlks * blkSz（u32*u32 ≤ 2^64，需检查 qint64 溢出）；失败返回 -1 并置 error
qint64 sparseTotalSize(const SparseHeader &h, QString *error)
{
    const quint64 total = static_cast<quint64>(h.totalBlks) * h.blkSz;
    if (total > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        setErr(error, QStringLiteral("镜像大小 %1 超出支持范围").arg(total));
        return -1;
    }
    return static_cast<qint64>(total);
}

// 核心：按 chunk 流式读入写盘（内存 O(chunk)）。输出设备须已预置为 totalBlks*blkSz 大小
// （DONTCARE 块只前移写位置，空洞读回为 0）。进度 = 已消费的 sparse 输入字节。
bool simg2imgChunks(QIODevice *in, QIODevice *out, const SparseHeader &h,
                    const std::function<void(quint64)> &progress, QString *error)
{
    const qint64 totalSize = sparseTotalSize(h, error);
    if (totalSize < 0) return false;
    const qint64 inSize = in->size();
    if (inSize < 0) { setErr(error, QStringLiteral("输入设备大小未知")); return false; }
    qint64 pos = h.fileHdrSz;
    qint64 written = 0;
    QByteArray dataBuf(1024 * 1024, Qt::Uninitialized); // RAW 数据复用读缓冲
    QByteArray fillBuf;                                 // FILL 块复用缓冲
    QByteArray chBuf(h.chunkHdrSz, Qt::Uninitialized);  // chunk 头缓冲
    char pat[4];
    if (progress) progress(0);
    for (quint32 i = 0; i < h.totalChunks; ++i) {
        if (pos + h.chunkHdrSz > inSize) { setErr(error, QStringLiteral("sparse 文件被截断（chunk 头越界）")); return false; }
        if (!in->seek(pos) || !readExact(in, chBuf.data(), h.chunkHdrSz)) {
            setErr(error, QStringLiteral("sparse 文件被截断（chunk 头不完整）"));
            return false;
        }
        const quint16 type = qFromLittleEndian<quint16>(chBuf.constData());
        const quint32 chunkSz = qFromLittleEndian<quint32>(chBuf.constData() + 4);
        const quint32 totalSz = qFromLittleEndian<quint32>(chBuf.constData() + 8);
        const qint64 chunkBytes = static_cast<qint64>(chunkSz) * h.blkSz;
        if (written + chunkBytes > totalSize) { setErr(error, QStringLiteral("chunk 超出镜像声明大小")); return false; }
        if (totalSz < h.chunkHdrSz) { setErr(error, QStringLiteral("非法 chunk total_sz")); return false; }
        const qint64 dataSz = totalSz - h.chunkHdrSz;
        const qint64 dataPos = pos + h.chunkHdrSz;
        switch (type) {
        case kChunkRaw: {
            if (dataSz != chunkBytes) { setErr(error, QStringLiteral("RAW chunk 数据长度与块数不匹配")); return false; }
            if (dataPos + dataSz > inSize) { setErr(error, QStringLiteral("sparse 文件被截断（RAW 数据越界）")); return false; }
            if (!in->seek(dataPos)) { setErr(error, QStringLiteral("读取 sparse 失败")); return false; }
            qint64 remaining = dataSz;
            while (remaining > 0) {
                const qint64 n = in->read(dataBuf.data(), qMin(remaining, static_cast<qint64>(dataBuf.size())));
                if (n <= 0) { setErr(error, QStringLiteral("sparse 文件被截断（RAW 数据不足）")); return false; }
                if (!writeAll(out, dataBuf.constData(), n)) { setErr(error, QStringLiteral("写输出失败")); return false; }
                remaining -= n;
            }
            break;
        }
        case kChunkFill: {
            if (dataSz != 4) { setErr(error, QStringLiteral("FILL chunk 数据长度必须为 4")); return false; }
            if (dataPos + 4 > inSize) { setErr(error, QStringLiteral("sparse 文件被截断（FILL 数据越界）")); return false; }
            if (!in->seek(dataPos) || !readExact(in, pat, 4)) {
                setErr(error, QStringLiteral("sparse 文件被截断（FILL 数据不足）"));
                return false;
            }
            // 4 字节 pattern 按 pattern[i % 4] 重复填充整块（Android libsparse 语义）
            fillBuf.resize(static_cast<int>(h.blkSz));
            for (int j = 0; j < fillBuf.size(); ++j)
                fillBuf[j] = pat[j % 4];
            for (quint32 b = 0; b < chunkSz; ++b)
                if (!writeAll(out, fillBuf.constData(), fillBuf.size())) { setErr(error, QStringLiteral("写输出失败")); return false; }
            break;
        }
        case kChunkDontCare:
            // 输出设备已预置为 totalSize，直接前移写位置（空洞读回为 0，与旧接口零填充逐字节一致）
            if (!out->seek(out->pos() + chunkBytes)) { setErr(error, QStringLiteral("DONTCARE 块写位置移动失败")); return false; }
            break;
        case kChunkCrc32:
            setErr(error, QStringLiteral("sparse 含 CRC32 chunk，不支持"));
            return false;
        default:
            setErr(error, QStringLiteral("不支持的 chunk 类型 0x%1").arg(type, 4, 16, QLatin1Char('0')));
            return false;
        }
        written += chunkBytes;
        pos += totalSz;
        if (progress) progress(static_cast<quint64>(qMin(pos, inSize)));
    }
    if (written != totalSize) { setErr(error, QStringLiteral("chunk 总量与头声明不一致")); return false; }
    if (progress) progress(static_cast<quint64>(inSize));
    return true;
}

// 核心：按块流式分析 FILL/RAW 并写出（内存 O(blockSize)，输入顺序单遍 + 至多一块回看）。
// chunk 边界与旧接口 img2simg 完全一致（FILL = 连续全同整块；RAW = 其余连续块，
// 末块不足时补零 pad 到整块）。进度 = 已消费的输入字节。
bool img2simgIo(QIODevice *in, QIODevice *out, quint32 blockSize,
                const std::function<void(quint64)> &progress, QString *error)
{
    if (blockSize == 0 || blockSize > kMaxBlockSize) {
        setErr(error, QStringLiteral("非法 block size %1").arg(blockSize));
        return false;
    }
    const qint64 fileSize = in->size();
    if (fileSize <= 0) { setErr(error, QStringLiteral("输入为空")); return false; }
    const quint64 totalBlksU = (static_cast<quint64>(fileSize) + blockSize - 1) / blockSize;
    if (totalBlksU > std::numeric_limits<quint32>::max()) { setErr(error, QStringLiteral("镜像过大")); return false; }
    const quint32 totalBlks = static_cast<quint32>(totalBlksU);

    auto put16 = [](QByteArray &d, int off, quint16 v) { d[off] = char(v); d[off + 1] = char(v >> 8); };
    auto put32 = [](QByteArray &d, int off, quint32 v) {
        d[off] = char(v); d[off + 1] = char(v >> 8); d[off + 2] = char(v >> 16); d[off + 3] = char(v >> 24);
    };
    auto writeSparseHeader = [&](quint32 chunkCount) -> bool {
        QByteArray hdr(kSparseHeaderSize, '\0');
        put32(hdr, 0, kSparseMagic);
        put16(hdr, 4, 1); put16(hdr, 6, 0);
        // AOSP 布局：file_hdr_sz/chunk_hdr_sz 为 u16（偏移 8/10），blk_sz 等从偏移 12 起
        put16(hdr, 8, 28); put16(hdr, 10, 12);
        put32(hdr, 12, blockSize);
        put32(hdr, 16, totalBlks);
        put32(hdr, 20, chunkCount);
        return writeAll(out, hdr.constData(), hdr.size());
    };
    auto writeChunkHeader = [&](qint64 at, quint16 type, quint32 blocks, quint32 totalSz) -> bool {
        QByteArray ch(kChunkHeaderSize, '\0');
        put16(ch, 0, type);
        put32(ch, 4, blocks);
        put32(ch, 8, totalSz);
        if (!out->seek(at)) return false;
        return writeAll(out, ch.constData(), ch.size());
    };
    auto allEqual = [](const QByteArray &b, int count, char pat) -> bool {
        const char *p = b.constData();
        for (int i = 0; i < count; ++i)
            if (p[i] != pat) return false;
        return true;
    };

    QByteArray block(static_cast<int>(blockSize), Qt::Uninitialized);
    QByteArray probe(static_cast<int>(blockSize), Qt::Uninitialized);
    QByteArray rawPad(static_cast<int>(blockSize), '\0');
    QByteArray ph(kSparseHeaderSize, '\0');     // 占位头（末尾回填真实头）
    QByteArray chunkPh(kChunkHeaderSize, '\0'); // 占位 chunk 头（run 结束回填）

    auto readBlockAt = [&](qint64 off, QByteArray &buf, qint64 &len) -> bool {
        len = qMin<qint64>(blockSize, fileSize - off);
        if (!in->seek(off)) return false;
        qint64 got = 0;
        while (got < len) {
            const qint64 n = in->read(buf.data() + got, len - got);
            if (n <= 0) return false;
            got += n;
        }
        return true;
    };

    if (!writeAll(out, ph.constData(), ph.size())) { setErr(error, QStringLiteral("写输出失败")); return false; }

    quint64 inPos = 0; // 已消费输入字节
    quint32 chunkCount = 0;
    if (progress) progress(0);

    while (inPos < static_cast<quint64>(fileSize)) {
        const qint64 off = static_cast<qint64>(inPos);
        qint64 len = 0;
        if (!readBlockAt(off, block, len)) { setErr(error, QStringLiteral("读输入失败")); return false; }

        if (len == static_cast<qint64>(blockSize) && allEqual(block, static_cast<int>(len), block[0])) {
            // FILL run：连续全同整块
            const char pat = block[0];
            quint64 n = 1;
            while (inPos + n * blockSize < static_cast<quint64>(fileSize)) {
                const qint64 po = static_cast<qint64>(inPos + n * blockSize);
                qint64 plen = 0;
                if (!readBlockAt(po, probe, plen)) { setErr(error, QStringLiteral("读输入失败")); return false; }
                if (plen != static_cast<qint64>(blockSize)) break; // 末块不足整块：run 终止（与旧接口一致）
                if (!allEqual(probe, static_cast<int>(plen), pat)) break;
                ++n;
            }
            if (n > std::numeric_limits<quint32>::max()) { setErr(error, QStringLiteral("镜像过大")); return false; }
            const qint64 headerAt = out->pos();
            if (!writeAll(out, chunkPh.constData(), chunkPh.size())) { setErr(error, QStringLiteral("写输出失败")); return false; }
            if (!writeChunkHeader(headerAt, kChunkFill, static_cast<quint32>(n), 16)) { setErr(error, QStringLiteral("写输出失败")); return false; }
            const QByteArray pat4(4, pat);
            if (!writeAll(out, pat4.constData(), pat4.size())) { setErr(error, QStringLiteral("写输出失败")); return false; }
            inPos += n * blockSize;
            ++chunkCount;
            if (progress) progress(inPos);
            continue;
        }

        // RAW run：连续非全同块（末块不足整块时补零 pad，与旧接口一致）
        const qint64 headerAt = out->pos();
        if (!writeAll(out, chunkPh.constData(), chunkPh.size())) { setErr(error, QStringLiteral("写输出失败")); return false; }
        quint64 n = 0;
        qint64 scanOff = off;
        qint64 scanLen = len;
        bool termByFill = false;
        while (true) {
            if (scanLen == static_cast<qint64>(blockSize) && allEqual(block, static_cast<int>(scanLen), block[0])) {
                termByFill = true;
                break; // 终止块（全同整块）不写入本 chunk，留给下一轮
            }
            if (scanLen == static_cast<qint64>(blockSize)) {
                if (!writeAll(out, block.constData(), scanLen)) { setErr(error, QStringLiteral("写输出失败")); return false; }
            } else {
                // 末块不足整块：补零 pad
                std::memcpy(rawPad.data(), block.constData(), static_cast<size_t>(scanLen));
                if (!writeAll(out, rawPad.constData(), rawPad.size())) { setErr(error, QStringLiteral("写输出失败")); return false; }
            }
            ++n;
            if (scanLen < static_cast<qint64>(blockSize)) break; // 末块，EOF
            const qint64 nextOff = scanOff + blockSize;
            if (nextOff >= fileSize) break;                     // 恰好整文件读完
            if (!readBlockAt(nextOff, block, scanLen)) { setErr(error, QStringLiteral("读输入失败")); return false; }
            scanOff = nextOff;
        }
        if (n == 0) { setErr(error, QStringLiteral("内部错误：RAW run 为空")); return false; }
        if (n > std::numeric_limits<quint32>::max()) { setErr(error, QStringLiteral("镜像过大")); return false; }
        const quint64 padded = n * blockSize;
        if (padded + kChunkHeaderSize > std::numeric_limits<quint32>::max()) { setErr(error, QStringLiteral("镜像过大")); return false; }
        if (!writeChunkHeader(headerAt, kChunkRaw, static_cast<quint32>(n),
                              static_cast<quint32>(kChunkHeaderSize + padded))) {
            setErr(error, QStringLiteral("写输出失败"));
            return false;
        }
        if (!out->seek(headerAt + kChunkHeaderSize + static_cast<qint64>(padded))) { setErr(error, QStringLiteral("写输出失败")); return false; }
        inPos = termByFill ? static_cast<quint64>(scanOff) : static_cast<quint64>(fileSize);
        ++chunkCount;
        if (progress) progress(inPos);
    }

    if (!out->seek(0) || !writeSparseHeader(chunkCount)) { setErr(error, QStringLiteral("写输出失败")); return false; }
    if (progress) progress(static_cast<quint64>(fileSize));
    return true;
}
} // namespace

bool isSparse(const QByteArray &header)
{
    return header.size() >= 4 && qFromLittleEndian<quint32>(header.constData()) == kSparseMagic;
}

QByteArray simg2img(const QByteArray &sparse)
{
    if (sparse.size() < kSparseHeaderSize) return {};
    QString err;
    SparseHeader h;
    QBuffer inBuf(const_cast<QByteArray *>(&sparse));
    if (!inBuf.open(QIODevice::ReadOnly)) return {};
    if (!parseSparseHeader(&inBuf, h, &err)) return {};
    const qint64 totalSize = sparseTotalSize(h, &err);
    if (totalSize < 0) return {};
    if (totalSize > static_cast<qint64>(std::numeric_limits<qsizetype>::max())) return {};
    QByteArray out(static_cast<qsizetype>(totalSize), '\0'); // DONTCARE 区域依赖零填充
    QBuffer outBuf(&out);
    if (!outBuf.open(QIODevice::WriteOnly)) return {};
    if (!simg2imgChunks(&inBuf, &outBuf, h, {}, &err)) return {};
    return out;
}

QByteArray img2simg(const QByteArray &raw, quint32 blockSize)
{
    if (raw.isEmpty() || blockSize == 0) return {};
    QString err;
    QBuffer inBuf(const_cast<QByteArray *>(&raw));
    if (!inBuf.open(QIODevice::ReadOnly)) return {};
    QByteArray out;
    QBuffer outBuf(&out);
    if (!outBuf.open(QIODevice::WriteOnly)) return {};
    if (!img2simgIo(&inBuf, &outBuf, blockSize, {}, &err)) return {};
    return out;
}

bool simg2imgStream(const QString &inPath, const QString &outPath,
                    const std::function<void(quint64)> &progress, QString *error)
{
    QFile in(inPath);
    if (!in.open(QIODevice::ReadOnly)) {
        setErr(error, QStringLiteral("无法打开输入文件 %1: %2").arg(inPath, in.errorString()));
        return false;
    }
    SparseHeader h;
    if (!parseSparseHeader(&in, h, error)) return false;
    const qint64 totalSize = sparseTotalSize(h, error);
    if (totalSize < 0) return false;

    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setErr(error, QStringLiteral("无法打开输出文件 %1: %2").arg(outPath, out.errorString()));
        return false;
    }
    // 预置为 totalSize：DONTCARE 空洞读回为 0（稀疏文件），与旧接口零填充逐字节一致
    if (!out.resize(totalSize)) {
        setErr(error, QStringLiteral("无法预分配输出文件 %1: %2").arg(outPath, out.errorString()));
        out.close();
        QFile::remove(outPath);
        return false;
    }
    const bool ok = simg2imgChunks(&in, &out, h, progress, error);
    out.close();
    if (!ok) QFile::remove(outPath);
    return ok;
}

bool img2simgStream(const QString &inPath, const QString &outPath, quint32 blockSize,
                    const std::function<void(quint64)> &progress, QString *error)
{
    QFile in(inPath);
    if (!in.open(QIODevice::ReadOnly)) {
        setErr(error, QStringLiteral("无法打开输入文件 %1: %2").arg(inPath, in.errorString()));
        return false;
    }
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setErr(error, QStringLiteral("无法打开输出文件 %1: %2").arg(outPath, out.errorString()));
        return false;
    }
    const bool ok = img2simgIo(&in, &out, blockSize, progress, error);
    out.close();
    if (!ok) QFile::remove(outPath);
    return ok;
}

} // namespace imgsparse
