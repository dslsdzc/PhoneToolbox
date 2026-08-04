#include "image_engine/fs/ext4_reader.h"

#include <QtEndian>
#include <QMap>
#include <QSet>

// 布局对照（均经本机 e2fsprogs mke2fs 真实镜像 xxd 与内核/e2fsprogs 源码核实）：
//
// 1. superblock @1024（前 1024B 为引导区）：见头文件注释。
// 2. GDT：blockSize==1024 → block 2，否则 block 1；描述符 32B（非 64BIT）或
//    64B（64BIT，s_desc_size @254；内核规则：64BIT 时 <64 视为 64，否则恒 32）。
// 3. inode 定位：组 = (nid-1)/inodesPerGroup，组内索引 = (nid-1)%inodesPerGroup，
//    偏移 = (bg_inode_table + 索引) * blockSize（bg_inode_table 描述符 @8，64BIT
//    时并入 @40 高 32 位）。
// 4. inode 字段偏移见头文件；extent 树在 i_block@40：
//    ext4_extent_header: magic 0xF30A u16@0 | eh_entries u16@2 | eh_max u16@4 |
//    eh_depth u8@6 | eh_generation u32@8
//    depth==0 → ext4_extent(12B): ee_block u32@0 | ee_len u16@4（bit15 未写）|
//    ee_start_hi u16@6 | ee_start_lo u32@8（start 恒 = hi<<32|lo，内核即如此）
//    depth>0 → ext4_extent_idx(12B): ei_block u32@0 | ei_leaf_lo u32@4 |
//    ei_leaf_hi u16@8，索引块含同结构头+子条目
// 5. 目录项 ext4_dir_entry_2: inode u32@0 | rec_len u16@4（4 对齐，最后一项延伸
//    到块尾）| name_len u8@6 | file_type u8@7 | name@8。无 FILETYPE 特性(0x2)时
//    为老格式 ext2_dir_entry：name_len u16@6。file_type 1=regular 2=dir 7=symlink。
//    htree dx 根块：dot(rec_len=12) + dotdot(rec_len=blocksize-12) 覆盖索引区，
//    线性遍历天然跳过（与 e2fsprogs dir_iterate 完全一致，无需 dx 树遍历）。
//    metadata_csum 目录块尾部 ext2_dir_entry_tail(inode=0, rec_len=12,
//    name_len=0xDE) 由 inode==0 规则跳过。
// 6. inline data（i_flags & 0x10000000）：前 60B 在 i_block@40；其余在
//    "system.data" xattr 值：xattr 体起点 = 128+extra_isize，magic u32
//    (0xEA020000) 后即条目（ext4_xattr_entry: e_name_len u8@0 | e_name_index
//    u8@1 | e_value_offs u16@2 | e_value_inum u32@4 | e_value_size u32@8 |
//    e_hash u32@12 | name@16；末条目 = 4 零字节），值位置 = 体起点+4+
//    e_value_offs。内核 ext4_read_inline_data 同此（xattr.h 仅 4B 头 +
//    e_name_len 在前的布局，本实现已按内核修正 brief 描述）。
// 7. inline 目录：数据区 = [父 inode u32 @0][真实目录项 @4..]（"."/".." 隐式）。
// 8. 快速符号链接（i_size<60，无 EXTENTS 标志）：目标在 i_block@40。

