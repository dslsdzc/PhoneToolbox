#pragma once
#include <QByteArray>

namespace imgcomp {
QByteArray zstdCompress(const QByteArray &data, int level = 3);
QByteArray zstdDecompress(const QByteArray &data);
} // namespace imgcomp
