#pragma once
#include <QByteArray>

namespace imgcomp {
QByteArray lz4Compress(const QByteArray &data);
QByteArray lz4Decompress(const QByteArray &data);
} // namespace imgcomp