namespace imgext4 {

namespace {

constexpr qint64 kSuperOffset = 1024;
constexpr qint64 kOffInodesCount   = kSuperOffset + 0;
constexpr qint64 kOffBlocksCountLo = kSuperOffset + 4;
constexpr qint64 kOffFreeBlocksLo  = kSuperOffset + 12;
constexpr qint64 kOffFirstDataBlk  = kSuperOffset + 20;
constexpr qint64 kOffLogBlockSize  = kSuperOffset + 24;
constexpr qint64 kOffBlocksPerGrp  = kSuperOffset + 32;
constexpr qint64 kOffInodesPerGrp  = kSuperOffset + 40;
constexpr qint64 kOffMagic         = kSuperOffset + 56;
constexpr qint64 kOffInodeSize     = kSuperOffset + 88;
constexpr qint64 kOffFeatureInc    = kSuperOffset + 96;
constexpr qint64 kOffFeatureRoCompat = kSuperOffset + 100;   // s_feature_ro_compat
constexpr qint64 kOffDescSize      = kSuperOffset + 254;
constexpr qint64 kOffBlocksHi      = kSuperOffset + 336;

constexpr qint64 kSuperMinLen = kSuperOffset + 360;  // 1384：所有 superblock 读取终点

constexpr quint16 kExt4Magic = 0xEF53;
// EXT4_FEATURE_RO_COMPAT_METADATA_CSUM = 0x0400（e2fsprogs ext4_fs.h；位于
// feature_ro_compat @100，非 feature_compat）
constexpr quint32 kRoCompatMetaCsum = 0x0400;
constexpr quint32 kIncompat64Bit     = 0x0080;
constexpr quint32 kIncompatFiletype  = 0x0002;
constexpr quint32 kIncompatMetaBg    = 0x0010;
constexpr quint32 kIncompatInlineData = 0x2000;
constexpr quint32 kGoodOldInodeSize = 128;
constexpr quint32 kMaxLogBlockSize = 6;

// ---- inode 内偏移 ----
constexpr int kInMode       = 0;
constexpr int kInSizeLo     = 4;
constexpr int kInLinks      = 26;
constexpr int kInBlocks     = 28;    // 512B 单位
constexpr int kInFlags      = 32;
constexpr int kInBlock      = 40;    // i_block[60]
constexpr int kInSizeHigh   = 108;
constexpr int kInExtraIsize = 128;
constexpr int kInChecksumLo = 112;

// ---- i_flags ----
constexpr quint32 kFlExtents     = 0x00080000;
constexpr quint32 kFlIndex       = 0x00001000;
constexpr quint32 kFlInlineData  = 0x10000000;
constexpr quint32 kFlHugeFile    = 0x00040000;

// ---- extent ----
constexpr quint32 kExtMagic = 0xF30A;
constexpr quint16 kExtUnwritten = 0x8000;
constexpr int kMaxExtentDepth = 8;      // 内核实际 ≤ 5
constexpr quint32 kExtMaxLen = 32768;   // 单个 extent 最多 32768 块
constexpr quint32 kExtMaxWritableLen = 32767;  // ee_len 仅低 15 位（bit15=未写标志）
                                               // 写入上限；32768(0x8000) 读回为 0

// ---- 目录 ----
constexpr int kMinRecLen = 12;          // sizeof(ext4_dir_entry_2) 的头 8B + 1 名对齐
constexpr int kMaxNameLen = 255;
constexpr int kMaxDirDepth = 128;       // 目录递归深度上限（防恶意超深链栈溢出）
constexpr quint8 kFtReg = 1, kFtDir = 2, kFtSymlink = 7;
constexpr quint32 kModeDir = 0x4000, kModeReg = 0x8000, kModeSymlink = 0xA000;

// ---- inline data / xattr ----
constexpr int kMinInlineDataSize = 60;  // EXT4_MIN_INLINE_DATA_SIZE = i_block 60B
constexpr quint32 kXattrMagic = 0xEA020000;
constexpr quint8 kXattrIndexSystem = 7;
constexpr quint32 kXattrEntryHdr = 16;  // 条目头到 name 的长度

void setErr(QString *error, const QString &msg)
{
    if (error)
        *error = msg;
}

bool inBounds(const QByteArray &img, qint64 off, qint64 len)
{
    if (off < 0 || len < 0)
        return false;
    const qint64 size = img.size();
    return off <= size && len <= size - off;
}

quint16 le16p(const uchar *p)
{
    return quint16(p[0]) | (quint16(p[1]) << 8);
}
quint32 le32p(const uchar *p)
{
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}
quint64 le64p(const uchar *p)
{
    quint64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= quint64(p[i]) << (i * 8);
    return v;
}

bool readU16(const QByteArray &img, qint64 off, quint16 &v)
{
    if (!inBounds(img, off, 2))
        return false;
    v = le16p(reinterpret_cast<const uchar *>(img.constData()) + off);
    return true;
}
bool readU32(const QByteArray &img, qint64 off, quint32 &v)
{
    if (!inBounds(img, off, 4))
        return false;
    v = le32p(reinterpret_cast<const uchar *>(img.constData()) + off);
    return true;
}
bool readU64(const QByteArray &img, qint64 off, quint64 &v)
{
    if (!inBounds(img, off, 8))
        return false;
    v = le64p(reinterpret_cast<const uchar *>(img.constData()) + off);
    return true;
}

bool validSuper(const SuperBlock &sb, QString *error)
{
    if (sb.blockSize < 1024 || sb.blockSize > 65536) {
        setErr(error, QStringLiteral("无效的 ext4 blockSize %1").arg(sb.blockSize));
        return false;
    }
    if (sb.inodeSize < kGoodOldInodeSize || sb.inodeSize > sb.blockSize) {
        setErr(error, QStringLiteral("无效的 ext4 inodeSize %1").arg(sb.inodeSize));
        return false;
    }
    if (sb.inodeCount == 0 || sb.blockCount == 0) {
        setErr(error, QStringLiteral("无效的 ext4 inode/block 计数"));
        return false;
    }
    if (sb.inodesPerGroup == 0 || sb.blocksPerGroup == 0) {
        setErr(error, QStringLiteral("无效的 ext4 每块组 inode/block 数"));
        return false;
    }
    if (sb.featureIncompat & kIncompatMetaBg) {
        setErr(error, QStringLiteral("ext4 META_BG 特性暂不支持"));
        return false;
    }
    return true;
}

// 块组描述符
struct GroupDesc {
    quint64 blockBitmap = 0;
    quint64 inodeBitmap = 0;
    quint64 inodeTable = 0;
    quint64 freeBlocks = 0;
};

bool groupDesc(const QByteArray &img, const SuperBlock &sb, quint64 group,
               GroupDesc &out, QString *error)
{
    if (!validSuper(sb, error))
        return false;
    const quint64 groups = (sb.inodeCount + sb.inodesPerGroup - 1) / sb.inodesPerGroup;
    if (group >= groups) {
        setErr(error, QStringLiteral("块组 %1 越界").arg(group));
        return false;
    }
    // GDT：blockSize==1024 → block 2（superblock 占 block 1），否则 block 1
    const quint64 gdtBlock = (sb.blockSize == 1024) ? 2 : 1;
    const qint64 off = qint64(gdtBlock * quint64(sb.blockSize) +
                              group * quint64(sb.descSize));
    // 读取终点：非 64BIT 到 free_blocks(+14)；64BIT 到 free_blocks_hi(+46)
    const qint64 need = (sb.featureIncompat & kIncompat64Bit) ? 46 : 14;
    if (!inBounds(img, off, need)) {
        setErr(error, QStringLiteral("块组描述符 %1 超出镜像范围").arg(group));
        return false;
    }
    const uchar *p = reinterpret_cast<const uchar *>(img.constData()) + off;
    out.blockBitmap = le32p(p + 0);
    out.inodeBitmap = le32p(p + 4);
    out.inodeTable = le32p(p + 8);
    out.freeBlocks = le16p(p + 12);
    if (sb.featureIncompat & kIncompat64Bit) {
        // 高 32 位（64 字节描述符 @32/@36/@40；@44 为 free_blocks 高 16 位）
        if (sb.descSize >= 48) {
            out.blockBitmap |= quint64(le32p(p + 32)) << 32;
            out.inodeBitmap |= quint64(le32p(p + 36)) << 32;
            out.inodeTable  |= quint64(le32p(p + 40)) << 32;
            out.freeBlocks  |= quint64(le16p(p + 44)) << 16;
        }
    }
    return true;
}

struct Inode {
    quint16 mode = 0;
    quint64 size = 0;
    quint32 flags = 0;
    quint32 blocks = 0;        // i_blocks（512B 单位）
    qint64 offset = 0;         // 镜像内 inode 起点
};

bool inodeLocation(const QByteArray &img, const SuperBlock &sb, quint64 nid,
                   qint64 &off, QString *error)
{
    if (nid == 0) {
        setErr(error, QStringLiteral("inode 0 非法"));
        return false;
    }
    const quint64 group = (nid - 1) / sb.inodesPerGroup;
    const quint64 idx = (nid - 1) % sb.inodesPerGroup;
    GroupDesc gd;
    if (!groupDesc(img, sb, group, gd, error))
        return false;
    // 组内 inode 表可能不足一个整块（末组），逐 inode 定位：
    // offset = bg_inode_table * blockSize + idx * inodeSize
    if (gd.inodeTable > (Q_UINT64_C(0x7FFFFFFF) / sb.blockSize) ||
        idx > (Q_UINT64_C(0x7FFFFFFF) / sb.inodeSize)) {
        setErr(error, QStringLiteral("inode 表地址溢出"));
        return false;
    }
    const qint64 base = qint64(gd.inodeTable * sb.blockSize + idx * sb.inodeSize);
    if (!inBounds(img, base, qint64(sb.inodeSize))) {
        setErr(error, QStringLiteral("inode %1 超出镜像范围").arg(nid));
        return false;
    }
    off = base;
    return true;
}

bool readInode(const QByteArray &img, const SuperBlock &sb, quint64 nid,
               Inode &ino, QString *error)
{
    qint64 off;
    if (!inodeLocation(img, sb, nid, off, error))
        return false;
    const uchar *p = reinterpret_cast<const uchar *>(img.constData()) + off;
    ino.offset = off;
    ino.mode = le16p(p + kInMode);
    ino.flags = le32p(p + kInFlags);
    ino.blocks = le32p(p + kInBlocks);
    quint64 size = le32p(p + kInSizeLo);
    if (sb.inodeSize > kGoodOldInodeSize) {
        // 内核 ext4_iget：i_extra_isize 存在（inodeSize>128）时并入 i_size_high
        size |= quint64(le32p(p + kInSizeHigh)) << 32;
    }
    ino.size = size;
    return true;
}

// ---- extent 树 ----
struct Extent {
    quint64 lblock = 0;
    quint32 len = 0;
    quint64 pblock = 0;
};

// 递归读 extent 树（root 在 inode i_block@40；索引块指向更深层块）
bool readExtentsRec(const QByteArray &img, const SuperBlock &sb,
                    const Inode &ino, qint64 headerOff, int depth,
                    QList<Extent> &out, QString *error)
{
    if (depth > kMaxExtentDepth) {
        setErr(error, QStringLiteral("extent 树深度超限"));
        return false;
    }
    if (!inBounds(img, headerOff, 12)) {
        setErr(error, QStringLiteral("extent 头超出镜像范围"));
        return false;
    }
    const uchar *p = reinterpret_cast<const uchar *>(img.constData()) + headerOff;
    if (le16p(p) != kExtMagic) {
        setErr(error, QStringLiteral("extent 头魔数无效"));
        return false;
    }
    const quint16 entries = le16p(p + 2);
    const quint16 max = le16p(p + 4);
    const quint8 ehDepth = p[6];
    if (entries > max || entries > 340) {   // 340 = 4096/12 上限
        setErr(error, QStringLiteral("extent 条目数无效（%1/%2）").arg(entries).arg(max));
        return false;
    }
    const qint64 maxSpace = (depth == 0) ? 60 : qint64(sb.blockSize);
    if (qint64(12) + qint64(entries) * 12 > maxSpace) {
        setErr(error, QStringLiteral("extent 条目超出所在区域"));
        return false;
    }
    for (quint16 i = 0; i < entries; ++i) {
        const qint64 e = headerOff + 12 + qint64(i) * 12;
        if (!inBounds(img, e, 12)) {
            setErr(error, QStringLiteral("extent 条目越界"));
            return false;
        }
        const uchar *ep = reinterpret_cast<const uchar *>(img.constData()) + e;
        if (ehDepth == 0) {
            Extent ex;
            ex.lblock = le32p(ep);
            const quint16 rawLen = le16p(ep + 4);
            ex.len = rawLen & ~kExtUnwritten;   // bit15 = 未写标记，块数取低 15 位
            ex.pblock = (quint64(le16p(ep + 6)) << 32) | le32p(ep + 8);
            if (ex.len == 0) {
                setErr(error, QStringLiteral("extent 长度为零"));
                return false;
            }
            if (ex.len > kExtMaxLen) {
                setErr(error, QStringLiteral("extent 长度超限"));
                return false;
            }
            // 物理块范围必须落在镜像内（乘法溢出安全检查）
            if (ex.pblock > (Q_UINT64_C(0x7FFFFFFFFFFFFFFF) / sb.blockSize)) {
                setErr(error, QStringLiteral("extent 物理块号过大"));
                return false;
            }
            const qint64 dataOff = qint64(ex.pblock * sb.blockSize);
            if (!inBounds(img, dataOff, qint64(ex.len) * sb.blockSize)) {
                setErr(error, QStringLiteral("extent 数据区超出镜像范围"));
                return false;
            }
            out.append(ex);
        } else {
            const quint64 leaf = (quint64(le16p(ep + 8)) << 32) | le32p(ep + 4);
            if (leaf > (Q_UINT64_C(0x7FFFFFFFFFFFFFFF) / sb.blockSize)) {
                setErr(error, QStringLiteral("extent 索引块号过大"));
                return false;
            }
            // 递归 depth+1（根=0）：守卫 depth > kMaxExtentDepth 才能拦下环状
            // 索引块（A→B→A）——旧实现传 depth-1 使守卫恒不成立 → 无限递归栈溢出
            if (!readExtentsRec(img, sb, ino, qint64(leaf * sb.blockSize),
                                depth + 1, out, error))
                return false;
        }
    }
    return true;
}

bool readExtents(const QByteArray &img, const SuperBlock &sb, const Inode &ino,
                 QList<Extent> &out, QString *error)
{
    if (!(ino.flags & kFlExtents)) {
        setErr(error, QStringLiteral("inode 未使用 extent（legacy block map 暂不支持）"));
        return false;
    }
    if (!readExtentsRec(img, sb, ino, ino.offset + kInBlock, 0, out, error))
        return false;
    // 按逻辑块号排序（内核保证按序；防御性排序便于提取/容量计算）
    std::sort(out.begin(), out.end(),
              [](const Extent &a, const Extent &b) { return a.lblock < b.lblock; });
    return true;
}

// 读 "system.data" xattr 值（inline data 的 60B 之后部分）
bool readSystemData(const QByteArray &img, const SuperBlock &sb,
                    const Inode &ino, qint64 &valueOff, quint32 &valueSize,
                    QString *error)
{
    if (sb.inodeSize <= kGoodOldInodeSize) {
        setErr(error, QStringLiteral("inline 文件需要 inodeSize>128 的 xattr 空间"));
        return false;
    }
    quint16 extraIsize;
    if (!readU16(img, ino.offset + kInExtraIsize, extraIsize) || extraIsize < 4) {
        setErr(error, QStringLiteral("inode extra_isize 无效"));
        return false;
    }
    const qint64 base = ino.offset + kGoodOldInodeSize + extraIsize;
    quint32 magic;
    if (!readU32(img, base, magic) || magic != kXattrMagic) {
        setErr(error, QStringLiteral("inline inode xattr 魔数无效"));
        return false;
    }
    // 条目从 base+4 起（内核 ext4_xattr_ibody_header 仅 4B magic；末条目 = 4 零字节）
    qint64 entryOff = base + 4;
    const qint64 end = ino.offset + qint64(sb.inodeSize);
    for (;;) {
        if (!inBounds(img, entryOff, 4)) {
            setErr(error, QStringLiteral("xattr 条目越界"));
            return false;
        }
        quint32 first;
        if (!readU32(img, entryOff, first))
            return false;
        if (first == 0)
            break;                              // IS_LAST_ENTRY
        if (!inBounds(img, entryOff, kXattrEntryHdr)) {
            setErr(error, QStringLiteral("xattr 条目头越界"));
            return false;
        }
        const uchar *ep = reinterpret_cast<const uchar *>(img.constData()) + entryOff;
        const quint8 nameLen = ep[0];
        const quint8 nameIndex = ep[1];
        const quint16 valueOffs = le16p(ep + 2);
        const quint32 valueInum = le32p(ep + 4);
        const quint32 vSize = le32p(ep + 8);
        // 条目实际长度（4 对齐）
        const qint64 entryLen = qint64((nameLen + kXattrEntryHdr + 3) & ~3);
        if (!inBounds(img, entryOff, entryLen) || entryLen < kXattrEntryHdr) {
            setErr(error, QStringLiteral("xattr 条目长度无效"));
            return false;
        }
        if (nameLen == 4 && nameIndex == kXattrIndexSystem && valueInum == 0 &&
            !memcmp(ep + kXattrEntryHdr, "data", 4)) {
            if (!inBounds(img, base + 4 + valueOffs, vSize)) {
                setErr(error, QStringLiteral("system.data 值越界"));
                return false;
            }
            valueOff = base + 4 + valueOffs;
            valueSize = vSize;
            return true;
        }
        entryOff += entryLen;
        if (entryOff > end) {
            setErr(error, QStringLiteral("xattr 条目链越界"));
            return false;
        }
    }
    setErr(error, QStringLiteral("inline inode 缺少 system.data 条目"));
    return false;
}

// 读 inline 数据：前 60B 在 i_block@40，其余在 system.data 值
bool readInlineData(const QByteArray &img, const SuperBlock &sb,
                    const Inode &ino, QByteArray &out, QString *error)
{
    out.clear();
    out.reserve(int(qMin<quint64>(ino.size, 64 * 1024 * 1024)));
    const quint64 part1 = qMin<quint64>(ino.size, quint64(kMinInlineDataSize));
    if (!inBounds(img, ino.offset + kInBlock, qint64(part1))) {
        setErr(error, QStringLiteral("inline 数据区越界"));
        return false;
    }
    out.append(img.mid(ino.offset + kInBlock, qint64(part1)));
    if (ino.size > quint64(kMinInlineDataSize)) {
        qint64 vOff;
        quint32 vSize;
        if (!readSystemData(img, sb, ino, vOff, vSize, error))
            return false;
        const quint64 rest = ino.size - quint64(kMinInlineDataSize);
        if (quint64(vSize) < rest) {
            setErr(error, QStringLiteral("system.data 值小于文件剩余长度"));
            return false;
        }
        if (!inBounds(img, vOff, qint64(rest))) {
            setErr(error, QStringLiteral("system.data 值越界"));
            return false;
        }
        out.append(img.mid(vOff, qint64(rest)));
    }
    return true;
}

// 读普通文件数据（extent 文件）：按 extent 顺序拼接前 i_size 字节
bool readExtentData(const QByteArray &img, const SuperBlock &sb,
                    const Inode &ino, QList<Extent> &extents,
                    QByteArray &out, QString *error)
{
    out.clear();
    out.reserve(int(qMin<quint64>(ino.size, 64 * 1024 * 1024)));
    quint64 need = ino.size;
    for (const Extent &ex : extents) {
        if (need == 0)
            break;
        const quint64 avail = quint64(ex.len) * sb.blockSize;
        const quint64 take = qMin(avail, need);
        const qint64 off = qint64(ex.pblock * sb.blockSize);
        if (!inBounds(img, off, qint64(take))) {
            setErr(error, QStringLiteral("文件数据区超出镜像范围"));
            return false;
        }
        out.append(img.mid(off, qint64(take)));
        need -= take;
    }
    if (need != 0) {
        // 稀疏文件空洞（逻辑块未被 extent 覆盖）被保守拒绝：不补零返回，
        // 避免无意识扩大文件（替换语义下空洞区不参与容量计算，跳过即可）
        setErr(error, QStringLiteral("extent 不足以覆盖文件大小"));
        return false;
    }
    return true;
}

bool readFileData(const QByteArray &img, const SuperBlock &sb,
                  const Inode &ino, QByteArray &out, QString *error)
{
    if (ino.flags & kFlInlineData)
        return readInlineData(img, sb, ino, out, error);
    QList<Extent> extents;
    if (!readExtents(img, sb, ino, extents, error))
        return false;
    return readExtentData(img, sb, ino, extents, out, error);
}

// ---- 目录项 ----
struct Dirent {
    quint64 nid = 0;
    QByteArray name;
    quint8 fileType = 0;
};

// 解析一个目录缓冲（线性目录项链；htree 的 dx 根块同样适用——dot/dotdot 的
// rec_len 覆盖索引区，天然跳过；metadata_csum 的目录尾项 inode==0 跳过）。
// blockLen 为块长（目录块或 inline 剩余区）；filetype 特性关闭时按老格式
// ext2_dir_entry（name_len u16@6，无 file_type）解析。
bool parseDirBuffer(const QByteArray &img, qint64 blockOff, qint64 blockLen,
                    bool filetype, quint32 blockSize, QList<Dirent> &out,
                    QString *error)
{
    qint64 pos = 0;
    while (pos + kMinRecLen <= blockLen) {
        const uchar *p = reinterpret_cast<const uchar *>(img.constData()) + blockOff + pos;
        quint32 rlen = le16p(p + 4);
        // 65536B 块时 rec_len 65535/0 表示整块（ext4_rec_len_from_disk）
        if (blockSize == 65536 && (rlen == 65535 || rlen == 0))
            rlen = blockSize;
        if (rlen < kMinRecLen || (rlen % 4) != 0 || pos + qint64(rlen) > blockLen) {
            setErr(error, QStringLiteral("目录项 rec_len 无效"));
            return false;
        }
        const quint32 inode = le32p(p);
        quint32 nameLen;
        quint8 fileType = 0;
        if (filetype) {
            nameLen = p[6];
            fileType = p[7];
        } else {
            nameLen = quint32(le16p(p + 6));   // 老格式 16 位 name_len，无 file_type
        }
        if (nameLen > kMaxNameLen || qint64(nameLen) + 8 > qint64(rlen)) {
            setErr(error, QStringLiteral("目录项名字长度无效"));
            return false;
        }
        if (inode != 0) {
            Dirent d;
            d.nid = inode;
            d.fileType = fileType;
            d.name = QByteArray(reinterpret_cast<const char *>(p + 8), int(nameLen));
            out.append(d);
        }
        pos += qint64(rlen);
    }
    return true;
}

// 读目录的条目列表（inline 目录：数据区 = [父 inode u32][目录项 @4..]；
// 块目录：按 extent 逐块线性解析）
bool readDirEntries(const QByteArray &img, const SuperBlock &sb,
                    const Inode &dirIno, QList<Dirent> &out, QString *error)
{
    out.clear();
    if (dirIno.flags & kFlInlineData) {
        QByteArray data;
        if (!readInlineData(img, sb, dirIno, data, error))
            return false;
        if (data.size() < 4) {
            setErr(error, QStringLiteral("inline 目录数据过短"));
            return false;
        }
        // 前 4B 为父目录 inode（"."/".." 隐式，不枚举）；条目从 @4 起
        return parseDirBuffer(data, 4, data.size() - 4,
                              (sb.featureIncompat & kIncompatFiletype) != 0,
                              sb.blockSize, out, error);
    }
    QList<Extent> extents;
    if (!readExtents(img, sb, dirIno, extents, error))
        return false;
    if (extents.isEmpty()) {
        setErr(error, QStringLiteral("目录没有数据块"));
        return false;
    }
    for (const Extent &ex : extents) {
        for (quint32 i = 0; i < ex.len; ++i) {
            const qint64 off = qint64((ex.pblock + i) * sb.blockSize);
            if (!inBounds(img, off, qint64(sb.blockSize))) {
                setErr(error, QStringLiteral("目录块超出镜像范围"));
                return false;
            }
            if (!parseDirBuffer(img, off, qint64(sb.blockSize),
                                (sb.featureIncompat & kIncompatFiletype) != 0,
                                sb.blockSize, out, error))
                return false;
        }
    }
    return true;
}

bool listDirRec(const QByteArray &img, const SuperBlock &sb, quint64 nid,
                const QString &prefix, int depth, QSet<quint64> &visitedDirs,
                QList<imgfs::FsEntry> &out, QString *error)
{
    if (depth > kMaxDirDepth) {
        setErr(error, QStringLiteral("目录深度超限（>%1）").arg(kMaxDirDepth));
        return false;
    }
    if (visitedDirs.contains(nid)) {
        setErr(error, QStringLiteral("目录 inode %1 重复出现（镜像损坏）").arg(nid));
        return false;
    }
    visitedDirs.insert(nid);

    Inode ino;
    if (!readInode(img, sb, nid, ino, error))
        return false;
    if ((ino.mode & kModeDir) != kModeDir) {
        setErr(error, QStringLiteral("根 inode %1 不是目录").arg(nid));
        return false;
    }

    QList<Dirent> entries;
    if (!readDirEntries(img, sb, ino, entries, error))
        return false;

    for (const Dirent &e : entries) {
        if (e.name == "." || e.name == "..")
            continue;
        const QString path = prefix.isEmpty()
                ? QString::fromUtf8(e.name.constData(), e.name.size())
                : prefix + QLatin1Char('/') +
                  QString::fromUtf8(e.name.constData(), e.name.size());

        Inode child;
        if (!readInode(img, sb, e.nid, child, error))
            return false;

        imgfs::FsEntry fe;
        fe.path = path;
        fe.isDir = (child.mode & kModeDir) == kModeDir;
        fe.size = child.size;
        out.append(fe);

        if (fe.isDir) {
            if (!listDirRec(img, sb, e.nid, path, depth + 1, visitedDirs,
                            out, error))
                return false;
        }
    }
    return true;
}

// 按路径找到目标 inode（中间组件必须是目录）
bool walkPath(const QByteArray &img, const SuperBlock &sb, const QString &path,
              quint64 &nid, QString *error)
{
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        setErr(error, QStringLiteral("空路径"));
        return false;
    }
    nid = sb.rootInode;
    for (int i = 0; i < parts.size(); ++i) {
        const QString &part = parts.at(i);
        if (part == QLatin1String(".") || part == QLatin1String("..")) {
            setErr(error, QStringLiteral("路径包含 '.' 或 '..' 组件"));
            return false;
        }
        const QByteArray name = part.toUtf8();

        Inode ino;
        if (!readInode(img, sb, nid, ino, error))
            return false;
        if ((ino.mode & kModeDir) != kModeDir) {
            setErr(error, QStringLiteral("路径 '%1' 的中间组件不是目录").arg(path));
            return false;
        }
        QList<Dirent> entries;
        if (!readDirEntries(img, sb, ino, entries, error))
            return false;

        bool found = false;
        for (const Dirent &e : entries) {
            if (e.name == name) {
                nid = e.nid;
                found = true;
                break;
            }
        }
        if (!found) {
            setErr(error, QStringLiteral("路径不存在: %1").arg(path));
            return false;
        }
    }
    return true;
}

// 写小端（调用方保证边界；用于 replaceFile 的原地修改）
void put16(QByteArray &d, qint64 off, quint16 v)
{
    d[int(off)] = char(v & 0xFF);
    d[int(off + 1)] = char((v >> 8) & 0xFF);
}
void put32(QByteArray &d, qint64 off, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        d[int(off + i)] = char((v >> (i * 8)) & 0xFF);
}

// 读块位图并找连续空闲块（runs 按需取用；块号必须 < blockCount，
// 非 64BIT 时块号 ≤ 0xFFFFFFFF）
bool findFreeRuns(const QByteArray &img, const SuperBlock &sb, quint32 needBlocks,
                  QList<Extent> &runs, QString *error)
{
    const quint64 groups = (sb.inodeCount + sb.inodesPerGroup - 1) / sb.inodesPerGroup;
    const bool is64 = (sb.featureIncompat & kIncompat64Bit) != 0;
    quint32 remaining = needBlocks;
    for (quint64 g = 0; g < groups && remaining > 0; ++g) {
        GroupDesc gd;
        if (!groupDesc(img, sb, g, gd, error))
            return false;
        if (gd.blockBitmap > (Q_UINT64_C(0x7FFFFFFF) / sb.blockSize)) {
            setErr(error, QStringLiteral("块位图块号过大"));
            return false;
        }
        const qint64 bmOff = qint64(gd.blockBitmap * sb.blockSize);
        // 组内块数（末组可能不满）
        const quint64 first = g * quint64(sb.blocksPerGroup);
        const quint64 inGroup = qMin<quint64>(quint64(sb.blocksPerGroup),
                                              sb.blockCount > first ? sb.blockCount - first : 0);
        const qint64 bmBytes = qint64((inGroup + 7) / 8);
        if (!inBounds(img, bmOff, bmBytes)) {
            setErr(error, QStringLiteral("块位图超出镜像范围"));
            return false;
        }
        const uchar *bm = reinterpret_cast<const uchar *>(img.constData()) + bmOff;
        // 逐位扫描空闲（0 = 空闲）
        quint64 i = 0;
        while (i < inGroup && remaining > 0) {
            if (!(bm[i >> 3] & (0x80u >> (i & 7)))) {   // 空闲
                quint64 runLen = 0;
                while (i + runLen < inGroup &&
                       !(bm[(i + runLen) >> 3] & (0x80u >> ((i + runLen) & 7))))
                    ++runLen;
                // 每段 ≤ 32767 块（ee_len 低 15 位可写上限）：超长空闲区拆成
                // 多段，剩余部分下次迭代继续取（i += take 而非 runLen）。
                // 旧实现直接取整段 → put16 截断 ee_len → 读回 0 → 自校验失败
                const quint32 take = quint32(qMin<quint64>(
                    qMin<quint64>(runLen, remaining), kExtMaxWritableLen));
                const quint64 block = first + i;
                if (!is64 && block > 0xFFFFFFFFull) {
                    setErr(error, QStringLiteral("非 64BIT 文件系统空闲块超出 32 位"));
                    return false;
                }
                Extent ex;
                ex.lblock = 0;   // 占位（后续重排）
                ex.len = take;
                ex.pblock = block;
                runs.append(ex);
                remaining -= take;
                i += take;
            } else {
                ++i;
            }
        }
    }
    if (remaining != 0) {
        setErr(error, QStringLiteral("镜像空闲块不足（需 %1 块）").arg(needBlocks));
        return false;
    }
    return true;
}

// 位图置位/清位（块号必须合法；freeDelta 记录该位图所属组的净变化）
bool setBitmapBit(QByteArray &img, const SuperBlock &sb, quint64 group,
                  quint64 block, bool used, QMap<quint64, qint64> &freeDelta,
                  QString *error)
{
    if (block >= sb.blockCount) {
        setErr(error, QStringLiteral("块号 %1 越界").arg(block));
        return false;
    }
    GroupDesc gd;
    if (!groupDesc(img, sb, group, gd, error))
        return false;
    if (gd.blockBitmap > (Q_UINT64_C(0x7FFFFFFF) / sb.blockSize)) {
        setErr(error, QStringLiteral("块位图块号过大"));
        return false;
    }
    const qint64 bmOff = qint64(gd.blockBitmap * sb.blockSize);
    const quint64 bit = block - group * quint64(sb.blocksPerGroup);
    const qint64 byte = bmOff + qint64(bit >> 3);
    if (!inBounds(img, byte, 1)) {
        setErr(error, QStringLiteral("块位图字节越界"));
        return false;
    }
    const quint8 mask = 0x80u >> (bit & 7);
    uchar &b = *(reinterpret_cast<uchar *>(img.data()) + byte);
    const bool wasUsed = (b & mask) != 0;
    if (used == wasUsed)
        return true;                    // 幂等
    if (used)
        b = uchar(b | mask);
    else
        b = uchar(b & ~mask);
    freeDelta[group] = freeDelta.value(group, 0) + (used ? -1 : 1);
    return true;
}

// 更新镜像内空闲块计数（superblock + 块组描述符），不维护 metadata_csum
void updateFreeCounts(QByteArray &img, const SuperBlock &sb,
                      const QMap<quint64, qint64> &freeDelta)
{
    if (freeDelta.isEmpty())
        return;
    qint64 total = 0;
    for (auto it = freeDelta.begin(); it != freeDelta.end(); ++it) {
        total += it.value();
        // 组描述符 @12 为 16 位计数（64 字节描述符 +44 为高 16 位），
        // 合并 (hi<<16)|lo 整体加减 delta 后再拆分 —— 旧实现 lo/hi 各加一次
        // delta（计数被加了两次），且不处理低 16 位借位
        const quint64 gdtBlock = (sb.blockSize == 1024) ? 2 : 1;
        const qint64 dOff = qint64(gdtBlock * quint64(sb.blockSize) +
                                   it.key() * quint64(sb.descSize));
        const qint64 need = (sb.descSize >= 48) ? 46 : 14;
        if (!inBounds(img, dOff, need))
            continue;
        const uchar *gp = reinterpret_cast<const uchar *>(img.constData()) + dOff;
        qint64 combined = le16p(gp + 12);
        if (sb.descSize >= 48)
            combined |= qint64(le16p(gp + 44)) << 16;
        combined += it.value();
        combined = qMax<qint64>(0, qMin<qint64>(0xFFFFFFFFLL, combined));
        put16(img, dOff + 12, quint16(combined & 0xFFFF));
        if (sb.descSize >= 48)
            put16(img, dOff + 44, quint16((combined >> 16) & 0xFFFF));
    }
    // superblock s_free_blocks_count_lo @12（64BIT 时并入 @332 高 32 位）
    qint64 sbLo = le32p(reinterpret_cast<const uchar *>(img.constData()) + kOffFreeBlocksLo);
    qint64 sbHi = 0;
    if (sb.featureIncompat & kIncompat64Bit) {
        quint32 hi;
        if (readU32(img, kSuperOffset + 332, hi))
            sbHi = qint64(hi);
    }
    sbLo += total;
    if (sb.featureIncompat & kIncompat64Bit) {
        // 低 32 位溢出进位到高 32 位
        while (sbLo < 0 || sbLo > 0xFFFFFFFFull) {
            if (sbLo < 0) { sbLo += 0x100000000ull; --sbHi; }
            else { sbLo -= 0x100000000ull; ++sbHi; }
        }
        put32(img, kOffFreeBlocksLo, quint32(sbLo));
        put32(img, kSuperOffset + 332, quint32(sbHi & 0xFFFFFFFFu));
    } else {
        put32(img, kOffFreeBlocksLo, quint32(qMax<qint64>(0, sbLo) & 0xFFFFFFFFu));
    }
}

} // namespace

