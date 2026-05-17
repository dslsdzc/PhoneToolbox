#ifndef TOOL_PANEL_H
#define TOOL_PANEL_H

#include <QWidget>
#include <QListWidget>
#include <QPushButton>
#include <QComboBox>
#include "core/device_detector.h"
#include "core/restart_tool.h"

class ToolPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ToolPanel(QWidget *parent = nullptr);
    
    void updateDeviceList(const QMap<QString, DeviceInfo> &devices);
    QString getSelectedDevice() const;
    void selectToolByIndex(int index);

signals:
    void deviceSelectionChanged(const QString &deviceId);
    void outputMessage(const QString &message, bool isError = false);
    void refreshRequested();
    void toolSelected(int index);

private slots:
    void onDeviceListSelectionChanged();
    void onRestartButtonClicked();
    void onRefreshButtonClicked();
    void onRestartToolOutput(const QString &message, bool isError);
    void onToolSelectionChanged();

private:
    void setupUI();
    void setupConnections();

    QListWidget *m_deviceList;
    QComboBox *m_restartModeCombo;
    QPushButton *m_restartButton;
    QPushButton *m_refreshButton;
    QListWidget *m_toolSelector;

    RestartTool *m_restartTool;
    QMap<QString, DeviceInfo> m_currentDevices;
    QString m_currentSelectedDevice;
};

#endif // TOOL_PANEL_H
