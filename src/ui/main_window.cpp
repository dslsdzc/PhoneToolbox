#include "main_window.h"
#include "core/adb_embedded.h"
#include "core/resource_monitor.h"
#include "plugins/plugin_manager.h"
#include "ui/settings_panel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_mainSplitter(nullptr)
    , m_rightSplitter(nullptr)
    , m_stack(nullptr)
    , m_toolPanel(nullptr)
    , m_deviceInfoPanel(nullptr)
    , m_flashPanel(nullptr)
    , m_systemToolPanel(nullptr)
    , m_vulnPanel(nullptr)
    , m_imageToolPanel(nullptr)
    , m_pluginPanel(nullptr)
    , m_settingsPanel(nullptr)
    , m_outputPanel(nullptr)
{
    setupUI();
    setupConnections();

    // F2-P：插件系统 — 启动时扫描并加载插件（华为刷写 .so 等）。
    // 搜索顺序：环境变量（调试/绿色版）→ exe 同目录 plugins（Windows / 构建树）
    // → Linux 安装前缀下的 lib/phonetoolbox/plugins。
    // 加载失败不再只写日志：PluginToolPanel 会逐项显示"加载失败 + 原因"。
    const QStringList pluginDirs = {
        qEnvironmentVariable("PHONETOOLBOX_PLUGIN_DIR"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/plugins"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../lib/phonetoolbox/plugins"),
        QCoreApplication::applicationDirPath() + QStringLiteral("/../lib64/phonetoolbox/plugins"),
    };
    for (const QString &dir : pluginDirs) {
        if (dir.isEmpty() || !QDir(dir).exists())
            continue;
        PluginManager::instance().scanPlugins(dir);
        break;
    }
    m_pluginPanel->setRecords(PluginManager::instance().records());
    const QStringList pluginErrors = PluginManager::instance().loadErrors();
    for (const QString &err : pluginErrors)
        m_outputPanel->appendOutput(err, true);

    if (AdbEmbedded::instance().initialize()) {
        m_deviceDetector.startMonitoring();
        m_outputPanel->appendOutput("ADB initialized");
    } else {
        m_outputPanel->appendOutput("Failed to initialize ADB tools", true);
    }

    // H1：启动整体 CPU 采样（各面板已在 setupUI 中构造并连接 cpuHigh 信号，
    // 首个采样仅建立基准；>80% 时自动降级各监听模块，<70% 恢复）
    ResourceMonitor::instance().start();

    m_outputPanel->appendOutput("Phone Toolbox started");
}

MainWindow::~MainWindow()
{
    m_deviceDetector.stopMonitoring();
}

void MainWindow::setupUI()
{
    setWindowTitle("Phone Toolbox");
    setMinimumSize(1200, 800);
    setAcceptDrops(true);

    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QHBoxLayout *mainLayout = new QHBoxLayout(centralWidget);

    m_mainSplitter = new QSplitter(Qt::Horizontal, this);
    m_rightSplitter = new QSplitter(Qt::Vertical, this);

    m_toolPanel = new ToolPanel(this);
    m_deviceInfoPanel = new DeviceInfoPanel(this);
    m_flashPanel = new FlashPanel(this);
    m_systemToolPanel = new SystemToolPanel(this);
    m_vulnPanel = new VulnPanel(this);
    m_imageToolPanel = new ImageToolPanel(this);
    m_pluginPanel = new PluginToolPanel(this);
    m_settingsPanel = new SettingsPanel(this);
    m_outputPanel = new OutputPanel(this);

    // stack: index 0 = device info, index 1 = flash panel, index 2 = system tools,
    //        index 3 = vuln panel, index 4 = image tool panel, index 5 = plugin panel
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_deviceInfoPanel);
    m_stack->addWidget(m_flashPanel);
    m_stack->addWidget(m_systemToolPanel);
    m_stack->addWidget(m_vulnPanel);
    m_stack->addWidget(m_imageToolPanel);
    m_stack->addWidget(m_pluginPanel);
    m_stack->addWidget(m_settingsPanel);
    m_stack->setCurrentIndex(0);

    m_rightSplitter->addWidget(m_stack);
    m_rightSplitter->addWidget(m_outputPanel);

    m_rightSplitter->setStretchFactor(0, 2);
    m_rightSplitter->setStretchFactor(1, 1);

    m_mainSplitter->addWidget(m_toolPanel);
    m_mainSplitter->addWidget(m_rightSplitter);

    m_mainSplitter->setStretchFactor(0, 1);
    m_mainSplitter->setStretchFactor(1, 2);

    mainLayout->addWidget(m_mainSplitter);
}

