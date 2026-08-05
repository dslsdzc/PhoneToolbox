#include "payload_image.h"
#include "bspatch_image.h"
#include "compression/compressor.h"
#include "wire_format.h"
#include <QFile>
#include <QCryptographicHash>
#include <QtEndian>
#include <limits>
#include <utility>

namespace imgpayload {

namespace {
quint64 readU64(const QByteArray &d, int off)
{
    return qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(d.constData() + off));
}

bool isDiffOp(int type)
{
    switch (type) {
    case OP_BSDIFF: case OP_SOURCE_COPY: case OP_SOURCE_BSDIFF:
    case OP_PUFFDIFF: case OP_BROTLI_BSDIFF: case OP_ZUCCHINI:
        return true;
    default:
        return false;
    }
}

// 解析单个 Extent 消息（start_block=1, num_blocks=2，字段顺序任意）：
// src_extents=4 / dst_extents=6 的每个 repeated 字段各含一个 Extent，调用方负责追加。
static QList<Extent> parseExtents(const QByteArray &bytes)
{
    bool ok = false;
    const QList<pbwire::Field> fields = pbwire::parseMessage(bytes, ok);
    if (!ok)
        return {};
    Extent e;
    for (const pbwire::Field &f : fields) {
        if (f.number == 1 && f.wireType == 0)
            e.startBlock = f.varint;
        else if (f.number == 2 && f.wireType == 0)
            e.numBlocks = f.varint;
    }
    return QList<Extent>{e};
}

// 依次拼接 extents 对应的块数据（含边界检查）；失败返回空并填 error
static QByteArray gatherExtents(const QByteArray &image, quint64 blockSize,
                                const QList<Extent> &extents, quint64 &totalBlocks, QString *error)
{
    totalBlocks = 0;
    QByteArray out;
    for (const Extent &e : extents) {
        if (e.numBlocks == 0)
            continue;
        if (e.startBlock > std::numeric_limits<quint64>::max() / blockSize
            || e.numBlocks > std::numeric_limits<quint64>::max() / blockSize) {
            if (error) *error = "extent 越界";
            return {};
        }
        const quint64 off = e.startBlock * blockSize;
        const quint64 len = e.numBlocks * blockSize;
        if (off > static_cast<quint64>(image.size())
            || len > static_cast<quint64>(image.size()) - off) {
            if (error) *error = "extent 超出旧镜像范围";
            return {};
        }
        out.append(image.mid(static_cast<int>(off), static_cast<int>(len)));
        totalBlocks += e.numBlocks;
    }
    return out;
}

// 将 data 按块顺序写入 dst_extents；data 总长必须恰好等于 dst 块数 * blockSize
static bool writeExtents(QByteArray &out, quint64 blockSize, const QList<Extent> &extents,
                         const QByteArray &data, QString *error)
{
    int pos = 0;
    for (const Extent &e : extents) {
        if (e.numBlocks == 0)
            continue;
        if (e.startBlock > std::numeric_limits<quint64>::max() / blockSize
            || e.numBlocks > std::numeric_limits<quint64>::max() / blockSize) {
            if (error) *error = "extent 越界";
            return false;
        }
        const quint64 off = e.startBlock * blockSize;
        const quint64 len = e.numBlocks * blockSize;
        if (off > static_cast<quint64>(out.size())
            || len > static_cast<quint64>(data.size()) - static_cast<quint64>(pos)) {
            if (error) *error = "extent 超出输出/数据范围";
            return false;
        }
        out.replace(static_cast<int>(off), static_cast<int>(len),
                    data.mid(pos, static_cast<int>(len)));
        pos += static_cast<int>(len);
    }
    if (pos != data.size()) {
        if (error) *error = "解包结果与 dst_extents 长度不符";
        return false;
    }
    return true;
}

// ================= 流式解包辅助（Task G2） =================

bool streamFail(QString *error, const QString &msg)
{
    if (error) *error = msg;
    return false;
}

bool readExact(QIODevice *d, char *buf, qint64 len)
{
    qint64 got = 0;
    while (got < len) {
        const qint64 n = d->read(buf + got, len - got);
        if (n <= 0)
            return false;
        got += n;
    }
    return true;
}

bool writeAll(QIODevice *d, const char *buf, qint64 len)
{
    qint64 done = 0;
    while (done < len) {
        const qint64 n = d->write(buf + done, len - done);
        if (n <= 0)
            return false;
        done += n;
    }
    return true;
}

// 输出落位区间（文件偏移 + 长度）：op 数据按块顺序依次写入各区间
struct DstSpan {
    qint64 fileOff = 0;
    qint64 len = 0;
};

// 与旧接口 writeExtents 等价的落位表：dataLen 字节按块顺序写入 dst_extents 各区段，
// 总长必须恰等于 dst 块数 * blockSize（错误消息与旧接口一致）。
bool buildDstSpans(quint64 blockSize, const QList<Extent> &extents, qint64 dataLen,
                   QList<DstSpan> &spans, QString *error)
{
    const quint64 kMax = static_cast<quint64>(std::numeric_limits<qint64>::max());
    qint64 pos = 0;
    for (const Extent &e : extents) {
        if (e.numBlocks == 0)
            continue;
        if (e.startBlock > kMax / blockSize || e.numBlocks > kMax / blockSize)
            return streamFail(error, "extent 越界");
        const qint64 off = static_cast<qint64>(e.startBlock * blockSize);
        const qint64 len = static_cast<qint64>(e.numBlocks * blockSize);
        if (len > dataLen - pos)
            return streamFail(error, "extent 超出输出/数据范围");
        spans.append(DstSpan{off, len});
        pos += len;
    }
    if (pos != dataLen)
        return streamFail(error, "解包结果与 dst_extents 长度不符");
    return true;
}

// 顺序消费 spans 写出（自动跨区段分段）
class SpanWriter
{
public:
    explicit SpanWriter(QList<DstSpan> spans) : m_spans(std::move(spans)) {}

