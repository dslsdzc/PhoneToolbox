#pragma once
#include "root_patcher/root_patcher.h"
#include "image_engine/boot_image.h"

#include <QStringList>
#include <QUrl>

namespace patcher {

// KernelSU 系 patcher：官方 tiann/KernelSU / KernelSU-Next / SukiSU-Ultra /
// ReSukiSU 参数化（variant 不同 → 注入物下载源与 KMI 覆盖不同，注入机制相同）。
//
// 注入机制（联网验证 2026-08-05，对照 tiann/KernelSU、KernelSU-Next、
// SukiSU-Ultra、ReSukiSU 四个仓库 userspace/ksud/src/boot_patch.rs 源码与
// release 产物，见 task-C4-report.md）：
//   1. parseBootImage 解包 → detectRamdiskFormat + decompressRamdisk 解压
//      → newc cpio 解析
//   2. Magisk 修补检测（cpio 含 .backup 条目）→ 拒绝（ksud 同：
//      "Cannot work with Magisk patched image"）
//   3. 未修补（ramdisk 无 kernelsu.ko）且存在 init 条目 → init 改名
//      init.real（ksud: cpio.mv("init", "init.real")，模式/内容原样保留）
//   4. 写入 init = ksuinit wrapper（S_IFREG|0755，ksud add 0755 init）
//   5. 写入 kernelsu.ko（S_IFREG|0755）
//   6. cpio 重序列化 → 按原始压缩格式重压 → 更新 BootInfo.ramdisk →
//      repackBootImage
//   已修补镜像（ramdisk 含 kernelsu.ko）再次注入与 ksud 一致为幂等覆盖：
//   跳过 init→init.real 改名（保留原 init.real），仅重写 init 与 kernelsu.ko。
//   init.rc 不做修改：KERNEL_SU_RC 的 init 脚本注入由内核侧 kprobe 在
//   运行时完成（vfs_read hook 拦截 init 对 /system/etc/init/atrace.rc 的
//   读取并前置追加），ramdisk 层只做 init 链替换 —— 这是 ksud 实际形态。
//   ksu_config/force_debuggable/adb_debug.prop 为 ksud 可选参数，默认不写。
//
// 入口参数化（cfg.variant："official"/"next"/"suki"/"resuki"；按 RootType
// 默认映射）：
//   cfg.koPath  —— LKM 注入物：*.ko 文件，或 *.zip（PK 魔数判定）：
//                  含 "*_kernelsu.ko" 条目 → LKM 包（ReSukiSU lkm-all.zip
//                  形态，按 KMI 提取对应条目）；含 "Image*" → AnyKernel3
//                  预置内核包（非 GKI 路径，见下）
//   cfg.apkPath  —— ksuinit init wrapper 二进制路径（可含 .zip：
//                   ReSukiSU ksuinit.zip 形态，按条目名 ksuinit 提取）
//   cfg.deviceKmi —— 期望 KMI（供 UI 匹配下载）；空则运行时 detectKmi
//
// 非 GKI 路径（AnyKernel3）：KMI 匹配失败（kernel 二进制无 KMI 串、cmdline
// 无 androidboot.kmi 且未提供 koPath）时 —— 若 koPath 为 AnyKernel3 包则
// 解包提取内核文件（Image.gz.dtb > Image.gz > Image.lz4 > Image.lzma >
// Image.xz > Image.bz2 > Image，AnyKernel3 约定置于包根目录）→ 替换 boot
// kernel 段 → 重打包（ReSukiSU 文档确认的 magiskboot 手动修补机制）。
// 诚实边界：预置内核可用性依赖社区仓库覆盖；无匹配时返回明确错误并建议
// APatch（内核 3.18+ 仅需 boot.img）/Magisk，不假装支持。
class KernelSuPatcher : public RootPatcher
{
public:
    bool patch(const QByteArray &bootImage, const PatchConfig &cfg,
               QByteArray &out, QString *error) override;

    // 从 boot 镜像提取 KMI（如 "android13-5.15"）：
    //   1) kernel 二进制内扫描 "x.y...androidN"（ksud parse_kmi 同款算法，
    //      权威路径 —— GKI 内核版本串如 "5.15.137-android13-8-00001-..."，
    //      KMI = android{ver}-{major.minor}）
    //   2) boot cmdline "androidboot.kmi=" 兜底（部分非标准内核自带）
    // 均未发现 → 返回空串（非 GKI 判定依据）。
    static QString detectKmi(const imgboot::BootInfo &info);

    // 变体支持的 KMI 列表。联网验证 2026-08：四个渠道 release 资产同为
    // 7 个 {kmi}_kernelsu.ko（android12-5.10 / android13-5.10 /
    // android13-5.15 / android14-5.15 / android14-6.1 / android15-6.6 /
    // android16-6.12）；ReSukiSU 为 cctv18/ReSukiSU_CI 的 lkm-all.zip 内含
    // 同 7 个 ko。未知 variant 返回空列表。
    static QStringList supportedKmis(const QString &variant);

    // kernelsu.ko 下载 URL（供 UI 经 AssetsDownloader 使用）。
    // official/next/suki：{kmi}_kernelsu.ko 直接资产（GitHub
    // releases/latest/download 已验证可解析）；resuki：lkm-all.zip
    // （内含 7 个 ko，解压后填入 koPath）。未知 variant → 空 URL。
    static QUrl koUrl(const QString &variant, const QString &kmi);

    // ksuinit init wrapper 下载 URL。official：ksuinit 直接资产（静态链接
    // aarch64 ELF，联网验证产物）；resuki：ksuinit.zip（解压后填入
    // apkPath）；next/suki：release 不发布独立 ksuinit 资产（仅内嵌于
    // ksud 二进制，API 404 验证）→ 回退官方 KernelSU ksuinit URL ——
    // wrapper 为 KernelSU 通用 init 链，fork 差异均在 .ko 与管理器。
    static QUrl ksuinitUrl(const QString &variant);

    // 版本化缓存 key 前缀（完整 key 由调用方追加版本，如 "ksu-official-3.2.5"）。
    static QString assetKey(const QString &variant);
};

} // namespace patcher
