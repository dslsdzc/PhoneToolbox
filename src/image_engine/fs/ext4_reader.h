#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include "image_engine/fs/fs_image.h"

namespace imgext4 {

// superblock 布局按 Linux 内核 fs/ext4/ext4.h 的 struct ext4_super_block 核对
// (torvalds/linux master)，并经本机 e2fsprogs mke2fs 真实镜像 xxd 双重验证：
//   superblock 位于文件偏移 1024（前 1024 字节为引导/保留区）
//   s_inodes_count     LE32 @0     s_blocks_count_lo LE32 @4
//   s_free_blocks_count_lo LE32 @12
//   s_first_data_block LE32 @20    s_log_block_size  LE32 @24 (blockSize=1024<<log)
//   s_blocks_per_group LE32 @32    s_inodes_per_group LE32 @40（@32 是
//   blocks_per_group，brief 的 inodes_per_group@32 有误，内核为准）
//   s_magic            LE16 @56 (0xEF53) → 文件偏移 1080
//   s_inode_size       LE16 @88    s_feature_incompat LE32 @96
//   s_desc_size        LE16 @254   s_blocks_count_hi LE32 @336 (64BIT 时)
//
// 块组描述符表：blockSize==1024 时在 block 2（偏移 2048），否则 block 1
// （偏移 blockSize；superblock 在 block 0 内 1024..2048，GDT 紧随其后）。
// 描述符 ext4_group_desc（descSize 32 或 64；64BIT 时高 32 位在 +32/+36/+40）：
//   bg_block_bitmap LE32 @0 | bg_inode_bitmap LE32 @4 | bg_inode_table LE32 @8
//   bg_free_blocks_count_lo LE16 @12（64 字节描述符 +44 为高 16 位）
// 64BIT 时 s_desc_size 至少 64（内核 ext4_fill_super 规则），否则恒 32。
//
// inode（ext4_inode，相对 inode 起点）：i_mode u16@0、i_size_lo u32@4、
// i_links_count u16@26、i_blocks u32@28（512B 单位）、i_flags u32@32、
// i_block[60]@40（extent 树起点）、i_size_high u32@108（仅 inodeSize>128 时
// 有效，内核 ext4_iget 按 i_extra_isize 存在性判断，与 64BIT 无关）、
// i_extra_isize u16@128（inodeSize>128 时存在）。
struct SuperBlock {
    quint32 blockSize = 4096;        // s_log_block_size @24（blockSize = 1024 << log）
    quint64 inodeCount = 0;          // s_inodes_count @0
    quint64 blockCount = 0;          // s_blocks_count_lo @4（64BIT 时并入 @336 高 32 位）
    quint32 inodeSize = 0;           // s_inode_size @88（128/256/512...）
    quint64 rootInode = 2;           // ext2/3/4 根目录 inode 恒为 2
    // ---- B12 追加字段（向后兼容，parseSuper 填充）----
    quint32 inodesPerGroup = 0;      // s_inodes_per_group @40
    quint32 blocksPerGroup = 0;      // s_blocks_per_group @32
    quint32 firstDataBlock = 0;      // s_first_data_block @20（1024B 块时=1，否则 0）
    quint32 descSize = 0;            // s_desc_size @254（64BIT 时 ≥64，否则 32）
    quint32 featureIncompat = 0;     // s_feature_incompat @96
};

bool isExt4(const QByteArray &image);              // magic 0xEF53 LE @1080
bool parseSuper(const QByteArray &image, SuperBlock &out);

// 递归遍历整棵树与按路径提取文件（目录项 ext4_dir_entry_2：inode u32@0、
// rec_len u16@4、name_len u8@6、file_type u8@7、name@8；注意 brief 写的
// name_len u16@6/file_type u8@8 与内核不符，已按内核+真实镜像核实修正）。
// htree（EXT4_INDEX_FL）目录无需哈希树遍历：dx 根块用 dot(rec_len=12)+
// dotdot(rec_len=blocksize-12) 覆盖索引项，线性解析自然跳过（与 e2fsprogs
// dir_iterate 一致，实测 debugfs 可线性列出 dx 目录）。
bool listTree(const QByteArray &image, const SuperBlock &sb,
              QList<imgfs::FsEntry> &out, QString *error);
bool extractFile(const QByteArray &image, const SuperBlock &sb,
                 const QString &path, QByteArray &data, QString *error);

// 在原镜像上替换文件内容（修改 image 本身，B13 的 FsImage 包装持镜像副本）：
// 新数据 ≤ 原 inode 已分配空间 → 原地写 + 更新 i_size（extent 布局不变）；
// 更大（或 inline 文件超 60B）→ 在块位图中找空闲连续块重建 extent 树
// （参考 e2fsprogs debugfs write 行为），同步更新块位图与空闲块计数。
// 返回的替换后完整镜像由 repack() 给出（调用方把替换后的副本传入）。
bool replaceFile(QByteArray &image, const SuperBlock &sb,
                 const QString &path, const QByteArray &data, QString *error);
// 校验后返回完整镜像（替换后的副本原样返回；解析器自校验由 replaceFile 内部完成）
QByteArray repack(const QByteArray &image, const SuperBlock &sb);

} // namespace imgext4