    bool write(QFile &out, const char *data, qint64 len, QString *error)
    {
        while (len > 0) {
            if (m_idx >= m_spans.size())
                return streamFail(error, "写入位置超出输出范围");
            const DstSpan &sp = m_spans[m_idx];
            if (m_off >= sp.len) {
                ++m_idx;
                m_off = 0;
                continue;
            }
            const qint64 n = qMin(len, sp.len - m_off);
            if (!out.seek(sp.fileOff + m_off) || !writeAll(&out, data, n))
                return streamFail(error, "写输出失败");
            m_off += n;
            m_written += n;
            data += n;
            len -= n;
        }
        return true;
    }

    qint64 written() const { return m_written; }

private:
    QList<DstSpan> m_spans;
    int m_idx = 0;
    qint64 m_off = 0;
    qint64 m_written = 0;
};

// 从旧镜像文件依次读取 src_extents 对应块数据（与旧接口 gatherExtents 相同语义/边界检查；
// SOURCE_BSDIFF 的 bspatch 输入需要全量驻留内存）
bool gatherExtentsFile(QFile &f, quint64 blockSize, const QList<Extent> &extents,
                       QByteArray &out, QString *error)
{
    const quint64 kMax = static_cast<quint64>(std::numeric_limits<qint64>::max());
    const qint64 fileSize = f.size();
    qint64 total = 0;
    for (const Extent &e : extents) {
        if (e.numBlocks == 0)
            continue;
        if (e.startBlock > kMax / blockSize || e.numBlocks > kMax / blockSize)
            return streamFail(error, "extent 越界");
        const qint64 off = static_cast<qint64>(e.startBlock * blockSize);
        const qint64 len = static_cast<qint64>(e.numBlocks * blockSize);
        if (off > fileSize || len > fileSize - off)
            return streamFail(error, "extent 超出旧镜像范围");
        if (len > kMax - total)
            return streamFail(error, "extent 越界");
        total += len;
    }
    if (total > static_cast<qint64>(std::numeric_limits<qsizetype>::max()))
        return streamFail(error, "旧镜像片段过大");
    out.resize(static_cast<qsizetype>(total));
    qint64 pos = 0;
    for (const Extent &e : extents) {
        if (e.numBlocks == 0)
            continue;
        const qint64 off = static_cast<qint64>(e.startBlock * blockSize);
        const qint64 len = static_cast<qint64>(e.numBlocks * blockSize);
        if (!f.seek(off) || !readExact(&f, out.data() + pos, len))
            return streamFail(error, "读旧镜像失败");
        pos += len;
    }
    return true;
}

// 校验 blob 相对偏移并返回绝对文件偏移（与旧接口相同的定位与越界语义）
bool blobOffset(qint64 totalBase, qint64 fileSize, const InstallOp &op, qint64 &off, QString *error)
{
    if (op.dataOffset >= static_cast<quint64>(fileSize))
        return streamFail(error, "payload 数据越界");
    if (op.dataOffset > static_cast<quint64>(std::numeric_limits<qint64>::max() - totalBase))
        return streamFail(error, "payload 数据越界");
    const qint64 off0 = totalBase + static_cast<qint64>(op.dataOffset);
    if (off0 > fileSize
        || op.dataLength > static_cast<quint64>(fileSize) - static_cast<quint64>(off0))
        return streamFail(error, "payload 数据越界");
    off = off0;
    return true;
}

// 逐 op 流式处理（与旧接口 extractPartition 的 op 循环逐字节等价）：REPLACE 从 payload
// 文件 seek 到 blob 位置（OP_REPLACE 边读边写 + 增量 SHA-256，内存 O(chunk)；压缩系 blob
// 整体读入解压，内存 O(op)）；diff 系从旧镜像文件读取 src_extents 片段（SOURCE_COPY 边读
// 边写；SOURCE_BSDIFF 片段驻留内存供 bspatch）。进度 = 已消费 blob 字节/本分区总 blob 字节。
// 所有失败路径返回 false；输出文件清理由调用方统一处理。
bool processOps(QFile &in, QFile &out, qint64 totalBase, quint64 blockSize, quint64 outSize,
                const Partition &part, const QString &oldImagePath,
                const std::function<void(quint64)> &progress, QString *error)
{
    // 进度总量 = 本分区所有 blob 消费 op（REPLACE 系 + SOURCE_BSDIFF）的 dataLength 之和
    quint64 blobTotal = 0;
    for (const InstallOp &op : part.ops) {
        if (op.type == OP_ZERO || op.type == OP_DISCARD)
            continue;
        if (!isDiffOp(op.type) || op.type == OP_SOURCE_BSDIFF) {
            if (blobTotal > std::numeric_limits<quint64>::max() - op.dataLength)
                blobTotal = std::numeric_limits<quint64>::max(); // 恶意 manifest: 饱和不失败
            else
                blobTotal += op.dataLength;
        }
    }

    QFile oldF;
    QFile *oldFile = nullptr; // diff op 首次出现时懒打开（无 diff op 时旧镜像可缺省）
    quint64 blobConsumed = 0;
    QByteArray ioBuf(1024 * 1024, Qt::Uninitialized); // REPLACE/SOURCE_COPY 复用读缓冲

    if (progress) progress(0);
    for (const InstallOp &op : part.ops) {
        if (op.type == OP_ZERO || op.type == OP_DISCARD)
            continue; // 零块: 输出文件已预置为全零（resize 空洞读回为 0）

        if (isDiffOp(op.type)) {
            if (!oldFile) {
                if (oldImagePath.isEmpty())
                    return streamFail(error, QString("分区 %1 包含差分操作(type=%2)，需要旧镜像")
                                                 .arg(part.name).arg(op.type));
                oldF.setFileName(oldImagePath);
                if (!oldF.open(QIODevice::ReadOnly))
                    return streamFail(error, QString("无法打开旧镜像文件 %1: %2")
                                                 .arg(oldImagePath, oldF.errorString()));
                oldFile = &oldF;
            }
            QList<DstSpan> spans;
            if (op.type == OP_SOURCE_COPY) {
                // 与旧接口相同: 仅支持单连续 src_extent，src→dst 整块复制（边读边写）
                if (op.srcExtents.size() != 1)
                    return streamFail(error, QString("分区 %1 SOURCE_COPY 非单连续 src_extents，暂不支持")
                                                 .arg(part.name));
                const Extent &se = op.srcExtents[0];
                const quint64 kMax = static_cast<quint64>(std::numeric_limits<qint64>::max());
                if (se.startBlock > kMax / blockSize || se.numBlocks > kMax / blockSize)
                    return streamFail(error, "extent 越界");
                const qint64 srcOff = static_cast<qint64>(se.startBlock * blockSize);
                const qint64 srcLen = static_cast<qint64>(se.numBlocks * blockSize);
                if (srcOff > oldFile->size() || srcLen > oldFile->size() - srcOff)
                    return streamFail(error, "extent 超出旧镜像范围");
                quint64 dstBlocks = 0;
                for (const Extent &e : op.dstExtents) {
                    if (e.numBlocks > std::numeric_limits<quint64>::max() - dstBlocks)
                        return streamFail(error, "dst_extents 块数溢出");
                    dstBlocks += e.numBlocks;
                }
                if (dstBlocks != se.numBlocks)
                    return streamFail(error, "SOURCE_COPY src/dst 块数不一致");
                if (!buildDstSpans(blockSize, op.dstExtents, srcLen, spans, error))
                    return false;
                SpanWriter w(std::move(spans));
                if (!oldFile->seek(srcOff))
                    return streamFail(error, "读旧镜像失败");
                qint64 remaining = srcLen;
                while (remaining > 0) {
                    const qint64 n = qMin(remaining, static_cast<qint64>(ioBuf.size()));
                    if (!readExact(oldFile, ioBuf.data(), n))
                        return streamFail(error, "读旧镜像失败");
                    if (!w.write(out, ioBuf.constData(), n, error))
                        return false;
                    remaining -= n;
                }
                continue; // SOURCE_COPY 不消费 blob
            }
            if (op.type == OP_SOURCE_BSDIFF) {
                qint64 off = 0;
                if (!blobOffset(totalBase, in.size(), op, off, error))
                    return false;
                QByteArray patchBlob(static_cast<qsizetype>(op.dataLength), Qt::Uninitialized);
                if (!in.seek(off)
                    || !readExact(&in, patchBlob.data(), static_cast<qint64>(op.dataLength)))
                    return streamFail(error, "payload 文件被截断");
                if (!op.dataHash.isEmpty()) {
                    if (QCryptographicHash::hash(patchBlob, QCryptographicHash::Sha256) != op.dataHash)
                        return streamFail(error, "SHA-256 校验失败");
                }
                QByteArray oldFrag;
                if (!gatherExtentsFile(*oldFile, blockSize, op.srcExtents, oldFrag, error))
                    return false;
                bool ok = false;
                const QByteArray data = imgbspatch::applyBsdiff(oldFrag, patchBlob, &ok);
                if (!ok)
                    return streamFail(error, "bspatch 应用失败");
                if (!buildDstSpans(blockSize, op.dstExtents, data.size(), spans, error))
                    return false;
                SpanWriter w(std::move(spans));
                if (!w.write(out, data.constData(), data.size(), error))
                    return false;
                blobConsumed += op.dataLength;
                if (progress) progress(blobConsumed);
                continue;
            }
            return streamFail(error, QString("暂不支持的差分类型 %1").arg(op.type));
        }

        // ---- REPLACE 系 ----
        qint64 off = 0;
        if (!blobOffset(totalBase, in.size(), op, off, error))
            return false;

        if (op.type == OP_REPLACE) {
            // 与旧接口一致: 空 blob 的 REPLACE 视为解压失败
            if (op.dataLength == 0)
                return streamFail(error, QString("解压失败 type=%1").arg(op.type));
            QList<DstSpan> spans;
            if (op.dstExtents.isEmpty()) {
                // 无 extent 的简化 manifest: 回退 dataOffset/blockSize 连续映射
                const quint64 blockIdx = op.dataOffset / blockSize;
                const quint64 dstU = blockIdx * blockSize;
                if (dstU > outSize || op.dataLength > outSize - dstU)
                    return streamFail(error, "解压后数据超出输出范围");
                spans.append(DstSpan{static_cast<qint64>(dstU), static_cast<qint64>(op.dataLength)});
            } else if (!buildDstSpans(blockSize, op.dstExtents,
                                      static_cast<qint64>(op.dataLength), spans, error)) {
                return false;
            }
            if (!in.seek(off))
                return streamFail(error, "读输入失败");
            // 边读边写 + 增量 SHA-256（校验在末尾完成；失败即删输出文件，语义与旧接口一致）
            SpanWriter w(std::move(spans));
            QCryptographicHash h(QCryptographicHash::Sha256);
            qint64 remaining = static_cast<qint64>(op.dataLength);
            while (remaining > 0) {
                const qint64 n = qMin(remaining, static_cast<qint64>(ioBuf.size()));
                if (!readExact(&in, ioBuf.data(), n))
                    return streamFail(error, "payload 文件被截断");
                h.addData(ioBuf.constData(), static_cast<qsizetype>(n));
                if (!w.write(out, ioBuf.constData(), n, error))
                    return false;
                remaining -= n;
                if (progress)
                    progress(blobConsumed + op.dataLength - static_cast<quint64>(remaining));
            }
            if (!op.dataHash.isEmpty() && h.result() != op.dataHash)
                return streamFail(error, "SHA-256 校验失败");
            blobConsumed += op.dataLength;
            continue;
        }

        // 压缩系: blob 整体读入（解压需全量输入）
        QByteArray blob(static_cast<qsizetype>(op.dataLength), Qt::Uninitialized);
        if (!in.seek(off) || !readExact(&in, blob.data(), static_cast<qint64>(op.dataLength)))
            return streamFail(error, "payload 文件被截断");
        if (!op.dataHash.isEmpty()) {
            if (QCryptographicHash::hash(blob, QCryptographicHash::Sha256) != op.dataHash)
                return streamFail(error, "SHA-256 校验失败");
        }
        QByteArray data;
        switch (op.type) {
        case OP_REPLACE_BZ: data = imgcomp::decompress(imgcomp::Type::Bzip2, blob); break;
        case OP_REPLACE_XZ: data = imgcomp::decompress(imgcomp::Type::Xz, blob); break;
        case OP_REPLACE_ZSTD:
        case OP_REPLACE_ZSTD_INC_WINDOW:
            data = imgcomp::decompress(imgcomp::Type::Zstd, blob); break;
        default:
            return streamFail(error, QString("不支持的 REPLACE 类型 %1").arg(op.type));
        }
        if (data.isEmpty())
            return streamFail(error, QString("解压失败 type=%1").arg(op.type));
        QList<DstSpan> spans;
        if (op.dstExtents.isEmpty()) {
            const quint64 blockIdx = op.dataOffset / blockSize;
            const quint64 dstU = blockIdx * blockSize;
            if (dstU > outSize || static_cast<quint64>(data.size()) > outSize - dstU)
                return streamFail(error, "解压后数据超出输出范围");
            spans.append(DstSpan{static_cast<qint64>(dstU), static_cast<qint64>(data.size())});
        } else if (!buildDstSpans(blockSize, op.dstExtents, data.size(), spans, error)) {
            return false;
        }
        SpanWriter w(std::move(spans));
        if (!w.write(out, data.constData(), data.size(), error))
            return false;
        blobConsumed += op.dataLength;
        if (progress) progress(blobConsumed);
    }
    if (progress) progress(blobTotal);
    return true;
}
} // namespace

