#include "fs_image.h"
#include "image_engine/fs/erofs_reader.h"
#include "image_engine/fs/ext4_reader.h"
#include "image_engine/registry.h"

// B13: FsImage 抽象接线 —— 按 imgreg::detect 结果把镜像分派到 imgerofs / imgext4
// 实现。openFsImage 返回堆上 FsImage（调用方负责 delete）；非法/不支持输入返回
// nullptr 并置 error（全局契约: 失败返回 false/空，不得崩溃）。

namespace imgfs {

namespace {

// 顶层目录过滤: dir 为 "" / "/" / "." 时返回整树；否则仅保留 dir/ 前缀下的条目。
// 底层 listTree 始终返回整树，此处按接口语义裁剪。
bool filterDir(const QString &dir, const QList<FsEntry> &all, QList<FsEntry> &out)
{
    if (dir.isEmpty() || dir == QLatin1String("/") || dir == QLatin1String(".")) {
        out = all;
        return true;
    }
    QString prefix = dir;
    if (prefix.startsWith(QLatin1Char('/')))
        prefix.remove(0, 1);
    if (!prefix.endsWith(QLatin1Char('/')))
        prefix += QLatin1Char('/');
    out.clear();
    for (const FsEntry &e : all)
        if (e.path.startsWith(prefix))
            out.append(e);
    return true;
}

// EROFS 只读包装（B10 解析器无写回能力）
class ErofsImage : public FsImage
{
public:
    ErofsImage(const QByteArray &image, const imgerofs::SuperBlock &sb)
        : m_image(image), m_sb(sb) {}

    bool list(const QString &dir, QList<FsEntry> &out) override
    {
        QList<FsEntry> all;
        QString err;
        if (!imgerofs::listTree(m_image, m_sb, all, &err))
            return false;
        return filterDir(dir, all, out);
    }
    bool extract(const QString &path, QByteArray &data) override
    {
        QString err;
        return imgerofs::extractFile(m_image, m_sb, path, data, &err);
    }
    // EROFS 只读: 替换不支持
    bool replace(const QString &path, const QByteArray &data) override { return false; }
    // EROFS 只读: 原样返回当前镜像
    QByteArray repack() override { return m_image; }

private:
    QByteArray m_image;
    imgerofs::SuperBlock m_sb;
};

// ext4 包装（B12 支持替换/重打包；replaceFile 直接修改 m_image 副本，repack 返回之）
class Ext4Image : public FsImage
{
public:
    Ext4Image(const QByteArray &image, const imgext4::SuperBlock &sb)
        : m_image(image), m_sb(sb) {}

    bool list(const QString &dir, QList<FsEntry> &out) override
    {
        QList<FsEntry> all;
        QString err;
        if (!imgext4::listTree(m_image, m_sb, all, &err))
            return false;
        return filterDir(dir, all, out);
    }
    bool extract(const QString &path, QByteArray &data) override
    {
        QString err;
        return imgext4::extractFile(m_image, m_sb, path, data, &err);
    }
    bool replace(const QString &path, const QByteArray &data) override
    {
        QString err;
        return imgext4::replaceFile(m_image, m_sb, path, data, &err);
    }
    QByteArray repack() override { return imgext4::repack(m_image, m_sb); }

private:
    QByteArray m_image;
    imgext4::SuperBlock m_sb;
};

} // namespace

FsImage *openFsImage(const QByteArray &image, QString *error)
{
    if (error)
        error->clear();
    if (image.isEmpty()) {
        if (error) *error = QStringLiteral("镜像为空");
        return nullptr;
    }
    // 按 registry detect 结果分派（魔数判定与各模块 isXxx 一致）
    const imgreg::Detected d = imgreg::detect(image, QString());
    switch (d.format) {
    case imgreg::Format::Erofs: {
        imgerofs::SuperBlock sb;
        if (!imgerofs::parseSuper(image, sb)) {
            if (error) *error = QStringLiteral("EROFS superblock 解析失败");
            return nullptr;
        }
        return new ErofsImage(image, sb);
    }
    case imgreg::Format::Ext4: {
        imgext4::SuperBlock sb;
        if (!imgext4::parseSuper(image, sb)) {
            if (error) *error = QStringLiteral("ext4 superblock 解析失败");
            return nullptr;
        }
        return new Ext4Image(image, sb);
    }
    default:
        if (error) *error = QStringLiteral("不是文件系统镜像（支持 EROFS / ext4）");
        return nullptr;
    }
}

} // namespace imgfs
