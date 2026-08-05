#pragma once
#include "root_patcher/root_patcher.h"

namespace patcher {

// SuperSU 系 patcher（Task C7）：老设备 ramdisk 级 su 注入。
//
// 机制（联网验证 2026-08-05，见 task-C7-report.md）：
//   1. 用户提供 SuperSU recovery 刷入包（ZIP：update-binary + 各 ABI 的
//      su 二进制 + Superuser.apk），从中提取 su 二进制 —— su 与 daemonsu
//      为同一文件（update-binary cp_perm 同一 $BIN/su 到 /system/xbin/su、
//      /system/bin/.ext/.su、/system/xbin/daemonsu，SuperSU v2.46-2.82
//      产物逐一核实）
//   2. boot 解包 → ramdisk 解压 → cpio 加入 sbin/su（0755，早期引导可用
//      副本）与 init.superuser.rc（0750，daemonsu 服务模板）
//   3. init.rc 末尾追加 "import /init.superuser.rc"（若无该行；dkp 内核
//      包 rd/ 目录同款做法，Android init 加载期导入合并）
//
// 诚实边界：本修补只提供 boot 侧服务接线 —— daemonsu 服务指向
// /system/xbin/daemonsu（老 ROM 布局约定），设备 /system 需存在该二进制
// （先前经 SuperSU 刷入或 ROM 自带）服务才能启动；SuperSU 2.80+ 闭源停更，
// 新版系统（SAR/system-as-root、无 /init.rc 的 ramdisk 布局）不适用。
// 提取受限或路径不符时返回明确错误并提示改用 Magisk，不假装完整支持。
// 不做 LineageOS 注入（其 su 为 ROM 内置，开发者选项启用，无需修补 boot）。
class RamdiskSuPatcher : public RootPatcher
{
public:
    bool patch(const QByteArray &bootImage, const PatchConfig &cfg,
               QByteArray &out, QString *error) override;
};

} // namespace patcher
