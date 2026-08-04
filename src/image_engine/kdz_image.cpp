#include "kdz_image.h"
#include <QtEndian>
#include <zlib.h>
#include <cstring>
#include <limits>

namespace imgkdz {

namespace {

// ==================== 参考源码（IOMonster/thecubed kdztools, GPLv3） ====================
// KDZ v3（unkdz.py 中 header_type 2, 本任务按 brief 称为 v3）:
//   libexec/kdz.py KDZFile._dz_header = b"\x28\x05\x00\x00\x24\x38\x22\x25" (8B 魔数)
//   记录表每条 272B: name[256] + length u64 LE + offset u64 LE
//     （libexec/kdz.py _dz_format_dict: 256s/Q/Q, Struct("<"), _dz_length=272）
//   unkdz.py getPartitions: 魔数后（偏移 8 起）循环读记录; 每条记录后读 1B:
//     \x03 → 下一条是最后一条记录（写布局见 mkkdz.py: 0x03 写于最后一个记录前）
//     \x00 → 记录表结束; 其它 → 下一条记录紧跟
//   数据区: 各记录 offset 处, length 字节
// DZ（undz.py + libexec/dz.py）:
//   主头 512B @0: magic 32 96 18 74 @0; formatMajor u32 @4 (=2); formatMinor u32 @8 (=1);
//     reserved0 @12; device[32] @16; version[144] @48; chunkCount u32 @192; md5[16] @196;
//     ... pad[164] @348
//   chunk: 512B 子头 + dataSize 字节 zlib 流（undz.py extract(): zlib.decompress）:
//     magic 30 12 95 78 @0; sliceName[32] @4; chunkName[64] @36;
//     targetSize u32 @100（解压后字节数）; dataSize u32 @104（压缩字节数）;
//     md5[16] @108; targetAddr u32 @124（eMMC 起始块号）; trimCount u32 @128;
//     dev u32 @132; crc32 u32 @136; pad[372] @140
//   eMMC 偏移 = targetAddr << shiftLBA, shiftLBA 默认 9（512 字节扇区, undz.py 默认值）
//
//   B4 遗留评估（CRC32/MD5 校验）: 子头带 md5@108 / crc32@136，但 zlib 流解压本身
//   已逐块校验 adler32（损坏即 inflate 返回 Z_DATA_ERROR），校验和再验属冗余加固；
//   且严格门禁对真实固件的未知 chunk 变体/尾部数据有误杀风险（无真实固件回归样本）。
//   决定: 暂不实现，保留为真实固件回归时的跟踪项。
// =============================================================================

constexpr unsigned char kKdzMagic[8]   = {0x28, 0x05, 0x00, 0x00, 0x24, 0x38, 0x22, 0x25};
constexpr unsigned char kDzMagic[4]    = {0x32, 0x96, 0x18, 0x74};
constexpr unsigned char kChunkMagic[4] = {0x30, 0x12, 0x95, 0x78};
constexpr qsizetype kKdzRecordLen   = 272; // name[256] + length(8) + offset(8)
constexpr qsizetype kDzHeaderLen    = 512;
constexpr qsizetype kDzChunkHeadLen = 512;
constexpr quint64 kMaxMerge     = 1ULL << 33;      // 分区镜像上限 8GB（eMMC 分区通常 < 8GB）
constexpr int kSectorShift      = 9;               // 512 字节扇区（undz.py shiftLBA 默认）
constexpr quint32 kMaxChunks    = 4096;            // 防呆: 分区数上限

// 解压 zlib 流（与 undz.py zlib.decompress 同格式）; 流式解压不信任头部声明的
// 解压大小, 总量封顶 8GB。返回 true 表示流完整解压（out 可能为空即空 payload 合法）。
bool inflateZlib(const QByteArray &src, QByteArray &out)
{
    out.clear();
    if (src.isEmpty())
        return true;
    z_stream strm{};
    if (inflateInit(&strm) != Z_OK) // windowBits=15: zlib 格式
        return false;
    const quint64 maxOut =
        qMin<quint64>(kMaxMerge, static_cast<quint64>(std::numeric_limits<qsizetype>::max()));
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(src.constData()));
    strm.avail_in = static_cast<uInt>(src.size());
    QByteArray chunkBuf(64 * 1024, Qt::Uninitialized);
    int r;
    for (;;) {
        strm.next_out = reinterpret_cast<Bytef *>(chunkBuf.data());
        strm.avail_out = static_cast<uInt>(chunkBuf.size());
        r = inflate(&strm, Z_NO_FLUSH);
        const qsizetype produced = chunkBuf.size() - static_cast<qsizetype>(strm.avail_out);
        if (produced > 0) {
            if (static_cast<quint64>(out.size()) + static_cast<quint64>(produced) > maxOut) {
                inflateEnd(&strm);
                return false;
            }
            out.append(chunkBuf.constData(), produced);
        }
        if (r == Z_STREAM_END)
            break;
        if (r != Z_OK && r != Z_BUF_ERROR) { // Z_DATA_ERROR 等 → 损坏
            inflateEnd(&strm);
            return false;
        }
        if (produced == 0 && strm.avail_in == 0) { // 输入耗尽但流未结束 → 截断
            inflateEnd(&strm);
            return false;
        }
    }
    inflateEnd(&strm);
    return true;
}

// 去尾部 NUL; 内部含 NUL 返回 false（参考工具对内部 NUL 同样拒绝）
bool trimName(const char *p, int width, QString &out)
{
    int len = 0;
    while (len < width && p[len] != '\0') ++len;
    for (int i = len; i < width; ++i)
        if (p[i] != '\0') return false;
    out = QString::fromUtf8(p, len);
    return true;
}

} // namespace

