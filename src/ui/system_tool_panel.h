#ifndef SYSTEM_TOOL_PANEL_H
#define SYSTEM_TOOL_PANEL_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QStackedWidget>
#include <QProcess>
#include <QLineEdit>
#include <QTimer>
#include <QGridLayout>
#include <QVector>
#include "core/device_detector.h"
#include "core/device_info.h"
#include "ui/live_chart_widget.h"

class SystemToolPanel : public QWidget
{
    Q_OBJECT

public:
    explicit SystemToolPanel(QWidget *parent = nullptr);
    void setDeviceInfo(const DeviceInfo &info);
    void clearDeviceInfo();

signals:
    void outputMessage(const QString &msg, bool isError);
    void switchToDeviceInfo();

private slots:
    void onCategoryChanged(int index);

    // 1. 性能调优
    void onCpuGovernor();       void onGpuGovernor();
    void onThermalControl();    void onMemoryOptimize();
    void onIOScheduler();
    void onPerformancePoll();
    void onPerfTimerToggle();
    void onSaveChart();

    // 2. 界面定制
    void onSetDpi();            void onSetFont();
    void onBootAnimation();     void onStatusBar();
    void onNavBar();            void onAnimationScale();

    // 3. 功能增强 (引导)

    // 4. 分区管理
    void onListBlockDevices();  void onSuperInfo();
    void onMountSystem();       void onUnmountSystem();

    // 5. 应用管理
    void onAppRefreshList();    void onAppLaunch();
    void onAppFreezeToggle();   void onAppUninstall();
    void onAppExtract();

    // 6. 安全隐私
    void onEditBuildProp();     void onSpoofDevice();
    void onSelinuxMode();       void onSafetynetBypass();
    void onSignatureSpoof();

    // 7. 开发调试
    void onLogcat();            void onDumpsys();
    void onShizukuStatus();     void onTakeBugreport();

private:
    void setupUI();
    QWidget *createPage(int category);
    QString executeAdb(const QStringList &args, int timeoutMs = 15000);
    QString adbShell(const QString &cmd, int timeoutMs = 15000);
    void runAdbAsync(const QStringList &args);
    void appendOutput(const QString &msg, bool isError = false);
    void logAndRefresh(const QString &label, const QString &result);
    void startMonitorProcess();
    void stopMonitorProcess();
    QString monitorExec(const QString &cmd, int timeoutMs = 5000);

    // Top
    QLabel *m_deviceLabel;
    QLabel *m_rootLabel;
    QPushButton *m_backBtn;

    // Category navigation
    QListWidget *m_categoryList;
    QStackedWidget *m_categoryStack;

    // Category page widgets
    QList<QWidget*> m_pages;

    // 1. 性能调优
    QTimer *m_perfTimer = nullptr;
    int m_cpuCoreCount = 0;
    bool m_polling = false;
    QWidget *m_perfPage = nullptr;
    QGridLayout *m_cpuGrid = nullptr;
    QVector<LiveChartWidget*> m_cpuCharts;
    LiveChartWidget *m_gpuChart = nullptr;
    LiveChartWidget *m_tempChart = nullptr;
    LiveChartWidget *m_memChart = nullptr;
    QLabel *m_perfStatus = nullptr;
    QPushButton *m_perfToggleBtn = nullptr;
    QPushButton *m_saveChartBtn = nullptr;
    // Full history buffers for image export
    QVector<QVector<double>> m_cpuFullHistory;
    QVector<double> m_gpuFullHistory, m_tempFullHistory, m_memFullHistory, m_swapFullHistory;
    int m_tempZoneIndex = 0;
    QString m_gpuFreqPath;
    // 2. 界面定制
    QLabel *m_dpiStatus, *m_animStatus;
    // 4. 分区管理
    QLabel *m_blockStatus;
    // 5. 应用管理
    QListWidget *m_appList;
    QLineEdit *m_searchBox = nullptr;
    QLabel *m_appStatus;
    int m_appFilter = 0; // 0=all, 1=third, 2=system
    // 6. 安全隐私
    QLabel *m_selinuxLabel, *m_buildPropStatus;
    // 7. 开发调试
    QLabel *m_shizukuStatus;

    DeviceInfo m_deviceInfo;
    QProcess *m_asyncProc = nullptr;
    QProcess *m_monitorProc = nullptr;
};

#endif // SYSTEM_TOOL_PANEL_H
