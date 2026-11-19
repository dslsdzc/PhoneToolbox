#include "root_agent_manager_panel.h"

#include "core/adb_embedded.h"
#include "core/device_detector.h"
#include "core/root_agent/root_agent_client.h"
#include "core/root_agent/root_agent_executor.h"
#include "core/root_agent/root_agent_installer.h"
#include "core/root_agent/root_shell.h"

#include <QCheckBox>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPointer>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr quint16 kLegacyAgentPort = 32145;
constexpr char kAgentPackage[] = "com.phonetoolbox.agent";
constexpr char kLegacyStartAction[] = "com.phonetoolbox.agent.START";
} // namespace

RootAgentManagerPanel::RootAgentManagerPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    auto *title = new QLabel(QStringLiteral("Root 代理管理"), this);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: bold;"));
    layout->addWidget(title);

    auto *hint = new QLabel(
        QStringLiteral("每台设备独立配对/密钥/会话。Android 5.0+ 从 APK 界面输入配对码；"
                       "Android 1.0–4.4 的配对码只显示在手机 App 屏幕上，桌面端必须手动输入，"
                       "不会通过 adb 自动读取或广播下发。\n"
                       "注意：配对成功即授予完整 Root 权限：可执行任意命令、读写任意文件，"
                       "没有按应用或按操作的权限隔离。只与你信任的电脑配对。"),
        this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral(
        "color: #9a4b00; background: #fff3cd; padding: 6px; font-size: 11px;"));
    layout->addWidget(hint);

    m_table = new QTableWidget(0, 5, this);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("设备"),
        QStringLiteral("型号"),
        QStringLiteral("模式"),
        QStringLiteral("配对"),
        QStringLiteral("连接"),
    });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);
    m_table->setMinimumHeight(160);
    layout->addWidget(m_table, 1);

    auto *configGroup = new QGroupBox(QStringLiteral("当前设备"), this);
    auto *configLayout = new QVBoxLayout(configGroup);

    auto *form = new QFormLayout();
    m_pairCodeEdit = new QLineEdit(configGroup);
    m_pairCodeEdit->setPlaceholderText(QStringLiteral("APK 显示的 6 位配对码（手动输入）"));
    form->addRow(QStringLiteral("配对码"), m_pairCodeEdit);
    configLayout->addLayout(form);

    auto *buttons = new QHBoxLayout();
    m_installBtn = new QPushButton(QStringLiteral("安装 Root 代理…"), configGroup);
    auto *pairBtn = new QPushButton(QStringLiteral("配对并连接"), configGroup);
    auto *auditBtn = new QPushButton(QStringLiteral("查看审计"), configGroup);
    auto *autoLabel = new QLabel(
        QStringLiteral("包名和端口自动识别；已配对设备自动连接，离开此页自动断开。"),
        configGroup);
    autoLabel->setStyleSheet(QStringLiteral("color: #888; font-size: 11px;"));
    buttons->addWidget(m_installBtn);
    buttons->addWidget(pairBtn);
    buttons->addWidget(auditBtn);
    buttons->addSpacing(12);
    buttons->addWidget(autoLabel, 1);
    configLayout->addLayout(buttons);
    layout->addWidget(configGroup);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 11px;"));
    m_log->setMinimumHeight(110);
    layout->addWidget(m_log, 1);

    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &RootAgentManagerPanel::onSelectionChanged);
    connect(m_installBtn, &QPushButton::clicked, this, &RootAgentManagerPanel::onInstallAgent);
    connect(pairBtn, &QPushButton::clicked, this, &RootAgentManagerPanel::onPairAndConnect);
    connect(auditBtn, &QPushButton::clicked, this, &RootAgentManagerPanel::onShowAudit);

    m_installer = new RootAgentInstaller(this);
    connect(m_installer, &RootAgentInstaller::logMessage, this,
            [this](const QString &text) { appendLog(text); });
    connect(m_installer, &RootAgentInstaller::finished, this,
            [this](bool ok, const QString &packageId, const QString &error) {
                m_installBtn->setEnabled(true);
                if (!ok) {
                    appendLog(QStringLiteral("安装失败：%1").arg(error), true);
                    return;
                }

                const QString serial = m_currentSerial;
                if (serial.isEmpty())
                    return;

                QSettings settings;
                settings.setValue(settingsKey(serial, QStringLiteral("package")),
                                  packageId);
                settings.remove(settingsKey(serial, QStringLiteral("clientPrivate")));
                settings.remove(settingsKey(serial, QStringLiteral("agentPublic")));

                Session &session = m_sessions[serial];
                if (session.client) {
                    session.client->disconnectFromAgent();
                    session.client->stopForward();
                }
                session.connected = false;
                session.connecting = false;
                session.paired = false;
                session.packageScanned = true;

                const bool legacy = isLegacyDevice(serial);
                session.state = legacy ? QStringLiteral("已安装，请在手机输入配对码")
                                       : QStringLiteral("已安装，等待配对");

                if (legacy) {
                    appendLog(QStringLiteral("已安装 legacy 代理。请在手机打开 PhoneToolbox Agent，"
                                             "输入屏幕上的配对码后点「配对并连接」。"));
                    // Start the agent in the background, but do not invent or
                    // read a pair code: it must be typed from the phone screen.
                    maybeLaunchAgent(serial, packageId);
                } else {
                    appendLog(QStringLiteral("已安装 %1。请在手机上查看配对码，输入后点「配对并连接」。")
                                  .arg(packageId));
                }
                updateRow(serial);
                autoConnect(serial);
            });

    m_autoTimer = new QTimer(this);
    m_autoTimer->setInterval(5000);
    connect(m_autoTimer, &QTimer::timeout, this, &RootAgentManagerPanel::onAutoTimer);
    m_autoTimer->start();
}

