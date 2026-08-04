#include "boot_image.h"
#include <QList>
#include <QtEndian>

namespace imgboot {

namespace {
constexpr int kHdrV0 = 1632; // v0（AOSP boot_img_hdr_v0，packed）
constexpr int kHdrV1 = 1648; // v1: + recovery_dtbo_size(4)@1632 + recovery_dtbo_offset(8)@1636 + header_size(4)@1644
constexpr int kHdrV2 = 1660; // v2: + dtb_size(4)@1648 + dtb_addr(8)@1652
constexpr int kHdrV3 = 1580; // v3/v4

quint64 alignUp(quint64 v, quint32 page) { return (v + page - 1) / page * page; }
} // namespace

bool isBootImage(const QByteArray &header)
{
    return header.size() >= 8 && header.left(8) == "ANDROID!";
}

bool parseBootImage(const QByteArray &raw, BootInfo &out)
{
    if (!isBootImage(raw))
        return false;
    out.raw = raw;
    // header_version 在 v0 与 v3/v4 布局中偏移均为 40（AOSP bootimg.h）
    if (raw.size() < 44)
        return false;
    const quint32 ver = qFromLittleEndian<quint32>(raw.constData() + 40);
    out.headerVersion = ver;
    if (ver <= 2) {
        if (raw.size() < kHdrV0) return false;
        out.pageSize = qFromLittleEndian<quint32>(raw.constData() + 36);
        if (out.pageSize == 0) return false;
        out.kernelSize = qFromLittleEndian<quint32>(raw.constData() + 8);
        out.ramdiskSize = qFromLittleEndian<quint32>(raw.constData() + 16);
        const quint32 osVer = qFromLittleEndian<quint32>(raw.constData() + 44);
        Q_UNUSED(osVer); // 按正确偏移读取（os_version@44）；BootInfo 暂不承载该字段
        out.cmdline = raw.mid(64, 512).split('\0').first(); // name@48, cmdline@64
        quint32 secondSize = 0;
        quint32 recoveryDtboSize = 0;
        if (ver >= 2) {
            if (raw.size() < kHdrV2) return false;
            out.dtbSize = qFromLittleEndian<quint32>(raw.constData() + 1648); // dtb_size
            secondSize = qFromLittleEndian<quint32>(raw.constData() + 24);
            // recovery_dtbo_size 为 uint32@1632（packed：recovery_dtbo_offset 紧随 @1636，
            // 读 8 字节会跨字段）
            recoveryDtboSize = qFromLittleEndian<quint32>(raw.constData() + 1632);
        }
        // header 占第一页（mkbootimg 将 header 补零到 page_size），数据段从页边界开始
        quint64 off = alignUp(static_cast<quint64>(kHdrV0), out.pageSize);
        if (out.kernelSize) {
            if (off + out.kernelSize > static_cast<quint64>(raw.size())) return false;
            out.kernel = raw.mid(static_cast<int>(off), static_cast<int>(out.kernelSize));
            off = alignUp(off + out.kernelSize, out.pageSize);
        }
        if (out.ramdiskSize) {
            if (off + out.ramdiskSize > static_cast<quint64>(raw.size())) return false;
            out.ramdisk = raw.mid(static_cast<int>(off), static_cast<int>(out.ramdiskSize));
            off = alignUp(off + out.ramdiskSize, out.pageSize);
        }
        if (out.dtbSize) {
            // v2 段序：kernel → ramdisk → second → recovery_dtbo → dtb
            off = alignUp(off + secondSize, out.pageSize);
            off = alignUp(off + recoveryDtboSize, out.pageSize);
            if (off + out.dtbSize > static_cast<quint64>(raw.size())) return false;
            out.dtb = raw.mid(static_cast<int>(off), static_cast<int>(out.dtbSize));
        }
        return true;
    }
    // v3/v4
    if (raw.size() < kHdrV3) return false;
    out.pageSize = 4096; // v3/v4 固定页
    out.kernelSize = qFromLittleEndian<quint32>(raw.constData() + 8);
    out.ramdiskSize = qFromLittleEndian<quint32>(raw.constData() + 12);
    out.cmdline = raw.mid(44, 1536).split('\0').first(); // v3+ cmdline 合并为 1536B
    quint64 off = 4096; // header 补零到固定 4096 页，段从页边界开始
    if (out.kernelSize) {
        if (off + out.kernelSize > static_cast<quint64>(raw.size())) return false;
        out.kernel = raw.mid(static_cast<int>(off), static_cast<int>(out.kernelSize));
        off = alignUp(off + out.kernelSize, out.pageSize);
    }
    if (out.ramdiskSize) {
        if (off + out.ramdiskSize > static_cast<quint64>(raw.size())) return false;
        out.ramdisk = raw.mid(static_cast<int>(off), static_cast<int>(out.ramdiskSize));
    }
    return true;
}

} // namespace imgboot
