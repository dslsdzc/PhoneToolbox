#pragma once
#include "root_patcher/root_patcher.h"

#include <QUrl>

namespace patcher {

// Magisk 系 patcher：官方 Magisk / Magisk Alpha / Kitsune Magisk 参数化。
// 三者注入机制完全相同（从 APK 的 lib/<abi>/libmagiskinit.so 提取注入物 →
// ramdisk 中 magiskinit 接管 init），仅下载源不同：
//   官方   topjohnwu/Magisk  —— asset 形如 Magisk-v30.7.apk（联网验证 2026-08）
//   Alpha  vvb2060/Magisk    —— asset 固定 app-release.apk，tag=版本号（如 30700）
//   Kitsune —— 原 HuskyDG/magisk-files 与 KitsuneMagisk/KitsuneMagisk 均已
//              下线（API 404，2026-08 验证），现有源均为个人镜像 → 不内置
//              URL，downloadUrl() 返回空，须手动指定 APK（UI 提示）
//
// 注入机制（联网验证，与 Magisk v23-v30.7 的 boot_patch.sh / cpio.cpp /
// init.cpp 一致，见 task-C3-report.md）：
//   1. parseBootImage 解包 → decompressRamdisk 解压 → newc cpio 解析
//   2. 原 init 条目备份为 .backup/init（运行时 magiskinit 的 backup_init()
//      读取该条目并 rename 回 /init 交给真实 init 接管 —— 官方实现无
//      init.orig 条目，与任务 brief 初稿的表述不同）
//   3. init 条目替换为 magiskinit（S_IFREG|0750）
//   4. 写入 .backup/ 目录（S_IFDIR，官方 mkdir 000）与 .backup/.magisk 配置
//   5. cpio 重序列化 → 按原始压缩格式重压 → 更新 BootInfo.ramdisk →
//      repackBootImage
//
// 完整修补（overlay.d/sbin、fstab verity 剥离、内核 hexpatch 等）超出本
// 实现范围 —— UI 提示"高级修补请用对应 App 完成"。
class MagiskPatcher : public RootPatcher
{
public:
    bool patch(const QByteArray &bootImage, const PatchConfig &cfg,
               QByteArray &out, QString *error) override;

    // 下载源映射（供 UI 层经 AssetsDownloader 使用；key → URL）。
    // 返回 releases 页 URL（asset 命名随版本变化，由调用方解析具体文件）；
    // Kitsune 返回空 URL（官方源已下线，须手动指定 APK）。
    static QUrl downloadUrl(const QString &variant);

    // 版本化缓存 key 前缀（完整 key 由调用方追加版本，如 "magisk-v30.7"）。
    static QString assetKey(const QString &variant);
};

} // namespace patcher
