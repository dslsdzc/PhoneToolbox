#include "payload_image.h"
#include "bspatch_image.h"
#include "compression/compressor.h"
#include "wire_format.h"
#include <QCryptographicHash>
#include <QtEndian>
#include <limits>

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

    // 输出大小: 非 diff 操作取 max(dataOffset+dataLength)，diff 操作取 max(dst_extents 末端)，
    // 再 ceil 到 blockSize。先扫一遍求 maxEnd 再一次性 resize（不边写边扩）。
    quint64 maxEnd = 0;
    for (const InstallOp &op : part.ops) {
        if (isDiffOp(op.type)) {
            for (const Extent &e : op.dstExtents) {
                if (e.startBlock > std::numeric_limits<quint64>::max() - e.numBlocks)
                    continue; // 溢出（恶意/损坏 manifest）→ 写入阶段会报越界
                const quint64 total = e.startBlock + e.numBlocks;
                if (total > std::numeric_limits<quint64>::max() / blockSize)
                    continue;
                const quint64 end = total * blockSize;
                if (end > maxEnd)
                    maxEnd = end;
            }
            continue;
        }
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
        if (!op.dataHash.isEmpty()) {
            const QByteArray actual = QCryptographicHash::hash(data, QCryptographicHash::Sha256);
            if (actual != op.dataHash) {
                if (error) *error = "SHA-256 校验失败";
                return {};
            }
        }
        // 写入定位: blockIdx = dataOffset / blockSize 的连续映射（简化实现；真实 payload
        // 按 dst_extents 定位，非连续 extents 支持在 Task 15 完善）。
        // 缓冲区按 dataOffset+dataLength 预算: REPLACE 的 data 恰为 dataLength 恒可放下；
        // 解压类 op 的 data 为解压后大小（真实 payload 中恰好填满一块），超出预算视为异常。
        const quint64 blockIdx = op.dataOffset / blockSize;
        const qint64 dst = static_cast<qint64>(blockIdx * blockSize);
        if (dst > out.size() || data.size() > out.size() - dst) {
            if (error) *error = "解压后数据超出输出范围";
            return {};
        }
        out.replace(static_cast<int>(dst), data.size(), data);
    }
    return out;
}

} // namespace imgpayload
