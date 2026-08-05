#include "root_patcher/apatch_patcher.h"
#include "root_patcher/zip_util.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSysInfo>
#include <QTemporaryDir>

#include <functional>

namespace patcher {
namespace {

// 读文件（目录/空文件/不可读一律失败，写 err）。
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
    if (data->isEmpty()) {
        if (err)
            *err = QStringLiteral("文件为空或不可读：%1").arg(path);
        return false;
    }
    return true;
}

// 写文件（失败写 err）。
bool writeFile(const QString &path, const QByteArray &data, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = QStringLiteral("无法写入文件：%1").arg(path);
        return false;
    }
    if (f.write(data) != data.size()) {
        if (err)
            *err = QStringLiteral("写入文件失败：%1").arg(path);
        return false;
    }
    f.close();
    return true;
}

// 运行 kptools 子进程。cwd 为工作目录，args 为参数；timeoutMs 超时即杀。
// 成功返回 true 并回填输出（stdout+stderr），失败写 error。
bool runKptools(const QString &kptoolsPath, const QString &cwd, const QStringList &args,
                int timeoutMs, QByteArray *output, QString *error)
{
    auto fail = [error](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    QProcess proc;
    proc.setProgram(kptoolsPath);
    proc.setArguments(args);
    proc.setWorkingDirectory(cwd);
    proc.start();
    if (!proc.waitForStarted(10000))
        return fail(QStringLiteral("kptools 无法启动：%1").arg(proc.errorString()));
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(2000);
        return fail(QStringLiteral("kptools 执行超时（%1s）：%2")
                        .arg(timeoutMs / 1000)
                        .arg(args.join(QLatin1Char(' '))));
    }
    *output = proc.readAllStandardOutput() + proc.readAllStandardError();
    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
        return fail(QStringLiteral("kptools %1 失败（rc=%2）：%3")
                        .arg(args.join(QLatin1Char(' ')))
                        .arg(proc.exitCode())
                        .arg(QString::fromUtf8(*output).simplified().left(400)));
    return true;
}

// 目录内按前缀找文件（kptools*/kpimg* 形态，KernelPatch release 资产）。
// rank(name) 打分：负值候选跳过（如 .7z 压缩包），正分按高者优先，同分按
// entryList 字母序（首个）。精确名优先（C8 吸收的 C7 Minor：手动目录同时
// 含 "kpimg" 与 "kpimg-*" 时取精确名，与 zip_util extractZipEntryByPrefix
// 语义一致）。无可用候选时写 err。
bool findInDirByPrefix(const QString &dirPath, const QString &prefix,
                       const std::function<int(const QString &)> &rank, QString *found,
                       QString *err)
{
    QDir dir(dirPath);
    const QStringList entries = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
    if (entries.contains(prefix)) {
        *found = dir.filePath(prefix);
        return true;
    }
    int best = -1; // 负分候选视为不可用（.7z 等不可执行压缩包）
    QString picked;
    for (const auto &e : entries) {
        if (!e.startsWith(prefix))
            continue;
        const int r = rank(e);
        if (r < 0)
            continue;
        if (r > best) {
            best = r;
            picked = e;
        }
    }
    if (picked.isEmpty()) {
        if (err)
            *err = QStringLiteral("目录 %1 中未找到 %2 文件").arg(dirPath, prefix);
        return false;
    }
    *found = dir.filePath(picked);
    return true;
}

// 默认评分：任意前缀候选同权（kpimg* 形态，字母序首个）。
int plainRank(const QString &)
{
    return 1;
}

