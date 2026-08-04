#pragma once
#include <QByteArray>

namespace imgcomp {

enum class Type { None, Gzip, Bzip2, Lz4, Xz, Zstd, Brotli };

// 压缩/解压；失败返回空 QByteArray。Gzip 用 zlib 内部封装（见 Task 6 前由 bzip2/xz 提供），
// 本任务先实现 Zstd 分支，其余分支由后续任务填充。
QByteArray compress(Type t, const QByteArray &data);
QByteArray decompress(Type t, const QByteArray &data);

} // namespace imgcomp
