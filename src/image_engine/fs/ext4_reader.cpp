#include "image_engine/fs/ext4_reader.h"

#include <QtEndian>

namespace imgext4 {

namespace {

// superblock 位于文件偏移 1024；以下偏移均为 superblock 内偏移（相对 1024）。
constexpr int kSuperOffset = 1024;
constexpr int kMagicOffset = kSuperOffset + 56;      // s_magic LE16
constexpr int kInodesCount = kSuperOffset + 0;       // s_inodes_count LE32
constexpr int kBlocksCountLo = kSuperOffset + 4;     // s_blocks_count_lo LE32
constexpr int kLogBlockSize = kSuperOffset + 24;     // s_log_block_size LE32
constexpr int kInodeSize = kSuperOffset + 88;        // s_inode_size LE16
constexpr int kFeatureIncompat = kSuperOffset + 96;  // s_feature_incompat LE32
constexpr int kBlocksCountHi = kSuperOffset + 336;   // s_blocks_count_hi LE32

constexpr quint16 kExt4Magic = 0xEF53;
constexpr quint32 kIncompat64Bit = 0x0080;  // EXT4_FEATURE_INCOMPAT_64BIT
constexpr quint32 kGoodOldInodeSize = 128;  // EXT4_GOOD_OLD_INODE_SIZE

// blockSize = 1024 << s_log_block_size；内核允许 1024..65536（log 0..6）
constexpr quint32 kMaxLogBlockSize = 6;

} // namespace

bool isExt4(const QByteArray &image)
{
    if (image.size() < kMagicOffset + 2)
        return false;
    const auto *p = reinterpret_cast<const uchar *>(image.constData()) + kMagicOffset;
    return p[0] == static_cast<uchar>(kExt4Magic & 0xFF)
        && p[1] == static_cast<uchar>(kExt4Magic >> 8);
}

bool parseSuper(const QByteArray &image, SuperBlock &out)
{
    if (!isExt4(image))
        return false;
    // 边界守卫：字段读取终点为 blocks_count_hi@1360+4=1364（64BIT 时），
    // inode_size@1112+2=1114、feature_incompat@1120+4=1124 均在内；
    // isExt4 只保证 1082 字节，截断输入 [1082, 1363] 必须拒绝而非越界读。
    if (image.size() < kSuperOffset + 360)   // 1384 ≥ 1364（所有读取终点）
        return false;
    const uchar *p = reinterpret_cast<const uchar *>(image.constData());

    const quint32 logBlockSize = qFromLittleEndian<quint32>(p + kLogBlockSize);
    if (logBlockSize > kMaxLogBlockSize)
        return false;
    const quint64 blockSize = quint64(1024) << logBlockSize;

    const quint32 inodeSize = qFromLittleEndian<quint16>(p + kInodeSize);
    if (inodeSize < kGoodOldInodeSize || inodeSize > blockSize)
        return false;

    const quint64 inodeCount = qFromLittleEndian<quint32>(p + kInodesCount);
    if (inodeCount == 0)
        return false;

    quint64 blockCount = qFromLittleEndian<quint32>(p + kBlocksCountLo);
    if (qFromLittleEndian<quint32>(p + kFeatureIncompat) & kIncompat64Bit)
        blockCount |= quint64(qFromLittleEndian<quint32>(p + kBlocksCountHi)) << 32;
    if (blockCount == 0)
        return false;

    out.blockSize = static_cast<quint32>(blockSize);
    out.inodeCount = inodeCount;
    out.blockCount = blockCount;
    out.inodeSize = inodeSize;
    out.rootInode = 2; // ext2/3/4 根目录 inode 恒为 2
    return true;
}

bool listTree(const QByteArray &, const SuperBlock &, QList<imgfs::FsEntry> &,
              QString *error)
{
    if (error)
        *error = QStringLiteral("ext4 目录遍历尚未实现（任务 B12）");
    return false;
}

bool extractFile(const QByteArray &, const SuperBlock &, const QString &,
                 QByteArray &, QString *error)
{
    if (error)
        *error = QStringLiteral("ext4 文件提取尚未实现（任务 B12）");
    return false;
}

} // namespace imgext4
