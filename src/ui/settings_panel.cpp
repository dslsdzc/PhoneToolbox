#include "settings_panel.h"

#include "root_agent_manager_panel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QVBoxLayout>

SettingsPanel::SettingsPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(5, 5, 5, 5);

    auto *title = new QLabel(QStringLiteral("设置"), this);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: bold;"));
    layout->addWidget(title);

    auto *content = new QHBoxLayout();
    m_categories = new QListWidget(this);
    m_categories->setMaximumWidth(180);
    m_categories->setMinimumWidth(140);
    m_categories->addItem(QStringLiteral("Root 代理管理"));
    content->addWidget(m_categories, 0);

    m_stack = new QStackedWidget(this);
    m_rootAgentPanel = new RootAgentManagerPanel(m_stack);
    m_stack->addWidget(m_rootAgentPanel);
    content->addWidget(m_stack, 1);

    layout->addLayout(content, 1);

    connect(m_categories, &QListWidget::currentRowChanged, m_stack,
            &QStackedWidget::setCurrentIndex);
    connect(m_rootAgentPanel, &RootAgentManagerPanel::outputMessage, this,
            &SettingsPanel::outputMessage);
    connect(m_rootAgentPanel, &RootAgentManagerPanel::rootAgentStateChanged,
            this, &SettingsPanel::rootAgentStateChanged);
    m_categories->setCurrentRow(0);
}

void SettingsPanel::setDevices(const QMap<QString, DeviceInfo> &devices)
{
    m_rootAgentPanel->setDevices(devices);
}
