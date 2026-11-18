#ifndef ROOT_AGENT_MANAGER_PANEL_H
#define ROOT_AGENT_MANAGER_PANEL_H

#include <QMap>
#include <QWidget>

#include "core/device_info.h"

class RootAgentClient;
class RootAgentInstaller;
class QCheckBox;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTimer;

// 设置 → Root 代理管理：多设备表格 + 每台设备独立配置/会话。
// 配对后自动获取端口并连接；Root 状态统一显示在设备信息页。
class RootAgentManagerPanel : public QWidget
{
    Q_OBJECT

public:
    explicit RootAgentManagerPanel(QWidget *parent = nullptr);
    ~RootAgentManagerPanel() override;
    void setDevices(const QMap<QString, DeviceInfo> &devices);

signals:
    void outputMessage(const QString &text, bool isError);
    // Root 代理通道状态：连接成功/断开，以及实际 uid=0 检测结果。
    // 界面不单独显示 Root 列，状态统一由设备信息页展示。
    void rootAgentStateChanged(const QString &serial, bool connected, bool isRoot);

private slots:
    void onSelectionChanged();
    void onPairAndConnect();
    void onInstallAgent();
    void onShowAudit();
    void onAutoTimer();

private:
    struct Session {
        RootAgentClient *client = nullptr;
        quint16 port = 0;
        bool paired = false;
        bool connected = false;
        bool connecting = false;
        bool packageScanned = false;
        qint64 lastPackageScan = 0;
        qint64 lastLaunchAttempt = 0;
        QString state = QStringLiteral("未连接");
        QString lastError;
    };

    RootAgentClient *clientFor(const QString &serial);
    void rebuildTable();
    void updateRow(const QString &serial);
    void loadConfig(const QString &serial);
    void appendLog(const QString &text, bool isError = false);
    bool ensureAdb(QString *adbPath);
    bool discoverPort(const QString &serial, const QString &package,
                      quint16 *port, bool *paired, QString *error);
    bool queryProvider(const QString &serial, const QString &package,
                       quint16 *port, bool *paired, QString *error);
    QString discoverPackage(const QString &serial, QString *error);
    bool ensurePackage(const QString &serial, QString *package, QString *error);
    void refreshPackage(const QString &serial);
    void maybeLaunchAgent(const QString &serial, const QString &package);
    void autoConnect(const QString &serial);
    void disconnectAll();
    QString packageFor(const QString &serial) const;
    QString settingsKey(const QString &serial, const QString &name) const;
    static QString modeName(int mode);
    int rowFor(const QString &serial) const;

    QMap<QString, DeviceInfo> m_devices;
    QMap<QString, Session> m_sessions;
    QString m_currentSerial;

    QTableWidget *m_table = nullptr;
    QLineEdit *m_pairCodeEdit = nullptr;
    QPlainTextEdit *m_log = nullptr;
    QTimer *m_autoTimer = nullptr;
    RootAgentInstaller *m_installer = nullptr;
    QPushButton *m_installBtn = nullptr;
};

#endif // ROOT_AGENT_MANAGER_PANEL_H
