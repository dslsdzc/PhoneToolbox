#pragma once
#include <QByteArray>
#include <QString>

namespace imgdat {
// 将 transfer.list + .dat 应用为 raw 镜像（sdat2img）
bool sdat2img(const QString &transferListPath, const QString &datPath,
              QByteArray &outRaw, QString *error);
} // namespace imgdat
