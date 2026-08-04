#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

namespace imgfs {

struct FsEntry {
    QString path;
    bool isDir = false;
    quint64 size = 0;
    QByteArray data;
};

// 文件系统镜像统一接口（EROFS/ext4 等实现；B10 起接入）
class FsImage {
public:
    virtual ~FsImage() = default;
    virtual bool list(const QString &dir, QList<FsEntry> &out) = 0;
    virtual bool extract(const QString &path, QByteArray &data) = 0;
    virtual bool replace(const QString &path, const QByteArray &data) = 0;
    virtual QByteArray repack() = 0;
};

// 打开镜像并按格式分派（B13 registry 接线后实现）
FsImage *openFsImage(const QByteArray &image, QString *error);

} // namespace imgfs