RootAgentManagerPanel::~RootAgentManagerPanel()
{
    // 析构子对象时 RootAgentClient 会 emit disconnected；此时表格/日志控件
    // 可能已经被销毁，槽里再 updateRow 就会访问悬空指针。
    if (m_autoTimer)
        m_autoTimer->stop();
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        RootShell::instance().unregisterExecutor(it.key());
        if (it->client) {
            it->client->disconnect(this);
            it->client->disconnectFromAgent();
        }
    }
    m_sessions.clear();
}

void RootAgentManagerPanel::setDevices(const QMap<QString, DeviceInfo> &devices)
{
    m_devices = devices;

    QSet<QString> current;
    for (auto it = devices.constBegin(); it != devices.constEnd(); ++it)
        current.insert(it.key());
    for (const QString &serial : m_sessions.keys()) {
        if (current.contains(serial))
            continue;
        Session &session = m_sessions[serial];
        RootShell::instance().unregisterExecutor(serial);
        if (session.client) {
            session.client->disconnectFromAgent();
            session.client->stopForward();
            session.client->deleteLater();
        }
        m_sessions.remove(serial);
    }

    rebuildTable();
    // Root 通道是全局能力，不依赖设置页是否可见：所有已配对设备都自动连接。
    const QStringList serials = m_devices.keys();
    for (const QString &serial : serials)
        autoConnect(serial);

    if (!m_currentSerial.isEmpty()) {
        Session &session = m_sessions[m_currentSerial];
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (packageFor(m_currentSerial).isEmpty() || now - session.lastPackageScan > 60000)
            refreshPackage(m_currentSerial);
    }
}

void RootAgentManagerPanel::onAutoTimer()
{
    if (!m_currentSerial.isEmpty() && packageFor(m_currentSerial).isEmpty()) {
        Session &session = m_sessions[m_currentSerial];
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now - session.lastPackageScan >= 30000) {
            session.lastPackageScan = now;
            refreshPackage(m_currentSerial);
        }
    }
    const QStringList serials = m_devices.keys();
    for (const QString &serial : serials)
        autoConnect(serial);
}

void RootAgentManagerPanel::rebuildTable()
{
    const QString keep = m_currentSerial;
    m_table->blockSignals(true);
    m_table->setRowCount(0);

    QSettings settings;
    QStringList serials = m_devices.keys();
    serials.sort();
    for (const QString &serial : serials) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *deviceItem = new QTableWidgetItem(serial);
        deviceItem->setData(Qt::UserRole, serial);
        m_table->setItem(row, 0, deviceItem);
        m_table->setItem(row, 1, new QTableWidgetItem(m_devices.value(serial).model));
        m_table->setItem(row, 2, new QTableWidgetItem(modeName(m_devices.value(serial).mode)));

        const bool configured =
            !settings.value(settingsKey(serial, QStringLiteral("clientPrivate")))
                 .toString().isEmpty()
            && !settings.value(settingsKey(serial, QStringLiteral("agentPublic")))
                    .toString().isEmpty();
        Session &session = m_sessions[serial];
        if (configured)
            session.paired = true;
        m_table->setItem(row, 3, new QTableWidgetItem(
                                   configured ? QStringLiteral("已配对")
                                              : QStringLiteral("未配对")));
        m_table->setItem(row, 4, new QTableWidgetItem(session.state));
    }
    m_table->blockSignals(false);

    if (!serials.isEmpty()) {
        int row = rowFor(keep);
        if (row < 0)
            row = 0;
        m_table->selectRow(row);
    }
    onSelectionChanged();
}

