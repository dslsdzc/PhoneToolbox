#pragma once
#include <QByteArray>

namespace imgcomp {
QByteArray bzip2Compress(const QByteArray &data);
QByteArray bzip2Decompress(const QByteArray &data);
} // namespace imgcomp
