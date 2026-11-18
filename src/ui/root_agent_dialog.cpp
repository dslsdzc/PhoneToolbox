#include "root_agent_dialog.h"

#include "core/adb_embedded.h"
#include "core/root_agent/root_agent_client.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {
const char *kSettingsPackage = "rootAgent/package";
const char *kSettingsPort = "rootAgent/port";
const char *kSettingsClientPrivate = "rootAgent/clientPrivate";
const char *kSettingsAgentPublic = "rootAgent/agentPublic";
}

RootAgentDialog::RootAgentDialog(const QString &serial,
                                 const QMap<QString, DeviceInfo> &devices,
                                 QWidget *parent)
    : QDialog(parent)
    , m_serial(serial)
    , m_client(new RootAgentClient(this))
{
    setWindowTitle(QStringLiteral("Root 代理"));
    setMinimumSize(760, 600);

    auto *layout = new QVBoxLayout(this);

    auto *hint = new QLabel(
        QStringLiteral("首次使用：在设备上打开 APK 并启动服务，输入它显示的配对码，"
                       "点「配对」；之后连接走 Noise 加密，不再依赖 token。"),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color: #888; font-size: 11px;"));
    layout->addWidget(hint);

    m_deviceCombo = new QComboBox(this);
    for (auto it = devices.constBegin(); it != devices.constEnd(); ++it) {
        const QString label = it.value().model.isEmpty()
            ? it.key()
            : QStringLiteral("%1 (%2)").arg(it.key(), it.value().model);
        m_deviceCombo->addItem(label, it.key());
    }
    if (m_deviceCombo->count() == 0)
        m_deviceCombo->addItem(serial, serial);
    const int initial = m_deviceCombo->findData(serial);
    if (initial >= 0)
        m_deviceCombo->setCurrentIndex(initial);
    layout->addWidget(m_deviceCombo);

    m_packageEdit = new QLineEdit(this);
    m_packageEdit->setPlaceholderText(QStringLiteral("设备端 APK 的包名"));

    m_portSpin = new QSpinBox(this);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(1);

    m_pairCodeEdit = new QLineEdit(this);
    m_pairCodeEdit->setPlaceholderText(QStringLiteral("APK 显示的 6 位配对码"));

    auto *form = new QFormLayout();
    form->addRow(QStringLiteral("包名"), m_packageEdit);
    form->addRow(QStringLiteral("设备端口"), m_portSpin);
    form->addRow(QStringLiteral("配对码"), m_pairCodeEdit);
    layout->addLayout(form);

    m_dangerousCheck = new QCheckBox(
        QStringLiteral("允许危险操作（dd / 写分区 / shell）"), this);
    m_dangerousCheck->setToolTip(QStringLiteral(
        "默认不授予。开启后配对记录里才包含 dangerous 能力；"
        "agent 会拒绝未授权的 dd/写分区/shell。"));
    layout->addWidget(m_dangerousCheck);

    auto *buttons = new QHBoxLayout();
    m_startBtn = new QPushButton(QStringLiteral("启动设备端"), this);
    m_discoverBtn = new QPushButton(QStringLiteral("自动获取端口"), this);
    m_pairBtn = new QPushButton(QStringLiteral("配对"), this);
    m_connectBtn = new QPushButton(QStringLiteral("连接"), this);
    m_auditBtn = new QPushButton(QStringLiteral("查看审计"), this);
    m_detectBtn = new QPushButton(QStringLiteral("检测 Root"), this);
    m_disconnectBtn = new QPushButton(QStringLiteral("断开"), this);
    m_detectBtn->setEnabled(false);
    m_disconnectBtn->setEnabled(false);
    buttons->addWidget(m_startBtn);
    buttons->addWidget(m_discoverBtn);
    buttons->addWidget(m_pairBtn);
    buttons->addWidget(m_connectBtn);
    buttons->addWidget(m_auditBtn);
    buttons->addWidget(m_detectBtn);
    buttons->addWidget(m_disconnectBtn);
    buttons->addStretch();
    layout->addLayout(buttons);

    m_status = new QLabel(QStringLiteral("未连接"), this);
    m_status->setStyleSheet(QStringLiteral("font-weight: bold;"));
    layout->addWidget(m_status);

    m_pairStatus = new QLabel(
        m_clientPrivateHex.isEmpty() || m_agentPublicHex.isEmpty()
            ? QStringLiteral("配对状态：未配对")
            : QStringLiteral("配对状态：已配对"),
        this);
    m_pairStatus->setStyleSheet(QStringLiteral("color: #888; font-size: 11px;"));
    layout->addWidget(m_pairStatus);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 11px;"));
    layout->addWidget(m_log, 1);

    connect(m_startBtn, &QPushButton::clicked, this, &RootAgentDialog::onStartDevice);
    connect(m_discoverBtn, &QPushButton::clicked, this, &RootAgentDialog::onDiscover);
    connect(m_pairBtn, &QPushButton::clicked, this, &RootAgentDialog::onPair);
    connect(m_connectBtn, &QPushButton::clicked, this, &RootAgentDialog::onConnect);
    connect(m_auditBtn, &QPushButton::clicked, this, &RootAgentDialog::onShowAudit);
    connect(m_detectBtn, &QPushButton::clicked, this, &RootAgentDialog::onDetectRoot);
    connect(m_disconnectBtn, &QPushButton::clicked, this, &RootAgentDialog::onDisconnect);

    connect(m_client, &RootAgentClient::connected, this, [this]() {
        appendLog(QStringLiteral("已连接（Noise）"));
        m_status->setText(QStringLiteral("已连接（Noise）"));
        updateUi(true);
    });
    connect(m_client, &RootAgentClient::disconnected, this, [this]() {
        if (!m_client->isConnected()) {
            m_status->setText(QStringLiteral("未连接"));
            updateUi(false);
        }
    });
    connect(m_client, &RootAgentClient::errorOccurred, this,
            [this](const QString &message) {
                appendLog(message, true);
                if (!m_client->isConnected())
                    updateUi(false);
            });
    connect(m_client, &RootAgentClient::pairingFinished, this,
            [this](bool ok, const QString &clientPrivate, const QString &agentPublic,
                   const QString &error) {
                if (ok) {
                    m_clientPrivateHex = clientPrivate;
                    m_agentPublicHex = agentPublic;
                    m_client->setIdentity(clientPrivate, agentPublic);
                    QSettings settings;
                    settings.setValue(settingsKey(QStringLiteral("clientPrivate")),
                                      clientPrivate);
                    settings.setValue(settingsKey(QStringLiteral("agentPublic")),
                                      agentPublic);
                    m_pairStatus->setText(QStringLiteral("配对状态：已配对"));
                    m_status->setText(QStringLiteral("配对成功，可以连接"));
                    appendLog(QStringLiteral("配对成功"));
                } else {
                    m_status->setText(QStringLiteral("配对失败"));
                    appendLog(QStringLiteral("配对失败：%1").arg(error), true);
                }
                updateUi(false);
            });
    connect(m_client, &RootAgentClient::rootDetected, this,
            [this](bool isRoot, const QString &detail) {
                m_status->setText(isRoot ? QStringLiteral("已 Root（agent 返回 uid=0）")
                                         : QStringLiteral("未 Root（agent 返回非 root）"));
                appendLog(QStringLiteral("root 检测：%1").arg(detail));
                m_detectBtn->setEnabled(true);
            });
    connect(m_client, &RootAgentClient::execFinished, this,
            [this](int exitCode, const QString &stdoutText, const QString &stderrText,
                   bool timedOut) {
                appendLog(QStringLiteral("命令完成 exit=%1 timed_out=%2")
                              .arg(exitCode)
                              .arg(timedOut));
                if (!stdoutText.isEmpty())
                    appendLog(stdoutText);
                if (!stderrText.isEmpty())
                    appendLog(stderrText, true);
            });

    loadSettings();
    connect(m_deviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &RootAgentDialog::onDeviceChanged);
    updateUi(false);
}

RootAgentDialog::~RootAgentDialog() = default;

QString RootAgentDialog::settingsKey(const QString &name) const
{
    return QStringLiteral("rootAgent/devices/%1/%2").arg(m_serial, name);
}

void RootAgentDialog::loadSettings()
{
    QSettings settings;
    const auto read = [&](const char *legacy, const QString &name) {
        const QString key = settingsKey(name);
        return settings.contains(key) ? settings.value(key)
                                      : settings.value(QLatin1String(legacy));
    };

    m_packageEdit->setText(read(kSettingsPackage, QStringLiteral("package")).toString());
    m_portSpin->setValue(read(kSettingsPort, QStringLiteral("port")).toInt());
    m_clientPrivateHex = read(kSettingsClientPrivate,
                              QStringLiteral("clientPrivate")).toString();
    m_agentPublicHex = read(kSettingsAgentPublic,
                            QStringLiteral("agentPublic")).toString();
    m_dangerousCheck->setChecked(
        read("rootAgent/allowDangerous", QStringLiteral("allowDangerous")).toBool());
    m_pairCodeEdit->clear();
    m_pairStatus->setText(
        m_clientPrivateHex.isEmpty() || m_agentPublicHex.isEmpty()
            ? QStringLiteral("配对状态：未配对")
            : QStringLiteral("配对状态：已配对"));
}

void RootAgentDialog::onDeviceChanged(int index)
{
    if (index < 0)
        return;
    const QString next = m_deviceCombo->itemData(index).toString();
    if (next.isEmpty() || next == m_serial)
        return;

    saveSettings();
    m_client->disconnectFromAgent();
    m_client->stopForward();
    m_serial = next;
    loadSettings();
    m_status->setText(QStringLiteral("未连接"));
    updateUi(false);
}

void RootAgentDialog::appendLog(const QString &text, bool isError)
{
    const QString line = isError ? QStringLiteral("[错误] %1").arg(text) : text;
    m_log->appendPlainText(line);
}

void RootAgentDialog::saveSettings()
{
    QSettings settings;
    settings.setValue(settingsKey(QStringLiteral("package")),
                      m_packageEdit->text().trimmed());
    settings.setValue(settingsKey(QStringLiteral("port")), m_portSpin->value());
    settings.setValue(settingsKey(QStringLiteral("allowDangerous")),
                      m_dangerousCheck ? m_dangerousCheck->isChecked() : false);
}

void RootAgentDialog::updateUi(bool connected)
{
    m_deviceCombo->setEnabled(!connected);
    m_startBtn->setEnabled(!connected);
    m_discoverBtn->setEnabled(!connected);
    m_pairBtn->setEnabled(!connected);
    m_connectBtn->setEnabled(!connected && !m_clientPrivateHex.isEmpty()
                             && !m_agentPublicHex.isEmpty());
    m_auditBtn->setEnabled(!m_packageEdit->text().trimmed().isEmpty());
    m_detectBtn->setEnabled(connected);
    m_disconnectBtn->setEnabled(connected);
    m_packageEdit->setEnabled(!connected);
    m_portSpin->setEnabled(!connected);
    m_pairCodeEdit->setEnabled(!connected);
    m_dangerousCheck->setEnabled(!connected);
}

bool RootAgentDialog::ensureAdb(QString *adbPath)
{
    QString path = AdbEmbedded::instance().getAdbPath();
    if (path.isEmpty()) {
        AdbEmbedded::instance().initialize();
        path = AdbEmbedded::instance().getAdbPath();
    }
    if (adbPath)
        *adbPath = path;
    return !path.isEmpty();
}

void RootAgentDialog::onStartDevice()
{
    const QString package = m_packageEdit->text().trimmed();
    if (package.isEmpty()) {
        appendLog(QStringLiteral("先填写设备端 APK 的包名"), true);
        return;
    }

    QString adbPath;
    if (!ensureAdb(&adbPath)) {
        appendLog(QStringLiteral("找不到 adb"), true);
        return;
    }

    QProcess process;
    QStringList args;
    if (!m_serial.isEmpty())
        args << QStringLiteral("-s") << m_serial;
    args << QStringLiteral("shell") << QStringLiteral("am") << QStringLiteral("start")
         << QStringLiteral("-n")
         << QStringLiteral("%1/com.local.service.MainActivity").arg(package);
    process.start(adbPath, args);
    if (!process.waitForFinished(5000)) {
        appendLog(QStringLiteral("启动设备端超时"), true);
        return;
    }
    appendLog(QString::fromLocal8Bit(process.readAllStandardOutput()).trimmed());
    appendLog(QString::fromLocal8Bit(process.readAllStandardError()).trimmed(), true);
}

void RootAgentDialog::onDiscover()
{
    const QString package = m_packageEdit->text().trimmed();
    if (package.isEmpty()) {
        appendLog(QStringLiteral("先填写设备端 APK 的包名"), true);
        return;
    }

    QString adbPath;
    if (!ensureAdb(&adbPath)) {
        appendLog(QStringLiteral("找不到 adb"), true);
        return;
    }

    QProcess process;
    QStringList args;
    if (!m_serial.isEmpty())
        args << QStringLiteral("-s") << m_serial;
    args << QStringLiteral("shell") << QStringLiteral("content")
         << QStringLiteral("query") << QStringLiteral("--uri")
         << QStringLiteral("content://%1.agentinfo/agent").arg(package);
    process.start(adbPath, args);
    if (!process.waitForFinished(5000)) {
        appendLog(QStringLiteral("读取 agent 信息超时"), true);
        return;
    }

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QString error = QString::fromLocal8Bit(process.readAllStandardError());
    if (!error.trimmed().isEmpty())
        appendLog(error.trimmed(), true);
    if (output.trimmed().isEmpty()) {
        appendLog(QStringLiteral("没有读到 agent 信息（APK 未启动或包名不对）"), true);
        return;
    }

    const QRegularExpression portRegex(QStringLiteral("port=(\\d+)"));
    const QRegularExpression pairedRegex(QStringLiteral("paired=(\\d+)"));

    const auto portMatch = portRegex.match(output);
    const auto pairedMatch = pairedRegex.match(output);

    if (portMatch.hasMatch()) {
        const int port = portMatch.captured(1).toInt();
        if (port > 0)
            m_portSpin->setValue(port);
    }

    const bool paired = pairedMatch.hasMatch() && pairedMatch.captured(1) == QStringLiteral("1");
    appendLog(output.trimmed());
    if (paired) {
        m_status->setText(QStringLiteral("agent 已配对，可直接连接"));
        m_pairStatus->setText(QStringLiteral("配对状态：已配对"));
    } else {
        m_status->setText(QStringLiteral("已获取端口；配对码请从 APK 界面手动输入"));
        m_pairStatus->setText(QStringLiteral("配对状态：未配对"));
    }
}

void RootAgentDialog::onPair()
{
    const QString code = m_pairCodeEdit->text().trimmed();
    if (code.isEmpty()) {
        appendLog(QStringLiteral("先输入 APK 显示的配对码"), true);
        return;
    }

    QString adbPath;
    if (!ensureAdb(&adbPath)) {
        appendLog(QStringLiteral("找不到 adb"), true);
        return;
    }

    saveSettings();
    m_client->setSerial(m_serial);

    QString error;
    if (!m_client->startForward(adbPath, quint16(m_portSpin->value()), &error)) {
        appendLog(QStringLiteral("adb forward 失败：%1").arg(error), true);
        return;
    }

    m_status->setText(QStringLiteral("配对中…"));
    m_client->pair(code, 10000, m_dangerousCheck->isChecked());
}

void RootAgentDialog::onConnect()
{
    if (m_clientPrivateHex.isEmpty() || m_agentPublicHex.isEmpty()) {
        appendLog(QStringLiteral("尚未配对，请先输入配对码并点「配对」"), true);
        return;
    }

    QString adbPath;
    if (!ensureAdb(&adbPath)) {
        appendLog(QStringLiteral("找不到 adb"), true);
        return;
    }

    saveSettings();
    m_client->setSerial(m_serial);
    m_client->setIdentity(m_clientPrivateHex, m_agentPublicHex);

    QString error;
    if (!m_client->startForward(adbPath, quint16(m_portSpin->value()), &error)) {
        appendLog(QStringLiteral("adb forward 失败：%1").arg(error), true);
        return;
    }

    m_status->setText(QStringLiteral("连接中…"));
    m_client->connectToAgent();
}

void RootAgentDialog::onShowAudit()
{
    const QString package = m_packageEdit->text().trimmed();
    if (package.isEmpty()) {
        appendLog(QStringLiteral("先填写设备端 APK 的包名"), true);
        return;
    }

    QString adbPath;
    if (!ensureAdb(&adbPath)) {
        appendLog(QStringLiteral("找不到 adb"), true);
        return;
    }

    QProcess process;
    QStringList args;
    if (!m_serial.isEmpty())
        args << QStringLiteral("-s") << m_serial;
    args << QStringLiteral("shell") << QStringLiteral("content")
         << QStringLiteral("query") << QStringLiteral("--uri")
         << QStringLiteral("content://%1.agentinfo/audit").arg(package);
    process.start(adbPath, args);
    if (!process.waitForFinished(5000)) {
        appendLog(QStringLiteral("读取审计超时"), true);
        return;
    }

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QString error = QString::fromLocal8Bit(process.readAllStandardError());
    if (!output.trimmed().isEmpty())
        appendLog(output.trimmed());
    if (!error.trimmed().isEmpty())
        appendLog(error.trimmed(), true);
}

void RootAgentDialog::onDetectRoot()
{
    if (!m_client->isConnected()) {
        appendLog(QStringLiteral("尚未连接"), true);
        return;
    }
    m_status->setText(QStringLiteral("检测中…"));
    m_detectBtn->setEnabled(false);
    m_client->detectRoot();
}

void RootAgentDialog::onDisconnect()
{
    m_client->disconnectFromAgent();
    m_client->stopForward();
    m_status->setText(QStringLiteral("未连接"));
    updateUi(false);
    appendLog(QStringLiteral("已断开"));
}