QString RootAgentManagerPanel::modeName(int mode)
{
    switch (mode) {
    case DeviceDetector::MODE_ADB: return QStringLiteral("ADB");
    case DeviceDetector::MODE_FASTBOOT: return QStringLiteral("Fastboot");
    case DeviceDetector::MODE_FASTBOOTD: return QStringLiteral("Fastbootd");
    case DeviceDetector::MODE_EDL_9008: return QStringLiteral("EDL");
    case DeviceDetector::MODE_MTK_DA: return QStringLiteral("MTK DA");
    case DeviceDetector::MODE_RECOVERY: return QStringLiteral("Recovery");
    case DeviceDetector::MODE_MTK_BROM: return QStringLiteral("MTK BROM");
    case DeviceDetector::MODE_SPD: return QStringLiteral("SPD");
    case DeviceDetector::MODE_SAMSUNG_ODIN: return QStringLiteral("Odin");
    case DeviceDetector::MODE_SAMSUNG_EUB: return QStringLiteral("EUB");
    default: return QStringLiteral("Unknown");
    }
}

int RootAgentManagerPanel::rowFor(const QString &serial) const
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *item = m_table->item(row, 0);
        if (item && item->data(Qt::UserRole).toString() == serial)
            return row;
    }
    return -1;
}

void RootAgentManagerPanel::updateRow(const QString &serial)
{
    const int row = rowFor(serial);
    if (row < 0)
        return;

    const Session &session = m_sessions[serial];
    m_table->item(row, 3)->setText(session.paired ? QStringLiteral("已配对")
                                                  : QStringLiteral("未配对"));
    m_table->item(row, 4)->setText(session.connected ? QStringLiteral("已连接")
                                                      : session.state);
}

QString RootAgentManagerPanel::settingsKey(const QString &serial,
                                           const QString &name) const
{
    return QStringLiteral("rootAgent/devices/%1/%2").arg(serial, name);
}

QString RootAgentManagerPanel::packageFor(const QString &serial) const
{
    QSettings settings;
    const QString key = settingsKey(serial, QStringLiteral("package"));
    return settings.contains(key) ? settings.value(key).toString()
                                  : settings.value(QStringLiteral("rootAgent/package")).toString();
}

void RootAgentManagerPanel::onSelectionChanged()
{
    const int row = m_table->currentRow();
    if (row < 0) {
        m_currentSerial.clear();
        return;
    }
    const QTableWidgetItem *item = m_table->item(row, 0);
    if (!item)
        return;
    m_currentSerial = item->data(Qt::UserRole).toString();
    loadConfig(m_currentSerial);
    autoConnect(m_currentSerial);
}

void RootAgentManagerPanel::loadConfig(const QString &serial)
{
    QSettings settings;
    const auto read = [&](const char *legacy, const QString &name) {
        const QString key = settingsKey(serial, name);
        return settings.contains(key) ? settings.value(key)
                                      : settings.value(QLatin1String(legacy));
    };

    m_pairCodeEdit->clear();
    m_pairCodeEdit->setReadOnly(false);
    m_pairCodeEdit->setPlaceholderText(
        QStringLiteral("手机 App 显示的 6 位配对码（手动输入）"));
    updateRow(serial);
}

void RootAgentManagerPanel::appendLog(const QString &text, bool isError)
{
    const QString prefix = m_currentSerial.isEmpty()
        ? QString()
        : QStringLiteral("[%1] ").arg(m_currentSerial);
    m_log->appendPlainText((isError ? QStringLiteral("[错误] ") : QString()) + prefix + text);
    emit outputMessage(prefix + text, isError);
}

bool RootAgentManagerPanel::ensureAdb(QString *adbPath)
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

