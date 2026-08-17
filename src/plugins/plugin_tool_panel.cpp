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
        for (const QString &cap : p->capabilities()) {
            auto *item = new QListWidgetItem(QStringLiteral("%1 — %2").arg(p->name(), cap), m_list);
            item->setData(Qt::UserRole, cap);
            item->setData(Qt::UserRole + 1, QVariant::fromValue(static_cast<void *>(p)));
        }
    }
}

void PluginToolPanel::onExecuteClicked()
{
    QListWidgetItem *item = m_list->currentItem();
    if (!item)
        return;
    const QString cap = item->data(Qt::UserRole).toString();
    auto *p = static_cast<ProtocolPlugin *>(item->data(Qt::UserRole + 1).value<void *>());
    if (!p || cap.isEmpty())
        return;
    QString error;
    if (!p->execute(cap, QVariantMap(), &error))
        emit outputMessage(QStringLiteral("[插件] %1 执行失败: %2").arg(p->name(), error), true);
    else
        emit outputMessage(QStringLiteral("[插件] %1 执行完成").arg(p->name()), false);
}
