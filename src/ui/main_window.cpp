#include "main_window.h"
#include "core/adb_embedded.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
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
    , m_outputPanel(nullptr)
{
    setupUI();
    setupConnections();

    if (AdbEmbedded::instance().initialize()) {
        m_deviceDetector.startMonitoring();
        m_outputPanel->appendOutput("ADB initialized");
    } else {
        m_outputPanel->appendOutput("Failed to initialize ADB tools", true);
    }

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
    m_outputPanel = new OutputPanel(this);

    // stack: index 0 = device info, index 1 = flash panel, index 2 = system tools, index 3 = vuln panel
    m_stack = new QStackedWidget(this);
    m_stack->addWidget(m_deviceInfoPanel);
    m_stack->addWidget(m_flashPanel);
    m_stack->addWidget(m_systemToolPanel);
    m_stack->addWidget(m_vulnPanel);
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
}

void MainWindow::onDeviceConnected(const DeviceInfo &info)
{
    m_currentDevices[info.serialNumber] = info;
    m_toolPanel->updateDeviceList(m_currentDevices);

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

        if (m_stack->currentIndex() == 1) {
            m_flashPanel->clearDeviceInfo();
        } else if (m_stack->currentIndex() == 3) {
            m_vulnPanel->clearDeviceInfo();
        }
    }
}

void MainWindow::onDeviceModeChanged(const QString &serial, DeviceDetector::DeviceMode newMode)
{
    if (m_currentDevices.contains(serial)) {
        m_currentDevices[serial].mode = newMode;
        m_toolPanel->updateDeviceList(m_currentDevices);

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
