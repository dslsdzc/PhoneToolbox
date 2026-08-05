#include "root_patcher/kernelsu_patcher.h"
#include "root_patcher/ramdisk_utils.h"
#include "root_patcher/zip_util.h"
#include "root_patcher/cpio_util.h"

#include <QFile>
#include <QRegularExpression>

#include <cctype>

namespace patcher {
namespace {

// ============================================================
// KMI 扫描：与 ksud parse_kmi（boot_patch.rs）一致 —— 在 kernel 二进制内
// 逐窗搜索 "x.y" 前缀（x∈5..9），窗口到 NUL/100B 处截断后正则
// (\d+\.\d+)(?:\S+)?(android\d+)，首个命中即返回 "androidN-x.y"。
// GKI 内核版本串实际形态："5.15.137-android13-8-00001-g97e1e0cd3750"
// ============================================================

QString scanKmiInKernel(const QByteArray &kernel)
{
    static const QRegularExpression re(QStringLiteral("(\\d+\\.\\d+)(?:\\S+)?(android\\d+)"));
    const int n = kernel.size();
    for (int i = 0; i + 3 < n; ++i) {
        const char c = kernel[i];
        if (c < '5' || c > '9' || kernel[i + 1] != '.')
            continue;
        if (!std::isdigit(static_cast<uchar>(kernel[i + 2])))
            continue;
        // 与 ksud 一致：'5' 开头要求下一位也必须是数字（排除误匹配）
        if (c == '5' && !std::isdigit(static_cast<uchar>(kernel[i + 3])))
            continue;
        QByteArray window = kernel.mid(i, qMin(100, n - i));
        const int nul = window.indexOf('\0');
        if (nul >= 0)
            window.truncate(nul);
        const QRegularExpressionMatch m = re.match(QString::fromLatin1(window));
        if (m.hasMatch())
            return m.captured(2) + QLatin1Char('-') + m.captured(1);
    }
    return QString();
}

// boot cmdline 兜底：androidboot.kmi=androidNN-x.y（部分非标准内核自带）
QString scanKmiInCmdline(const QByteArray &cmdline)
{
    static const QRegularExpression re(
        QStringLiteral("androidboot\\.kmi\\s*=\\s*([\\w\\.-]+)"));
    const QRegularExpressionMatch m = re.match(QString::fromLatin1(cmdline));
    if (!m.hasMatch())
        return QString();
    const QString v = m.captured(1);
    // 校验 androidNN-x.y 形态，避免把无关键值当 KMI
    static const QRegularExpression shape(
        QStringLiteral("^android\\d+-\\d+\\.\\d+[^-]*$"));
    return shape.match(v).hasMatch() ? v : QString();
}

// 加载注入物文件（失败写 err 并返回空）。目录直接拒绝；zip 按需在调用处处理。
bool readFile(const QString &path, QByteArray *data, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("无法打开文件：%1").arg(path);
        return false;
    }
    *data = f.readAll();
    f.close();
    // 目录/不可读文件：POSIX 下目录可 open 但 readAll 为空 —— 归为失败
    if (data->isEmpty()) {
        if (err)
            *err = QStringLiteral("文件为空或不可读：%1").arg(path);
        return false;
    }
    return true;
}

} // namespace

// ============================================================
// KernelSuPatcher
// ============================================================

