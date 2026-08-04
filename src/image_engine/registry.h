#pragma once
#include <QByteArray>
#include <QString>

namespace imgreg {

enum class Format {
    Unknown, Payload, Zip, Tar, TarMd5, Sparse, Super, Boot, VendorBoot,
    Vbmeta, Dtb, Br, Lz4, Xz, Gzip, Zstd, Brotli, Dat, Pac, Kdz,
    UpdateApp, UpdateBin, Sin, DiskGpt, TwrpWin, Erofs, Ext4, RawImage
};

struct Detected {
    Format format = Format::Unknown;
    QString detail;
};

// 魔数优先，扩展名兜底
Detected detect(const QByteArray &header, const QString &fileName);

} // namespace imgreg
