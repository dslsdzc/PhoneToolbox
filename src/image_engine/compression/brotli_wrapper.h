#pragma once
#include <QByteArray>

namespace imgcomp {
QByteArray brotliCompress(const QByteArray &data);
QByteArray brotliDecompress(const QByteArray &data);
} // namespace imgcomp