bool parseKdz(const QByteArray &kdz, QList<DzFile> &out, QString *error)
{
    out.clear();
    // v3 魔数 + 至少一条完整记录
    if (kdz.size() < 8 + kKdzRecordLen || memcmp(kdz.constData(), kKdzMagic, 8) != 0) {
        if (error) *error = QStringLiteral("非法 KDZ 头（非 v3 魔数）");
        return false;
    }
    // 魔数之后即第一条记录（unkdz.py openFile 消费 8B 魔数后开始读）。
    // 兼容 mkkdz.py 单记录输出: 0x03 标记被写在偏移 8（该工具的 N=1 缺陷，
    // 真实文件遵循读取约定, 魔数后直接是记录）→ 跳过该标记。
    qsizetype pos = 8;
    if (pos < kdz.size() && static_cast<uchar>(kdz[pos]) == 0x03)
        ++pos;
    // unkdz.py getPartitions 的停止语义（等价 cont = not last）:
    // 读到 0x03 后【再解析一条记录然后无条件停止】——不依赖 0x00 终止符/EOF。
    // 否则 mkkdz.py 产物（0x03 后最后记录直接接数据、无 0x00 填充）会把
    // 载荷字节当作记录解析而错误拒绝。
    bool last = false;
    while (pos + kKdzRecordLen <= kdz.size()) {
        const char *p = kdz.constData() + pos;
        QString name;
        if (!trimName(p, 256, name)) {
            out.clear();
            if (error) *error = QStringLiteral("KDZ 记录名内部含 NUL");
            return false;
        }
        const quint64 length = qFromLittleEndian<quint64>(p + 256);
        const quint64 offset = qFromLittleEndian<quint64>(p + 264);
        if (offset > static_cast<quint64>(kdz.size()) ||
            length > static_cast<quint64>(kdz.size()) - offset) {
            out.clear();
            if (error) *error = QStringLiteral("KDZ 记录偏移越界");
            return false;
        }
        DzFile f;
        f.name = name;
        f.data = kdz.mid(static_cast<qsizetype>(offset), static_cast<qsizetype>(length));
        out.append(f);
        if (last) // 0x03 之后的最后一条记录已解析 → 无条件停止
            break;
        pos += kKdzRecordLen;
        if (pos >= kdz.size())
            break;
        const uchar c = static_cast<uchar>(kdz[pos]);
        if (c == 0x03) { // 下一条为最后一条记录（mkkdz.py 将 0x03 写于最后一个记录前）
            last = true;
            ++pos;
        } else if (c == 0x00) { // 记录表结束
            break;
        }
        // 其它字节: 下一条记录直接紧跟
    }
    if (out.isEmpty()) {
        if (error) *error = QStringLiteral("KDZ 记录表为空");
        return false;
    }
    return true;
}