int RootAgentManagerPanel::deviceSdk(const QString &serial)
{
    if (serial.isEmpty())
        return -1;

    Session &session = m_sessions[serial];
    if (session.sdk > 0)
        return session.sdk;

    QString adbPath;
    if (!ensureAdb(&adbPath))
        return -1;

    QProcess process;
    process.start(adbPath, {QStringLiteral("-s"), serial,
                            QStringLiteral("shell"), QStringLiteral("getprop"),
                            QStringLiteral("ro.build.version.sdk")});
    if (!process.waitForFinished(5000)) {
        process.kill();
        process.waitForFinished(1000);
        return -1;
    }

    bool ok = false;
    const int sdk = QString::fromLocal8Bit(process.readAllStandardOutput())
                        .trimmed()
                        .toInt(&ok);
    if (ok && sdk > 0)
        session.sdk = sdk;
    return session.sdk;
}

bool RootAgentManagerPanel::isLegacyDevice(const QString &serial)
{
    const int sdk = deviceSdk(serial);
    return sdk > 0 && sdk < 21;
}

bool RootAgentManagerPanel::queryProvider(const QString &serial,
                                          const QString &package,
                                          quint16 *port, bool *paired,
                                          QString *error)
{
    if (serial.isEmpty() || package.isEmpty() || !port) {
        if (error)
            *error = QStringLiteral("没有选中设备或尚未识别包名");
        return false;
    }

    QString adbPath;
    if (!ensureAdb(&adbPath)) {
        if (error)
            *error = QStringLiteral("找不到 adb");
        return false;
    }

    QProcess process;
    QStringList args;
    args << QStringLiteral("-s") << serial
         << QStringLiteral("shell") << QStringLiteral("content")
         << QStringLiteral("query") << QStringLiteral("--uri")
         << QStringLiteral("content://%1.agentinfo/agent").arg(package);
    process.start(adbPath, args);
    if (!process.waitForFinished(5000)) {
        if (error)
            *error = QStringLiteral("读取 agent 信息超时");
        return false;
    }

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QString stderrText = QString::fromLocal8Bit(process.readAllStandardError());
    if (!stderrText.trimmed().isEmpty()) {
        if (error)
            *error = stderrText.trimmed();
        return false;
    }

    const QRegularExpression portRegex(QStringLiteral("port=(\\d+)"));
    const QRegularExpression pairedRegex(QStringLiteral("paired=(\\d+)"));
    const auto portMatch = portRegex.match(output);
    if (!portMatch.hasMatch()) {
        if (error)
            *error = QStringLiteral("没有读到端口（APK 未启动）");
        return false;
    }

    *port = quint16(portMatch.captured(1).toUShort());
    if (paired) {
        const auto pairedMatch = pairedRegex.match(output);
        *paired = pairedMatch.hasMatch() && pairedMatch.captured(1) == QStringLiteral("1");
    }
    return *port != 0;
}

bool RootAgentManagerPanel::discoverPort(const QString &serial,
                                         const QString &package,
                                         quint16 *port, bool *paired,
                                         QString *error)
{
    if (isLegacyDevice(serial)) {
        // Android 1.0-4.4 has no `content` command and the APK has no
        // Activity/Provider. The agent always listens on a fixed port; the
        // desktop adb-forwards to it and keeps pairing state locally.
        if (!port) {
            if (error)
                *error = QStringLiteral("无效的端口参数");
            return false;
        }
        *port = kLegacyAgentPort;
        if (paired)
            *paired = true;
        return true;
    }
    return queryProvider(serial, package, port, paired, error);
}

