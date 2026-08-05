#pragma once
#include "root_patcher/root_patcher.h"

#include <QString>
#include <QUrl>

namespace patcher {

// APatch/KernelPatch 系 patcher（PC 侧修补编排，官方 kptools 子进程）。
//
// 注入机制（联网验证 2026-08-05，一手来源：bmax121/KernelPatch tools/patch.c
// 源码、KernelPatch 0.13.3 预编译 kptools-linux 对真实 arm64 内核的实测、
// bmax121/APatch boot_patch.sh + PatchesViewModel.kt、APatch_11219 官方 APK
// 实物，见 task-C5-report.md）：
//   1. kpimg（KernelPatch 内核镜像，头 64B setup_header 含 KP_MAGIC "KP1158"）
//      追加到解压后内核镜像的 4K 对齐尾部（align_ceil(kimg,4K)）；内核启动
//      分支指令（ARM64 Image+0x4 的 b stext）改写为跳转 kpimg 入口
//      （align_kimg_len+4K，实测补丁后分支目标 == 该地址）；preset 填充
//      kallsyms 解析出的符号偏移、header_backup（原内核头备份，供 unpatch）、
//      superkey。**不是**"kpatch 写入内核数据段 + 修改头部 magic"。
//   2. kpatch（KernelPatch 用户空间工具）仅可经 kptools -K 可选嵌入；APatch
//      官方 boot_patch.sh 不嵌入（-p 无 -K）→ 本实现同样不注入 kpatch 可执行
//      文件，仅注入 kpimg。
//   3. kallsyms 恢复 + ARM64 指令改写 + IKCONFIG 解析为完整二进制分析引擎
//      （kptools 上千行），本任务范围外 → C5 不原生重实现，采用官方 kptools
//      子进程编排（与 mtk_handler→mtk_binary 桥接同模式）。kptools 官方
//      声明可在任意平台编译，且 KernelPatch release 提供预编译 kptools-linux/
//      kptools-mac/kptools-msys2-win.7z + kpimg-android —— PC 侧修补路径
//      官方存在（本机实测可跑通全流程）。
//
// 注入物来源（诚实边界，与官方 APatch 修补流程逐命令对齐）：
//   a) cfg.apkPath —— APatch 管理器 APK（官方 release 单资产，联网验证
//      APatch_11219 仅 arm64-v8a ABI）。按官方 App prepare() 同款提取：
//      lib/<abi>/libkptools.so → kptools（回退链仅含宿主可执行 ABI——
//      arm64 Linux 宿主为 arm64-v8a，不可执行 ABI 自动跳过）+
//      assets/kpimg → kpimg。
//      **宿主架构门禁**：APK 内 libkptools.so 为 Android arm64 ELF，仅
//      arm64 Linux 宿主可执行（QProcess exec）；x86_64/macOS/Windows 宿主
//      提前拒绝并提示改用 kpatchPath 手动指定 kptools-linux。
//   b) cfg.kpatchPath —— 用户手动提供的**目录**，须含 kptools* 与 kpimg*
//      文件（KernelPatch release 预编译资产解包形态：kptools-linux/
//      kptools-mac/kpimg-android 等，文件名前缀匹配）。多平台候选共存时按
//      Q_OS_* 匹配本宿主平台（kptools-linux/mac/win），.7z 压缩包跳过。
//      kptools 无执行位时 patcher 负责 chmod +x。
//   a/b 互斥（同时指定 → 明确报错，防 kptools/kpimg 跨版本混配）。
//
// 流程（与官方 boot_patch.sh 逐命令对齐；superkey 用官方默认 —— 不传
// -s/-S 即 root-skey 零哈希模式，等价于脚本 SUPERKEY=su 的默认分支；
// PatchConfig 无 superkey 字段，自定义 key 需官方 App，见报告）：
//   kptools unpack boot.img
//     → CONFIG_KALLSYMS 门禁：kptools -i kernel -f | grep CONFIG_KALLSYMS=y
//       （官方同款：无 IKCONFIG/CONFIG_KALLSYMS 的内核直接拒绝）
//     → kptools -p -i kernel.ori -k kpimg -o kernel（先 mv kernel kernel.ori）
//     → 验证：kptools -l -i kernel 输出含 patched=true（kpimg preset 存在，
//       拒绝交付未成功修补的产物）
//     → kptools repack boot.img → new-boot.img → 输出
// 失败路径一律返回 false 并写 error（全局契约），不产生部分产物。
class APatchPatcher : public RootPatcher
{
public:
    bool patch(const QByteArray &bootImage, const PatchConfig &cfg,
               QByteArray &out, QString *error) override;

    // APatch 管理器 APK 下载入口（releases/latest 页，asset 名随版本变化
    // 如 APatch_11219_0355948_HEAD-release-signed.apk，由 UI 层经 GitHub
    // API 解析具体 asset 后调用 AssetsDownloader；C2 验证 release 仅此单资产）。
    static QUrl apkDownloadUrl();

    // 版本化缓存 key 基名（完整 key 由调用方追加版本，如 "apatch-11219"）。
    static QString assetKey();
};

} // namespace patcher