bool isExt4(const QByteArray &image)
{
    if (image.size() < kOffMagic + 2)
        return false;
    const auto *p = reinterpret_cast<const uchar *>(image.constData()) + kOffMagic;
    return p[0] == static_cast<uchar>(kExt4Magic & 0xFF)
        && p[1] == static_cast<uchar>(kExt4Magic >> 8);
}

bool parseSuper(const QByteArray &image, SuperBlock &out)
{
    if (!isExt4(image))
        return false;
    if (image.size() < kSuperMinLen)
        return false;
    const uchar *p = reinterpret_cast<const uchar *>(image.constData());

    const quint32 logBlockSize = le32p(p + kOffLogBlockSize);
    if (logBlockSize > kMaxLogBlockSize)
        return false;
    const quint64 blockSize = quint64(1024) << logBlockSize;

    const quint32 inodeSize = le16p(p + kOffInodeSize);
    if (inodeSize < kGoodOldInodeSize || inodeSize > blockSize)
        return false;

    const quint64 inodeCount = le32p(p + kOffInodesCount);
    if (inodeCount == 0)
        return false;

    quint64 blockCount = le32p(p + kOffBlocksCountLo);
    const quint32 incompat = le32p(p + kOffFeatureInc);
    if (incompat & kIncompat64Bit)
        blockCount |= quint64(le32p(p + kOffBlocksHi)) << 32;
    if (blockCount == 0)
        return false;

    const quint32 inodesPerGroup = le32p(p + kOffInodesPerGrp);
    const quint32 blocksPerGroup = le32p(p + kOffBlocksPerGrp);
    if (inodesPerGroup == 0 || blocksPerGroup == 0)
        return false;

    // 内核 ext4_fill_super：64BIT → desc_size ≥ 64；否则恒 32
    quint32 descSize = le16p(p + kOffDescSize);
    if (incompat & kIncompat64Bit)
        descSize = qMax<quint32>(descSize, 64);
    else
        descSize = 32;

    out.blockSize = static_cast<quint32>(blockSize);
    out.inodeCount = inodeCount;
    out.blockCount = blockCount;
    out.inodeSize = inodeSize;
    out.rootInode = 2;
    out.inodesPerGroup = inodesPerGroup;
    out.blocksPerGroup = blocksPerGroup;
    out.firstDataBlock = le32p(p + kOffFirstDataBlk);
    out.descSize = descSize;
    out.featureIncompat = incompat;
    return true;
}

