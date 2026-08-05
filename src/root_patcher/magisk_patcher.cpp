#include "root_patcher/magisk_patcher.h"
#include "root_patcher/kernelsu_patcher.h"
#include "root_patcher/apatch_patcher.h"
#include "root_patcher/ramdisk_su_patcher.h"
#include "root_patcher/ramdisk_utils.h"
#include "root_patcher/zip_util.h"
#include "root_patcher/cpio_util.h"
#include "image_engine/boot_image.h"

#include <QFile>

namespace patcher {
namespace {

// 注入物 ABI 搜索顺序：ramdisk 的 init 运行于目标设备 CPU，PC 端无从得知
// ABI，按现代设备占比优先 arm64-v8a（官方 App 按设备 ro.product.cpu.abi
// 选择，此处给出回退链）。
QStringList abiCandidates()
{
    return {QStringLiteral("arm64-v8a"), QStringLiteral("armeabi-v7a"),
            QStringLiteral("x86_64"), QStringLiteral("x86")};
}

// .backup/.magisk 配置：C3 不做 fstab verity/encryption 剥离（KEEPVERITY/
// KEEPFORCEENCRYPT 保持 true 以如实反映未修补状态）；magiskinit 运行时仅
// 原样透传该文件（v25.2 init/rootdir.cpp magisk_cfg），字段供卸载/还原流程使用。
QByteArray magiskConfig()
{
    return QByteArray("KEEPVERITY=true\nKEEPFORCEENCRYPT=true\n"
                      "PATCHVBMETAFLAG=false\nRECOVERYMODE=false\n");
}

} // namespace

// ============================================================
// MagiskPatcher
// ============================================================

bool MagiskPatcher::patch(const QByteArray &bootImage, const PatchConfig &cfg,
                          QByteArray &out, QString *error)
{
    auto fail = [error](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (error)
        error->clear();

    if (cfg.type != RootType::Magisk && cfg.type != RootType::MagiskAlpha &&
        cfg.type != RootType::Kitsune)
        return fail(QStringLiteral("MagiskPatcher 仅处理 Magisk 系 RootType"));

    if (cfg.apkPath.isEmpty())
        return fail(
            QStringLiteral("未提供 Magisk APK：请先经 AssetsDownloader 下载注入物后填入 apkPath"));

    QFile apkFile(cfg.apkPath);
    if (!apkFile.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法打开 APK：%1").arg(cfg.apkPath));
    const QByteArray apk = apkFile.readAll();
    apkFile.close();
    if (apk.isEmpty())
        return fail(QStringLiteral("APK 文件为空"));

    // 从 APK 提取 magiskinit（真实产物为静态链接 ELF，注入逻辑不校验内容）
    QByteArray magiskinit;
    QString zipErr;
    for (const QString &abi : abiCandidates()) {
        if (extractZipEntry(apk, QStringLiteral("lib/%1/libmagiskinit.so").arg(abi), magiskinit,
                            &zipErr))
            break;
    }
    if (magiskinit.isEmpty())
        return fail(QStringLiteral("APK 内未找到 libmagiskinit.so（%1）").arg(zipErr));

    imgboot::BootInfo info;
    if (!imgboot::parseBootImage(bootImage, info))
        return fail(QStringLiteral("无法解析 boot 镜像"));
    if (info.ramdisk.isEmpty())
        return fail(
            QStringLiteral("boot 镜像不含 ramdisk（ramdiskless SAR 暂不支持，请用对应 App 修补）"));

    // 记录原始压缩格式，重压时保持（与 magiskboot repack 行为一致）
    QString fmt;
    patcher::detectRamdiskFormat(info.ramdisk, fmt);
    QByteArray ramdiskRaw;
    QString ramdiskErr;
    if (!patcher::decompressRamdisk(info.ramdisk, ramdiskRaw, &ramdiskErr))
        return fail(QStringLiteral("ramdisk 解压失败：%1").arg(ramdiskErr));

    CpioArchive cpio;
    if (!cpio.parse(ramdiskRaw, &ramdiskErr))
        return fail(QStringLiteral("ramdisk 不是有效 cpio：%1").arg(ramdiskErr));

    // 重复修补防护：官方 boot_patch.sh 先 "cpio test"（1=已 Magisk 修补）再
    // restore；已修补镜像含 .backup 标记，再次注入会把原 init 备份覆盖成
    // 旧 magiskinit（损坏还原链）→ 明确拒绝，交由调用方还原原厂镜像
    CpioEntry probe;
    if (cpio.find(QStringLiteral(".backup"), &probe))
        return fail(QStringLiteral("镜像已修补过，请先还原为原厂 boot 镜像"));

    // 注入链（与官方 boot_patch.sh "add 0750 init magiskinit" + "backup"
    // 一致）：原 init 条目备份为 .backup/init（运行时 magiskinit 的
    // backup_init() 将其 rename 回 /init 交给真实 init 接管），init 条目
    // 替换为 magiskinit。注意：官方实现不存在 init.orig 条目。
    CpioEntry origInit;
    if (cpio.find(QStringLiteral("init"), &origInit))
        cpio.addOrReplace(QStringLiteral(".backup/init"), origInit);
    CpioEntry magiskinitEntry;
    magiskinitEntry.mode = 0100750; // S_IFREG | 0750
    magiskinitEntry.data = magiskinit;
    cpio.addOrReplace(QStringLiteral("init"), magiskinitEntry);
    CpioEntry backupDir;
    backupDir.mode = 0040000; // S_IFDIR（官方 mkdir 000 .backup）
    cpio.addOrReplace(QStringLiteral(".backup"), backupDir);
    CpioEntry configEntry;
    configEntry.mode = 0100000; // S_IFREG（官方 add 000 .backup/.magisk）
    configEntry.data = magiskConfig();
    cpio.addOrReplace(QStringLiteral(".backup/.magisk"), configEntry);

    const QByteArray patchedCpio = cpio.serialize();
    const QByteArray newRamdisk = patcher::compressRamdisk(patchedCpio, fmt);
    if (newRamdisk.isEmpty())
        return fail(QStringLiteral("ramdisk 重压失败"));

    info.ramdisk = newRamdisk;
    out = imgboot::repackBootImage(info);
    return true;
}

QUrl MagiskPatcher::downloadUrl(const QString &variant)
{
    const QString v = variant.toLower();
    if (v == QLatin1String("alpha"))
        // vvb2060/Magisk：tag=版本号（如 30700），asset 固定 app-release.apk
        // （联网验证 2026-08）
        return QUrl(QStringLiteral("https://github.com/vvb2060/Magisk/releases/latest"));
    if (v == QLatin1String("kitsune"))
        // 原 HuskyDG/magisk-files 与 KitsuneMagisk/KitsuneMagisk 均已下线
        // （GitHub API 404，2026-08 验证），现有源均为个人镜像 → 不内置
        // URL，须手动指定 APK
        return QUrl();
    // 官方：asset 命名随版本变化（Magisk-v30.7.apk / Magisk-v28.1-28100.apk），
    // 由调用方经 GitHub API 解析具体 asset
    return QUrl(QStringLiteral("https://github.com/topjohnwu/Magisk/releases/latest"));
}

QString MagiskPatcher::assetKey(const QString &variant)
{
    const QString v = variant.toLower();
    if (v == QLatin1String("alpha"))
        return QStringLiteral("magisk-alpha");
    if (v == QLatin1String("kitsune"))
        return QStringLiteral("magisk-kitsune");
    return QStringLiteral("magisk");
}

// 工厂：C3 实现 Magisk 系；C4 登记 KernelSU 系（KernelSuPatcher，见
// kernelsu_patcher.h）；C5 登记 APatch/KernelPatch 系（APatchPatcher，见
// apatch_patcher.h）；C7 登记 RamdiskSu 系（RamdiskSuPatcher，见
// ramdisk_su_patcher.h）；其余类型由 C8 在实现中扩展。
RootPatcher *RootPatcher::create(RootType type)
{
    switch (type) {
    case RootType::Magisk:
    case RootType::MagiskAlpha:
    case RootType::Kitsune:
        return new MagiskPatcher;
    case RootType::KernelSU:
    case RootType::KernelSU_Next:
    case RootType::SukiSU:
    case RootType::ReSukiSU:
        return new KernelSuPatcher;
    case RootType::APatch:
    case RootType::KernelPatch:
        return new APatchPatcher;
    case RootType::RamdiskSu:
        return new RamdiskSuPatcher;
    default:
        return nullptr;
    }
}

} // namespace patcher