bool KernelSuPatcher::patch(const QByteArray &bootImage, const PatchConfig &cfg,
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

    const bool isKsu =
        cfg.type == RootType::KernelSU || cfg.type == RootType::KernelSU_Next ||
        cfg.type == RootType::SukiSU || cfg.type == RootType::ReSukiSU;
    if (!isKsu)
        return fail(QStringLiteral("KernelSuPatcher 仅处理 KernelSU 系 RootType"));

    // variant 归一：RootType 默认映射，cfg.variant 显式覆盖
    QString variant = cfg.variant.toLower();
    if (variant.isEmpty()) {
        switch (cfg.type) {
        case RootType::KernelSU_Next:
            variant = QStringLiteral("next");
            break;
        case RootType::SukiSU:
            variant = QStringLiteral("suki");
            break;
        case RootType::ReSukiSU:
            variant = QStringLiteral("resuki");
            break;
        default:
            variant = QStringLiteral("official");
        }
    }
    if (variant != QLatin1String("official") && variant != QLatin1String("next") &&
        variant != QLatin1String("suki") && variant != QLatin1String("resuki"))
        return fail(QStringLiteral("未知 KernelSU 变体：%1").arg(cfg.variant));

    // ---- 注入物加载 ----
    QByteArray ko, wrapper;
    bool koIsZip = false;
    if (!cfg.koPath.isEmpty()) {
        QByteArray raw;
        QString ferr;
        if (!readFile(cfg.koPath, &raw, &ferr))
            return fail(ferr);
        if (raw.isEmpty())
            return fail(QStringLiteral("注入物文件为空：%1").arg(cfg.koPath));
        koIsZip = isZip(raw);
        ko = raw; // zip 时保留原样，解包逻辑在下（LKM 包 / AnyKernel3 判定）
    }
    if (!cfg.apkPath.isEmpty()) {
        QByteArray raw;
        QString ferr;
        if (!readFile(cfg.apkPath, &raw, &ferr))
            return fail(ferr);
        if (raw.isEmpty())
            return fail(QStringLiteral("注入物文件为空：%1").arg(cfg.apkPath));
        if (isZip(raw)) {
            // ksuinit.zip 形态（ReSukiSU CI 资产，条目为
            // "aarch64-unknown-linux-musl/release/ksuinit"）：按条目名提取
            QStringList names;
            QString zerr;
            if (!zipEntryNames(raw, &names, &zerr))
                return fail(zerr);
            QString target;
            for (const auto &nm : names) {
                if (nm == QLatin1String("ksuinit") || nm.endsWith(QLatin1String("/ksuinit"))) {
                    target = nm;
                    break;
                }
            }
            if (target.isEmpty())
                return fail(QStringLiteral("ZIP 内未找到 ksuinit 条目（%1）").arg(zerr));
            if (!extractZipEntry(raw, target, wrapper, &zerr))
                return fail(zerr);
        } else {
            wrapper = raw;
        }
    }

    imgboot::BootInfo info;
    if (!imgboot::parseBootImage(bootImage, info))
        return fail(QStringLiteral("无法解析 boot 镜像"));

    // KMI：kernel 扫描优先（权威路径），其次 cmdline，最后用户指定
    //（cfg.deviceKmi —— 供 kernel 无 KMI 串时的 LKM 包条目选择/错误提示）
    QString kmi = detectKmi(info);
    if (kmi.isEmpty() && !cfg.deviceKmi.isEmpty())
        kmi = cfg.deviceKmi;

    // ---- 决策：AnyKernel3 内核替换（非 GKI 路径）vs LKM ramdisk 注入 ----
    if (koIsZip) {
        // zip 内含 *_kernelsu.ko → LKM 包（ReSukiSU lkm-all.zip 形态）；
        // 否则按 AnyKernel3 预置内核包处理
        QStringList names;
        QString zerr;
        if (!zipEntryNames(ko, &names, &zerr))
            return fail(zerr);
        QString koEntry;
        const QString wanted =
            kmi.isEmpty() ? QString() : QStringLiteral("%1_kernelsu.ko").arg(kmi);
        for (const auto &nm : names) {
            if (!nm.endsWith(QLatin1String("_kernelsu.ko")))
                continue;
            if (!wanted.isEmpty() && nm == wanted) {
                koEntry = nm;
                break;
            }
            if (koEntry.isEmpty())
                koEntry = nm;
        }
        if (!wanted.isEmpty() && koEntry != wanted)
            // KMI 已知但包内无对应 ko（如过期的 lkm-all 包）→ 明确报错而非
            // 拿错内核模块注入
            return fail(QStringLiteral("LKM 包内未找到 %1_kernelsu.ko").arg(wanted));
        if (wanted.isEmpty() && !koEntry.isEmpty())
            // KMI 未知（kernel 扫描与 cmdline 均无）且包内含多 ko —— 与 ksud
            // parse_kmi 失败时要求手动指定一致：拒绝静默任选第一个，提示
            // 经 cfg.deviceKmi 指定
            return fail(QStringLiteral("LKM 包内含内核模块但未识别 KMI：请经 "
                                       "deviceKmi 指定（如 android13-5.15）后重试"));
        if (!koEntry.isEmpty()) {
            if (!extractZipEntry(ko, koEntry, ko, &zerr))
                return fail(zerr);
            koIsZip = false;
        }
    }

    if (!koIsZip && !ko.isEmpty()) {
        // ---- LKM 注入（ksud boot-patch LKM 机制，见头文件注释）----
        if (wrapper.isEmpty()) {
            // next/suki 的 ksuinitUrl 回退官方 KernelSU 资产 —— 错误文案
            // 标注该来源与不保证声明（fork 定制行为可能不兼容）
            const bool officialFallback =
                variant == QLatin1String("next") || variant == QLatin1String("suki");
            return fail(QStringLiteral("缺少 ksuinit init wrapper：请经 ksuinitUrl(variant) "
                                       "下载后填入 apkPath%1")
                            .arg(officialFallback
                                     ? QStringLiteral("（next/suki 回退的 ksuinit 为官方 "
                                                      "KernelSU 版本，fork 定制行为不保证）")
                                     : QString()));
        }
        QString fmt = QStringLiteral("raw"); // 空 ramdisk 时 detect 返回 false，显式归 raw
        patcher::detectRamdiskFormat(info.ramdisk, fmt);
        QByteArray ramdiskRaw;
        QString ramdiskErr;
        if (!patcher::decompressRamdisk(info.ramdisk, ramdiskRaw, &ramdiskErr))
            return fail(QStringLiteral("ramdisk 解压失败：%1").arg(ramdiskErr));

        CpioArchive cpio;
        // 无 ramdisk（ksud "No ramdisk, create by default"）：空 cpio 直接注入
        if (!ramdiskRaw.isEmpty() && !cpio.parse(ramdiskRaw, &ramdiskErr))
            return fail(QStringLiteral("ramdisk 不是有效 cpio：%1").arg(ramdiskErr));

        // Magisk 修补产物拒绝（ksud: "Cannot work with Magisk patched image"）
        if (cpio.isMagiskPatched())
            return fail(QStringLiteral("镜像为 Magisk 修补产物，KernelSU 无法兼容："
                                       "请先还原为原厂 boot/init_boot 镜像"));

        // 已含 kernelsu.ko → 已修补：与 ksud 一致跳过 init→init.real 改名
        //（保留既有 init.real），幂等重写 init 与 kernelsu.ko
        if (!cpio.exists(QStringLiteral("kernelsu.ko")) &&
            cpio.exists(QStringLiteral("init"))) {
            cpio.rename(QStringLiteral("init"), QStringLiteral("init.real"));
        }
        CpioEntry initEntry;
        initEntry.mode = 0100755; // S_IFREG | 0755（ksud add 0755 init）
        initEntry.data = wrapper;
        cpio.addOrReplace(QStringLiteral("init"), initEntry);
        CpioEntry koCpioEntry;
        koCpioEntry.mode = 0100755; // S_IFREG | 0755（ksud add 0755 kernelsu.ko）
        koCpioEntry.data = ko;
        cpio.addOrReplace(QStringLiteral("kernelsu.ko"), koCpioEntry);

        const QByteArray newRamdisk = patcher::compressRamdisk(cpio.serialize(), fmt);
        if (newRamdisk.isEmpty())
            return fail(QStringLiteral("ramdisk 重压失败"));
        info.ramdisk = newRamdisk;
        out = imgboot::repackBootImage(info);
        return true;
    }

    // ---- 注入物缺失 / AnyKernel3 包 ----
    if (koIsZip) {
        // AnyKernel3 预置内核包：解包提取内核文件 → 替换 boot kernel 段
        //（AnyKernel3 约定内核文件置于包根：Image.gz.dtb > Image.gz >
        // Image.lz4 > Image.lzma > Image.xz > Image.bz2 > Image）
        static const QStringList kernelCandidates = {
            QStringLiteral("Image.gz.dtb"), QStringLiteral("Image.gz"),
            QStringLiteral("Image.lz4"),   QStringLiteral("Image.lzma"),
            QStringLiteral("Image.xz"),    QStringLiteral("Image.bz2"),
            QStringLiteral("Image")};
        QStringList names;
        QString zerr;
        if (!zipEntryNames(ko, &names, &zerr))
            return fail(zerr);
        QString kernelEntry;
        for (const auto &cand : kernelCandidates) {
            if (names.contains(cand)) {
                kernelEntry = cand;
                break;
            }
        }
        if (kernelEntry.isEmpty())
            return fail(QStringLiteral("ZIP 内未找到内核文件（Image/Image.gz 等，AnyKernel3 "
                                       "包结构）：%1").arg(zerr));
        QByteArray kernelImage;
        if (!extractZipEntry(ko, kernelEntry, kernelImage, &zerr))
            return fail(zerr);
        if (kernelImage.isEmpty())
            return fail(QStringLiteral("ZIP 内内核文件为空：%1").arg(kernelEntry));
        info.kernel = kernelImage; // repack 按新内核长度重写 kernel_size
        out = imgboot::repackBootImage(info);
        return true;
    }

    // ---- 无注入物：明确错误 + 兜底建议（诚实边界）----
    if (kmi.isEmpty())
        return fail(QStringLiteral(
            "未识别 KMI（非 GKI/非标准内核）且未提供注入物。KernelSU LKM 仅适用 "
            "GKI 内核（KMI 匹配）；非 GKI 设备需内核编译期集成或社区预置内核 "
            "（AnyKernel3 包）刷入。可改用：①APatch（内核 3.18+ 仅需 boot.img "
            "内核补丁）；②Magisk（ramdisk 注入）"));
    if (!supportedKmis(variant).contains(kmi))
        return fail(QStringLiteral("KMI %1 不在 %2 变体支持列表（%3）：可改用 APatch/"
                                   "Magisk，或提供该设备内核对应版本的 kernelsu.ko"
                                   "（社区构建，经 koPath 指定）")
                        .arg(kmi, variant, supportedKmis(variant).join(", ")));
    return fail(QStringLiteral("已识别 KMI=%1，但未提供 kernelsu.ko：请经 koUrl(\"%2\", \"%3\") "
                               "下载 %4_kernelsu.ko 后填入 koPath")
                    .arg(kmi, variant, kmi, kmi));
}