bool parseDz(const QByteArray &dz, QList<DzChunk> &out, QString *error)
{
    out.clear();
    if (dz.size() < kDzHeaderLen || memcmp(dz.constData(), kDzMagic, 4) != 0) {
        if (error) *error = QStringLiteral("非法 DZ 头（魔数错误）");
        return false;
    }
    const quint32 chunkCount = qFromLittleEndian<quint32>(dz.constData() + 192);
    if (chunkCount == 0 || chunkCount > kMaxChunks) {
        if (error) *error = QStringLiteral("DZ 分区数异常");
        return false;
    }
    qsizetype pos = kDzHeaderLen;
    for (quint32 i = 0; i < chunkCount; ++i) {
        if (pos + kDzChunkHeadLen > dz.size()) {
            out.clear();
            if (error) *error = QStringLiteral("DZ 分区表截断");
            return false;
        }
        const char *p = dz.constData() + pos;
        if (memcmp(p, kChunkMagic, 4) != 0) {
            out.clear();
            if (error) *error = QStringLiteral("DZ 分区头魔数错误");
            return false;
        }
        QString slice;
        if (!trimName(p + 4, 32, slice)) {
            out.clear();
            if (error) *error = QStringLiteral("DZ 分区名内部含 NUL");
            return false;
        }
        const quint32 dataSize = qFromLittleEndian<quint32>(p + 104);
        const quint32 targetAddr = qFromLittleEndian<quint32>(p + 124);
        pos += kDzChunkHeadLen;
        DzChunk c;
        c.partition = slice;
        c.offset = static_cast<quint64>(targetAddr) << kSectorShift; // eMMC 偏移
        if (dataSize > 0) {
            if (pos + dataSize > dz.size()) {
                out.clear();
                if (error) *error = QStringLiteral("DZ 分区数据越界");
                return false;
            }
            if (!inflateZlib(dz.mid(pos, dataSize), c.data)) {
                out.clear();
                if (error) *error = QStringLiteral("DZ 分区数据解压失败");
                return false;
            }
            pos += dataSize;
        }
        out.append(c);
    }
    return true;
}

QByteArray mergeChunks(const QList<DzChunk> &chunks, QString *error)
{
    // 按 eMMC 偏移合并为单镜像; 空洞填零; 后写 chunk 覆盖先写（与 DZ 写序一致）。
    // 逐 chunk 判定上限（2^33 = 8GB）: 同时防御 offset + size 的 quint64 回绕
    // （offset ≥ 2^64 - size 时求和回绕为小值会绕过旧的"maxEnd 上限"检查）。
    const quint64 cap =
        qMin<quint64>(kMaxMerge, static_cast<quint64>(std::numeric_limits<qsizetype>::max()));
    quint64 maxEnd = 0;
    for (const DzChunk &c : chunks) {
        const quint64 sz = static_cast<quint64>(c.data.size());
        if (c.offset > cap || sz > cap - c.offset) {
            if (error) *error = QStringLiteral("分区镜像过大（超过 8GB 上限）");
            return {};
        }
        maxEnd = qMax(maxEnd, c.offset + sz);
    }
    QByteArray out(static_cast<qsizetype>(maxEnd), 0);
    for (const DzChunk &c : chunks) {
        if (c.data.isEmpty())
            continue;
        out.replace(static_cast<qsizetype>(c.offset), c.data.size(), c.data);
    }
    return out;
}

} // namespace imgkdz