bool isPayload(const QByteArray &header)
{
    return header.size() >= 4 && header.left(4) == "CrAU";
}

bool parseManifest(const QByteArray &payload, PayloadInfo &out)
{
    if (!isPayload(payload) || payload.size() < 20)
        return false;
    const quint64 version = readU64(payload, 4);
    const quint64 manifestSize = readU64(payload, 12);
    int dataStart = 20 + (version >= 2 ? 4 : 0);
    if (dataStart > payload.size() || manifestSize > static_cast<quint64>(payload.size() - dataStart))
        return false;
    out.manifestRaw = payload.mid(dataStart, static_cast<int>(manifestSize));
    bool ok = false;
    const QList<pbwire::Field> manifest = pbwire::parseMessage(out.manifestRaw, ok);
    if (!ok)
        return false;
    for (const pbwire::Field &f : manifest) {
        if (f.number == 3 && f.wireType == 0)
            out.blockSize = f.varint;
        else if (f.number == 13 && f.wireType == 2) {
            bool pok = false;
            const QList<pbwire::Field> partFields = pbwire::parseMessage(f.bytes, pok);
            if (!pok)
                return false;
            Partition part;
            for (const pbwire::Field &pf : partFields) {
                if (pf.number == 1 && pf.wireType == 2)
                    part.name = QString::fromLatin1(pf.bytes);
                else if (pf.number == 8 && pf.wireType == 2) {
                    bool ook = false;
                    const QList<pbwire::Field> opFields = pbwire::parseMessage(pf.bytes, ook);
                    if (!ook)
                        return false;
                    InstallOp op;
                    for (const pbwire::Field &of : opFields) {
                        switch (of.number) {
                        case 1: if (of.wireType == 0) op.type = static_cast<int>(of.varint); break;
                        case 2: if (of.wireType == 0) op.dataOffset = of.varint; break;
                        case 3: if (of.wireType == 0) op.dataLength = of.varint; break;
                        case 4: if (of.wireType == 2) op.srcExtents += parseExtents(of.bytes); break;
                        case 6: if (of.wireType == 2) op.dstExtents += parseExtents(of.bytes); break;
                        case 7: if (of.wireType == 0) op.dstLength = of.varint; break;
                        case 8: if (of.wireType == 2) op.dataHash = of.bytes; break;
                        }
                    }
                    part.ops.append(op);
                }
            }
            out.partitions.append(part);
        }
    }
    return true;
}