// 从系统的 Provider 注册表反查随机包名：
//   [com.xxxx.yyyy.agentinfo]:
//     Provider{... com.xxxx.yyyy/com.local.service.AgentInfoProvider}
// 这样不需要用户知道/填写包名，也不依赖固定文件或固定 authority。
QString RootAgentManagerPanel::discoverPackage(const QString &serial, QString *error)
{
    if (serial.isEmpty()) {
        if (error)
            *error = QStringLiteral("没有选中设备");
        return QString();
    }

    if (isLegacyDevice(serial))
        return QString::fromLatin1(kAgentPackage);

    QString adbPath;
    if (!ensureAdb(&adbPath)) {
        if (error)
            *error = QStringLiteral("找不到 adb");
        return QString();
    }

    QProcess process;
    QStringList args;
    args << QStringLiteral("-s") << serial
         << QStringLiteral("shell") << QStringLiteral("dumpsys")
         << QStringLiteral("package") << QStringLiteral("providers");
    process.start(adbPath, args);
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(1000);
        if (error)
            *error = QStringLiteral("读取系统 Provider 列表超时");
        return QString();
    }

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    const QString stderrText = QString::fromLocal8Bit(process.readAllStandardError());
    if (output.isEmpty()) {
        if (error) {
            *error = stderrText.trimmed().isEmpty()
                ? QStringLiteral("系统没有返回 Provider 列表")
                : stderrText.trimmed();
        }
        return QString();
    }

    QStringList candidates;
    const QRegularExpression patterns[] = {
        QRegularExpression(QStringLiteral("\\[([A-Za-z0-9_.]+)\\.agentinfo\\]")),
        QRegularExpression(QStringLiteral("authority=([A-Za-z0-9_.]+)\\.agentinfo")),
    };
    for (const QRegularExpression &pattern : patterns) {
        auto matches = pattern.globalMatch(output);
        while (matches.hasNext()) {
            const QString candidate = matches.next().captured(1);
            if (!candidate.isEmpty() && !candidates.contains(candidate))
                candidates.append(candidate);
        }
    }

    if (candidates.isEmpty()) {
        if (error)
            *error = QStringLiteral("没有找到 .agentinfo Provider（APK 未安装？）");
        return QString();
    }

    if (candidates.size() > 1) {
        const QString configured = packageFor(serial);
        if (candidates.contains(configured))
            return configured;
        appendLog(QStringLiteral("发现多个代理包名，使用 %1").arg(candidates.first()));
    }
    return candidates.first();
}

bool RootAgentManagerPanel::ensurePackage(const QString &serial,
                                          QString *package, QString *error)
{
    if (!package || !error)
        return false;

    if (isLegacyDevice(serial)) {
        // Legacy APK always uses the fixed package name and has no Provider.
        *package = QString::fromLatin1(kAgentPackage);
        return true;
    }

    const QString configured = packageFor(serial);
    if (!configured.isEmpty()) {
        *package = configured;
        return true;
    }

    Session &session = m_sessions[serial];
    if (session.packageScanned) {
        *error = QStringLiteral("尚未识别到代理包名");
        return false;
    }

    session.packageScanned = true;
    session.lastPackageScan = QDateTime::currentMSecsSinceEpoch();

    const QString discovered = discoverPackage(serial, error);
    if (discovered.isEmpty())
        return false;

    QSettings settings;
    settings.setValue(settingsKey(serial, QStringLiteral("package")), discovered);
    appendLog(QStringLiteral("自动识别包名：%1").arg(discovered));
    *package = discovered;
    return true;
}

void RootAgentManagerPanel::maybeLaunchAgent(const QString &serial,
                                             const QString &package)
{
    if (serial.isEmpty() || package.isEmpty())
        return;

    Session &session = m_sessions[serial];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - session.lastLaunchAttempt < 30000)
        return;
    session.lastLaunchAttempt = now;

    QString adbPath;
    if (!ensureAdb(&adbPath))
        return;

    if (isLegacyDevice(serial)) {
        QProcess start;
        start.start(adbPath, {QStringLiteral("-s"), serial,
                              QStringLiteral("shell"), QStringLiteral("am"),
                              QStringLiteral("broadcast"),
                              QStringLiteral("-a"),
                              QString::fromLatin1(kLegacyStartAction)});
        if (!start.waitForFinished(5000)) {
            start.kill();
            appendLog(QStringLiteral("后台拉起 legacy 代理失败（超时）"), true);
            return;
        }
        if (start.exitCode() != 0) {
            appendLog(QStringLiteral("后台拉起 legacy 代理失败"), true);
            return;
        }
        appendLog(QStringLiteral("已后台拉起 legacy 代理（API %1），等待端口…")
                      .arg(deviceSdk(serial)));
        QTimer::singleShot(2500, this, [this, serial]() { autoConnect(serial); });
        return;
    }

    // 用显式广播拉起无界面的 AgentBootReceiver；不要用 am start，
    // 那会把 APK 的 Activity 带到前台。
    // -f 0x20 = FLAG_INCLUDE_STOPPED_PACKAGES，应用被 force-stop 后也能唤醒。
    QProcess start;
    start.start(adbPath, {QStringLiteral("-s"), serial, QStringLiteral("shell"),
                          QStringLiteral("am"), QStringLiteral("broadcast"),
                          QStringLiteral("-a"),
                          QStringLiteral("com.local.service.START_AGENT"),
                          QStringLiteral("-n"),
                          QStringLiteral("%1/com.local.service.AgentBootReceiver")
                              .arg(package),
                          QStringLiteral("-f"), QStringLiteral("0x20")});
    if (!start.waitForFinished(5000)) {
        start.kill();
        appendLog(QStringLiteral("后台拉起手机端失败（超时）"), true);
        return;
    }
    if (start.exitCode() != 0) {
        appendLog(QStringLiteral("后台拉起手机端失败"), true);
        return;
    }

    appendLog(QStringLiteral("已后台拉起手机端服务，等待端口…"));
    QTimer::singleShot(2500, this, [this, serial]() { autoConnect(serial); });
}

