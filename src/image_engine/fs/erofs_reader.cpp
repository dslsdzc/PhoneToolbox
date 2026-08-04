#include "image_engine/fs/erofs_reader.h"

// 布局对照 erofs-utils master（include/erofs_fs.h、lib/inode.c、lib/namei.c）与
// Linux 6.6+ fs/erofs/{erofs_fs.h,inode.c,dir.c} 逐字段核实（mkfs.erofs 输出即此布局）：
//
//   superblock（EROFS_SUPER_OFFSET=1024，相对 superblock 起点）:
//     +0 magic | +12 blkszbits u8 | +14 rootnid_2b le16（48BIT 时此字段为 blocks_hi）
//     +40 meta_blkaddr le32 | +80 feature_incompat le32 | +84 available_compr_algs le16
//     +112 rootnid_8b le64（48BIT 且非 0 时使用）
//
//   inode 定位：iloc = meta_blkaddr*blockSize + nid*32。
//   槽大小恒为 32B（erofs-utils EROFS_ISLOTBITS=5；内核 super.c
//   islotbits = ilog2(sizeof(struct erofs_inode_compact))），与 48BIT 无关。
//
//   struct erofs_inode_compact（32B，__packed，小端）:
//     +0 le16 i_format（bit0 = 版本 0 compact / 1 extended；bit1-3 = datalayout：
//       0 FLAT_PLAIN / 1 COMPRESSED_FULL / 2 FLAT_INLINE / 3 COMPRESSED_COMPACT /
//       4 CHUNK_BASED；bit4 = NLINK_1（非目录）/ DOT_OMITTED（目录））
//     +2 le16 i_xattr_icount | +4 le16 i_mode | +6 le16 i_nb（nlink 或 startblk_hi）
//     +8 le32 i_size | +12 le32 i_mtime | +16 le32 i_u（startblk_lo / blocks_lo / rdev）
//     +20 le32 i_ino | +24 le16 i_uid | +26 le16 i_gid | +28 le32 i_reserved
//
//   struct erofs_inode_extended（64B）:
//     +0 i_format | +2 i_xattr_icount | +4 i_mode | +6 i_nb
//     +8 le64 i_size | +16 le32 i_u | +20 le32 i_ino
//     +24 le32 i_uid | +28 le32 i_gid | +32 le64 i_mtime | +40 le32 i_mtime_nsec
//     +44 le32 i_nlink | +48 i_reserved2[16]
//
//   startblk = le32(i_u) | (le16(i_nb) << 32)；非 48BIT 时取 32 位
//   （内核/erofs-utils addrmask 逻辑）。
//
//   xattr ibody（inode 之后）：erofs_xattr_ibody_header 12B + (icount-1)*4B
//   （erofs_xattr_ibody_size：icount==0 时为 0）。
//
//   FLAT_PLAIN 数据 = startblk*blockSize 起 i_size 字节；
//   FLAT_INLINE 前 (i_size/blksz)*blksz 字节在数据区，尾部 i_size%blksz 内联于
//   iloc + inode_isize + xattr_isize 之后（mkfs 在 i_size%blksz==0 时降级为 PLAIN，
//   故 INLINE 必有非空尾部）。
//
//   struct erofs_dirent（12B，__packed）:
//     +0 le64 nid | +8 le16 nameoff（名字区偏移，块内相对）| +10 u8 file_type | +11 u8 resv
//   目录块 = dirent 定长数组 [0, nameoff0) + 名字区 [nameoff0, 块尾)；
//   de[0].nameoff 即名字区起点（mkfs 中 = N*12），名字在区内连续存放，
//   末尾 dirent 的名字长度以块内剩余为界的 strnlen 界定。

#include <QSet>

