#pragma once
#include <QByteArray>

namespace imgcomp {

enum class Type { None, Gzip, Bzip2, Lz4, Xz, Zstd, Brotli };

// 压缩/解压；失败返回空 QByteArray。全部类型分支已实现。
QByteArray compress(Type t, const QByteArray &data);
QByteArray decompress(Type t, const QByteArray &data);

// gzip 封装（zlib，带 gzip 头 + 魔数标记），委托到 compressor.cpp 内 impl。
QByteArray gzipCompress(const QByteArray &data);
QByteArray gzipDecompress(const QByteArray &data);

} // namespace imgcomp
