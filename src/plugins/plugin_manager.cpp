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
        QPluginLoader loader(d.absoluteFilePath(entry));
        QObject *obj = loader.instance();
        if (!obj) {
            m_errors << QStringLiteral("%1: %2").arg(entry, loader.errorString());
            continue;
        }
        ProtocolPlugin *plugin = qobject_cast<ProtocolPlugin *>(obj);
        if (!plugin) {
            m_errors << QStringLiteral("%1: 不是 ProtocolPlugin 插件").arg(entry);
            loader.unload();
            continue;
        }
        m_plugins.append(plugin);
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
    m_errors.clear();
}
