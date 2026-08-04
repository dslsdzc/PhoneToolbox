#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

namespace imgtar {

struct TarEntry {
    QString name;
    QByteArray data;
    bool isDir = false;
    bool isSymlink = false;
    QString linkTarget;
};

bool extractTar(const QByteArray &tar, QList<TarEntry> &entries);
QByteArray buildTar(const QList<TarEntry> &entries);      // Task 11
QByteArray appendMd5Footer(const QByteArray &tar);        // 三星 Odin 兼容尾部
bool verifyMd5Footer(const QByteArray &tarMd5);

} // namespace imgtar