void RootAgentManagerPanel::refreshPackage(const QString &serial)
{
    if (serial.isEmpty())
        return;

    Session &session = m_sessions[serial];
    session.lastPackageScan = QDateTime::currentMSecsSinceEpoch();

    QString error;
    const QString discovered = discoverPackage(serial, &error);
    if (discovered.isEmpty()) {
        // 设备离线/APK 未装时保留旧配置，等下一轮再试。
        return;
    }

    const QString configured = packageFor(serial);
    if (discovered != configured) {
        QSettings settings;
        settings.setValue(settingsKey(serial, QStringLiteral("package")), discovered);
        appendLog(QStringLiteral("自动识别包名：%1").arg(discovered));
    }
    session.packageScanned = true;
}

RootAgentClient *RootAgentManagerPanel::clientFor(const QString &serial)
{
    auto it = m_sessions.find(serial);
    if (it != m_sessions.end() && it->client)
        return it->client;

    auto *client = new RootAgentClient(this);
    m_sessions[serial].client = client;

    // 注册给全局 RootShell：系统工具 / 刷机 / 重启优先走这条代理通道。
    const QPointer<RootAgentClient> guard(client);
    RootShell::instance().registerExecutor(
        serial, [guard](const QString &command, int timeoutMs) {
            return runRootAgentCommand(guard.data(), command, timeoutMs);
        });

    connect(client, &RootAgentClient::connected, this, [this, serial]() {
        if (!m_sessions.contains(serial))
            return;
        Session &session = m_sessions[serial];
        session.connected = true;
        session.connecting = false;
        session.lastError.clear();
        session.state = QStringLiteral("已连接");
        updateRow(serial);
        appendLog(QStringLiteral("已连接（Noise）"));
        emit rootAgentStateChanged(serial, true, false);
        // 连接成功的提权探测只用于同步设备信息页，不在代理表格里显示。
        session.client->detectRoot();
    });
    connect(client, &RootAgentClient::disconnected, this, [this, serial]() {
        if (!m_sessions.contains(serial))
            return;
        Session &session = m_sessions[serial];
        session.connected = false;
        session.connecting = false;
        session.state = isVisible() ? QStringLiteral("等待重连…")
                                    : QStringLiteral("未连接");
        updateRow(serial);
        emit rootAgentStateChanged(serial, false, false);
    });
    connect(client, &RootAgentClient::errorOccurred, this,
            [this, serial](const QString &message) {
                if (!m_sessions.contains(serial))
                    return;
                Session &session = m_sessions[serial];
                session.connecting = false;
                session.state = QStringLiteral("错误");
                updateRow(serial);
                if (message != session.lastError) {
                    session.lastError = message;
                    appendLog(message, true);
                }
                // 端口是旧的（Provider 缓存）或 agent 被停掉时，连接会失败；
                // 已配对设备自动重启手机端服务，不需要用户手动打开 APK。
                if (!session.connected)
                    maybeLaunchAgent(serial, packageFor(serial));
            });
    connect(client, &RootAgentClient::rootDetected, this,
            [this, serial](bool isRoot, const QString &detail) {
                if (!m_sessions.contains(serial))
                    return;
                emit rootAgentStateChanged(serial, true, isRoot);
                appendLog(QStringLiteral("Root 代理提权：%1")
                              .arg(detail.isEmpty()
                                       ? (isRoot ? QStringLiteral("uid=0")
                                                 : QStringLiteral("未取得 uid=0"))
                                       : detail));
            });
    connect(client, &RootAgentClient::pairingFinished, this,
            [this, serial](bool ok, const QString &clientPrivate,
                           const QString &agentPublic, const QString &error) {
                if (!m_sessions.contains(serial))
                    return;
                Session &session = m_sessions[serial];
                if (!ok) {
                    session.connecting = false;
                    session.state = QStringLiteral("配对失败");
                    updateRow(serial);
                    appendLog(QStringLiteral("配对失败：%1").arg(error), true);
                    return;
                }

                QSettings settings;
                settings.setValue(settingsKey(serial, QStringLiteral("clientPrivate")),
                                  clientPrivate);
                settings.setValue(settingsKey(serial, QStringLiteral("agentPublic")),
                                  agentPublic);
                session.paired = true;
                session.connecting = true;
                session.state = QStringLiteral("配对成功，连接中…");
                updateRow(serial);
                appendLog(QStringLiteral("配对成功：这台电脑已获得完整 Root 权限，正在建立 Noise 连接…"));
                session.client->setIdentity(clientPrivate, agentPublic);
                session.client->connectToAgent();
            });
    connect(client, &RootAgentClient::execFinished, this,
            [this, serial](int exitCode, const QString &stdoutText,
                           const QString &stderrText, bool timedOut) {
                Q_UNUSED(serial)
                appendLog(QStringLiteral("命令完成 exit=%1 timed_out=%2")
                              .arg(exitCode)
                              .arg(timedOut));
                if (!stdoutText.isEmpty())
                    appendLog(stdoutText);
                if (!stderrText.isEmpty())
                    appendLog(stderrText, true);
            });

    return client;
}

