#ifndef SYSTEM_TOOL_PANEL_H
#define SYSTEM_TOOL_PANEL_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QStackedWidget>
#include <QProcess>
#include <QLineEdit>
#include <QCheckBox>
#include "core/device_detector.h"
#include "core/device_info.h"

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
    static QIcon generateAppIcon(QListWidgetItem *item);

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
    QLabel *m_cpuStatus, *m_gpuStatus, *m_thermalStatus, *m_memoryStatus, *m_ioStatus;
    // 2. 界面定制
    QLabel *m_dpiStatus, *m_animStatus;
    // 4. 分区管理
    QLabel *m_blockStatus;
    // 5. 应用管理
    QListWidget *m_appList;
    QLineEdit *m_searchBox = nullptr;
    QCheckBox *m_showIconsCheck = nullptr;
    QLabel *m_appStatus;
    int m_appFilter = 0; // 0=all, 1=third, 2=system
    // 6. 安全隐私
    QLabel *m_selinuxLabel, *m_buildPropStatus;
    // 7. 开发调试
    QLabel *m_shizukuStatus;

    DeviceInfo m_deviceInfo;
    QProcess *m_asyncProc = nullptr;
};

#endif // SYSTEM_TOOL_PANEL_H