QString KernelSuPatcher::detectKmi(const imgboot::BootInfo &info)
{
    if (!info.kernel.isEmpty()) {
        const QString fromKernel = scanKmiInKernel(info.kernel);
        if (!fromKernel.isEmpty())
            return fromKernel;
    }
    if (!info.cmdline.isEmpty()) {
        const QString fromCmdline = scanKmiInCmdline(info.cmdline);
        if (!fromCmdline.isEmpty())
            return fromCmdline;
    }
    return QString();
}

QStringList KernelSuPatcher::supportedKmis(const QString &variant)
{
    const QString v = variant.toLower();
    if (v != QLatin1String("official") && v != QLatin1String("next") &&
        v != QLatin1String("suki") && v != QLatin1String("resuki"))
        return {};
    // 联网验证 2026-08-05：四个渠道 release 资产同为 7 个 {kmi}_kernelsu.ko
    //（tiann/KernelSU v3.2.5、KernelSU-Next v3.3.0、SukiSU-Ultra v4.1.3、
    //  cctv18/ReSukiSU_CI lkm-all.zip —— 均经 gh api 逐一核对）
    return {QStringLiteral("android12-5.10"), QStringLiteral("android13-5.10"),
            QStringLiteral("android13-5.15"), QStringLiteral("android14-5.15"),
            QStringLiteral("android14-6.1"),  QStringLiteral("android15-6.6"),
            QStringLiteral("android16-6.12")};
}