QByteArray extractPartition(const QByteArray &payload, const Partition &part,
                            const QByteArray &oldImage, QString *error, quint64 blockSize)
{
    if (error) error->clear();

    // blob 起点 = 头 + manifestRaw: 4("CrAU") + 8(version) + 8(manifest_size)
    //            + (v2 起 4B metadata_signature_size) + manifestSize。
    // version(偏移 4) 决定是否读 sig_size(偏移 20)：v1 无 4B sig_size，带签名 payload 才
    // 不会错位。
    if (payload.size() < 20) {
        if (error) *error = "payload 头不完整";
        return {};
    }
    const quint64 version = readU64(payload, 4);
    const qint64 sigSizeBytes = (version >= 2) ? 4 : 0;
    if (payload.size() < 20 + sigSizeBytes) {
        if (error) *error = "payload 头不完整";
        return {};
    }
    const quint64 manifestSize = readU64(payload, 12);
    if (manifestSize > static_cast<quint64>(payload.size()) - (20 + sigSizeBytes)) {
        if (error) *error = "manifest_size 超界";
        return {};
    }
    const qint64 totalBase = 20 + sigSizeBytes + static_cast<qint64>(manifestSize);
    if (blockSize == 0) {
        if (error) *error = "block_size 非法";
        return {};
    }

    // 输出大小: 所有 op（含 ZERO/DISCARD 的输出空间）的 dst_extents 覆盖的最大块范围，
    // 再 ceil 到 blockSize。dst_extents 为空（无 extent 的简化 manifest）时回退到
    // max(dataOffset+dataLength)——解压类 op 此时按解压后大小预算（见下方回退写入路径）。
    // 先扫一遍求 maxEnd 再一次性 resize（不边写边扩）。
    quint64 maxEnd = 0;
    for (const InstallOp &op : part.ops) {
        bool hasExtents = false;
        for (const Extent &e : op.dstExtents) {
            hasExtents = true;
            if (e.startBlock > std::numeric_limits<quint64>::max() - e.numBlocks)
                continue; // 溢出（恶意/损坏 manifest）→ 写入阶段会报越界
            const quint64 total = e.startBlock + e.numBlocks;
            if (total > std::numeric_limits<quint64>::max() / blockSize)
                continue;
            const quint64 end = total * blockSize;
            if (end > maxEnd)
                maxEnd = end;
        }
        if (hasExtents)
            continue;
        if (op.dataOffset > std::numeric_limits<quint64>::max() - op.dataLength)
            continue; // 溢出（恶意/损坏 manifest）→ 逐 op 阶段会报"数据越界"，不计入分配
        const quint64 end = op.dataOffset + op.dataLength;
        if (end > maxEnd)
            maxEnd = end;
    }
    if (maxEnd > static_cast<quint64>(std::numeric_limits<int>::max())) {
        if (error) *error = "分区数据长度超出支持范围";
        return {};
    }
    const quint64 outSize = (maxEnd == 0) ? 0 : ((maxEnd - 1) / blockSize + 1) * blockSize;
    if (outSize > static_cast<quint64>(std::numeric_limits<int>::max())) {
        if (error) *error = "分区数据长度超出支持范围";
        return {};
    }
    QByteArray out(static_cast<int>(outSize), '\0');

    for (const InstallOp &op : part.ops) {
        if (op.type == OP_ZERO || op.type == OP_DISCARD)
            continue; // 零块: out 已零初始化
        if (isDiffOp(op.type)) {
            if (oldImage.isEmpty()) {
                if (error) *error = QString("分区 %1 包含差分操作(type=%2)，需要旧镜像")
                                         .arg(part.name).arg(op.type);
                return {};
            }
            QByteArray data;
            if (op.type == OP_SOURCE_COPY) {
                // SOURCE_COPY: src_extents → dst_extents 整块复制。
                // 本任务支持"单连续 src_extent 且与 dst 块数一致"的常见形态。
                if (op.srcExtents.size() != 1) {
                    if (error) *error = QString("分区 %1 SOURCE_COPY 非单连续 src_extents，暂不支持")
                                             .arg(part.name);
                    return {};
                }
                quint64 srcBlocks = 0;
                data = gatherExtents(oldImage, blockSize, op.srcExtents, srcBlocks, error);
                if (data.isEmpty() && error && !error->isEmpty())
                    return {};
                quint64 dstBlocks = 0;
                for (const Extent &e : op.dstExtents) {
                    if (e.numBlocks > std::numeric_limits<quint64>::max() - dstBlocks) {
                        if (error) *error = "dst_extents 块数溢出";
                        return {};
                    }
                    dstBlocks += e.numBlocks;
                }
                if (dstBlocks != srcBlocks) {
                    if (error) *error = "SOURCE_COPY src/dst 块数不一致";
                    return {};
                }
            } else if (op.type == OP_SOURCE_BSDIFF) {
                // SOURCE_BSDIFF: patch 应用对象是 src_extents 对应的旧数据块（不是整分区）。
                // blob 读取范围 = blob 起点(totalBase) + dataOffset（dataOffset 为 blob 相对偏移）。
                if (op.dataOffset >= static_cast<quint64>(payload.size())) {
                    if (error) *error = "payload 数据越界";
                    return {};
                }
                const qint64 off = totalBase + static_cast<qint64>(op.dataOffset);
                if (off < 0 || off > payload.size()
                    || op.dataLength > static_cast<quint64>(payload.size()) - static_cast<quint64>(off)) {
                    if (error) *error = "payload 数据越界";
                    return {};
                }
                QByteArray patchBlob = payload.mid(static_cast<int>(off), static_cast<int>(op.dataLength));
                if (!op.dataHash.isEmpty()) {
                    const QByteArray actual = QCryptographicHash::hash(patchBlob, QCryptographicHash::Sha256);
                    if (actual != op.dataHash) {
                        if (error) *error = "SHA-256 校验失败";
                        return {};
                    }
                }
                quint64 srcBlocks = 0;
                QByteArray oldFrag = gatherExtents(oldImage, blockSize, op.srcExtents, srcBlocks, error);
                if (oldFrag.isEmpty() && error && !error->isEmpty())
                    return {};
                bool ok = false;
                data = imgbspatch::applyBsdiff(oldFrag, patchBlob, &ok);
                if (!ok) {
                    if (error) *error = "bspatch 应用失败";
                    return {};
                }
            } else {
                if (error) *error = QString("暂不支持的差分类型 %1").arg(op.type);
                return {};
            }
            if (!writeExtents(out, blockSize, op.dstExtents, data, error))
                return {};
            continue;
        }
        // blob 读取范围 = blob 起点(totalBase) + dataOffset（dataOffset 为 blob 相对偏移）。
        // dataOffset 先与 payload 总长比较，保证下方有符号加法不回绕。
        if (op.dataOffset >= static_cast<quint64>(payload.size())) {
            if (error) *error = "payload 数据越界";
            return {};
        }
        const qint64 off = totalBase + static_cast<qint64>(op.dataOffset);
        if (off < 0 || off > payload.size()
            || op.dataLength > static_cast<quint64>(payload.size()) - static_cast<quint64>(off)) {
            if (error) *error = "payload 数据越界";
            return {};
        }
        QByteArray blob = payload.mid(static_cast<int>(off), static_cast<int>(op.dataLength));
        // data_sha256_hash（字段 8）的 AOSP 语义: 对压缩后 blob 计算（delta_performer.cc
        // ValidateOperationHash 对原始 blob 校验）。REPLACE 不压缩，data==blob，二者一致；
        // 解压类 op 必须先对 blob 校验再解压（与 SOURCE_BSDIFF 分支对 patch blob 校验一致）。
        if (!op.dataHash.isEmpty()) {
            const QByteArray actual = QCryptographicHash::hash(blob, QCryptographicHash::Sha256);
            if (actual != op.dataHash) {
                if (error) *error = "SHA-256 校验失败";
                return {};
            }
        }
        QByteArray data;
        switch (op.type) {
        case OP_REPLACE: data = blob; break;
        case OP_REPLACE_BZ: data = imgcomp::decompress(imgcomp::Type::Bzip2, blob); break;
        case OP_REPLACE_XZ: data = imgcomp::decompress(imgcomp::Type::Xz, blob); break;
        case OP_REPLACE_ZSTD:
        case OP_REPLACE_ZSTD_INC_WINDOW:
            data = imgcomp::decompress(imgcomp::Type::Zstd, blob); break;
        default:
            if (error) *error = QString("不支持的 REPLACE 类型 %1").arg(op.type);
            return {};
        }
        if (data.isEmpty()) {
            if (error) *error = QString("解压失败 type=%1").arg(op.type);
            return {};
        }
        // 写入定位: 真实 payload 按 dst_extents 决定目标块（dataOffset 只是 blobs 区偏移，
        // 与分区内块位置无固定关系；含 ZERO/DISCARD 的 payload 若按 dataOffset 连续映射
        // 会整体错位）。无 dst_extents 的简化 manifest 回退到 blockIdx = dataOffset/blockSize
        // 的连续映射，此时输出预算对应 dataOffset+dataLength。
        if (op.dstExtents.isEmpty()) {
            const quint64 blockIdx = op.dataOffset / blockSize;
            const qint64 dst = static_cast<qint64>(blockIdx * blockSize);
            if (dst > out.size() || data.size() > out.size() - dst) {
                if (error) *error = "解压后数据超出输出范围";
                return {};
            }
            out.replace(static_cast<int>(dst), data.size(), data);
        } else {
            if (!writeExtents(out, blockSize, op.dstExtents, data, error))
                return {};
        }
    }
    return out;
}