namespace imgerofs {

namespace {

constexpr quint32 kSuperOffset = 1024;        // EROFS_SUPER_OFFSET
constexpr quint32 kMagic = 0xE0F5E1E2;        // EROFS_SUPER_MAGIC_V1

// 相对 superblock 起点的字段偏移（内核 erofs_fs.h）
constexpr quint32 kOffBlkszbits       = 12;   // __u8
constexpr quint32 kOffRootNid         = 14;   // __le16
constexpr quint32 kOffMetaBlkAddr     = 40;   // __le32
constexpr quint32 kOffFeatureIncompat = 80;   // __le32
constexpr quint32 kOffComprAlgs       = 84;   // __le16
constexpr quint32 kOffRootNid8b       = 112;  // __le64 (48BIT)

// EROFS_FEATURE_INCOMPAT_*（erofs-utils erofs_fs.h）
constexpr quint32 kFeatIncompat48Bit   = 0x00000080;
constexpr quint32 kFeatIncompatMetabox = 0x00000100;

// inode 槽恒为 32B（erofs-utils EROFS_ISLOTBITS=5 / 内核 islotbits）
constexpr quint32 kInodeSlotSize    = 32;
constexpr quint32 kInodeCompactSize  = 32;
constexpr quint32 kInodeExtendedSize = 64;
constexpr quint32 kDirentSize  = 12;   // sizeof(struct erofs_dirent)
constexpr quint32 kXattrHeaderSize = 12;   // sizeof(struct erofs_xattr_ibody_header)
constexpr quint32 kMaxNameLen = 255;   // EROFS_NAME_LEN
constexpr quint64 kDirentNidMask = (quint64(1) << 63) - 1;  // 屏蔽 metabox 标志位
constexpr int kMaxDirDepth = 128;      // 目录递归深度上限（防恶意超深链栈溢出）

// i_format 位域（erofs_fs.h）
constexpr quint32 kFormatVersionBit    = 0;   // 0 = compact, 1 = extended
constexpr quint32 kFormatDatalayoutBit = 1;
constexpr quint32 kFormatDatalayoutMask = 0x7;
constexpr quint32 kFormatNlink1Bit = 4;      // 非目录：i_nb = startblk_hi
constexpr quint32 kFormatAllMask = 0x1F;     // EROFS_I_ALL

// erofs inode datalayout（i_format bit1-3）
constexpr quint32 kLayoutFlatPlain        = 0;
constexpr quint32 kLayoutCompressedFull   = 1;
constexpr quint32 kLayoutFlatInline       = 2;
constexpr quint32 kLayoutCompressedCompact = 3;
constexpr quint32 kLayoutChunkBased       = 4;

// S_IFMT 判定（i_mode 高 4 位）
constexpr quint32 kModeTypeMask = 0xF000;
constexpr quint32 kModeTypeDir  = 0x4000;   // S_IFDIR
constexpr quint32 kModeTypeReg  = 0x8000;   // S_IFREG

quint16 le16(const QByteArray &d, quint64 off)
{
    return quint16(uchar(d.at(qsizetype(off)))) |
           (quint16(uchar(d.at(qsizetype(off + 1)))) << 8);
}
quint32 le32(const QByteArray &d, quint64 off)
{
    return quint32(uchar(d.at(qsizetype(off)))) |
           (quint32(uchar(d.at(qsizetype(off + 1)))) << 8) |
           (quint32(uchar(d.at(qsizetype(off + 2)))) << 16) |
           (quint32(uchar(d.at(qsizetype(off + 3)))) << 24);
}
quint64 le64(const QByteArray &d, quint64 off)
{
    quint64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= quint64(uchar(d.at(qsizetype(off + i)))) << (i * 8);
    return v;
}

// 基于裸指针的小端读取（目录块解析用）
quint16 le16p(const uchar *p)
{
    return quint16(p[0]) | (quint16(p[1]) << 8);
}
quint64 le64p(const uchar *p)
{
    quint64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= quint64(p[i]) << (i * 8);
    return v;
}

void setErr(QString *error, const QString &msg)
{
    if (error)
        *error = msg;
}

bool inBounds(const QByteArray &img, quint64 off, quint64 len)
{
    const quint64 size = quint64(img.size());
    return off <= size && len <= size - off;
}

// 目录名字区内的有界 strnlen（跨平台，不依赖 POSIX strnlen）
quint32 boundedStrlen(const char *s, quint32 max)
{
    quint32 n = 0;
    while (n < max && s[n] != '\0')
        ++n;
    return n;
}

bool validSuper(const SuperBlock &sb, QString *error)
{
    if (sb.blockSize < 512 || sb.blockSize > 65536) {   // blkszbits 9..16
        setErr(error, QStringLiteral("无效的 EROFS blockSize %1").arg(sb.blockSize));
        return false;
    }
    return true;
}

struct ErofsInode {
    quint32 format = 0;
    quint32 datalayout = 0;
    quint32 mode = 0;
    quint64 size = 0;
    quint64 startBlk = 0;
    quint32 xattrIcount = 0;
    quint32 inodeSize = kInodeCompactSize;
    bool isDir() const { return (mode & kModeTypeMask) == kModeTypeDir; }
    bool isReg() const { return (mode & kModeTypeMask) == kModeTypeReg; }
};

// iloc = meta_blkaddr*blockSize + nid*32（nid 屏蔽 metabox 标志位）
bool inodeIloc(const QByteArray &img, const SuperBlock &sb, quint64 nid,
               quint64 &iloc, QString *error)
{
    const quint64 base = quint64(sb.metaBlkAddr) * sb.blockSize;
    const quint64 n = nid & kDirentNidMask;
    if (n > (Q_UINT64_C(0xFFFFFFFFFFFFFFFF) - base) / kInodeSlotSize) {
        setErr(error, QStringLiteral("nid %1 越界").arg(nid));
        return false;
    }
    iloc = base + n * kInodeSlotSize;
    if (!inBounds(img, iloc, kInodeCompactSize)) {
        setErr(error, QStringLiteral("inode nid %1 超出镜像范围").arg(nid));
        return false;
    }
    return true;
}

bool readInode(const QByteArray &img, const SuperBlock &sb, quint64 nid,
               ErofsInode &ino, QString *error)
{
    quint64 iloc;
    if (!inodeIloc(img, sb, nid, iloc, error))
        return false;

    const quint32 fmt = le16(img, iloc);
    if (fmt & ~kFormatAllMask) {   // 内核/erofs-utils: unsupported i_format
        setErr(error, QStringLiteral("nid %1 的 i_format 不支持").arg(nid));
        return false;
    }
    ino.format = fmt;
    ino.datalayout = (fmt >> kFormatDatalayoutBit) & kFormatDatalayoutMask;
    if (ino.datalayout > kLayoutChunkBased) {
        setErr(error, QStringLiteral("nid %1 的数据布局不支持").arg(nid));
        return false;
    }

    const bool extended = (fmt & (1u << kFormatVersionBit)) != 0;
    ino.inodeSize = extended ? kInodeExtendedSize : kInodeCompactSize;
    if (!inBounds(img, iloc, ino.inodeSize)) {
        setErr(error, QStringLiteral("inode nid %1 不完整").arg(nid));
        return false;
    }

    ino.xattrIcount = le16(img, iloc + 2);
    ino.mode = le16(img, iloc + 4);

    if (extended) {
        ino.size = le64(img, iloc + 8);
        ino.startBlk = quint64(le32(img, iloc + 16)) |
                       (quint64(le16(img, iloc + 6)) << 32);
    } else {
        ino.size = le32(img, iloc + 8);
        const bool nlink1 = !ino.isDir() && (fmt & (1u << kFormatNlink1Bit)) != 0;
        if (nlink1) {   // EROFS_I_NLINK_1_BIT: i_nb = startblk_hi, nlink = 1
            ino.startBlk = quint64(le32(img, iloc + 16)) |
                           (quint64(le16(img, iloc + 6)) << 32);
        } else {        // 其余（目录/常规 compact 文件）：i_nb = nlink，地址仅 32 位
            ino.startBlk = le32(img, iloc + 16);
        }
    }
    // 注：extended / NLINK_1 分支恒组合 48 位地址（与 erofs-utils master
    // lib/inode.c erofs_read_inode_from_disk 一致，不看 48BIT 特性位；
    // master 的 addrmask 仅用于 NULL_ADDR 判定，本实现不做 NULL_ADDR 特判，
    // 越界地址由后续数据区 bounds 检查兜底报错）。
    return true;
}

quint32 xattrIbodySize(const ErofsInode &ino)
{
    // erofs_xattr_ibody_size: icount==0 -> 0；否则 12 + (icount-1)*4
    return ino.xattrIcount ? kXattrHeaderSize + 4 * (ino.xattrIcount - 1) : 0;
}

// flat（未压缩）inode 数据读取；iloc 为 inode 自身偏移（INLINE 尾部定位用）。
// 压缩/块索引 inode 返回 false + 明确错误。
bool readFlatData(const QByteArray &img, const SuperBlock &sb,
                  const ErofsInode &ino, quint64 iloc,
                  QByteArray &out, QString *error)
{
    if (ino.datalayout == kLayoutCompressedFull ||
        ino.datalayout == kLayoutCompressedCompact) {
        setErr(error, QStringLiteral("文件使用压缩存储（layout %1），需要 LZ4 流式解压（尚未实现）")
                       .arg(ino.datalayout));
        return false;
    }
    if (ino.datalayout == kLayoutChunkBased) {
        setErr(error, QStringLiteral("chunk-based 文件暂不支持"));
        return false;
    }
    if (ino.datalayout != kLayoutFlatPlain && ino.datalayout != kLayoutFlatInline) {
        setErr(error, QStringLiteral("未知数据布局 %1").arg(ino.datalayout));
        return false;
    }

    const quint64 blksz = sb.blockSize;
    const quint64 inlineSize = (ino.datalayout == kLayoutFlatInline)
            ? ino.size % blksz : 0;   // INLINE 必有非空尾部（mkfs 保证）
    const quint64 dataSize = ino.size - inlineSize;

    out.clear();
    out.reserve(int(ino.size));
    if (dataSize) {
        const quint64 dataOff = ino.startBlk * blksz;
        if (!inBounds(img, dataOff, dataSize)) {
            setErr(error, QStringLiteral("文件数据区超出镜像范围（startblk %1）")
                           .arg(ino.startBlk));
            return false;
        }
        out.append(img.mid(qint64(dataOff), qint64(dataSize)));
    }
    if (inlineSize) {
        const quint64 tailOff = iloc + ino.inodeSize + xattrIbodySize(ino);
        if (!inBounds(img, tailOff, inlineSize)) {
            setErr(error, QStringLiteral("文件 inline 尾部超出镜像范围"));
            return false;
        }
        out.append(img.mid(qint64(tailOff), qint64(inlineSize)));
    }
    return true;
}

struct Dirent {
    quint64 nid;
    QByteArray name;
    quint32 fileType;
};

// 解析目录 inode 的目录块（新格式：12B 定长 dirent + nameoff 名字区；
// 目录数据本身按 flat 布局存储）。
bool readDirEntries(const QByteArray &img, const SuperBlock &sb,
                    const ErofsInode &dirIno, quint64 iloc,
                    QList<Dirent> &out, QString *error)
{
    if (dirIno.datalayout != kLayoutFlatPlain &&
        dirIno.datalayout != kLayoutFlatInline) {
        setErr(error, QStringLiteral("目录 inode 数据布局不支持（%1）")
                       .arg(dirIno.datalayout));
        return false;
    }
    QByteArray data;
    if (!readFlatData(img, sb, dirIno, iloc, data, error))
        return false;

    const quint32 blksz = sb.blockSize;
    quint64 pos = 0;
    while (pos < quint64(data.size())) {
        const quint32 count = quint32(qMin<quint64>(blksz, quint64(data.size()) - pos));
        const uchar *blk = reinterpret_cast<const uchar *>(data.constData()) + pos;
        const quint16 nameoff0 = le16p(blk + 8);   // de[0].nameoff（nameoff 字段在 dirent +8）
        // 内核/erofs-utils: nameoff 必须落在 [sizeof(dirent), 块大小) 且对齐 dirent
        if (nameoff0 < kDirentSize || nameoff0 >= count || (nameoff0 % kDirentSize) != 0) {
            setErr(error, QStringLiteral("目录块无效的 nameoff %1").arg(nameoff0));
            return false;
        }
        const uchar *end = blk + nameoff0;
        for (const uchar *de = blk; de < end; de += kDirentSize) {
            const quint16 nameoff = le16p(de + 8);
            const quint32 ft = de[10];
            if (nameoff >= count) {   // 防越界（strnlen/name 拷贝前）
                setErr(error, QStringLiteral("目录项 nameoff %1 越界").arg(nameoff));
                return false;
            }
            quint32 namelen;
            if (de + kDirentSize < end)
                namelen = quint32(le16p(de + kDirentSize + 8)) - nameoff;
            else
                namelen = boundedStrlen(reinterpret_cast<const char *>(blk + nameoff),
                                        count - nameoff);
            if (namelen < 1 || namelen > kMaxNameLen || nameoff + namelen > count) {
                setErr(error, QStringLiteral("目录项名字长度无效"));
                return false;
            }
            Dirent d;
            d.nid = le64p(de) & kDirentNidMask;
            d.name = QByteArray(reinterpret_cast<const char *>(blk + nameoff),
                                int(namelen));
            d.fileType = ft;
            out.append(d);
        }
        pos += count;
    }
    return true;
}

// 递归列出目录（visitedDirs 防目录环；depth 防无环超深链栈溢出）
bool listDirRec(const QByteArray &img, const SuperBlock &sb, quint64 nid,
                const QString &prefix, int depth, QSet<quint64> &visitedDirs,
                QList<imgfs::FsEntry> &out, QString *error)
{
    if (depth > kMaxDirDepth) {
        setErr(error, QStringLiteral("目录深度超限（>%1）").arg(kMaxDirDepth));
        return false;
    }
    const quint64 key = nid & kDirentNidMask;
    if (visitedDirs.contains(key)) {
        setErr(error, QStringLiteral("目录 nid %1 重复出现（镜像损坏）").arg(nid));
        return false;
    }
    visitedDirs.insert(key);

    ErofsInode ino;
    quint64 iloc;
    if (!inodeIloc(img, sb, nid, iloc, error) ||
        !readInode(img, sb, nid, ino, error))
        return false;
    if (!ino.isDir()) {
        setErr(error, QStringLiteral("根 nid %1 不是目录").arg(nid));
        return false;
    }

    QList<Dirent> entries;
    if (!readDirEntries(img, sb, ino, iloc, entries, error))
        return false;

    for (const Dirent &e : entries) {
        // mkfs 始终写入 "." 与 ".."，遍历时跳过
        if (e.name == "." || e.name == "..")
            continue;
        const QString path = prefix.isEmpty()
                ? QString::fromUtf8(e.name.constData(), e.name.size())
                : prefix + QLatin1Char('/') +
                  QString::fromUtf8(e.name.constData(), e.name.size());

        ErofsInode child;
        if (!readInode(img, sb, e.nid, child, error))
            return false;

        imgfs::FsEntry fe;
        fe.path = path;
        fe.isDir = child.isDir();
        fe.size = child.size;
        out.append(fe);

        if (child.isDir()) {
            if (!listDirRec(img, sb, e.nid, path, depth + 1, visitedDirs,
                            out, error))
                return false;
        }
    }
    return true;
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
    out.featureIncompat = featIncompat;
    out.metaBlkAddr = le32(image, kSuperOffset + kOffMetaBlkAddr);

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

bool listTree(const QByteArray &image, const SuperBlock &sb,
              QList<imgfs::FsEntry> &out, QString *error)
{
    if (error)
        error->clear();
    out.clear();
    if (!validSuper(sb, error))
        return false;
    if (sb.featureIncompat & kFeatIncompatMetabox) {
        setErr(error, QStringLiteral("EROFS METABOX 特性暂不支持"));
        return false;
    }
    QSet<quint64> visitedDirs;
    return listDirRec(image, sb, sb.rootNid, QString(), 0, visitedDirs, out, error);
}

bool extractFile(const QByteArray &image, const SuperBlock &sb,
                 const QString &path, QByteArray &data, QString *error)
{
    if (error)
        error->clear();
    data.clear();
    if (!validSuper(sb, error))
        return false;
    if (sb.featureIncompat & kFeatIncompatMetabox) {
        setErr(error, QStringLiteral("EROFS METABOX 特性暂不支持"));
        return false;
    }

    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        setErr(error, QStringLiteral("空路径"));
        return false;
    }

    quint64 nid = sb.rootNid;
    for (int i = 0; i < parts.size(); ++i) {
        const QString &part = parts.at(i);
        if (part == QLatin1String(".") || part == QLatin1String("..")) {
            setErr(error, QStringLiteral("路径包含 '.' 或 '..' 组件"));
            return false;
        }
        const QByteArray name = part.toUtf8();

        ErofsInode ino;
        quint64 iloc;
        if (!inodeIloc(image, sb, nid, iloc, error) ||
            !readInode(image, sb, nid, ino, error))
            return false;
        if (!ino.isDir()) {
            setErr(error, QStringLiteral("路径 '%1' 的中间组件不是目录").arg(path));
            return false;
        }

        QList<Dirent> entries;
        if (!readDirEntries(image, sb, ino, iloc, entries, error))
            return false;

        quint64 childNid = 0;
        bool found = false;
        for (const Dirent &e : entries) {
            if (e.name == name) {
                childNid = e.nid;
                found = true;
                break;
            }
        }
        if (!found) {
            setErr(error, QStringLiteral("路径不存在: %1").arg(path));
            return false;
        }
        nid = childNid;

        if (i == parts.size() - 1) {
            ErofsInode fin;
            quint64 filoc;
            if (!inodeIloc(image, sb, nid, filoc, error) ||
                !readInode(image, sb, nid, fin, error))
                return false;
            if (fin.isDir()) {
                setErr(error, QStringLiteral("'%1' 是一个目录").arg(path));
                return false;
            }
            if (!fin.isReg()) {
                setErr(error, QStringLiteral("'%1' 不是普通文件").arg(path));
                return false;
            }
            return readFlatData(image, sb, fin, filoc, data, error);
        }
    }
    setErr(error, QStringLiteral("内部错误"));
    return false;
}

} // namespace imgerofs
