#pragma once
#include <QByteArray>

namespace imgcomp {
QByteArray xzCompress(const QByteArray &data);
QByteArray xzDecompress(const QByteArray &data);
} // namespace imgcomp