QUrl KernelSuPatcher::koUrl(const QString &variant, const QString &kmi)
{
    const QString v = variant.toLower();
    // GitHub releases/latest/download/<asset> 重定向至最新 release 同名资产
    //（联网验证 2026-08：official android13-5.15_kernelsu.ko 200）
    if (v == QLatin1String("official"))
        return QUrl(QStringLiteral("https://github.com/tiann/KernelSU/releases/latest/"
                                   "download/%1_kernelsu.ko").arg(kmi));
    if (v == QLatin1String("next"))
        return QUrl(QStringLiteral("https://github.com/KernelSU-Next/KernelSU-Next/"
                                   "releases/latest/download/%1_kernelsu.ko").arg(kmi));
    if (v == QLatin1String("suki"))
        return QUrl(QStringLiteral("https://github.com/SukiSU-Ultra/SukiSU-Ultra/"
                                   "releases/latest/download/%1_kernelsu.ko").arg(kmi));
    if (v == QLatin1String("resuki"))
        // ReSukiSU 无 GitHub 主仓 release 资产（404 验证），发行通道为
        // cctv18/ReSukiSU_CI：lkm-all.zip 内含全部 7 个 ko
        return QUrl(QStringLiteral("https://github.com/cctv18/ReSukiSU_CI/releases/"
                                   "latest/download/lkm-all.zip"));
    return QUrl();
}

