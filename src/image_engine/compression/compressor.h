#pragma once
#include <QByteArray>

namespace imgcomp {

enum class Type { None, Gzip, Bzip2, Lz4, Xz, Zstd, Brotli };

// 压缩/解压；失败返回空 QByteArray。本任务已实现 Zstd 分支；
// Gzip（zlib 封装）、Bzip2、Xz、Lz4、Brotli 分支分别由后续任务填充。
QByteArray compress(Type t, const QByteArray &data);
QByteArray decompress(Type t, const QByteArray &data);

} // namespace imgcomp
