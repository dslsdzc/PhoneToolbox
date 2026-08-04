#include "root_patcher/apatch_patcher.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QProcess>
#include <QTemporaryDir>

#include <zlib.h>

#include <cstdio>
#include <cstring>

namespace patcher {
namespace {

// ============================================================
// 最小 ZIP 读取（PKWARE 规范；与 magisk_patcher.cpp / kernelsu_patcher.cpp
// 内实现同源 —— C5 按任务文件清单保持 apatch_patcher 自包含，C7 宜合并为
// 共享工具）
// ============================================================

quint16 le16(const QByteArray &d, int pos)
{
    return static_cast<quint16>(static_cast<uchar>(d[pos])) |
           (static_cast<quint16>(static_cast<uchar>(d[pos + 1])) << 8);
}

quint32 le32(const QByteArray &d, int pos)
{
    return static_cast<quint32>(static_cast<uchar>(d[pos])) |
           (static_cast<quint32>(static_cast<uchar>(d[pos + 1])) << 8) |
           (static_cast<quint32>(static_cast<uchar>(d[pos + 2])) << 16) |
           (static_cast<quint32>(static_cast<uchar>(d[pos + 3])) << 24);
}

struct ZipEntry {
    QString name;
    quint16 method = 0; // 0=store 8=deflate
    quint32 compSize = 0;
    quint32 uncompSize = 0;
    quint32 localOff = 0;
};

bool findEocd(const QByteArray &zip, quint16 *entryCount, quint32 *cdSize, quint32 *cdOffset)
{
    if (zip.size() < 22)
        return false;
    const int scan = qMin(zip.size(), 65536 + 22);
    for (int i = zip.size() - 22; i >= zip.size() - scan; --i) {
        if (i < 0)
            break;
        if (le32(zip, i) == 0x06054b50u) {
            const quint16 commentLen = le16(zip, i + 20);
            if (i + 22 + commentLen == zip.size()) {
                *entryCount = le16(zip, i + 10);
                *cdSize = le32(zip, i + 12);
                *cdOffset = le32(zip, i + 16);
                return true;
            }
        }
    }
    return false;
}

bool loadZipEntries(const QByteArray &zip, QList<ZipEntry> *out, QString *err)
{
    auto fail = [err](const QString &msg) {
        if (err)
            *err = msg;
        return false;
    };
    quint16 count;
    quint32 cdSize, cdOffset;
    if (!findEocd(zip, &count, &cdSize, &cdOffset))
        return fail(QStringLiteral("ZIP: 未找到 EOCD 记录"));
    if (count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu)
        return fail(QStringLiteral("ZIP: zip64 归档暂不支持"));
    if (static_cast<quint64>(cdOffset) + cdSize > static_cast<quint64>(zip.size()))
        return fail(QStringLiteral("ZIP: 中央目录越界"));
    qint64 pos = cdOffset;
    for (int i = 0; i < count; ++i) {
        if (pos + 46 > zip.size() || le32(zip, static_cast<int>(pos)) != 0x02014b50u)
            return fail(QStringLiteral("ZIP: 中央目录损坏"));
        const quint16 nameLen = le16(zip, static_cast<int>(pos) + 28);
        const quint16 extraLen = le16(zip, static_cast<int>(pos) + 30);
        const quint16 commentLen = le16(zip, static_cast<int>(pos) + 32);
        const qint64 entryEnd = pos + 46 + nameLen + extraLen + commentLen;
        if (entryEnd > zip.size())
            return fail(QStringLiteral("ZIP: 中央目录条目越界"));
        ZipEntry e;
        e.name = QString::fromUtf8(zip.constData() + static_cast<int>(pos) + 46, nameLen);
        e.method = le16(zip, static_cast<int>(pos) + 10);
        e.compSize = le32(zip, static_cast<int>(pos) + 20);
        e.uncompSize = le32(zip, static_cast<int>(pos) + 24);
        e.localOff = le32(zip, static_cast<int>(pos) + 42);
        out->append(e);
        pos = entryEnd;
    }
    return true;
}

bool isZip(const QByteArray &data)
{
    return data.size() >= 4 && static_cast<uchar>(data[0]) == 'P' &&
           static_cast<uchar>(data[1]) == 'K' && static_cast<uchar>(data[2]) == 0x03 &&
           static_cast<uchar>(data[3]) == 0x04;
}

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

// 从 APK 提取指定条目（含名称前缀匹配的宽松形态，如 assets/kpimg）。
bool extractZipEntryNames(const QByteArray &zip, const QString &entryName,
                          const QString &fallbackPrefix, QByteArray &out, QString *err)
{
    auto fail = [err](const QString &msg) {
        if (err)
            *err = msg;
        return false;
    };
    QList<ZipEntry> entries;
    if (!loadZipEntries(zip, &entries, err))
        return false;
    const ZipEntry *found = nullptr;
    for (const auto &e : entries) {
        if (e.name == entryName) {
            found = &e;
            break;
        }
    }
    if (!found && !fallbackPrefix.isEmpty()) {
        for (const auto &e : entries) {
            if (e.name.startsWith(fallbackPrefix)) {
                found = &e;
                break;
            }
        }
    }
    if (!found)
        return fail(QStringLiteral("ZIP: 未找到条目 %1").arg(entryName));
    if (static_cast<quint64>(found->localOff) + 30 > static_cast<quint64>(zip.size()) ||
        le32(zip, static_cast<int>(found->localOff)) != 0x04034b50u)
        return fail(QStringLiteral("ZIP: 本地文件头损坏"));
    const quint16 lhNameLen = le16(zip, static_cast<int>(found->localOff) + 26);
    const quint16 lhExtraLen = le16(zip, static_cast<int>(found->localOff) + 28);
    const qint64 dataOff = static_cast<qint64>(found->localOff) + 30 + lhNameLen + lhExtraLen;
    if (dataOff + found->compSize > zip.size())
        return fail(QStringLiteral("ZIP: 条目数据越界"));
    const QByteArray comp = zip.mid(static_cast<int>(dataOff), static_cast<int>(found->compSize));
    if (found->method == 0) {
        out = comp;
        return true;
    }
    if (found->method == 8) {
        if (found->uncompSize > 64u * 1024 * 1024)
            return fail(QStringLiteral("ZIP: 条目解压后过大"));
        QByteArray buf(static_cast<int>(found->uncompSize), Qt::Uninitialized);
        z_stream strm = {};
        if (inflateInit2(&strm, -MAX_WBITS) != Z_OK)
            return fail(QStringLiteral("ZIP: inflate 初始化失败"));
        strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(comp.constData()));
        strm.avail_in = static_cast<uInt>(comp.size());
        strm.next_out = reinterpret_cast<Bytef *>(buf.data());
        strm.avail_out = static_cast<uInt>(buf.size());
        const int rc = inflate(&strm, Z_FINISH);
        const bool streamOk = (rc == Z_STREAM_END);
        const int produced = static_cast<int>(buf.size()) - static_cast<int>(strm.avail_out);
        inflateEnd(&strm);
        if (!streamOk)
            return fail(QStringLiteral("ZIP: DEFLATE 解压失败"));
        out = buf.left(produced);
        return true;
    }
    return fail(QStringLiteral("ZIP: 不支持的压缩方法 %1").arg(found->method));
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

// 目录内按前缀找唯一文件（kptools*/kpimg* 形态，KernelPatch release 资产）。
bool findInDirByPrefix(const QString &dirPath, const QString &prefix, QString *found,
                       QString *err)
{
    QDir dir(dirPath);
    const QStringList entries = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
    for (const auto &e : entries) {
        if (e.startsWith(prefix)) {
            *found = dir.filePath(e);
            return true;
        }
    }
    if (err)
        *err = QStringLiteral("目录 %1 中未找到 %2 文件").arg(dirPath, prefix);
    return false;
}

constexpr int kKptoolsTimeoutMs = 120000;

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
        QByteArray apk;
        if (!readFile(cfg.apkPath, &apk, &sErr))
            return fail(sErr);
        if (!isZip(apk))
            return fail(QStringLiteral("APK 文件不是 ZIP 归档：%1").arg(cfg.apkPath));
        // APatch 管理器 APK 提取（官方 App prepare() 同款；APatch_11219 实物
        // 仅 arm64-v8a ABI，ABI 回退链兜底）
        static const QStringList abiChain = {QStringLiteral("arm64-v8a"),
                                             QStringLiteral("armeabi-v7a"),
                                             QStringLiteral("x86_64"),
                                             QStringLiteral("x86")};
        bool found = false;
        for (const auto &abi : abiChain) {
            const QString entry = QStringLiteral("lib/%1/libkptools.so").arg(abi);
            if (extractZipEntryNames(apk, entry, QString(), kptoolsData, &sErr)) {
                found = true;
                break;
            }
        }
        if (!found)
            return fail(QStringLiteral("APK 内未找到 libkptools.so（%1）：%2")
                            .arg(cfg.apkPath, sErr));
        // assets/kpimg 或 assets 下 kpimg* 前缀
        if (!extractZipEntryNames(apk, QStringLiteral("assets/kpimg"),
                                  QStringLiteral("assets/kpimg"), kpimgData, &sErr))
            return fail(QStringLiteral("APK 内未找到 kpimg（%1）：%2")
                            .arg(cfg.apkPath, sErr));
    } else {
        QFileInfo di(cfg.kpatchPath);
        if (!di.isDir())
            return fail(QStringLiteral("kpatchPath 不是目录（应为含 kptools/kpimg "
                                       "文件的目录）：%1").arg(cfg.kpatchPath));
        if (!findInDirByPrefix(cfg.kpatchPath, QStringLiteral("kptools"), &kptoolsPathSrc,
                               &sErr))
            return fail(sErr);
        if (!findInDirByPrefix(cfg.kpatchPath, QStringLiteral("kpimg"), &kpimgPathSrc, &sErr))
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
    QByteArray kout;
    if (!runKptools(kptools, tmp.path(), {QStringLiteral("unpack"), bootPath},
                    kKptoolsTimeoutMs, &kout, error))
        return fail(QStringLiteral("boot 镜像解包失败：%1").arg(*error));
    if (!QFile::exists(tmp.path() + QStringLiteral("/kernel")))
        return fail(QStringLiteral("boot 镜像解包失败：未产出 kernel 文件"));

    // ---- 2. CONFIG_KALLSYMS 门禁（官方 boot_patch.sh: kptools -i kernel -f
    //        | grep CONFIG_KALLSYMS=y；无 IKCONFIG 同样拒绝）----
    const QString kernelPath = tmp.path() + QStringLiteral("/kernel");
    if (!runKptools(kptools, tmp.path(), {QStringLiteral("-i"), QStringLiteral("kernel"),
                                          QStringLiteral("-f")},
                    kKptoolsTimeoutMs, &kout, error))
        return fail(QStringLiteral("内核 IKCONFIG 解析失败：%1").arg(*error));
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
                    kKptoolsTimeoutMs, &kout, error))
        return fail(QStringLiteral("内核补丁失败：%1").arg(*error));

    // ---- 4. 验证已修补（kptools -l -i kernel 输出含 patched=true，即
    //        kpimg preset（KP_MAGIC "KP1158"）已在镜像内解析成功）----
    if (!runKptools(kptools, tmp.path(),
                    {QStringLiteral("-l"), QStringLiteral("-i"), QStringLiteral("kernel")},
                    kKptoolsTimeoutMs, &kout, error))
        return fail(QStringLiteral("修补结果校验失败：%1").arg(*error));
    if (!kout.contains("patched=true"))
        return fail(QStringLiteral("修补结果校验失败：未检测到已修补标记"
                                   "（patched=true）—— kpimg 无效或与 kptools"
                                   "版本不匹配，镜像未成功修补"));

    // ---- 5. repack（官方: kptools repack "$BOOTIMAGE" → new-boot.img）----
    if (!runKptools(kptools, tmp.path(), {QStringLiteral("repack"), bootPath},
                    kKptoolsTimeoutMs, &kout, error))
        return fail(QStringLiteral("boot 镜像重打包失败：%1").arg(*error));
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
