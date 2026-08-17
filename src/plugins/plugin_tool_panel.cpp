#include "src/plugins/plugin_tool_panel.h"

#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "src/plugins/plugin_interface.h"

PluginToolPanel::PluginToolPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    m_list = new QListWidget(this);
    m_executeBtn = new QPushButton(QStringLiteral("执行"), this);
    m_executeBtn->setEnabled(false);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_executeBtn);
    connect(m_executeBtn, &QPushButton::clicked, this, &PluginToolPanel::onExecuteClicked);
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this] {
        m_executeBtn->setEnabled(m_list->currentRow() >= 0);
    });
}

void PluginToolPanel::setPlugins(const QList<ProtocolPlugin *> &plugins)
{
    m_plugins = plugins;
    m_list->clear();
    for (ProtocolPlugin *p : m_plugins) {
        for (const QString &cap : p->capabilities())
            m_list->addItem(QStringLiteral("%1 — %2").arg(p->name(), cap));
    }
}

void PluginToolPanel::onExecuteClicked()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_plugins.size())
        return;
    ProtocolPlugin *p = m_plugins.at(row);
    if (!p)
        return;
    QString error;
    if (!p->execute(p->capabilities().value(0), QVariantMap(), &error))
        emit outputMessage(QStringLiteral("[插件] %1 执行失败: %2").arg(p->name(), error), true);
    else
        emit outputMessage(QStringLiteral("[插件] %1 执行完成").arg(p->name()), false);
}
