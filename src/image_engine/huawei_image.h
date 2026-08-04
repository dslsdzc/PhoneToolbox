#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

namespace imghw {

struct AppFile {
    QString name;
    quint32 type = 0;       // 0x01=system 0x02=boot 0x03=recovery ...
    quint32 rawSize = 0;
    quint32 compSize = 0;
    quint32 offset = 0;     // 数据段内偏移
};

// 华为 update.app: 512B 头（魔数 0x55 0xAA）+ 64B 文件表条目 * N + 数据段
bool isUpdateApp(const QByteArray &header);   // 0x55 0xAA
bool parseUpdateApp(const QByteArray &app, QList<AppFile> &out, QString *error);
QByteArray extractFile(const QByteArray &app, const AppFile &f, QString *error);
QByteArray buildUpdateApp(const QList<AppFile> &files);

} // namespace imghw
