#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include <QMainWindow>
#include <QSplitter>
#include <QStackedWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include "core/device_detector.h"
#include "ui/tool_panel.h"
#include "ui/device_info_panel.h"
#include "ui/flash_panel.h"
#include "ui/system_tool_panel.h"
#include "ui/vuln_panel.h"
#include "ui/image_tool_panel.h"
#include "plugins/plugin_tool_panel.h"
#include "ui/output_panel.h"

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void onDeviceConnected(const DeviceInfo &info);
    void onDeviceDisconnected(const QString &serial);
    void onDeviceModeChanged(const QString &serial, DeviceDetector::DeviceMode newMode);
    void onDeviceSelectionChanged(const QString &deviceId);
    void onOutputMessage(const QString &message, bool isError = false);
    void onRefreshRequested();
    void onToolSelected(int index);

private:
    void setupUI();
    void setupConnections();
    void updateCurrentDeviceInfo();

    QSplitter *m_mainSplitter;
    QSplitter *m_rightSplitter;
    QStackedWidget *m_stack;

    ToolPanel *m_toolPanel;
    DeviceInfoPanel *m_deviceInfoPanel;
    FlashPanel *m_flashPanel;
    SystemToolPanel *m_systemToolPanel;
    VulnPanel *m_vulnPanel;
    ImageToolPanel *m_imageToolPanel;
    PluginToolPanel *m_pluginPanel;
    OutputPanel *m_outputPanel;

    DeviceDetector m_deviceDetector;
    QMap<QString, DeviceInfo> m_currentDevices;
};

#endif // MAIN_WINDOW_H
