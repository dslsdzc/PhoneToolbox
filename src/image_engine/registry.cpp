#include "registry.h"
#include <QFileInfo>
#include <QtEndian>

namespace imgreg {

namespace {

// ASCII 串转 UTF-16LE 落盘形式（字符间嵌 \0，如 pac 头版本串）
QByteArray toUtf16Le(const char *ascii)
{
    const QByteArray a(ascii);
    QByteArray out(a.size() * 2, 0);
    for (int i = 0; i < a.size(); ++i)
        out[i * 2] = a[i];
    return out;
}

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
    // pac 旧格式无魔数（divinebird C 版，头 1220B 无魔数）→ 按扩展名兜底
    if (lower.endsWith(".pac")) return Format::Pac;
    // 华为 update.bin（L2 型分区表）：无魔数 —— 文件头是签名头（解析靠文件名，
    // 见 imghw::parseUpdateBin 的 L2 型），文件名以 "update.bin" 结尾是唯一信号。
    // 注意顺序：真实 OTA 的 update.bin 常带 "CrAU"/ext4/sparse 等魔数，先走
    // detect() 的魔数分支；此处只兜底魔数不匹配的 L2 型文件。
    if (lower.endsWith("update.bin")) return Format::UpdateBin;
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
    // KDZ v3: 8B 魔数（kdztools unkdz.py _dz_header = \x28\x05\x00\x00\x24\x38\x22\x25）
    if (header.size() >= 8 && header.left(8) == QByteArrayLiteral("\x28\x05\x00\x00\x24\x38\x22\x25"))
        return {Format::Kdz, "LG KDZ 固件包"};
    // SIN v3: [0]=0x03 + "SIN"（flashtool S1ParseLib sin/v3）
    if (header.size() >= 4 && static_cast<uchar>(header[0]) == 0x03 &&
        header.mid(1, 3) == QByteArrayLiteral("SIN"))
        return {Format::Sin, "索尼 SIN v3 镜像"};
    // 华为 update.app: 魔数 0x55 0xAA（512B 头，与 imghw::isUpdateApp 一致）
    if (header.size() >= 2 && static_cast<uchar>(header[0]) == 0x55 &&
        static_cast<uchar>(header[1]) == 0xAA)
        return {Format::UpdateApp, "华为 update.app 固件"};
    // GPT: 主分区表头 "EFI PART" 位于 LBA1（偏移 512）
    if (header.size() >= 520 && header.mid(512, 8) == QByteArrayLiteral("EFI PART"))
        return {Format::DiskGpt, "GPT 磁盘镜像"};
    // TWRP 备份: 头魔数 "TWRP"（imgtwrp::isTwrpBackup 同判定）
    if (header.size() >= 4 && header.left(4) == QByteArrayLiteral("TWRP"))
        return {Format::TwrpWin, "TWRP 备份"};
    // pac 新格式: 版本串 "BP_R1.0.0"/"BP_R2.0.1" 以 UTF-16LE 落盘于偏移 0（imgpac 官方版本门禁）
    if (header.size() >= 18) {
        const QByteArray utf16 = header.left(18);
        if (utf16 == toUtf16Le("BP_R1.0.0") || utf16 == toUtf16Le("BP_R2.0.1"))
            return {Format::Pac, "pac 固件（新格式）"};
    }
    // pac 新格式辅助信号: 0xfffafffa 魔数 @2116（参考实现仅 CRC 用途，不作解析门禁）
    if (header.size() >= 2120 &&
        qFromLittleEndian<quint32>(header.constData() + 2116) == 0xFFFAFFFAu)
        return {Format::Pac, "pac 固件（新格式）"};
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
