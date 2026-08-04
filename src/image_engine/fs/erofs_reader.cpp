#include "image_engine/fs/erofs_reader.h"

// 布局对照 Linux 内核 fs/erofs/erofs_fs.h（v5.10 ~ v6.x + erofs-utils master，
// mkfs.erofs 输出即此布局）与 fs/erofs/super.c 的读取逻辑：
//
//   EROFS_SUPER_OFFSET = 1024（偏移 0 为保留区）
//   struct erofs_super_block（__packed，全部小端，相对 superblock 起点）:
//     +0   __le32 magic            = 0xE0F5E1E2 (EROFS_SUPER_MAGIC_V1)
//     +4   __le32 checksum
//     +8   __le32 feature_compat
//     +12  __u8   blkszbits        (blockSize = 1 << blkszbits)
//     +13  __u8   sb_extslots / reserved
//     +14  __le16 root_nid         (48BIT 特性未开启时的根目录 nid)
//     +16  __le64 inos
//     +24  __le64 build_time / epoch
//     +32  __le32 build_time_nsec / fixed_nsec
//     +36  __le32 blocks_lo
//     +40  __le32 meta_blkaddr
//     +44  __le32 xattr_blkaddr
//     +48  __u8   uuid[16]
//     +64  __u8   volume_name[16]
//     +80  __le32 feature_incompat
//     +84  __le16 available_compr_algs (bit0 = Z_EROFS_COMPRESSION_LZ4)
//     ...
//     +112 __le64 rootnid_8b       (EROFS_FEATURE_INCOMPAT_48BIT 置位时使用)
//
// 注：旧版（2021 年前）erofs_fs.h 中 blkszbits/root_nid 也曾位于同一偏移
// （+12 u8 / +14 u16），该段布局自 v5.10 起稳定不变，仅尾部扩展字段变化。

namespace imgerofs {

namespace {

constexpr quint32 kSuperOffset = 1024;        // EROFS_SUPER_OFFSET
constexpr quint32 kMagic = 0xE0F5E1E2;        // EROFS_SUPER_MAGIC_V1

// 相对 superblock 起点的字段偏移（内核 erofs_fs.h）
constexpr quint32 kOffBlkszbits      = 12;    // __u8
constexpr quint32 kOffRootNid        = 14;    // __le16
constexpr quint32 kOffFeatureIncompat = 80;   // __le32
constexpr quint32 kOffComprAlgs      = 84;    // __le16 available_compr_algs
constexpr quint32 kOffRootNid8b      = 112;   // __le64 rootnid_8b (48BIT)

// EROFS_FEATURE_INCOMPAT_48BIT (erofs-utils erofs_fs.h)
constexpr quint32 kFeatIncompat48Bit = 0x00000080;

quint16 le16(const QByteArray &d, quint32 off)
{
    return quint16(uchar(d[off])) | (quint16(uchar(d[off + 1])) << 8);
}
quint32 le32(const QByteArray &d, quint32 off)
{
    return quint32(uchar(d[off])) | (quint32(uchar(d[off + 1])) << 8) |
           (quint32(uchar(d[off + 2])) << 16) | (quint32(uchar(d[off + 3])) << 24);
}
quint64 le64(const QByteArray &d, quint32 off)
{
    quint64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= quint64(uchar(d[off + i])) << (i * 8);
    return v;
}

} // namespace

bool isErofs(const QByteArray &image)
{
    if (image.size() < int(kSuperOffset + 4))
        return false;
    return le32(image, kSuperOffset) == kMagic;   // 0xE0F5E1E2 小端: E2 E1 F5 E0
}

bool parseSuper(const QByteArray &image, SuperBlock &out)
{
    if (!isErofs(image))
        return false;
    // 至少读到 available_compr_algs（+84 的 2 字节）
    const int minRead = kSuperOffset + kOffComprAlgs + 2;
    if (image.size() < minRead)
        return false;

    const quint32 blkszbits = uchar(image[kSuperOffset + kOffBlkszbits]);
    if (blkszbits < 9 || blkszbits > 16)   // 512B .. 64KiB
        return false;
    out.blockSize = 1u << blkszbits;

    const quint32 featIncompat = le32(image, kSuperOffset + kOffFeatureIncompat);
    // 内核 super.c: 48BIT 且 rootnid_8b 非 0 时取 8 字节 nid，否则 2 字节
    if (featIncompat & kFeatIncompat48Bit) {
        if (image.size() < kSuperOffset + kOffRootNid8b + 8)
            return false;
        const quint64 nid8 = le64(image, kSuperOffset + kOffRootNid8b);
        out.rootNid = nid8 ? nid8 : le16(image, kSuperOffset + kOffRootNid);
    } else {
        out.rootNid = le16(image, kSuperOffset + kOffRootNid);
    }

    // 内核 z_erofs_parse_cfgs: algs==0 为 legacy 布局，唯一可用算法即 LZ4
    const quint16 algs = le16(image, kSuperOffset + kOffComprAlgs);
    out.isLz4 = algs ? ((algs & 0x0001) != 0) : true;

    return true;
}

} // namespace imgerofs
