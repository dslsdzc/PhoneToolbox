#include "root_patcher/ramdisk_su_patcher.h"
#include "root_patcher/ramdisk_utils.h"
#include "root_patcher/zip_util.h"
#include "root_patcher/cpio_util.h"
#include "image_engine/boot_image.h"

#include <QFile>

namespace patcher {
namespace {

// su 二进制 ABI 目录回退链（真实 SuperSU zip 目录名，联网验证
// v2.46-2.82 产物：arm/arm64/armv7/mips/mips64/x64/x86）。PC 端无从得知
// 设备 ABI，按老设备占比优先 arm（32 位主导），arm64 次之。
const char *kSuAbiCandidates[] = {"arm",  "arm64", "armv7", "x86",
                                  "x64",  "mips",  "mips64"};

// init.superuser.rc 服务模板（dkp 内核包 rd/ 目录同款，联网验证 2026-08-05：
// XDA dkp [d2att] 与 SlimKat 内核 ramdisk 两处独立来源一致）。
// 注意与 SuperSU 2.79+ systemless 模板（/sbin/launch_daemonsu.sh + su.img
// 挂载）不同：本模板为经典 /system/xbin/daemonsu 路径，服务能启动的前提
// 是设备 /system 已存在该二进制（先前 SuperSU 系统安装或 ROM 自带）——
// 诚实边界见头文件注释。
QByteArray initSupersuRcTemplate()
{
    return QByteArray(
        "# SuperSU daemonsu service (injected by PhoneToolbox C7)\n"
        "service daemonsu /system/xbin/daemonsu --auto-daemon\n"
        "    class core\n"
        "    user root\n"
        "    group root\n"
        "    oneshot\n");
}

// 判定 init.rc 内容是否已含 import /init.superuser.rc（行级匹配，容忍缩进）。
bool hasSupersuImport(const QByteArray &initRc)
{
    const QList<QByteArray> lines = initRc.split('\n');
    for (const QByteArray &line : lines) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed == "import /init.superuser.rc")
            return true;
    }
    return false;
}

} // namespace

