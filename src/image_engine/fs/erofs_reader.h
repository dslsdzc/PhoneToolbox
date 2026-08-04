#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include "image_engine/fs/fs_image.h"

namespace imgerofs {

// 来自 erofs-utils master / Linux 6.6+ fs/erofs/erofs_fs.h（EROFS_SUPER_OFFSET=1024）
struct SuperBlock {
    quint32 blockSize = 4096;        // blkszbits @super+12（blockSize = 1<<blkszbits）
    quint64 rootNid = 0;             // root_nid @super+14 / rootnid_8b @super+112 (48BIT)
    bool isLz4 = false;              // available_compr_algs @super+84（bit0 = LZ4）
    quint32 metaBlkAddr = 0;         // meta_blkaddr @super+40：inode 表起始块号
    quint32 featureIncompat = 0;     // feature_incompat @super+80
};

bool isErofs(const QByteArray &image);            // magic 0xE0F5E1E2 @1024
bool parseSuper(const QByteArray &image, SuperBlock &out);

// 递归遍历整棵树（根目录本身不出现在 out 中）。
// out 中 FsEntry.path 为相对路径（无前导 '/'，如 "subdir/inner.txt"），
// isDir/size 来自子 inode；文件 data 不填充（用 extractFile 获取内容）。
// 失败返回 false 并置 error，非法输入不崩溃。
bool listTree(const QByteArray &image, const SuperBlock &sb,
              QList<imgfs::FsEntry> &out, QString *error);

// 按路径提取文件内容（接受 "subdir/file.txt" 或 "/subdir/file.txt"，
// 自动忽略多余 '/'；"." / ".." 组件视为非法）。
// 压缩（LZ4）inode 返回 false 且 error 含 "LZ4" 标记；目录/特殊文件同样返回 false。
bool extractFile(const QByteArray &image, const SuperBlock &sb,
                 const QString &path, QByteArray &data, QString *error);

} // namespace imgerofs