// 宿主平台（linux/mac/win，Q_OS_* 编译期判定）。测试经 APATCH_PLATFORM
// 环境变量注入模拟平台（同 APATCH_HOST_ARCH 模式，见 test_patcher.cpp
// ScopedPlatform —— 使 Windows 分支可在 Linux CI 上测试）；非法注入值
// 回退真实平台。
QString hostPlatform()
{
    const QByteArray sim = qgetenv("APATCH_PLATFORM");
    if (!sim.isEmpty()) {
        const QString s = QString::fromLatin1(sim).toLower();
        if (s == QLatin1String("win") || s == QLatin1String("linux") ||
            s == QLatin1String("mac"))
            return s;
    }
#if defined(Q_OS_WIN)
    return QStringLiteral("win");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("mac");
#else
    return QStringLiteral("linux");
#endif
}

// kptools 候选评分（审查 Minor）：KernelPatch release 预编译资产多平台共存
//（kptools-linux / kptools-mac / kptools-msys2-win.7z，联网验证 0.13.3）——
// 修复前按字母序取首个，macOS/Windows 宿主会选到 kptools-linux 而非本平台
// 二进制。按宿主平台匹配（hostPlatform，测试可注入）：含本平台名
//（linux/mac/win）→ 2；其余前缀候选 → 1；.7z 压缩包（不可执行）→ -1。
// **Windows 复核修复（2 轮）**：官方 kptools-msys2-win.7z 解压后是
// win/kptools.exe（"win" 是目录名，文件名不含 "win"）—— 仅 contains("win")
// 会把解压产物也跳过，Windows 手动路径全场景不可用；现放行
// contains("win") || endsWith(".exe")。kptools-linux/mac 为其他平台 ELF，
// Windows 上必然无法执行 → -1 跳过；无候选时调用侧报解压指引（含
// win/kptools.exe 与 msys-2.0.dll），不再按字母序选中 linux/mac 后 exec
// 失败。
int kptoolsRank(const QString &name)
{
    if (name.endsWith(QLatin1String(".7z"), Qt::CaseInsensitive))
        return -1;
    const QString platform = hostPlatform();
    bool hostMatch = false;
    if (platform == QLatin1String("linux")) {
        hostMatch = name.contains(QLatin1String("linux"));
    } else if (platform == QLatin1String("mac")) {
        hostMatch = name.contains(QLatin1String("mac"));
    } else { // win
        hostMatch = name.contains(QLatin1String("win")) ||
                    name.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive);
        if (!hostMatch)
            return -1; // 其他平台 ELF（linux/mac）在 Windows 不可执行
    }
    return hostMatch ? 2 : 1;
}

constexpr int kKptoolsTimeoutMs = 120000;

// 宿主 CPU 架构。测试经 APATCH_HOST_ARCH 环境变量模拟 arm64 宿主
//（见 tests/test_patcher.cpp ScopedHostArch —— 真实宿主一律拒绝 apkPath，
// 仅测试钩子可放行）；真实环境直接取 QSysInfo（x86_64/arm64/arm/riscv64...）。
QString hostCpuArch()
{
    const QByteArray sim = qgetenv("APATCH_HOST_ARCH");
    if (!sim.isEmpty())
        return QString::fromLatin1(sim);
    return QSysInfo::currentCpuArchitecture();
}

// APK 内 libkptools.so 为 Android arm64 ELF（APatch 官方 APK 实测仅
// arm64-v8a，即 KernelPatch release 的 kptools-android 同一资产）——
// **bionic 动态链接 PIE（interpreter /system/bin/linker64）**，任何 PC
// 宿主（含 arm64 Linux）都无法 QProcess exec（复核实证证伪"arm64 放行"
// 假设）。→ apkPath 路径全宿主门禁拒绝，指引 kpatchPath 手动指定宿主
// 原生 kptools-linux（Q_OS_* 平台匹配）。空列表 = apkPath 不可用。
// arm64 放行仅保留为 APATCH_HOST_ARCH=arm64 测试钩子（模拟宿主，供
// apkPath 提取/流程测试覆盖）。
QStringList executableApkKptoolsAbis()
{
    // 仅测试钩子（ScopedHostArch("arm64")）模拟放行；真实宿主一律空链
    if (qEnvironmentVariableIsSet("APATCH_HOST_ARCH") &&
        hostCpuArch() == QLatin1String("arm64"))
        return {QStringLiteral("arm64-v8a")};
    return {};
}

} // namespace

