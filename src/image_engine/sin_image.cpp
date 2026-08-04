#include "sin_image.h"
#include <QtEndian>
#include <limits>
#include <lz4.h>

namespace imgsin {

namespace {

// ==================== SIN v3 结构（三源确认: flashtool S1ParseLib sin/v3, ROMExplorer,
// munjeni/sin2raw; 字段全为大端 BE） ====================
// 文件头: [0]=0x03 + "SIN" + headerLen u32 BE + type u32 BE（+ 保留字节; headerLen 为头总长）
// SinDataHeader: "MMCF" + mmcfLen u32 BE + "GPTP" + gptpLen u32 BE + GPTGUID(gptpLen-8)
// 块数组区域长 = mmcfLen - gptpLen（flashtool: addrLength = mmcfLen - gptpLen）;
// 数据基址 dataBase = headerLen + mmcfLen + 8（flashtool getDataOffset()）;
// 块数据实际文件偏移 = dataBase + dataOffset。
// ADDR 块（0x44B, 非压缩）:
//   magic@0 + blockLen@4 + dataOffset@8 + dataLen@16 + fileOffset@24 + hashType@32 + SHA256@36
// LZ4A 块（0x54B, LZ4 raw-block 压缩, 无 frame 头）:
//   magic@0 + blockLen@4 + dataOffset@8 + uncompDataLen@16 + compDataLen@24 + fileOffset@32
//   + reserved@40 + hashType@48 + SHA256@52
// hashType: 0x02 = SHA256(32B)（flashtool hashv3len = {0, 0, 32}）; 其余类型的哈希长度由
// blockLen 字段决定, 按 blockLen 步进跳过, 内容不校验。
// ========================================================================================

constexpr qsizetype kDataHdrLen    = 16;   // "MMCF" u32 "GPTP" u32
constexpr qsizetype kBlockMagicLen = 4;
constexpr qsizetype kAddrMinLen    = 36;   // ADDR 固定字段截至 hashType 结束
constexpr qsizetype kLz4aMinLen    = 52;   // LZ4A 固定字段截至 hashType 结束
constexpr int      kMaxBlocks      = 8192;       // 块描述数量上限（防呆）
constexpr quint64  kMaxRawSize     = 1ULL << 34; // 输出 raw 上限 16GB（0xFF 空洞为内存镜像）
constexpr quint64  kMaxLz4Block    = 1ULL << 30; // 单块解压上限 1GB（真实 LZ4 块远小于此）

inline quint32 rd32(const QByteArray &d, qsizetype off)
{
    return qFromBigEndian<quint32>(d.constData() + off);
}
inline quint64 rd64(const QByteArray &d, qsizetype off)
{
    return qFromBigEndian<quint64>(d.constData() + off);
}

// LZ4 raw-block 解压（SIN LZ4A 数据无 lz4frame 头, 不可用 imgcomp::lz4Decompress）。
// 失败 / 解压长度与 dstCapacity 不符返回空。
QByteArray lz4RawDecompress(const QByteArray &src, quint64 dstCapacity, QString *error)
{
    if (dstCapacity == 0 || dstCapacity > kMaxLz4Block) {
        if (error) *error = QStringLiteral("LZ4A 块解压长度异常");
        return {};
    }
    if (src.size() > std::numeric_limits<int>::max()) {
        if (error) *error = QStringLiteral("LZ4A 压缩数据过长");
        return {};
    }
    QByteArray out(static_cast<qsizetype>(dstCapacity), Qt::Uninitialized);
    const int r = LZ4_decompress_safe(src.constData(), out.data(),
                                      static_cast<int>(src.size()),
                                      static_cast<int>(dstCapacity));
    if (r < 0 || static_cast<quint64>(r) != dstCapacity) {
        if (error) *error = QStringLiteral("LZ4A raw-block 解压失败");
        return {};
    }
    return out;
}

} // namespace

bool isSinV3(const QByteArray &header)
{
    return header.size() >= 4 && header[0] == char(0x03)
        && header.mid(1, 3) == QByteArrayLiteral("SIN");
}

bool parseSin(const QByteArray &sin, QList<BlockDesc> &blocks, QString *error)
{
    blocks.clear();
    if (!isSinV3(sin)) {
        if (error) *error = QStringLiteral("非法 SIN 头（非 v3 魔数）");
        return false;
    }
    if (sin.size() < 8) {
        if (error) *error = QStringLiteral("SIN 文件过短");
        return false;
    }
    const quint64 hdrLen = rd32(sin, 4); // headerLen 从字段读取（真实文件可大于 15, 如 flashtool 24B 头）
    if (hdrLen < 12 || hdrLen > static_cast<quint64>(sin.size())) {
        if (error) *error = QStringLiteral("SIN 头长度异常");
        return false;
    }
    const qsizetype dataHdrOff = static_cast<qsizetype>(hdrLen);
    if (sin.size() - dataHdrOff < kDataHdrLen) {
        if (error) *error = QStringLiteral("SIN 文件过短");
        return false;
    }
    if (sin.mid(dataHdrOff, 4) != QByteArrayLiteral("MMCF")
        || sin.mid(dataHdrOff + 8, 4) != QByteArrayLiteral("GPTP")) {
        if (error) *error = QStringLiteral("缺少 SinDataHeader（MMCF/GPTP）");
        return false;
    }
    const quint64 mmcfLen = rd32(sin, dataHdrOff + 4);
    const quint64 gptpLen = rd32(sin, dataHdrOff + 12);
    if (gptpLen < 8 || mmcfLen < gptpLen) {
        if (error) *error = QStringLiteral("SinDataHeader 长度异常");
        return false;
    }
    const quint64 dataBase = hdrLen + mmcfLen + 8; // flashtool getDataOffset()
    if (dataBase > static_cast<quint64>(sin.size())) {
        if (error) *error = QStringLiteral("SIN 数据区越界");
        return false;
    }
    const qsizetype blocksStart = dataHdrOff + kDataHdrLen + static_cast<qsizetype>(gptpLen - 8);
    const qsizetype regionEnd =
        blocksStart + static_cast<qsizetype>(mmcfLen - gptpLen); // == dataBase
    if (regionEnd > sin.size()) {
        if (error) *error = QStringLiteral("SIN 块描述区越界");
        return false;
    }
    qsizetype off = blocksStart;
    while (off + kBlockMagicLen <= regionEnd) {
        if (blocks.size() >= kMaxBlocks) {
            if (error) *error = QStringLiteral("SIN 块描述数量超限");
            return false;
        }
        const QByteArray magic = sin.mid(off, kBlockMagicLen);
        const bool compressed = (magic == QByteArrayLiteral("LZ4A"));
        if (magic != QByteArrayLiteral("ADDR") && !compressed)
            break; // 容错兜底: mmcfLen 边界为主, 魔数不符即视为块数组结束（数据区特征）
        const quint64 bihLen = rd32(sin, off + 4);
        const qsizetype minLen = compressed ? kLz4aMinLen : kAddrMinLen;
        if (bihLen < static_cast<quint64>(minLen)
            || static_cast<quint64>(regionEnd - off) < bihLen) {
            if (error) *error = QStringLiteral("块描述长度越界");
            return false;
        }
        const quint64 dataOffset = rd64(sin, off + 8);
        if (dataOffset > static_cast<quint64>(sin.size())) {
            if (error) *error = QStringLiteral("块数据偏移越界");
            return false;
        }
        BlockDesc b;
        b.magic = magic;
        if (compressed) {
            b.blockSize  = rd64(sin, off + 16); // uncompDataLen
            b.dataLength = rd64(sin, off + 24); // compDataLen
            b.dataDest   = rd64(sin, off + 32); // fileOffset
            b.compressed = true;
        } else {
            b.dataLength = rd64(sin, off + 16); // dataLen
            b.dataDest   = rd64(sin, off + 24); // fileOffset
        }
        const quint32 hashType = rd32(sin, off + (compressed ? 48 : 32));
        if (hashType == 0x02 && bihLen - static_cast<quint64>(minLen) != 32) {
            if (error) *error = QStringLiteral("SHA256 哈希长度不符");
            return false;
        } // 其余 hashType 按 blockLen 步进跳过, 哈希内容不校验
        b.dataStart = dataBase + dataOffset; // 折算为绝对文件偏移
        blocks.append(b);
        off += static_cast<qsizetype>(bihLen);
    }
    if (blocks.isEmpty()) {
        if (error) *error = QStringLiteral("SIN 无块描述");
        return false;
    }
    return true;
}

QByteArray extractRaw(const QByteArray &sin, const QList<BlockDesc> &blocks, QString *error)
{
    if (blocks.isEmpty()) {
        if (error) *error = QStringLiteral("SIN 无块描述");
        return {};
    }
    // 输出 raw: 各块按 dataDest 落位, 空洞（未定义区域）0xFF 填充（sin2raw/flashtool 行为）
    quint64 rawSize = 0;
    for (const BlockDesc &b : blocks) {
        const quint64 writeLen = b.compressed ? b.blockSize : b.dataLength;
        if (writeLen > kMaxRawSize || b.dataDest > kMaxRawSize - writeLen) {
            if (error) *error = QStringLiteral("SIN 块落位超出上限");
            return {};
        }
        rawSize = qMax(rawSize, b.dataDest + writeLen);
    }
    if (rawSize > kMaxRawSize) {
        if (error) *error = QStringLiteral("SIN 输出镜像超出上限");
        return {};
    }
    QByteArray out(static_cast<qsizetype>(rawSize), char(0xFF));
    for (const BlockDesc &b : blocks) {
        const quint64 srcLen = b.dataLength;
        if (b.dataStart > static_cast<quint64>(sin.size())
            || srcLen > static_cast<quint64>(sin.size()) - b.dataStart) {
            if (error) *error = QStringLiteral("SIN 块数据越界");
            return {};
        }
        const QByteArray data = sin.mid(static_cast<qsizetype>(b.dataStart),
                                        static_cast<qsizetype>(srcLen));
        if (!b.compressed) {
            out.replace(static_cast<qsizetype>(b.dataDest), data.size(), data);
            continue;
        }
        const QByteArray dec = lz4RawDecompress(data, b.blockSize, error);
        if (dec.isEmpty() || static_cast<quint64>(dec.size()) != b.blockSize)
            return {}; // error 已由 lz4RawDecompress 设置
        out.replace(static_cast<qsizetype>(b.dataDest), dec.size(), dec);
    }
    return out;
}

} // namespace imgsin
