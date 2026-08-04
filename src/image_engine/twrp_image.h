#pragma once
#include <QByteArray>
#include <QString>

namespace imgtwrp {
bool isTwrpBackup(const QByteArray &header);            // "TWRP"
bool extractWin(const QString &winPath, QByteArray &outRaw, QString *error);
} // namespace imgtwrp