bool RamdiskSuPatcher::patch(const QByteArray &bootImage, const PatchConfig &cfg,
                             QByteArray &out, QString *error)
{
    auto fail = [error](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (error)
        error->clear();
    out.clear();

    if (cfg.type != RootType::RamdiskSu)
        return fail(QStringLiteral("RamdiskSuPatcher 仅处理 RamdiskSu RootType"));

    if (cfg.suZipPath.isEmpty())
        return fail(QStringLiteral("未提供 SuperSU ZIP：请经 suZipPath 指定（老设备 "
                                   "SuperSU recovery 刷入包，如 UPDATE-SuperSU-v2.82*.zip）"));

    // ---- 注入物：从 SuperSU 刷入包提取 su 二进制 ----
    QFile zipFile(cfg.suZipPath);
    if (!zipFile.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法打开 SuperSU ZIP：%1").arg(cfg.suZipPath));
    const QByteArray zip = zipFile.readAll();
    zipFile.close();
    if (zip.isEmpty())
        return fail(QStringLiteral("SuperSU ZIP 文件为空"));
    if (!patcher::isZip(zip))
        return fail(QStringLiteral("SuperSU ZIP 不是有效 ZIP 归档：%1").arg(cfg.suZipPath));

    // su 与 daemonsu 为同一文件（update-binary 把同一 $BIN/su 安装为
    // /system/xbin/su、/system/bin/.ext/.su、/system/xbin/daemonsu，
    // 联网验证 v2.46-2.82 update-binary 脚本）——提取任一 ABI 的 su 即可
    QByteArray su;
    QString zipErr;
    for (const char *abi : kSuAbiCandidates) {
        if (patcher::extractZipEntry(zip, QStringLiteral("%1/su").arg(QLatin1String(abi)), su,
                                     &zipErr))
            break;
    }
    if (su.isEmpty())
        return fail(QStringLiteral("SuperSU ZIP 内未找到 su 二进制（arm/su、arm64/su、"
                                   "armv7/su、x86/su、x64/su、mips/su、mips64/su，%1）："
                                   "请确认是官方 SuperSU 刷入包")
                        .arg(zipErr));
    // ELF 校验：条目名符合但内容非可执行文件 → 拒绝注入（防损坏/恶意包）
    if (su.size() < 4 || su.left(4) != QByteArrayLiteral("\x7f""ELF"))
        return fail(QStringLiteral("ZIP 内 su 二进制不是可执行 ELF（%1）：请确认是官方 "
                                   "SuperSU 刷入包")
                        .arg(cfg.suZipPath));

    // ---- boot 解包 ----
    imgboot::BootInfo info;
    if (!imgboot::parseBootImage(bootImage, info))
        return fail(QStringLiteral("无法解析 boot 镜像（请提供原厂 boot/init_boot 镜像）"));
    if (info.ramdisk.isEmpty())
        return fail(QStringLiteral("boot 镜像不含 ramdisk（SAR/system-as-root 设备"
                                   "无 /init.rc 布局，此机制不适用）：老设备建议 Magisk"));

    QString fmt;
    patcher::detectRamdiskFormat(info.ramdisk, fmt);
    QByteArray ramdiskRaw;
    QString ramdiskErr;
    if (!patcher::decompressRamdisk(info.ramdisk, ramdiskRaw, &ramdiskErr))
        return fail(QStringLiteral("ramdisk 解压失败：%1").arg(ramdiskErr));

    CpioArchive cpio;
    if (!cpio.parse(ramdiskRaw, &ramdiskErr))
        return fail(QStringLiteral("ramdisk 不是有效 cpio：%1").arg(ramdiskErr));

    // ---- 重复修补防护（SuperSU sukernel --patch-test "Already patched,
    //      aborting" 同款）----
    if (cpio.exists(QStringLiteral("init.superuser.rc")))
        return fail(QStringLiteral("镜像已含 SuperSU 注入标记（init.superuser.rc）："
                                   "请先还原为原厂 boot 镜像后再修补"));
    // Magisk/KernelSU 修补产物拒绝叠加（init 链已被接管，追加 import 会破坏）
    if (cpio.isMagiskPatched())
        return fail(QStringLiteral("镜像为 Magisk/KernelSU 修补产物，无法叠加 SuperSU："
                                   "请先还原为原厂 boot 镜像"));

    // ---- init.rc 定位（老设备 ramdisk 布局约定，缺失即不适用）----
    CpioEntry initRcEntry;
    if (!cpio.find(QStringLiteral("init.rc"), &initRcEntry))
        return fail(QStringLiteral("ramdisk 内未找到 /init.rc（老设备 ramdisk 布局"
                                   "约定）：该 boot 镜像不适用 SuperSU ramdisk 注入，"
                                   "老设备建议 Magisk"));
    if (hasSupersuImport(initRcEntry.data))
        return fail(QStringLiteral("init.rc 已含 import /init.superuser.rc："
                                   "请先还原为原厂 boot 镜像后再修补"));

    // ---- 注入链 ----
    CpioEntry suEntry;
    suEntry.mode = 0100755; // S_IFREG | 0755（early boot 可执行副本）
    suEntry.data = su;
    cpio.addOrReplace(QStringLiteral("sbin/su"), suEntry);

    CpioEntry rcEntry;
    rcEntry.mode = 0100750; // S_IFREG | 0750（sukernel --cpio-add 750 同款）
    rcEntry.data = initSupersuRcTemplate();
    cpio.addOrReplace(QStringLiteral("init.superuser.rc"), rcEntry);

    // init.rc 末尾追加 import（dkp "add an import line if it doesn't already
    // contain one" 同款；Android init 加载期把导入文件并入同一解析结果）
    QByteArray newInitRc = initRcEntry.data;
    if (!newInitRc.endsWith('\n'))
        newInitRc.append('\n');
    newInitRc.append("import /init.superuser.rc\n");
    CpioEntry initRcUpdated = initRcEntry; // 原模式保留
    initRcUpdated.data = newInitRc;
    cpio.addOrReplace(QStringLiteral("init.rc"), initRcUpdated);

    // ---- ramdisk 重压 → boot 重打包 ----
    const QByteArray newRamdisk = patcher::compressRamdisk(cpio.serialize(), fmt);
    if (newRamdisk.isEmpty())
        return fail(QStringLiteral("ramdisk 重压失败"));
    info.ramdisk = newRamdisk;
    out = imgboot::repackBootImage(info);
    return true;
}

} // namespace patcher
