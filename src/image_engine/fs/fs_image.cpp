#include "fs_image.h"
#include "image_engine/fs/erofs_reader.h"
#include "image_engine/fs/ext4_reader.h"
#include "image_engine/registry.h"
#include <limits>

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

namespace {

// 检测/解析 superblock 需要的最小前缀长度（EROFS 1144 / ext4 1384，取大者）
constexpr qint64 kDetectRead = 1384;

// 打开镜像文件并读检测前缀；返回 false + error（error 可为空则仅 false）
bool openAndPeek(const QString &imagePath, FsFile &f, QByteArray &peek, QString *error)
{
    if (!f.open(imagePath, error))
        return false;
    const qint64 sz = f.size();
    const qint64 len = qMin<qint64>(sz, kDetectRead);
    if (len < 0 || !f.readAt(0, len, peek, error))
        return false;
    return true;
}

} // namespace

bool FsFile::open(const QString &path, QString *error)
{
    if (error)
        error->clear();
    m_f.setFileName(path);
    if (!m_f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法打开镜像文件 %1: %2").arg(path, m_f.errorString());
        return false;
    }
    return true;
}

void FsFile::close()
{
    if (m_f.isOpen())
        m_f.close();
}

bool FsFile::readAt(qint64 off, qint64 len, QByteArray &out, QString *error)
{
    if (error)
        error->clear();
    out.clear();
    if (!m_f.isOpen()) {
        if (error) *error = QStringLiteral("镜像文件未打开");
        return false;
    }
    if (off < 0 || len < 0) {
        if (error) *error = QStringLiteral("读取偏移/长度无效");
        return false;
    }
    const qint64 sz = m_f.size();
    if (off > sz || len > sz - off) {
        if (error) *error = QStringLiteral("读取范围超出镜像大小");
        return false;
    }
    // G4 审查 Minor 吸收：QByteArray::resize 长度参数为 int，len > INT_MAX 时
    // int(len) 截断 → 分配小于实际读入量 → 越界写。显式拒绝（调用方均为
    // 块大小级读取，正常路径永不触达）。
    if (len > std::numeric_limits<int>::max()) {
        if (error) *error = QStringLiteral("读取长度超过上限（%1 字节）").arg(len);
        return false;
    }
    if (len == 0)
        return true;
    out.resize(int(len));
    if (m_f.seek(off) && m_f.read(out.data(), len) == len)
        return true;
    const QString es = m_f.errorString();
    out.clear();
    if (error) *error = QStringLiteral("读取镜像失败: %1").arg(es);
    return false;
}

bool extractFileStream(const QString &imagePath, const QString &inPath,
                       const QString &outPath,
                       const std::function<void(quint64)> &progress, QString *error)
{
    if (error)
        error->clear();
    FsFile f;
    QByteArray peek;
    if (!openAndPeek(imagePath, f, peek, error))
        return false;
    const imgreg::Detected d = imgreg::detect(peek, QString());
    switch (d.format) {
    case imgreg::Format::Erofs: {
        imgerofs::SuperBlock sb;
        if (!imgerofs::parseSuperFile(f, sb, error)) {
            if (error && error->isEmpty())
                *error = QStringLiteral("EROFS superblock 解析失败");
            return false;
        }
        return imgerofs::extractFileStream(f, sb, inPath, outPath, progress, error);
    }
    case imgreg::Format::Ext4: {
        imgext4::SuperBlock sb;
        if (!imgext4::parseSuperFile(f, sb, error)) {
            if (error && error->isEmpty())
                *error = QStringLiteral("ext4 superblock 解析失败");
            return false;
        }
        return imgext4::extractFileStream(f, sb, inPath, outPath, progress, error);
    }
    default:
        if (error) *error = QStringLiteral("不是文件系统镜像（支持 EROFS / ext4）");
        return false;
    }
}

bool listTreeLazy(const QString &imagePath, const QString &dir, QList<FsEntry> &out,
                  QString *error)
{
    if (error)
        error->clear();
    out.clear();
    FsFile f;
    QByteArray peek;
    if (!openAndPeek(imagePath, f, peek, error))
        return false;
    const imgreg::Detected d = imgreg::detect(peek, QString());
    switch (d.format) {
    case imgreg::Format::Erofs: {
        imgerofs::SuperBlock sb;
        if (!imgerofs::parseSuperFile(f, sb, error)) {
            if (error && error->isEmpty())
                *error = QStringLiteral("EROFS superblock 解析失败");
            return false;
        }
        return imgerofs::listTreeLazy(f, sb, dir, out, error);
    }
    case imgreg::Format::Ext4: {
        imgext4::SuperBlock sb;
        if (!imgext4::parseSuperFile(f, sb, error)) {
            if (error && error->isEmpty())
                *error = QStringLiteral("ext4 superblock 解析失败");
            return false;
        }
        return imgext4::listTreeLazy(f, sb, dir, out, error);
    }
    default:
        if (error) *error = QStringLiteral("不是文件系统镜像（支持 EROFS / ext4）");
        return false;
    }
}

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
