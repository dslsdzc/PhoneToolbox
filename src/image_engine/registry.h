#pragma once
#include <QByteArray>
#include <QString>

namespace imgreg {

enum class Format {
    Unknown, Payload, Zip, Tar, TarMd5, Sparse, Super, Boot, VendorBoot,
    Vbmeta, Dtb, Br, Lz4, Xz, Gzip, Zstd, Brotli, Dat, Pac, Kdz,
    UpdateApp, UpdateBin, Sin, DiskGpt, TwrpWin, Erofs, Ext4, RawImage,
    // OPPO 系固件包（无头魔数，判据在文件尾页）：registry 只按扩展名兜底，
    // 变体/合法性由 ImageWorker 的尾页二次探测判定（见 detect() 末尾注释）
    OFP, OPS
};

struct Detected {
    Format format = Format::Unknown;
    QString detail;
};

// 魔数优先，扩展名兜底
Detected detect(const QByteArray &header, const QString &fileName);

} // namespace imgreg