void RootAgentManagerPanel::autoConnect(const QString &serial)
{
    if (serial.isEmpty())
        return;

    Session &session = m_sessions[serial];
    if (session.connected || session.connecting)
        return;

    // agent 只在 ADB 模式下可用；Fastboot/EDL 等模式不去查端口。
    if (m_devices.contains(serial)
        && m_devices.value(serial).mode != DeviceDetector::MODE_ADB) {
        if (session.client) {
            session.client->disconnectFromAgent();
            session.client->stopForward();
        }
        session.connected = false;
        session.connecting = false;
        session.state = QStringLiteral("非 ADB 模式");
        updateRow(serial);
        return;
    }

    QSettings settings;
    const QString clientPrivate =
        settings.value(settingsKey(serial, QStringLiteral("clientPrivate"))).toString();
    const QString agentPublic =
        settings.value(settingsKey(serial, QStringLiteral("agentPublic"))).toString();
    if (clientPrivate.isEmpty() || agentPublic.isEmpty()) {
        session.state = QStringLiteral("未配对");
        updateRow(serial);
        return;
    }

    QString package;
    QString packageError;
    if (!ensurePackage(serial, &package, &packageError)) {
        session.state = QStringLiteral("未找到 Root 代理");
        updateRow(serial);
        return;
    }

    session.connecting = true;
    session.state = QStringLiteral("自动发现端口…");
    updateRow(serial);

    quint16 port = 0;
    bool paired = false;
    QString error;
    if (!discoverPort(serial, package, &port, &paired, &error)) {
        session.connecting = false;
        session.state = QStringLiteral("等待设备…");
        updateRow(serial);
        // 已配对设备不需要用户手动打开 APK：端口读不到时自动拉起。
        maybeLaunchAgent(serial, package);
        return;
    }
    if (!paired) {
        // 手机端已经删除这台电脑的配对记录。
        session.connecting = false;
        session.paired = false;
        session.state = QStringLiteral("设备端已删除配对");
        updateRow(serial);
        return;
    }

    QString adbPath;
    if (!ensureAdb(&adbPath)) {
        session.connecting = false;
        session.state = QStringLiteral("找不到 adb");
        updateRow(serial);
        return;
    }

    RootAgentClient *client = clientFor(serial);
    client->setSerial(serial);
    client->setIdentity(clientPrivate, agentPublic);
    if (!client->startForward(adbPath, port, &error)) {
        session.connecting = false;
        session.state = QStringLiteral("adb forward 失败");
        updateRow(serial);
        appendLog(QStringLiteral("adb forward 失败：%1").arg(error), true);
        return;
    }

    session.port = port;
    session.paired = true;
    session.state = QStringLiteral("连接中…");
    updateRow(serial);
    client->connectToAgent();
}

