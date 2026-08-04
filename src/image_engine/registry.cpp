#include "registry.h"
#include <QFileInfo>

namespace imgreg {

namespace {
Format byExtension(const QString &name)
{
    const QString lower = name.toLower();
    if (lower.endsWith(".tar.md5")) return Format::TarMd5;
    if (lower.endsWith(".tar")) return Format::Tar;
    if (lower.endsWith(".br")) return Format::Br;
    if (lower.endsWith(".lz4")) return Format::Lz4;
    if (lower.endsWith(".xz")) return Format::Xz;
    if (lower.endsWith(".gz")) return Format::Gzip;
    if (lower.endsWith(".zst") || lower.endsWith(".zstd")) return Format::Zstd;
    if (lower.endsWith(".img") || lower.endsWith(".raw")) return Format::RawImage;
    return Format::Unknown;
}
} // namespace

Detected detect(const QByteArray &header, const QString &fileName)
{
    if (header.size() >= 4 && header.left(4) == "CrAU")
        return {Format::Payload, "OTA payload"};
    if (header.size() >= 4 && header.left(4) == "PK\x03\x04")
        return {Format::Zip, "zip 刷机包"};
    if (header.size() >= 4 &&
        static_cast<uchar>(header[0]) == 0x3A && static_cast<uchar>(header[1]) == 0xFF &&
        static_cast<uchar>(header[2]) == 0x26 && static_cast<uchar>(header[3]) == 0xED)
        return {Format::Sparse, "Android sparse 镜像"};
    if (header.size() >= 8 && header.left(8) == "ANDROID!")
        return {Format::Boot, "boot 镜像"};
    if (header.size() >= 8 && header.left(8) == "VNDRBOOT")
        return {Format::VendorBoot, "vendor_boot 镜像"};
    if (header.size() >= 4 &&
        static_cast<uchar>(header[0]) == '0' && static_cast<uchar>(header[1]) == 'P' &&
        static_cast<uchar>(header[2]) == 'L' && static_cast<uchar>(header[3]) == 'A')
        return {Format::Super, "super 动态分区"};
    if (header.size() >= 4 &&
        static_cast<uchar>(header[0]) == 0xE2 && static_cast<uchar>(header[1]) == 0xE1 &&
        static_cast<uchar>(header[2]) == 0xF5 && static_cast<uchar>(header[3]) == 0x00)
        return {Format::Erofs, "EROFS 文件系统"};
    if (header.size() >= 2 && header.left(2) == "\x1f\x8b")
        return {Format::Gzip, "gzip 压缩"};
    if (header.size() >= 4 && header.left(4) == "\x28\xb5\x2f\xfd")
        return {Format::Zstd, "zstd 压缩"};
    if (header.size() >= 6 && header.left(6) == "\xFD\x37\x7A\x58\x5A\x00")
        return {Format::Xz, "xz 压缩"};
    if (header.size() >= 4 && header.left(4) == "\x04\x22\x4D\x18")
        return {Format::Lz4, "lz4 压缩"};
    // ext4: 偏移 1080 处 magic 0xEF53
    if (header.size() >= 1084 &&
        static_cast<uchar>(header[1080]) == 0x53 && static_cast<uchar>(header[1081]) == 0xEF)
        return {Format::Ext4, "ext4 文件系统"};
    // 兜底: 扩展名
    return {byExtension(fileName), "按扩展名识别"};
}

} // namespace imgreg