bool listTree(const QByteArray &image, const SuperBlock &sb,
              QList<imgfs::FsEntry> &out, QString *error)
{
    if (error)
        error->clear();
    out.clear();
    if (!validSuper(sb, error))
        return false;
    QSet<quint64> visitedDirs;
    return listDirRec(image, sb, sb.rootInode, QString(), 0, visitedDirs, out, error);
}

bool extractFile(const QByteArray &image, const SuperBlock &sb,
                 const QString &path, QByteArray &data, QString *error)
{
    if (error)
        error->clear();
    data.clear();
    if (!validSuper(sb, error))
        return false;

    quint64 nid;
    if (!walkPath(image, sb, path, nid, error))
        return false;
    Inode ino;
    if (!readInode(image, sb, nid, ino, error))
        return false;
    if ((ino.mode & kModeDir) == kModeDir) {
        setErr(error, QStringLiteral("'%1' 是一个目录").arg(path));
        return false;
    }
    if ((ino.mode & kModeSymlink) == kModeSymlink) {
        // 快速符号链接（<60B）：目标在 i_block@40；否则按数据块读
        if (ino.size < quint64(kMinInlineDataSize) && !(ino.flags & kFlInlineData)) {
            if (!inBounds(image, ino.offset + kInBlock, qint64(ino.size))) {
                setErr(error, QStringLiteral("符号链接目标越界"));
                return false;
            }
            data = image.mid(ino.offset + kInBlock, qint64(ino.size));
            return true;
        }
        return readFileData(image, sb, ino, data, error);
    }
    if ((ino.mode & kModeReg) != kModeReg) {
        setErr(error, QStringLiteral("'%1' 不是普通文件").arg(path));
        return false;
    }
    return readFileData(image, sb, ino, data, error);
}

