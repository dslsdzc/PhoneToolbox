#include "sin_image.h"
#include "compression/lz4_wrapper.h"
#include <QtEndian>
#include <limits>

namespace imgsin {

namespace {

// ==================== SIN v3 结构（参考: munjeni/sin2raw, flashtool S1ParseLib sin/v3） ==========
// 文件头 15B: [0]=0x03 + "SIN" + headerLen u32 LE + type u32 LE（+ 3B 保留）
// SinDataHeader 16B: "MMCF" + mmcfLen u32 + "GPTP" + gptpLen u32（GUID 长度 = gptpLen-8）
// GPTGUID 16B; 随后 BlockInfoHeader 连续数组, 数据区紧跟数组末尾:
//   magic "LZ4A"（压缩）或 "ADDR"（非压缩）+ BIHLength u32（0x54 / 0x44, 含 magic）
//   + 4B 保留 + dataStart u64 + dataLength u64 + dataDest u64
//   + (仅 LZ4A) blockSize u64 + destLength u64 + SHA256 32B
//   注: 字段偏移按本任务 brief 测试构造（dataStart@12 / dataLength@20 / dataDest@28, LE）;
//        与 flashtool S1ParseLib 实测布局（dataOffset@8 / dataLen@16 / fileOffset@24）相差 4B,
//        计划文档已标注"不假装确定", 需真实样本核对。块数组以首个非 ADDR/LZ4A 魔数终止
//        （数据区直接开始, 与 sin2raw/flashtool 遍历方式一致）。
// ========================================================================================

constexpr qsizetype kSinHeaderLen  = 15; // 0x03 "SIN" + 头长 u32 + type u32 + 3 保留
constexpr qsizetype kDataHdrLen    = 16; // "MMCF" + u32 + "GPTP" + u32
constexpr qsizetype kGptGuidLen    = 16; // GPTGUID
constexpr qsizetype kBlockMagicLen = 4;
constexpr qsizetype kAddrMinLen    = 36; // ADDR 固定字段截至 dataDest 结束
constexpr qsizetype kLz4aMinLen    = 52; // LZ4A 固定字段截至 destLength 结束
constexpr int      kMaxBlocks      = 8192;     // 块描述数量上限（防呆）
constexpr quint64  kMaxRawSize     = 1ULL << 34; // 输出 raw 上限 16GB（0xFF 空洞为内存镜像）

inline quint32 rd32(const QByteArray &d, qsizetype off)
{
    return qFromLittleEndian<quint32>(d.constData() + off);
}
inline quint64 rd64(const QByteArray &d, qsizetype off)
{
    return qFromLittleEndian<quint64>(d.constData() + off);
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
    if (sin.size() < kSinHeaderLen + kDataHdrLen + kGptGuidLen) {
        if (error) *error = QStringLiteral("SIN 文件过短");
        return false;
    }
    if (sin.mid(kSinHeaderLen, 4) != QByteArrayLiteral("MMCF")
        || sin.mid(kSinHeaderLen + 8, 4) != QByteArrayLiteral("GPTP")) {
        if (error) *error = QStringLiteral("缺少 SinDataHeader（MMCF/GPTP）");
        return false;
    }
    qsizetype off = kSinHeaderLen + kDataHdrLen + kGptGuidLen;
    while (off + kBlockMagicLen <= sin.size()) {
        if (blocks.size() >= kMaxBlocks) {
            if (error) *error = QStringLiteral("SIN 块描述数量超限");
            return false;
        }
        const QByteArray magic = sin.mid(off, kBlockMagicLen);
        const bool compressed = (magic == QByteArrayLiteral("LZ4A"));
        if (magic != QByteArrayLiteral("ADDR") && !compressed)
            break; // 块数组结束, 数据区开始
        const quint64 bihLen = rd32(sin, off + 4);
        const qsizetype minLen = compressed ? kLz4aMinLen : kAddrMinLen;
        if (bihLen < static_cast<quint64>(minLen)
            || bihLen > static_cast<quint64>(sin.size()) - static_cast<quint64>(off)) {
            if (error) *error = QStringLiteral("块描述长度越界");
            return false;
        }
        BlockDesc b;
        b.magic = magic;
        b.dataStart  = rd64(sin, off + 12);
        b.dataLength = rd64(sin, off + 20);
        b.dataDest   = rd64(sin, off + 28);
        if (compressed) {
            b.blockSize = rd64(sin, off + 36);
            b.compressed = true;
        }
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
        if (b.blockSize == 0) {
            if (error) *error = QStringLiteral("LZ4A 块缺少解压长度");
            return {};
        }
        const QByteArray dec = imgcomp::lz4Decompress(data);
        if (dec.isEmpty() || static_cast<quint64>(dec.size()) != b.blockSize) {
            if (error) *error = QStringLiteral("LZ4A 块解压失败或长度不符");
            return {};
        }
        out.replace(static_cast<qsizetype>(b.dataDest), dec.size(), dec);
    }
    return out;
}

} // namespace imgsin
