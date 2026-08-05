#pragma once
#include <QByteArray>
#include <QString>

namespace patcher {

// Root 方案类型。C3-C8 逐步实现：create() 对未实现类型返回 nullptr。
enum class RootType {
    Magisk,        // 官方 Magisk（topjohnwu/Magisk）
    MagiskAlpha,   // Magisk Alpha（vvb2060/Magisk 分支）
    Kitsune,       // Kitsune Magisk（原 Magisk Delta / HuskyDG 分支）
    KernelSU,      // KernelSU（GKI 内核模块注入）
    KernelSU_Next, // KernelSU Next 分支
    SukiSU,        // SukiSU（KernelSU 衍生）
    ReSukiSU,      // ReSukiSU（SukiSU 衍生）
    APatch,        // APatch（kernel patch 方案）
    KernelPatch,   // KernelPatch（KernelPatch 方案）
    RamdiskSu,     // Ramdisk 级 su 注入（SuperSU 系）
    ModuleInstall  // 模块框架安装
};

// 注入配置。各 patcher 按 type 取用相关字段，其余忽略：
//   Magisk 系：apkPath（注入 APK 本地路径；运行时下载经 AssetsDownloader
//              完成后由调用方填入，手动指定优先）
//   KernelSU 系：apkPath / koPath / deviceKmi
//   APatch/KernelPatch：apkPath（管理器 APK，提取 kptools+kpimg）或
//                      kpatchPath（手动目录，含 kptools*/kpimg* 文件）
//   RamdiskSu：suZipPath
//   ModuleInstall：moduleZipPath（标准 Magisk 模块 zip；bootImage 输入经
//                 ModuleInstaller 仅作框架门禁，见 module_installer.h）
struct PatchConfig {
    RootType type = RootType::Magisk;
    QString apkPath;    // 注入 APK 本地路径（Magisk/KernelSU/APatch 系）
    QString koPath;     // 内核模块 .ko 路径（KernelSU 系）
    QString kpatchPath; // APatch/KernelPatch：含 kptools*/kpimg* 文件的目录
                        //（KernelPatch release 预编译资产解包形态；与 apkPath
                        // 互斥，kptools/kpimg 由 APatchPatcher 自该目录取用）
    QString suZipPath;  // su 包 zip 路径（RamdiskSu）
    QString moduleZipPath; // 模块 zip 路径（ModuleInstall，标准 Magisk 模块格式）
    QString deviceKmi;  // 设备 KMI 版本（KernelSU 系选择内核）
    QString variant;    // 变体细分（Magisk 系 "official"/"alpha"/"kitsune"）
};

// 抽象根修补器接口。
// 全局契约：失败返回 false 并写 error（非空），绝不崩溃；patched 输出为
// repackBootImage 结果；自动备份由调用方（UI 层）处理。
class RootPatcher
{
public:
    virtual ~RootPatcher() = default;

    // 修补 boot 镜像（输入原样，输出新镜像）。bootImage 非 boot 镜像、
    // 注入物缺失或任一环节失败均返回 false 并写 error。
    virtual bool patch(const QByteArray &bootImage, const PatchConfig &cfg,
                       QByteArray &out, QString *error) = 0;

    // 工厂：按类型创建 patcher；未实现类型返回 nullptr（调用方须判空）。
    static RootPatcher *create(RootType type);
};

// 文件级修补入口（C6，供 UI 层调用）：读入 bootPath 的 boot 镜像字节 →
// create() 工厂按 cfg.type 派发到对应 patcher → 成功后把原镜像原样备份到
// "<源文件>.orig.bak"，并写 "<基名>_patched.img"（基名 = 去掉扩展名的文件名）。
// 失败返回 false 并写 error，且不落任何产物；已存在 .orig.bak 时拒绝修补
// （防止覆盖不可再生的原厂备份）。error / outPath 可传 nullptr（绝不崩溃）。
bool patchFile(const QString &bootPath, const PatchConfig &cfg,
               QString *outPath, QString *error);

} // namespace patcher
