#pragma once
#include <QByteArray>

namespace imgsparse {
bool isSparse(const QByteArray &header);                    // magic 0xED26FF3A
QByteArray simg2img(const QByteArray &sparse);              // 失败返回空
QByteArray img2simg(const QByteArray &raw, quint32 blockSize = 4096);
} // namespace imgsparse