// ============================================================
// APatchPatcher
// ============================================================

bool APatchPatcher::patch(const QByteArray &bootImage, const PatchConfig &cfg,
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

    const bool isApatch =
        cfg.type == RootType::APatch || cfg.type == RootType::KernelPatch;
    if (!isApatch)
        return fail(QStringLiteral("APatchPatcher 仅处理 APatch/KernelPatch RootType"));

    // ---- 注入物来源解析（apkPath 与 kpatchPath 互斥）----
    if (!cfg.apkPath.isEmpty() && !cfg.kpatchPath.isEmpty())
        return fail(QStringLiteral("apkPath 与 kpatchPath 不可同时指定："
                                   "APatch 官方流程与手动 kptools/kpimg 目录"
                                   "互斥，请二选一"));
    if (cfg.apkPath.isEmpty() && cfg.kpatchPath.isEmpty())
        return fail(QStringLiteral("缺少 APatch 注入物：请提供 APatch 管理器 APK"
                                   "（apkPath，官方 release 单资产）或含 kptools/"
                                   "kpimg 的目录（kpatchPath，KernelPatch release"
                                   "预编译资产解包形态）"));

    // ---- boot 镜像前置校验 ----
    if (bootImage.size() < 8 || bootImage.left(8) != QByteArrayLiteral("ANDROID!"))
        return fail(QStringLiteral("非 Android boot 镜像（缺少 ANDROID! 魔数）："
                                   "请提供原厂 boot/init_boot 镜像"));

    QTemporaryDir tmp;
    if (!tmp.isValid())
        return fail(QStringLiteral("无法创建临时工作目录"));

    // 解出 kptools 与 kpimg（临时目录内）
    QByteArray kptoolsData, kpimgData;
    QString kptoolsPathSrc, kpimgPathSrc;
    QString sErr;
    if (!cfg.apkPath.isEmpty()) {
        // 宿主架构门禁（审查 Important）：APK 内 libkptools.so 为 Android
        // arm64 ELF（bionic 动态链接 PIE，interpreter /system/bin/linker64），
        // QProcess 直接 exec 在任何 PC 宿主（含 arm64 Linux）都必然失败 ——
        // 在任何 APK 读取/解包之前提前拒绝并指引 kpatchPath 手动路径
        const QStringList abiChain = executableApkKptoolsAbis();
        if (abiChain.isEmpty())
            return fail(QStringLiteral(
                "APK 内 libkptools.so 为 arm64 Android 二进制，无法在本机运行："
                "请下载 KernelPatch release 的 kptools-linux 并以 kpatchPath 指定"));
        QByteArray apk;
        if (!readFile(cfg.apkPath, &apk, &sErr))
            return fail(sErr);
        if (!isZip(apk))
            return fail(QStringLiteral("APK 文件不是 ZIP 归档：%1").arg(cfg.apkPath));
        // APatch 管理器 APK 提取（官方 App prepare() 同款；APatch_11219 实物
        // 仅 arm64-v8a ABI，回退链仅含宿主可执行 ABI —— 不可执行 ABI 自动跳过）
        bool found = false;
        for (const auto &abi : abiChain) {
            const QString entry = QStringLiteral("lib/%1/libkptools.so").arg(abi);
            if (extractZipEntry(apk, entry, kptoolsData, &sErr)) {
                found = true;
                break;
            }
        }
        if (!found)
            return fail(QStringLiteral("APK 内未找到 libkptools.so（%1）：%2")
                            .arg(cfg.apkPath, sErr));
        // assets/kpimg 或 assets 下 kpimg* 前缀
        if (!extractZipEntryByPrefix(apk, QStringLiteral("assets/kpimg"), kpimgData,
                                    &sErr))
            return fail(QStringLiteral("APK 内未找到 kpimg（%1）：%2")
                            .arg(cfg.apkPath, sErr));
    } else {
        QFileInfo di(cfg.kpatchPath);
        if (!di.isDir())
            return fail(QStringLiteral("kpatchPath 不是目录（应为含 kptools/kpimg "
                                       "文件的目录）：%1").arg(cfg.kpatchPath));
        // kptools 按宿主平台匹配（kptools-linux/mac/win，跳过 .7z；
        // Windows 仅放行含 "win" 或 .exe 后缀候选）；kpimg 无平台区分
        //（kpimg-android），任意候选取字母序首个
        if (!findInDirByPrefix(cfg.kpatchPath, QStringLiteral("kptools"), kptoolsRank,
                               &kptoolsPathSrc, &sErr)) {
            if (hostPlatform() == QLatin1String("win"))
                // release 的 Windows 资产本身是 .7z（kptools-msys2-win.7z，
                // 解压后为 win/kptools.exe + msys-2.0.dll）—— 无 win/.exe
                // 候选时给出解压指引而非按字母序选中 linux/mac 二进制
                return fail(QStringLiteral("未找到 kptools（请先解压 "
                                           "kptools-msys2-win.7z，含 win/kptools.exe "
                                           "与 msys-2.0.dll）：%1").arg(sErr));
            return fail(sErr);
        }
        if (!findInDirByPrefix(cfg.kpatchPath, QStringLiteral("kpimg"), plainRank,
                               &kpimgPathSrc, &sErr))
            return fail(sErr);
    }

    const QString kptools = tmp.path() + QStringLiteral("/kptools");
    const QString kpimg = tmp.path() + QStringLiteral("/kpimg");
    if (!kptoolsPathSrc.isEmpty()) {
        if (!QFile::copy(kptoolsPathSrc, kptools))
            return fail(QStringLiteral("无法复制 kptools 到工作目录：%1").arg(kptoolsPathSrc));
    } else if (!writeFile(kptools, kptoolsData, &sErr)) {
        return fail(sErr);
    }
    if (!kpimgPathSrc.isEmpty()) {
        if (!QFile::copy(kpimgPathSrc, kpimg))
            return fail(QStringLiteral("无法复制 kpimg 到工作目录：%1").arg(kpimgPathSrc));
    } else if (!writeFile(kpimg, kpimgData, &sErr)) {
        return fail(sErr);
    }
    // kptools 需要执行位（APK 提取/zip 解压后通常丢失）
    QFile kpFile(kptools);
    if (!kpFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                               QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                               QFileDevice::ExeGroup | QFileDevice::ReadOther |
                               QFileDevice::ExeOther))
        return fail(QStringLiteral("无法设置 kptools 执行权限：%1").arg(kptools));

    const QString bootPath = tmp.path() + QStringLiteral("/boot.img");
    if (!writeFile(bootPath, bootImage, &sErr))
        return fail(sErr);

    // ---- 1. unpack（官方 boot_patch.sh: ./kptools unpack "$BOOTIMAGE"）----
    // 各步骤错误信息经局部 stepErr 拼接（error 可空，禁止 *error 解引用）
    QByteArray kout;
    QString stepErr;
    if (!runKptools(kptools, tmp.path(), {QStringLiteral("unpack"), bootPath},
                    kKptoolsTimeoutMs, &kout, &stepErr))
        return fail(QStringLiteral("boot 镜像解包失败：%1").arg(stepErr));
    if (!QFile::exists(tmp.path() + QStringLiteral("/kernel")))
        return fail(QStringLiteral("boot 镜像解包失败：未产出 kernel 文件"));

    // ---- 2. CONFIG_KALLSYMS 门禁（官方 boot_patch.sh: kptools -i kernel -f
    //        | grep CONFIG_KALLSYMS=y；无 IKCONFIG 同样拒绝）----
    const QString kernelPath = tmp.path() + QStringLiteral("/kernel");
    if (!runKptools(kptools, tmp.path(), {QStringLiteral("-i"), QStringLiteral("kernel"),
                                          QStringLiteral("-f")},
                    kKptoolsTimeoutMs, &kout, &stepErr))
        return fail(QStringLiteral("内核 IKCONFIG 解析失败：%1").arg(stepErr));
    if (!kout.contains("CONFIG_KALLSYMS=y"))
        return fail(QStringLiteral("内核未启用 CONFIG_KALLSYMS（或未启用 "
                                   "CONFIG_IKCONFIG 无法校验）：APatch 要求 "
                                   "CONFIG_KALLSYMS=y，该内核不支持 APatch。"
                                   "可改用 Magisk（ramdisk 注入）"));

    // ---- 3. patch（官方: mv kernel kernel.ori; kptools -p -i kernel.ori
    //        -k kpimg -o kernel；superkey 用官方默认 root-skey 零哈希模式，
    //        不传 -s/-S）----
    if (!QFile::rename(kernelPath, tmp.path() + QStringLiteral("/kernel.ori")))
        return fail(QStringLiteral("无法准备待修补内核文件"));
    if (!runKptools(kptools, tmp.path(),
                    {QStringLiteral("-p"), QStringLiteral("-i"),
                     QStringLiteral("kernel.ori"), QStringLiteral("-k"),
                     QStringLiteral("kpimg"), QStringLiteral("-o"),
                     QStringLiteral("kernel")},
                    kKptoolsTimeoutMs, &kout, &stepErr))
        return fail(QStringLiteral("内核补丁失败：%1").arg(stepErr));

    // ---- 4. 验证已修补（kptools -l -i kernel 输出含 patched=true，即
    //        kpimg preset（KP_MAGIC "KP1158"）已在镜像内解析成功）----
    if (!runKptools(kptools, tmp.path(),
                    {QStringLiteral("-l"), QStringLiteral("-i"), QStringLiteral("kernel")},
                    kKptoolsTimeoutMs, &kout, &stepErr))
        return fail(QStringLiteral("修补结果校验失败：%1").arg(stepErr));
    if (!kout.contains("patched=true"))
        return fail(QStringLiteral("修补结果校验失败：未检测到已修补标记"
                                   "（patched=true）—— kpimg 无效或与 kptools"
                                   "版本不匹配，镜像未成功修补"));

    // ---- 5. repack（官方: kptools repack "$BOOTIMAGE" → new-boot.img）----
    if (!runKptools(kptools, tmp.path(), {QStringLiteral("repack"), bootPath},
                    kKptoolsTimeoutMs, &kout, &stepErr))
        return fail(QStringLiteral("boot 镜像重打包失败：%1").arg(stepErr));
    const QString newBoot = tmp.path() + QStringLiteral("/new-boot.img");
    if (!QFile::exists(newBoot))
        return fail(QStringLiteral("boot 镜像重打包失败：未产出 new-boot.img"));
    if (!readFile(newBoot, &out, &sErr))
        return fail(sErr);
    if (out.isEmpty())
        return fail(QStringLiteral("重打包产物为空"));
    return true;
}

QUrl APatchPatcher::apkDownloadUrl()
{
    // APatch 官方 release 单资产（APatch_<build>_<sha>_HEAD-release-signed.apk，
    // 联网验证 2026-08-05 近 8 个 release 均如此）；asset 名随版本变化 → 返回
    // releases/latest 页，UI 层经 GitHub API 解析具体 asset
    return QUrl(QStringLiteral("https://github.com/bmax121/APatch/releases/latest"));
}

QString APatchPatcher::assetKey()
{
    return QStringLiteral("apatch");
}

} // namespace patcher