void MainWindow::setupConnections()
{
    // device detection signals
    connect(&m_deviceDetector, &DeviceDetector::deviceConnected,
            this, &MainWindow::onDeviceConnected);
    connect(&m_deviceDetector, &DeviceDetector::deviceDisconnected,
            this, &MainWindow::onDeviceDisconnected);
    connect(&m_deviceDetector, &DeviceDetector::deviceModeChanged,
            this, &MainWindow::onDeviceModeChanged);
    connect(&m_deviceDetector, &DeviceDetector::deviceInfoUpdated,
            this, &MainWindow::onDeviceInfoUpdated);

    // tool panel signals
    connect(m_toolPanel, &ToolPanel::deviceSelectionChanged,
            this, &MainWindow::onDeviceSelectionChanged);
    connect(m_toolPanel, &ToolPanel::outputMessage,
            this, &MainWindow::onOutputMessage);
    connect(m_toolPanel, &ToolPanel::refreshRequested,
            this, &MainWindow::onRefreshRequested);
    connect(m_toolPanel, &ToolPanel::toolSelected,
            this, &MainWindow::onToolSelected);

    // flash panel -> output
    connect(m_flashPanel, &FlashPanel::outputMessage,
            this, &MainWindow::onOutputMessage);

    // flash panel back button -> switch to device info
    connect(m_flashPanel, &FlashPanel::switchToDeviceInfo, this, [this]() {
        m_toolPanel->selectToolByIndex(0);
    });

    // settings panel -> output / root agent state
    connect(m_settingsPanel, &SettingsPanel::outputMessage,
            this, &MainWindow::onOutputMessage);
    connect(m_settingsPanel, &SettingsPanel::rootAgentStateChanged,
            this, &MainWindow::onRootAgentStateChanged);

    // system tool panel -> output
    connect(m_systemToolPanel, &SystemToolPanel::outputMessage,
            this, &MainWindow::onOutputMessage);

    // system tool panel back button -> switch to device info
    connect(m_systemToolPanel, &SystemToolPanel::switchToDeviceInfo, this, [this]() {
        m_toolPanel->selectToolByIndex(0);
    });

    // vuln panel -> output
    connect(m_vulnPanel, &VulnPanel::outputMessage,
            this, &MainWindow::onOutputMessage);

    // vuln panel back button -> switch to device info
    connect(m_vulnPanel, &VulnPanel::switchToDeviceInfo, this, [this]() {
        m_toolPanel->selectToolByIndex(0);
    });

    // image tool panel -> output
    connect(m_imageToolPanel, &ImageToolPanel::outputMessage,
            this, &MainWindow::onOutputMessage);

    // image tool panel back button -> switch to device info
    connect(m_imageToolPanel, &ImageToolPanel::switchToDeviceInfo, this, [this]() {
        m_toolPanel->selectToolByIndex(0);
    });

}

