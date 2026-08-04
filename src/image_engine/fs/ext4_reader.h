#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include "image_engine/fs/fs_image.h"

namespace imgext4 {

// superblock 布局按 Linux 内核 fs/ext4/ext4.h 的 struct ext4_super_block 核对
// (torvalds/linux master)：
//   superblock 位于文件偏移 1024（前 1024 字节为引导/保留区）
//   s_inodes_count     LE32 @0
//   s_blocks_count_lo  LE32 @4
//   s_log_block_size   LE32 @24   blockSize = 1024 << log
//   s_magic            LE16 @56   (0xEF53) → 文件偏移 1024+56=1080
//   s_first_ino        LE32 @84   (注意：@80 是 s_def_resuid，勿混)
//   s_inode_size       LE16 @88   (128/256/512...)
//   s_feature_compat   LE32 @92 | s_feature_incompat LE32 @96 |
//   s_feature_ro_compat LE32 @100
//   s_blocks_count_hi  LE32 @336  (仅 EXT4_FEATURE_INCOMPAT_64BIT=0x80 时有效)
//
// 块组描述符表紧随 superblock（1024 + blockSize 内；blockSize==1024 时在 block 2，
// 即偏移 2048），inode table 由块组描述符定位 —— B12 解析，此处仅读 superblock。
struct SuperBlock {
    quint32 blockSize = 4096;        // s_log_block_size @24（blockSize = 1024 << log）
    quint64 inodeCount = 0;          // s_inodes_count @0
    quint64 blockCount = 0;          // s_blocks_count_lo @4（64BIT 时并入 @336 高 32 位）
    quint32 inodeSize = 0;           // s_inode_size @88（128/256/512...）
    quint64 rootInode = 2;           // ext2/3/4 根目录 inode 恒为 2
};

bool isExt4(const QByteArray &image);              // magic 0xEF53 LE @1080
bool parseSuper(const QByteArray &image, SuperBlock &out);

// 递归遍历整棵树与按路径提取文件 —— 任务 B12 实现；当前返回 false
// 并在 error 中给出"尚未实现"标记（失败/非法输入一律返回 false，不崩溃）。
bool listTree(const QByteArray &image, const SuperBlock &sb,
              QList<imgfs::FsEntry> &out, QString *error);
bool extractFile(const QByteArray &image, const SuperBlock &sb,
                 const QString &path, QByteArray &data, QString *error);

} // namespace imgext4
