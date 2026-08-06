#pragma once

#include <QByteArray>
#include <QFile>
#include <QList>
#include <QString>
#include <functional>

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

// ---- G4 流式访问核心：镜像文件句柄 + 随机读（替代整镜像读入内存）----
// 只读打开镜像文件；readAt 按 [off, off+len) 局部读取，越界返回 false 并置 error。
// 所有流式接口（imgerofs/imgext4 的 parseSuperFile/listTreeLazy/extractFileStream）
// 均基于该句柄访问 superblock/inode/目录块/extent 数据，内存 O(块大小)。
class FsFile {
public:
    FsFile() = default;
    ~FsFile() { close(); }
    bool open(const QString &path, QString *error);
    void close();
    bool isOpen() const { return m_f.isOpen(); }
    qint64 size() const { return m_f.size(); }   // 未打开为 -1
    // 读 [off, off+len) 到 out（len ≤ 块大小级别，调用方保证）；失败 false + error
    bool readAt(qint64 off, qint64 len, QByteArray &out, QString *error);

private:
    QFile m_f;
};

// 流式提取文件：从镜像文件按 inPath 提取写入 outPath（大文件边读边写，内存 O(chunk)）。
// progress(bytes)：字节级进度回调，单调递增、范围 [0, 文件大小]（已写入的内容字节数），
// 可传空回调。失败返回 false 且 error 非空；失败时输出文件会被删除（本接口拥有输出文件）。
bool extractFileStream(const QString &imagePath, const QString &inPath,
                       const QString &outPath,
                       const std::function<void(quint64)> &progress = {},
                       QString *error = nullptr);

// 按目录增量加载：只读指定目录的直接子项（不全树一次），输出结果与
// listTree 的对应子树逐项一致（path 为相对根的完整路径，如 "subdir/inner.txt"）。
// dir 为 "" / "/" / "." 时列根目录直接子项；dir 必须存在且为目录，否则 false + error。
bool listTreeLazy(const QString &imagePath, const QString &dir, QList<FsEntry> &out,
                  QString *error = nullptr);

} // namespace imgfs