void MainWindow::onDeviceConnected(const DeviceInfo &info)
{
    // 设备轮询会重新构造 DeviceInfo，保留 Root 代理通道状态，
    // 否则连接状态会在下一次刷新时被清掉。
    const DeviceInfo previous = m_currentDevices.value(info.serialNumber);
    DeviceInfo merged = info;
    merged.rootAgentConnected = previous.rootAgentConnected;
    merged.rootAgentRoot = previous.rootAgentRoot;
    m_currentDevices[info.serialNumber] = merged;
    m_toolPanel->updateDeviceList(m_currentDevices);
    m_vulnPanel->setDevices(m_currentDevices);
    m_settingsPanel->setDevices(m_currentDevices);

    QString modeStr;
    switch (info.mode) {
    case DeviceDetector::MODE_ADB: modeStr = "ADB"; break;
    case DeviceDetector::MODE_FASTBOOT: modeStr = "Fastboot"; break;
    case DeviceDetector::MODE_FASTBOOTD: modeStr = "Fastbootd"; break;
    case DeviceDetector::MODE_EDL_9008: modeStr = "EDL 9008"; break;
    case DeviceDetector::MODE_MTK_DA: modeStr = "MTK DA"; break;
    default: modeStr = "unknown"; break;
    }

    m_outputPanel->appendOutput(QString("Device connected: %1 (%2) - %3")
                               .arg(info.serialNumber)
                               .arg(info.model)
                               .arg(modeStr));
}

void MainWindow::onDeviceDisconnected(const QString &serial)
{
    if (m_currentDevices.contains(serial)) {
        m_outputPanel->appendOutput(QString("Device disconnected: %1").arg(serial));
        m_currentDevices.remove(serial);
        m_toolPanel->updateDeviceList(m_currentDevices);
        m_vulnPanel->setDevices(m_currentDevices);
        m_settingsPanel->setDevices(m_currentDevices);

        if (m_stack->currentIndex() == 1) {
            m_flashPanel->clearDeviceInfo();
        } else if (m_stack->currentIndex() == 3) {
            m_vulnPanel->clearDeviceInfo();
        }
    }
}

void MainWindow::onDeviceInfoUpdated(const DeviceInfo &info)
{
    if (!m_currentDevices.contains(info.serialNumber))
        return;

    // 后台线程只发一次“信息变化”；这里做最终的 UI 更新。
    // 不重建设备列表，避免选中项和设备详情页被清空。
    const DeviceInfo previous = m_currentDevices.value(info.serialNumber);
    DeviceInfo merged = info;
    merged.rootAgentConnected = previous.rootAgentConnected;
    merged.rootAgentRoot = previous.rootAgentRoot;
    m_currentDevices[info.serialNumber] = merged;

    if (m_toolPanel->getSelectedDevice() == info.serialNumber)
        updateCurrentDeviceInfo();
}

void MainWindow::onRootAgentStateChanged(const QString &serial, bool connected,
                                         bool isRoot)
{
    if (!m_currentDevices.contains(serial))
        return;

    DeviceInfo &info = m_currentDevices[serial];
    info.rootAgentConnected = connected;
    info.rootAgentRoot = isRoot;

    m_outputPanel->appendOutput(
        QString("Root agent: %1 %2")
            .arg(serial, !connected ? QStringLiteral("disconnected")
                                    : (isRoot ? QStringLiteral("uid=0")
                                              : QStringLiteral("connected"))));

    if (m_toolPanel->getSelectedDevice() == serial)
        updateCurrentDeviceInfo();
}

void MainWindow::onDeviceModeChanged(const QString &serial, DeviceDetector::DeviceMode newMode)
{
    if (m_currentDevices.contains(serial)) {
        m_currentDevices[serial].mode = newMode;
        m_toolPanel->updateDeviceList(m_currentDevices);
        m_vulnPanel->setDevices(m_currentDevices);
        m_settingsPanel->setDevices(m_currentDevices);

        QString modeStr;
        switch (newMode) {
        case DeviceDetector::MODE_ADB: modeStr = "ADB"; break;
        case DeviceDetector::MODE_FASTBOOT: modeStr = "Fastboot"; break;
        case DeviceDetector::MODE_FASTBOOTD: modeStr = "Fastbootd"; break;
        default: modeStr = "unknown"; break;
        }

        m_outputPanel->appendOutput(QString("Device mode changed: %1 -> %2").arg(serial).arg(modeStr));

        if (m_toolPanel->getSelectedDevice() == serial) {
            updateCurrentDeviceInfo();
        }
    }
}

