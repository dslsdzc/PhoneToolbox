#include "payload_image.h"
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
                        case 7: if (of.wireType == 2) op.dataHash = of.bytes; break;
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
                            const QByteArray &oldImage, QString *error)
{
    if (error) error->clear();
    // oldImage 本任务未使用: 全量（REPLACE 系）解包不依赖旧镜像；diff 解包（Task 15）才需要。

    // blob 起点 = 头 + manifestRaw: 4("CrAU") + 8(version) + 8(manifest_size)
    //            + 4(metadata_signature_size, v2 固定 4B) + manifestRaw.size()
    if (payload.size() < 24) {
        if (error) *error = "payload 头不完整";
        return {};
    }
    const quint64 manifestSize = readU64(payload, 12);
    if (manifestSize > static_cast<quint64>(payload.size())) {
        if (error) *error = "manifest_size 超界";
        return {};
    }
    const qint64 totalBase = 4 + 8 + 8 + 4 + static_cast<qint64>(manifestSize);

    // 输出大小: 所有非 diff 操作的最大 dataOffset+dataLength，ceil 到 blockSize。
    // 先扫一遍求 maxEnd 再一次性 resize（不边写边扩）。
    const quint64 blockSize = 4096; // manifest block_size(3) 未随 part 传入，按缺省值
    quint64 maxEnd = 0;
    for (const InstallOp &op : part.ops) {
        if (isDiffOp(op.type))
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
    const quint64 outSize = ((maxEnd + blockSize - 1) / blockSize) * blockSize;
    if (outSize > static_cast<quint64>(std::numeric_limits<int>::max())) {
        if (error) *error = "分区数据长度超出支持范围";
        return {};
    }
    QByteArray out(static_cast<int>(outSize), '\0');

    for (const InstallOp &op : part.ops) {
        if (op.type == OP_ZERO || op.type == OP_DISCARD)
            continue; // 零块: out 已零初始化
        if (isDiffOp(op.type)) {
            if (error) {
                *error = QString("分区 %1 包含差分操作(type=%2)，需要旧镜像才能解包")
                             .arg(part.name).arg(op.type);
            }
            return {};
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
