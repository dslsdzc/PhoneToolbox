#pragma once
#include "root_patcher/root_patcher.h"

#include <QMap>
#include <QString>
#include <QStringList>

namespace patcher {

// 模块框架安装（Task C8）：Zygisk Next / Riru / LSPosed 等模块 zip → 可安装
// 的模块树（module.prop + 文件树）。
//
// 机制（联网验证 2026-08-05，见 task-C8-report.md —— 与 brief 初稿的关键差异
// 已按权威信息纠正）：
//   Magisk 官方 docs/guides.md：模块是位于 /data/adb/modules/<id>/ 的目录
//   （data 分区），安装 zip 为"module 文件的 zip"（module.prop + 文件树 +
//   可选 META-INF/com/google/android/{update-binary,updater-script} 安装脚手架）；
//   运行时由设备上的 magiskd/ksud 在启动阶段读取并系统级挂载。**boot ramdisk
//   不存在模块注入机制**（ramdisk 每启重建、/data 挂载覆盖其 data/ 目录）——
//   brief 初稿"写入 ramdisk 的 /data/adb/modules/ 预留结构"为错误机制，故本
//   实现不向 ramdisk 注入模块，而是产出可直接安装的模块包/树。
//
// 本实现职责（诚实边界）：
//   1. 校验模块 zip：ZIP 归档、根级 module.prop（Magisk v28+ 静态读取位置）、
//      官方严格格式（id 正则 ^[a-zA-Z][a-zA-Z0-9._-]+$、versionCode 整数、
//      LF 行尾）、禁 install.sh（官方规范明令）、updater-script 存在时须为
//      "#MAGISK"（recovery 刷入标识）
//   2. 解包文件树：剔除 META-INF/ 安装脚手架（module_installer.sh "Remove
//      stuffs that don't belong to modules" 同款）
//   3. 重打包"干净模块 zip"（规范化 module.prop + 文件树）作为 out；模块树与
//      元数据经 meta()/tree() 访问器暴露，供 UI 直推 /data/adb/modules/<id>/
//      （官方手动安装流程：adb push 文件树 → 重启生效；KernelSU 亦可用
//      `ksud module install <zip>`）
//   4. bootImage 输入为可选框架门禁：非空时须为已 Magisk 修补（.backup 链）
//      或已 KernelSU 修补（init.real + kernelsu.ko）的镜像，否则拒绝（模块
//      运行时依赖 magiskd/ksud）
//
// 运行时激活（模块挂载/zygisk 加载）由对应 root 方案处理 —— UI 须标注
// "模块激活需对应 App（Magisk/KernelSU/APatch）"。非 Magisk 模块 zip
// （普通 recovery 刷入包）与 zip64 归档明确拒绝。
class ModuleInstaller : public RootPatcher
{
public:
    // 解析后的模块元数据（官方 module.prop 严格格式字段）
    struct ModuleMeta {
        QString id;           // 唯一标识，决定安装目录 /data/adb/modules/<id>/
        QString name;
        QString version;
        int versionCode = 0;
        QString author;
        QString description;
        QString updateJson;   // 可选：远程 update.json 检查源
    };

    bool patch(const QByteArray &bootImage, const PatchConfig &cfg,
               QByteArray &out, QString *error) override;

    // patch() 成功后的元数据与模块树（路径 → 内容；不含 module.prop 与
    // META-INF/）。调用方在 patch 返回 true 后使用，未 patch/失败时为空。
    const ModuleMeta &meta() const { return m_meta; }
    const QMap<QString, QByteArray> &tree() const { return m_tree; }

private:
    bool parseModuleProp(const QByteArray &raw, QString *error);
    QByteArray buildModuleZip() const;

    ModuleMeta m_meta;
    QMap<QString, QByteArray> m_tree;
    // 规范允许的未知单行字段（按出现顺序保留并重写）
    QList<QPair<QString, QByteArray>> m_extraProps;
};

} // namespace patcher
