#include "src/plugins/plugin_manager.h"

#include <QDir>
#include <QPluginLoader>

PluginManager &PluginManager::instance()
{
    static PluginManager mgr;
    return mgr;
}

PluginManager::PluginManager(QObject *parent)
    : QObject(parent)
{
}

void PluginManager::scanPlugins(const QString &dir)
{
    clear();
    QDir d(dir);
    const QStringList entries = d.entryList(QDir::Files);
    for (const QString &entry : entries) {
        if (!entry.endsWith(QStringLiteral(".so")) && !entry.endsWith(QStringLiteral(".dll"))
            && !entry.endsWith(QStringLiteral(".dylib")))
            continue;

        PluginRecord record;
        record.filePath = d.absoluteFilePath(entry);
        record.name = entry;

        QPluginLoader loader(record.filePath);
        QObject *obj = loader.instance();
        if (!obj) {
            record.error = loader.errorString();
            m_errors << QStringLiteral("%1: %2").arg(entry, record.error);
            m_records.append(record);
            continue;
        }
        ProtocolPlugin *plugin = qobject_cast<ProtocolPlugin *>(obj);
        if (!plugin) {
            record.error = QStringLiteral("不是 ProtocolPlugin 插件");
            m_errors << QStringLiteral("%1: %2").arg(entry, record.error);
            loader.unload();
            m_records.append(record);
            continue;
        }

        record.loaded = true;
        record.plugin = plugin;
        record.name = plugin->name();
        record.description = plugin->description();
        record.capabilities = plugin->capabilities();
        m_plugins.append(plugin);
        m_records.append(record);
    }
}

QList<ProtocolPlugin *> PluginManager::byCapability(const QString &capability) const
{
    QList<ProtocolPlugin *> out;
    for (ProtocolPlugin *p : m_plugins) {
        if (p->capabilities().contains(capability))
            out.append(p);
    }
    return out;
}

void PluginManager::clear()
{
    // QPluginLoader 实例生命周期：直接删除 QObject（插件库保持加载）
    for (ProtocolPlugin *p : m_plugins)
        delete p;
    m_plugins.clear();
    m_records.clear();
    m_errors.clear();
}
