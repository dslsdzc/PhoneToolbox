#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include "src/plugins/plugin_interface.h"

// 插件管理器：扫描目录加载 ProtocolPlugin 插件（QPluginLoader）。
// 加载失败/接口不符的插件记录错误并跳过（不崩溃）。
class PluginManager : public QObject {
    Q_OBJECT
public:
    static PluginManager &instance();

    // 扫描 dir 下全部 .so/.dll（仅 ProtocolPlugin 接口）
    void scanPlugins(const QString &dir);
    QList<ProtocolPlugin *> plugins() const { return m_plugins; }
    QList<ProtocolPlugin *> byCapability(const QString &capability) const;
    QStringList loadErrors() const { return m_errors; }
    void clear(); // 卸载全部（析构/重扫用）

private:
    explicit PluginManager(QObject *parent = nullptr);
    QList<ProtocolPlugin *> m_plugins;
    QStringList m_errors;
};
