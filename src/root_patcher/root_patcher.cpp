// root_patcher.cpp —— 文件级修补入口 patchFile（Task C6）
//
// 职责（UI 层唯一修补入口）：
//   1. 读入 boot 镜像文件字节
//   2. RootPatcher::create() 工厂按 cfg.type 派发到对应 patcher
//   3. 成功：原镜像原样备份到 "<源文件>.orig.bak"，修补产物写
//      "<基名>_patched.img"（基名 = 去掉扩展名的文件名）
//
// 契约：失败返回 false 并写 error（非空），不落任何产物（已存在 .orig.bak
// 时拒绝修补，防止覆盖不可再生的原厂备份）；error / outPath 可传 nullptr。

#include "root_patcher/root_patcher.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <memory>

namespace patcher {

namespace {

bool fail(QString *error, const QString &msg)
{
    if (error)
        *error = msg;
    return false;
}

// 写文件；失败时清理不完整产物并报错（error 可空）
bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return fail(error,
                    QStringLiteral("无法写入 %1：%2").arg(path, f.errorString()));
    if (f.write(data) != data.size()) {
        f.close();
        QFile::remove(path); // 清理不完整产物
        return fail(error,
                    QStringLiteral("写入 %1 不完整：%2").arg(path, f.errorString()));
    }
    return true;
}

QString rootTypeName(RootType t)
{
    switch (t) {
    case RootType::Magisk:
        return QStringLiteral("Magisk");
    case RootType::MagiskAlpha:
        return QStringLiteral("Magisk Alpha");
    case RootType::Kitsune:
        return QStringLiteral("Kitsune Magisk");
    case RootType::KernelSU:
        return QStringLiteral("KernelSU");
    case RootType::KernelSU_Next:
        return QStringLiteral("KernelSU Next");
    case RootType::SukiSU:
        return QStringLiteral("SukiSU");
    case RootType::ReSukiSU:
        return QStringLiteral("ReSukiSU");
    case RootType::APatch:
        return QStringLiteral("APatch");
    case RootType::KernelPatch:
        return QStringLiteral("KernelPatch");
    case RootType::RamdiskSu:
        return QStringLiteral("RamdiskSu");
    case RootType::ModuleInstall:
        return QStringLiteral("ModuleInstall");
    }
    return QStringLiteral("未知类型(%1)").arg(static_cast<int>(t));
}

} // namespace

bool patchFile(const QString &bootPath, const PatchConfig &cfg,
               QString *outPath, QString *error)
{
    if (error)
        error->clear();
    if (outPath)
        outPath->clear();

    if (bootPath.isEmpty())
        return fail(error, QStringLiteral("boot 镜像路径为空"));

    const QFileInfo srcInfo(bootPath);
    if (!srcInfo.exists() || !srcInfo.isFile())
        return fail(error, QStringLiteral("boot 镜像不存在：%1").arg(bootPath));

    // 已存在 .orig.bak 视为上一次修补痕迹：拒绝覆盖备份（原厂镜像不可再生）
    const QString backupPath = srcInfo.absoluteFilePath() + QLatin1String(".orig.bak");
    if (QFile::exists(backupPath))
        return fail(error,
                    QStringLiteral("已存在原镜像备份 %1：请先还原为原厂 boot 后再修补")
                        .arg(backupPath));

    QFile src(bootPath);
    if (!src.open(QIODevice::ReadOnly))
        return fail(error,
                    QStringLiteral("无法读取 boot 镜像 %1：%2").arg(bootPath, src.errorString()));
    const QByteArray boot = src.readAll();
    src.close();
    if (boot.isEmpty())
        return fail(error, QStringLiteral("boot 镜像为空文件：%1").arg(bootPath));

    // 工厂派发：未实现类型 → 明确错误
    std::unique_ptr<RootPatcher> p(RootPatcher::create(cfg.type));
    if (!p)
        return fail(error,
                    QStringLiteral("%1 方案尚未实现，无法修补").arg(rootTypeName(cfg.type)));

    QByteArray patched;
    QString patchErr;
    if (!p->patch(boot, cfg, patched, &patchErr)) {
        // 遗留吸收（C3 concern）：Kitsune 无官方下载源，须手动指定 APK
        if (cfg.type == RootType::Kitsune && cfg.apkPath.isEmpty() &&
            !patchErr.contains(QStringLiteral("手动")))
            patchErr += QStringLiteral("（Kitsune 无官方下载源，请手动指定 APK）");
        return fail(error, patchErr);
    }

    // 防御：patcher 返回 true 但产物为空（异常实现/极端输入）→ 拒绝交付，
    // 不写备份不留产物（与"失败不落产物"契约一致）
    if (patched.isEmpty())
        return fail(error, QStringLiteral("修补产物为空，拒绝交付（patcher 实现异常）"));

    // 成功：先备份原镜像（失败不留任何产物），再写修补产物
    if (!writeFile(backupPath, boot, error))
        return false;

    const QString patchedPath = srcInfo.absoluteDir().filePath(
        srcInfo.completeBaseName() + QLatin1String("_patched.img"));
    if (!writeFile(patchedPath, patched, error)) {
        // 产物写失败：移除刚创建的备份，保持"存在 .orig.bak ⟺ 存在产物"
        QFile::remove(backupPath);
        return false;
    }

    if (outPath)
        *outPath = patchedPath;
    return true;
}

} // namespace patcher