bool parseManifestFile(const QString &payloadPath, PayloadInfo &out)
{
    QFile f(payloadPath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    if (f.size() < 20)
        return false;
    const QByteArray head = f.read(24); // 4("CrAU") + 8(version) + 8(manifest_size) + 4(sig_size)
    if (head.size() < 20 || head.left(4) != "CrAU")
        return false;
    const quint64 version = readU64(head, 4);
    const qint64 dataStart = 20 + (version >= 2 ? 4 : 0);
    if (dataStart > head.size())
        return false;
    const quint64 manifestSize = readU64(head, 12);
    if (manifestSize > static_cast<quint64>(f.size()) - static_cast<quint64>(dataStart))
        return false;
    if (!f.seek(dataStart))
        return false;
    const QByteArray manifest = f.read(static_cast<qint64>(manifestSize));
    if (manifest.size() != static_cast<qint64>(manifestSize))
        return false;
    // 复用 parseManifest（对"头 + manifest"缓冲做相同的魔数/尺寸校验与字段解析）
    return parseManifest(head.left(dataStart) + manifest, out);
}

bool extractPartitionStream(const QString &payloadPath, const Partition &part,
                            const QString &outPath,
                            const std::function<void(quint64)> &progress,
                            QString *error,
                            const QString &oldImagePath,
                            quint64 blockSize)
{
    if (error) error->clear();
    if (blockSize == 0)
        return streamFail(error, "block_size 非法");

    QFile in(payloadPath);
    if (!in.open(QIODevice::ReadOnly))
        return streamFail(error, QString("无法打开输入文件 %1: %2").arg(payloadPath, in.errorString()));
    if (in.size() < 20)
        return streamFail(error, "payload 头不完整");
    const QByteArray head = in.read(20);
    if (head.size() < 20 || head.left(4) != "CrAU")
        return streamFail(error, "payload 头不完整");
    const quint64 version = readU64(head, 4);
    const qint64 sigSizeBytes = (version >= 2) ? 4 : 0;
    if (in.size() < 20 + sigSizeBytes)
        return streamFail(error, "payload 头不完整");
    const quint64 manifestSize = readU64(head, 12);
    if (manifestSize > static_cast<quint64>(in.size()) - (20 + sigSizeBytes))
        return streamFail(error, "manifest_size 超界");
    const qint64 totalBase = 20 + sigSizeBytes + static_cast<qint64>(manifestSize);

    // 输出大小: 与旧接口相同的预算（dst_extents 覆盖末端 ceil 到 blockSize；无 extent 的
    // op 回退 dataOffset+dataLength）。文件接口无 QByteArray 的 2GB int 限制。
    const quint64 kMaxU = std::numeric_limits<quint64>::max();
    quint64 maxEnd = 0;
    for (const InstallOp &op : part.ops) {
        bool hasExtents = false;
        for (const Extent &e : op.dstExtents) {
            hasExtents = true;
            if (e.startBlock > kMaxU - e.numBlocks)
                continue; // 溢出（恶意/损坏 manifest）→ 写入阶段会报越界
            const quint64 total = e.startBlock + e.numBlocks;
            if (total > kMaxU / blockSize)
                continue;
            const quint64 end = total * blockSize;
            if (end > maxEnd)
                maxEnd = end;
        }
        if (hasExtents)
            continue;
        if (op.dataOffset > kMaxU - op.dataLength)
            continue; // 溢出（恶意/损坏 manifest）→ 逐 op 阶段会报"数据越界"，不计入分配
        const quint64 end = op.dataOffset + op.dataLength;
        if (end > maxEnd)
            maxEnd = end;
    }
    const quint64 outSize = (maxEnd == 0) ? 0 : ((maxEnd - 1) / blockSize + 1) * blockSize;
    if (outSize > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        return streamFail(error, "分区数据长度超出支持范围");
    }

    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return streamFail(error, QString("无法创建输出文件 %1: %2").arg(outPath, out.errorString()));
    }
    // 预置为 outSize: ZERO/DISCARD 空洞读回为 0，与旧接口零初始化逐字节一致
    if (!out.resize(static_cast<qint64>(outSize))) {
        const QString errMsg = QString("无法预分配输出文件 %1: %2").arg(outPath, out.errorString());
        out.close();
        QFile::remove(outPath);
        return streamFail(error, errMsg);
    }
    const bool ok = processOps(in, out, totalBase, blockSize, outSize, part, oldImagePath,
                               progress, error);
    out.close();
    if (!ok)
        QFile::remove(outPath);
    return ok;
}

} // namespace imgpayload