void RootAgentManagerPanel::onPairAndConnect()
{
    pairAndConnect(m_currentSerial);
}

void RootAgentManagerPanel::pairAndConnect(const QString &serial)
{
    if (serial.isEmpty()) {
        appendLog(QStringLiteral("先选中设备"), true);
        return;
    }

    const QString code = m_pairCodeEdit->text().trimmed();
    if (code.isEmpty()) {
        appendLog(QStringLiteral("请在手机打开 PhoneToolbox Agent，"
                                 "输入界面显示的 6 位配对码"), true);
        return;
    }

    Session &session = m_sessions[serial];
    if (session.client) {
        session.client->disconnectFromAgent();
        session.client->stopForward();
    }
    session.connected = false;
    session.connecting = false;

    QString package;
    QString packageError;
    if (!ensurePackage(serial, &package, &packageError)) {
        appendLog(QStringLiteral("识别包名失败：%1").arg(packageError), true);
        return;
    }

    quint16 port = 0;
    bool paired = false;
    QString error;
    if (!discoverPort(serial, package, &port, &paired, &error)) {
        appendLog(QStringLiteral("获取端口失败：%1").arg(error), true);
        return;
    }

    QString adbPath;
    if (!ensureAdb(&adbPath)) {
        appendLog(QStringLiteral("找不到 adb"), true);
        return;
    }

    RootAgentClient *client = clientFor(serial);
    client->setSerial(serial);

    if (!client->startForward(adbPath, port, &error)) {
        appendLog(QStringLiteral("adb forward 失败：%1").arg(error), true);
        return;
    }

    session.port = port;
    session.connecting = true;
    session.state = QStringLiteral("配对中…");
    updateRow(serial);
    // Root 代理配对即授予完整 root 能力；不再单独询问“允许危险操作”。
    client->pair(code, 10000, true);
}

void RootAgentManagerPanel::onInstallAgent()
{
    if (m_currentSerial.isEmpty()) {
        appendLog(QStringLiteral("先选中设备"), true);
        return;
    }

    if (QMessageBox::question(
            this, QStringLiteral("安装 Root 代理"),
            QStringLiteral("将安装 PhoneToolbox Agent（固定包名 com.phonetoolbox.agent）。\n\n"
                           "注意：配对成功即授予完整 Root 权限：可执行任意命令、读写任意文件，"
                           "没有按应用或按操作的权限隔离。\n\n"
                           "Android 1.0–4.4 安装后会打开手机端界面；配对码只显示在手机屏幕上，"
                           "桌面端需要手动输入，不会通过 adb 自动读取。\n"
                           "Android 5.0+ 也需要打开一次 APK 查看配对码。继续？"),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;

    QString adbPath;
    if (!ensureAdb(&adbPath)) {
        appendLog(QStringLiteral("找不到 adb"), true);
        return;
    }

    m_installBtn->setEnabled(false);
    appendLog(QStringLiteral("开始生成并安装 Root 代理…"));
    m_installer->start(m_currentSerial, adbPath);
}

void RootAgentManagerPanel::disconnectAll()
{
    for (auto it = m_sessions.begin(); it != m_sessions.end(); ++it) {
        if (it->client) {
            it->client->disconnectFromAgent();
            it->client->stopForward();
        }
        it->connected = false;
        it->connecting = false;
        it->state = QStringLiteral("未连接");
    }
    if (!m_currentSerial.isEmpty())
        updateRow(m_currentSerial);
}

void RootAgentManagerPanel::onShowAudit()
{
    if (m_currentSerial.isEmpty()) {
        appendLog(QStringLiteral("先选中设备"), true);
        return;
    }

    if (isLegacyDevice(m_currentSerial)) {
        appendLog(QStringLiteral("legacy 设备（Android 1.0–4.4）没有 Provider；"
                                 "审计记录在设备端 agent 的 state/audit.log。"), true);
        return;
    }

    QString package = packageFor(m_currentSerial);
    if (package.isEmpty()) {
        QString packageError;
        if (!ensurePackage(m_currentSerial, &package, &packageError)) {
            appendLog(QStringLiteral("识别包名失败：%1").arg(packageError), true);
            return;
        }
    }

    QString adbPath;
    if (!ensureAdb(&adbPath))
        return;

    QProcess process;
    QStringList args;
    args << QStringLiteral("-s") << m_currentSerial
         << QStringLiteral("shell") << QStringLiteral("content")
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