void MainWindow::onDeviceSelectionChanged(const QString &deviceId)
{
    if (deviceId.isEmpty() || !m_currentDevices.contains(deviceId)) {
        m_deviceInfoPanel->clearDeviceInfo();
        m_flashPanel->clearDeviceInfo();
        m_systemToolPanel->clearDeviceInfo();
    } else {
        m_deviceInfoPanel->updateDeviceInfo(m_currentDevices[deviceId]);
        m_flashPanel->setDeviceInfo(m_currentDevices[deviceId]);
        m_systemToolPanel->setDeviceInfo(m_currentDevices[deviceId]);
        m_vulnPanel->setDeviceInfo(m_currentDevices[deviceId]);
    }
}

void MainWindow::onOutputMessage(const QString &message, bool isError)
{
    m_outputPanel->appendOutput(message, isError);
}

void MainWindow::onRefreshRequested()
{
    m_outputPanel->appendOutput("Manual device refresh...");
    m_deviceDetector.startMonitoring();
}

void MainWindow::onToolSelected(int index)
{
    QString deviceId = m_toolPanel->getSelectedDevice();

    if (index == 1) {
        // switch to flash panel
        if (!deviceId.isEmpty() && m_currentDevices.contains(deviceId)) {
            m_flashPanel->setDeviceInfo(m_currentDevices[deviceId]);
        } else {
            m_flashPanel->clearDeviceInfo();
        }
    } else if (index == 2) {
        // switch to system tools
        if (!deviceId.isEmpty() && m_currentDevices.contains(deviceId)) {
            m_systemToolPanel->setDeviceInfo(m_currentDevices[deviceId]);
        } else {
            m_systemToolPanel->clearDeviceInfo();
        }
    } else if (index == 3) {
        // switch to vuln panel
        if (!deviceId.isEmpty() && m_currentDevices.contains(deviceId)) {
            m_vulnPanel->setDeviceInfo(m_currentDevices[deviceId]);
        } else {
            m_vulnPanel->clearDeviceInfo();
        }
    } else if (index == 6) {
        m_settingsPanel->setDevices(m_currentDevices);
    }
    m_stack->setCurrentIndex(index);
}

static bool isRomFile(const QString &path)
{
    QString lower = path.toLower();
    return lower.endsWith(".zip") || lower.endsWith(".tar") ||
           lower.endsWith(".tar.md5") || lower.endsWith(".img") ||
           lower.endsWith(".gz") || lower.endsWith(".br") ||
           lower.endsWith(".sh") || lower.endsWith(".bat");
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        for (const QUrl &url : event->mimeData()->urls()) {
            if (isRomFile(url.toLocalFile())) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    event->ignore();
}

void MainWindow::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasUrls()) return;

    for (const QUrl &url : event->mimeData()->urls()) {
        QString filePath = url.toLocalFile();
        if (!filePath.isEmpty() && isRomFile(filePath)) {
            // switch to flash panel and load the file
            m_stack->setCurrentIndex(1);
            m_flashPanel->updateFileInfo(filePath);
            QString deviceId = m_toolPanel->getSelectedDevice();
            if (!deviceId.isEmpty() && m_currentDevices.contains(deviceId)) {
                m_flashPanel->setDeviceInfo(m_currentDevices[deviceId]);
            }
            event->acceptProposedAction();
            m_outputPanel->appendOutput(QString("已载入: %1").arg(QFileInfo(filePath).fileName()));
            return;
        }
    }
}

void MainWindow::updateCurrentDeviceInfo()
{
    QString deviceId = m_toolPanel->getSelectedDevice();
    if (deviceId.isEmpty() || !m_currentDevices.contains(deviceId)) {
        m_deviceInfoPanel->clearDeviceInfo();
        m_flashPanel->clearDeviceInfo();
        m_vulnPanel->clearDeviceInfo();
    } else {
        m_deviceInfoPanel->updateDeviceInfo(m_currentDevices[deviceId]);
        m_flashPanel->setDeviceInfo(m_currentDevices[deviceId]);
        m_vulnPanel->setDeviceInfo(m_currentDevices[deviceId]);
    }
}
