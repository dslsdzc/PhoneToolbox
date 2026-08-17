#pragma once

// 插件系统接口（计划 F2-P）—— 主项目通用能力
// 协议类插件（如华为刷写）以独立 .so 实现本接口，主程序运行时加载，
// 不编译链接插件代码（法务/许可隔离：删除插件文件即完整移除）。
//
// 注：接口类不声明 Q_OBJECT（Qt 标准插件模式）。若声明 Q_OBJECT，接口的
// 键函数（qt_metacall 等，moc 生成）只在主程序侧具体化，基类 vtable 将
// 成为跨模块强依赖：插件 .so 需从主程序解析 vtable，而 -rdynamic 不导出
// 未被共享库引用的弱符号，导致 QPluginLoader 加载失败。无 Q_OBJECT 时
// 无键函数，vtable 在每个使用它的 TU（含插件 .so 自身）弱发射，插件
// 完全自包含，不依赖主程序导出任何符号。

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#define ProtocolPlugin_iid "com.phonetoolbox.ProtocolPlugin/1.0"

class ProtocolPlugin : public QObject {
public:
    explicit ProtocolPlugin(QObject *parent = nullptr) : QObject(parent) {}
    ~ProtocolPlugin() override = default;

    virtual QString name() const = 0;
    virtual QString description() const = 0;
    // 能力列表（如 "huawei-usb-update.flash"）
    virtual QStringList capabilities() const = 0;
    // 执行能力；成功返回 true；失败返回 false 并填 error（契约：失败不崩溃）
    virtual bool execute(const QString &capability, const QVariantMap &params,
                         QString *error) = 0;
};

Q_DECLARE_INTERFACE(ProtocolPlugin, ProtocolPlugin_iid)