bool replaceFile(QByteArray &image, const SuperBlock &sb,
                 const QString &path, const QByteArray &data, QString *error)
{
    if (error)
        error->clear();
    if (!validSuper(sb, error))
        return false;
    if (!isExt4(image)) {
        setErr(error, QStringLiteral("不是 ext4 镜像"));
        return false;
    }
    // metadata_csum 镜像：替换会改写 inode/组描述符/superblock，其 crc32c 校验和
    // （ext4_csum：种子 = s_uuid + 组号，e2fsprogs lib/ext2fs/csum.c）随之失效 →
    // 内核挂载/读取报校验错误。重算校验和未实现前，对替换明确拒绝（比"改完
    // 不可挂载"安全）；读路径 listTree/extractFile 不校验校验和，不受影响。
    if (image.size() >= kOffFeatureRoCompat + 4 &&
        (le32p(reinterpret_cast<const uchar *>(image.constData()) + kOffFeatureRoCompat)
         & kRoCompatMetaCsum)) {
        setErr(error, QStringLiteral("metadata_csum 镜像暂不支持替换"));
        return false;
    }

    quint64 nid;
    if (!walkPath(image, sb, path, nid, error))
        return false;
    Inode ino;
    if (!readInode(image, sb, nid, ino, error))
        return false;
    if ((ino.mode & kModeDir) == kModeDir) {
        setErr(error, QStringLiteral("'%1' 是目录，不能替换").arg(path));
        return false;
    }
    if ((ino.mode & kModeSymlink) == kModeSymlink) {
        setErr(error, QStringLiteral("'%1' 是符号链接，不能替换").arg(path));
        return false;
    }
    if (!(ino.flags & (kFlExtents | kFlInlineData))) {
        setErr(error, QStringLiteral("'%1' 未使用 extent/inline（legacy block map 暂不支持）")
                       .arg(path));
        return false;
    }

    QList<Extent> extents;
    quint64 capacity = 0;
    if (!(ino.flags & kFlInlineData)) {
        if (!readExtents(image, sb, ino, extents, error))
            return false;
        for (const Extent &ex : extents)
            capacity += quint64(ex.len) * sb.blockSize;
    }

    const quint64 newSize = quint64(data.size());
    bool needsGrow = !(ino.flags & kFlInlineData)
            ? (newSize > capacity)
            : (newSize > quint64(kMinInlineDataSize));

    // 原地写前置校验：extent 必须从逻辑块 0 起连续覆盖 ceil(newSize/blk) 块。
    // 含空洞/非零起点的稀疏文件（如 {lb0, lb2}）逐 extent 顺序写后，内核视角下
    // 空洞处读零、后续块落位错乱 —— 而自校验按 extent 拼接读取不建模空洞，
    // 恰好通过 → 静默错误成功。不满足连续覆盖 → 改走增长路径重建稠密树。
    if (!needsGrow && !(ino.flags & kFlInlineData)) {
        const quint64 needBlocks = (newSize + sb.blockSize - 1) / sb.blockSize;
        quint64 covered = 0;
        for (const Extent &ex : extents) {
            if (covered >= needBlocks)
                break;                      // 前缀已连续覆盖
            if (ex.lblock != covered) {     // 空洞或非零起点 → 稀疏
                needsGrow = true;
                break;
            }
            covered += ex.len;
        }
    }

    if (!needsGrow) {
        // ---- 原地写（extent 覆盖或 inline ≤60B），只更新 i_size ----
        if (ino.flags & kFlInlineData) {
            if (!inBounds(image, ino.offset + kInBlock, qint64(newSize))) {
                setErr(error, QStringLiteral("inline 数据区越界"));
                return false;
            }
            if (!data.isEmpty())
                memcpy(image.data() + ino.offset + kInBlock, data.constData(), qint64(newSize));
            // 清掉剩余 inline 区（避免残留旧数据）
            const qint64 zeroFrom = ino.offset + kInBlock + qint64(newSize);
            const qint64 zeroLen = qint64(kMinInlineDataSize) - qint64(newSize);
            if (zeroLen > 0 && inBounds(image, zeroFrom, zeroLen))
                memset(image.data() + zeroFrom, 0, zeroLen);
        } else {
            // 逐 extent 覆盖；尾部（newSize..capacity）清零
            quint64 pos = 0;
            for (const Extent &ex : extents) {
                const quint64 avail = quint64(ex.len) * sb.blockSize;
                const qint64 off = qint64(ex.pblock * sb.blockSize);
                const quint64 head = qMin(avail, newSize > pos ? newSize - pos : 0);
                if (head > 0) {
                    if (!inBounds(image, off, qint64(head))) {
                        setErr(error, QStringLiteral("替换数据越界"));
                        return false;
                    }
                    memcpy(image.data() + off, data.constData() + qint64(pos), qint64(head));
                }
                const qint64 zeroFrom = off + qint64(head);
                const qint64 zeroLen = qint64(avail - head);
                if (zeroLen > 0 && inBounds(image, zeroFrom, zeroLen))
                    memset(image.data() + zeroFrom, 0, zeroLen);
                pos += avail;
            }
        }
        // i_size_lo @4；inodeSize>128 时并入 i_size_high @108（内核读侧同规则）
        put32(image, ino.offset + kInSizeLo, quint32(newSize & 0xFFFFFFFFu));
        if (sb.inodeSize > kGoodOldInodeSize)
            put32(image, ino.offset + kInSizeHigh, quint32(newSize >> 32));
        // 自校验：重新读 inode（i_size 已更新）并重新解析比较
        Inode fresh;
        if (!readInode(image, sb, nid, fresh, error))
            return false;
        QByteArray check;
        if (!readFileData(image, sb, fresh, check, error))
            return false;
        if (check != data) {
            setErr(error, QStringLiteral("替换后自校验失败"));
            return false;
        }
        return true;
    }

    // ---- 增长：分配空闲块、重建 extent 树 ----
    const quint64 needBytes = newSize;
    const quint32 needBlocks = quint32((needBytes + sb.blockSize - 1) / sb.blockSize);
    QList<Extent> runs;
    if (!findFreeRuns(image, sb, needBlocks, runs, error))
        return false;
    // 根 extent 头可容纳 (60-12)/12 = 4 个 extent
    if (runs.size() > 4) {
        setErr(error, QStringLiteral("空闲块过于碎片化（>4 段）"));
        return false;
    }
    // 先行校验（提交前，不修改镜像）：根 extent 区域与全部新分配块数据区必须落在
    // 镜像内 —— 若等步骤 3/5 才发现越界，位图/树已被改写 → "已污染再报错"。
    // findFreeRuns 已保证 run.pblock < blockCount，此处补乘法溢出与镜像边界。
    const qint64 root = ino.offset + kInBlock;
    if (!inBounds(image, root, 12 + 12 * runs.size())) {
        setErr(error, QStringLiteral("extent 根区域越界"));
        return false;
    }
    for (const Extent &run : runs) {
        if (run.pblock > (Q_UINT64_C(0x7FFFFFFFFFFFFFFF) / sb.blockSize) ||
            !inBounds(image, qint64(run.pblock * sb.blockSize),
                      qint64(run.len) * sb.blockSize)) {
            setErr(error, QStringLiteral("新分配块越界"));
            return false;
        }
    }

    // 1) 释放旧 extent 数据块
    // 注：仅回收叶 extent 覆盖的数据块；原 depth>0 树的索引块未单独追踪，
    // 保持位图占用（不重复分配即可，属保守选择，不影响正确性）
    QMap<quint64, qint64> freeDelta;
    if (!(ino.flags & kFlInlineData)) {
        for (const Extent &ex : extents) {
            for (quint32 i = 0; i < ex.len; ++i) {
                const quint64 block = ex.pblock + i;
                if (block >= sb.blockCount)   // 旧树越界块：不动位图
                    continue;
                const quint64 group = block / sb.blocksPerGroup;
                if (!setBitmapBit(image, sb, group, block, false, freeDelta, error))
                    return false;
            }
        }
    }
    // 2) 占用新块
    quint32 lblock = 0;
    for (const Extent &run : runs) {
        for (quint32 i = 0; i < run.len; ++i) {
            const quint64 block = run.pblock + i;
            const quint64 group = block / sb.blocksPerGroup;
            if (!setBitmapBit(image, sb, group, block, true, freeDelta, error))
                return false;
        }
        lblock += run.len;
    }

    // 3) 重建 extent 树（root 头 + 每段一个 extent；区域已在提交前校验）
    put16(image, root, quint16(kExtMagic & 0xFFFF));
    put16(image, root + 2, quint16(runs.size()));
    put16(image, root + 4, 4);                       // eh_max
    image[int(root + 6)] = char(0);                  // eh_depth
    put32(image, root + 8, 0);                       // eh_generation
    quint32 pos = 0;
    for (int i = 0; i < runs.size(); ++i) {
        const qint64 e = root + 12 + qint64(i) * 12;
        put32(image, e, pos);                        // ee_block（连续逻辑块）
        put16(image, e + 4, quint16(runs[i].len));   // ee_len（≤32768）
        put16(image, e + 6, quint16((runs[i].pblock >> 32) & 0xFFFF));   // ee_start_hi
        put32(image, e + 8, quint32(runs[i].pblock & 0xFFFFFFFFu));      // ee_start_lo
        pos += runs[i].len;
    }

    // 4) inode 更新：flags（inline→extent 转换）、i_size、i_blocks
    quint32 flags = ino.flags;
    if (flags & kFlInlineData)
        flags = (flags & ~kFlInlineData) | kFlExtents;
    put32(image, ino.offset + kInFlags, flags);
    put32(image, ino.offset + kInSizeLo, quint32(newSize & 0xFFFFFFFFu));
    if (sb.inodeSize > kGoodOldInodeSize)
        put32(image, ino.offset + kInSizeHigh, quint32(newSize >> 32));
    const quint64 allocBytes = quint64(needBlocks) * sb.blockSize;
    put32(image, ino.offset + kInBlocks, quint32(allocBytes / 512));  // i_blocks（512B 单位）

    // 5) 写数据 + 更新空闲计数
    quint64 pos2 = 0;
    for (const Extent &run : runs) {
        const qint64 off = qint64(run.pblock * sb.blockSize);
        const qint64 len = qint64(run.len) * sb.blockSize;
        if (!inBounds(image, off, len)) {
            setErr(error, QStringLiteral("新分配块越界"));
            return false;
        }
        const qint64 head = qint64(qMin<quint64>(quint64(len), newSize - pos2));
        if (head > 0)
            memcpy(image.data() + off, data.constData() + qint64(pos2), head);
        if (len - head > 0)
            memset(image.data() + off + head, 0, len - head);
        pos2 += quint64(len);
    }
    updateFreeCounts(image, sb, freeDelta);

    // 6) 自校验：重新读 inode（i_size/flags 已更新）并重新解析比较
    Inode fresh;
    if (!readInode(image, sb, nid, fresh, error))
        return false;
    QByteArray check;
    if (!readFileData(image, sb, fresh, check, error))
        return false;
    if (check != data) {
        setErr(error, QStringLiteral("替换后自校验失败"));
        return false;
    }
    return true;
}

QByteArray repack(const QByteArray &image, const SuperBlock &sb)
{
    // 校验后返回完整镜像（调用方把 replaceFile 修改过的副本传入）
    if (!isExt4(image))
        return QByteArray();
    SuperBlock check;
    if (!parseSuper(image, check))
        return QByteArray();
    if (check.blockSize != sb.blockSize || check.inodeSize != sb.inodeSize ||
        check.inodesPerGroup != sb.inodesPerGroup ||
        check.blocksPerGroup != sb.blocksPerGroup)
        return QByteArray();
    return image;
}

} // namespace imgext4
