#pragma once
#include <QByteArray>
#include <QString>
#include <functional>

namespace imgsparse {

// 整内存接口（小文件/测试用；内部与流式接口共享同一核心逻辑）
bool isSparse(const QByteArray &header);                    // magic 0xED26FF3A
QByteArray simg2img(const QByteArray &sparse);              // 失败返回空
QByteArray img2simg(const QByteArray &raw, quint32 blockSize = 4096);

// ---- 流式接口（GB 级大文件路径，内存 O(chunk)/O(block)）----
// progress(bytes)：字节级进度回调，单调递增、范围 [0, 输入文件大小]（已消费的输入字节数）；
//                 每 chunk/块回调一次，可传空回调。
// 失败返回 false 且 error 非空（可传 nullptr 忽略）；失败时输出文件会被删除（本接口拥有输出文件）。
bool simg2imgStream(const QString &inPath, const QString &outPath,
                    const std::function<void(quint64)> &progress = {},
                    QString *error = nullptr);
bool img2simgStream(const QString &inPath, const QString &outPath, quint32 blockSize = 4096,
                    const std::function<void(quint64)> &progress = {},
                    QString *error = nullptr);

} // namespace imgsparse
