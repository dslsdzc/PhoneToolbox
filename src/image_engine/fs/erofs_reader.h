#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <functional>
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
// 失败返回 false 并置 error，非法输入不崩溃；目录深度超过 128 报"目录深度超限"。
bool listTree(const QByteArray &image, const SuperBlock &sb,
              QList<imgfs::FsEntry> &out, QString *error);

// 按路径提取文件内容（接受 "subdir/file.txt" 或 "/subdir/file.txt"，
// 自动忽略多余 '/'；"." / ".." 组件视为非法）。
// 压缩（LZ4）inode 返回 false 且 error 含 "LZ4" 标记；目录/特殊文件同样返回 false。
bool extractFile(const QByteArray &image, const SuperBlock &sb,
                 const QString &path, QByteArray &data, QString *error);

// ---- G4 流式接口（镜像文件随机读，不整镜像读入内存）----
// f 已由调用方打开（imgfs::FsFile）；内存版与文件版共享同一解析核心，结果一致。
bool parseSuperFile(imgfs::FsFile &f, SuperBlock &out, QString *error = nullptr);

// 按目录增量加载：只读 dir 的直接子项（不全树一次）。dir 为 "" / "/" / "."
// 时列根目录直接子项；条目 path 为相对根的完整路径（如 "subdir/inner.txt"），
// 与 listTree 中对应子树逐项一致（isDir/size 来自子 inode，data 不填充）。
// dir 不存在或不是目录 → false + error；META_BOX 等不支持特性照旧拒绝。
bool listTreeLazy(imgfs::FsFile &f, const SuperBlock &sb, const QString &dir,
                  QList<imgfs::FsEntry> &out, QString *error = nullptr);

// 流式提取文件到 outPath（大文件边读边写，内存 O(chunk)），结果与 extractFile
// 逐字节一致（路径语义/压缩拒绝/LZ4 标记错误均相同）。
// progress(bytes)：单调递增、范围 [0, 文件大小]（已写入的内容字节数），可传空回调。
// 失败返回 false + error；输出文件会被删除。
bool extractFileStream(imgfs::FsFile &f, const SuperBlock &sb,
                       const QString &path, const QString &outPath,
                       const std::function<void(quint64)> &progress = {},
                       QString *error = nullptr);

} // namespace imgerofs
