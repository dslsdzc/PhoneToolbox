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
    // super 动态分区: geometry magic 0x616c4467（"gDla"）小端落盘，位于偏移 4096
    // （LP_METADATA_GEOMETRY_OFFSET）；偏移 0 是保留区，"0PLA" metadata 头在偏移 8192。
    if (header.size() >= 4100 &&
        static_cast<uchar>(header[4096]) == 'g' && static_cast<uchar>(header[4097]) == 'D' &&
        static_cast<uchar>(header[4098]) == 'l' && static_cast<uchar>(header[4099]) == 'a')
        return {Format::Super, "super 动态分区"};
    // EROFS: magic 0xE0F5E1E2 小端落盘为 E2 E1 F5 E0，superblock 位于偏移 1024
    // （EROFS_SUPER_OFFSET），偏移 0 是保留区。
    if (header.size() >= 1028 &&
        static_cast<uchar>(header[1024]) == 0xE2 && static_cast<uchar>(header[1025]) == 0xE1 &&
        static_cast<uchar>(header[1026]) == 0xF5 && static_cast<uchar>(header[1027]) == 0xE0)
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
