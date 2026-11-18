#ifndef SETTINGS_PANEL_H
#define SETTINGS_PANEL_H

#include <QMap>
#include <QWidget>

#include "core/device_info.h"

class QListWidget;
class QStackedWidget;
class RootAgentManagerPanel;

// 设置工具页：左侧分类，右侧对应设置面板。
// Root 代理管理只是其中一个子项，后续设置项继续往这里加。
class SettingsPanel : public QWidget
{
    Q_OBJECT

public:
    explicit SettingsPanel(QWidget *parent = nullptr);
    void setDevices(const QMap<QString, DeviceInfo> &devices);

signals:
    void outputMessage(const QString &text, bool isError);
    void rootAgentStateChanged(const QString &serial, bool connected, bool isRoot);

private:
    QListWidget *m_categories = nullptr;
    QStackedWidget *m_stack = nullptr;
    RootAgentManagerPanel *m_rootAgentPanel = nullptr;
};

#endif // SETTINGS_PANEL_H
