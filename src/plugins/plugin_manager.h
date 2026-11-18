#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include "src/plugins/plugin_interface.h"

// 单个插件文件的扫描结果。**失败的也要保留**：插件面板要让用户看到
// "这个插件加载失败了、原因是什么"，而不是只在启动日志里闪一下。
struct PluginRecord {
    QString filePath;
    QString name;          // 加载成功时取插件自报名字；失败时退化为文件名
    QString description;
    QStringList capabilities;
    bool loaded = false;
    QString error;         // loaded=false 时的失败原因
    ProtocolPlugin *plugin = nullptr; // 仅 loaded=true 时有效
};

// 插件管理器：扫描目录加载 ProtocolPlugin 插件（QPluginLoader）。
// 加载失败/接口不符的插件记录错误并跳过（不崩溃）。
class PluginManager : public QObject {
    Q_OBJECT
public:
    static PluginManager &instance();

    // 扫描 dir 下全部 .so/.dll（仅 ProtocolPlugin 接口）
    void scanPlugins(const QString &dir);
    QList<ProtocolPlugin *> plugins() const { return m_plugins; }
    // 全量扫描结果（含加载失败的文件），供插件面板展示状态。
    QList<PluginRecord> records() const { return m_records; }
    QList<ProtocolPlugin *> byCapability(const QString &capability) const;
    QStringList loadErrors() const { return m_errors; }
    void clear(); // 卸载全部（析构/重扫用）

private:
    explicit PluginManager(QObject *parent = nullptr);
    QList<ProtocolPlugin *> m_plugins;
    QList<PluginRecord> m_records;
    QStringList m_errors;
};
