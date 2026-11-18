#ifndef ROOT_AGENT_DIALOG_H
#define ROOT_AGENT_DIALOG_H

#include <QDialog>
#include <QMap>
#include <QString>

#include "core/device_info.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class RootAgentClient;

// Root 代理控制面板：启动设备端 APK、CPace 配对、Noise 连接、检测 root。
class RootAgentDialog : public QDialog
{
    Q_OBJECT

public:
    explicit RootAgentDialog(const QString &serial,
                             const QMap<QString, DeviceInfo> &devices,
                             QWidget *parent = nullptr);
    ~RootAgentDialog() override;

private slots:
    void onDeviceChanged(int index);
    void onStartDevice();
    void onDiscover();
    void onPair();
    void onConnect();
    void onShowAudit();
    void onDetectRoot();
    void onDisconnect();

private:
    void appendLog(const QString &text, bool isError = false);
    void saveSettings();
    void loadSettings();
    QString settingsKey(const QString &name) const;
    void updateUi(bool connected);
    bool ensureAdb(QString *adbPath);

    QString m_serial;
    RootAgentClient *m_client = nullptr;
    QComboBox *m_deviceCombo = nullptr;
    QLineEdit *m_packageEdit = nullptr;
    QSpinBox *m_portSpin = nullptr;
    QLineEdit *m_pairCodeEdit = nullptr;
    QCheckBox *m_dangerousCheck = nullptr;
    QPushButton *m_startBtn = nullptr;
    QPushButton *m_discoverBtn = nullptr;
    QPushButton *m_pairBtn = nullptr;
    QPushButton *m_connectBtn = nullptr;
    QPushButton *m_auditBtn = nullptr;
    QPushButton *m_detectBtn = nullptr;
    QPushButton *m_disconnectBtn = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_pairStatus = nullptr;
    QPlainTextEdit *m_log = nullptr;

    QString m_clientPrivateHex;
    QString m_agentPublicHex;
};

#endif // ROOT_AGENT_DIALOG_H