QUrl KernelSuPatcher::ksuinitUrl(const QString &variant)
{
    const QString v = variant.toLower();
    if (v == QLatin1String("official"))
        // ksuinit 资产为静态链接 aarch64 ELF（联网验证 v3.2.5 产物）
        return QUrl(QStringLiteral("https://github.com/tiann/KernelSU/releases/latest/"
                                   "download/ksuinit"));
    if (v == QLatin1String("resuki"))
        // ksuinit.zip 内含 aarch64-unknown-linux-musl/release/ksuinit
        return QUrl(QStringLiteral("https://github.com/cctv18/ReSukiSU_CI/releases/"
                                   "latest/download/ksuinit.zip"));
    if (v == QLatin1String("next") || v == QLatin1String("suki"))
        // KernelSU-Next / SukiSU-Ultra 不发布独立 ksuinit 资产（仅内嵌于
        // ksud 二进制，API 404 验证）→ 回退官方 KernelSU ksuinit：wrapper
        // 为 KernelSU 通用 init 链（fork 差异在 .ko 与管理器），可混用。
        // 标注：ksuinit 为官方 KernelSU 版本，fork 定制行为不保证
        //（如 Next 的自定义 init 参数/钩子），文档与错误文案须同步说明。
        return QUrl(QStringLiteral("https://github.com/tiann/KernelSU/releases/latest/"
                                   "download/ksuinit"));
    return QUrl();
}

QString KernelSuPatcher::assetKey(const QString &variant)
{
    const QString v = variant.toLower();
    if (v == QLatin1String("next"))
        return QStringLiteral("ksu-next");
    if (v == QLatin1String("suki"))
        return QStringLiteral("ksu-suki");
    if (v == QLatin1String("resuki"))
        return QStringLiteral("ksu-resuki");
    return QStringLiteral("ksu-official");
}

} // namespace patcher
