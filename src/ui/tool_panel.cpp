#include "tool_panel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFormLayout>
#include <QLabel>

ToolPanel::ToolPanel(QWidget *parent)
    : QWidget(parent)
    , m_deviceList(nullptr)
    , m_restartModeCombo(nullptr)
    , m_restartButton(nullptr)
    , m_refreshButton(nullptr)
    , m_toolSelector(nullptr)
    , m_restartTool(new RestartTool(this))
    , m_currentSelectedDevice("")
{
    setupUI();
    setupConnections();
}

void ToolPanel::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(5, 5, 5, 5);

    // device list
    QGroupBox *deviceGroup = new QGroupBox(QStringLiteral("设备列表"), this);
    QVBoxLayout *deviceLayout = new QVBoxLayout(deviceGroup);
    m_deviceList = new QListWidget(this);
    m_deviceList->setSelectionMode(QAbstractItemView::SingleSelection);

    m_deviceList->setStyleSheet("QListWidget { "
                               "border-radius: 3px; "
                               "}"
                               "QListWidget::item:selected { "
                               "background-color: palette(highlight); "
                               "}");

    deviceLayout->addWidget(m_deviceList);

    // restart tool
    QGroupBox *restartGroup = new QGroupBox(QStringLiteral("重启工具"), this);
    QFormLayout *restartLayout = new QFormLayout(restartGroup);

    m_restartModeCombo = new QComboBox(this);
    m_restartModeCombo->addItem(QStringLiteral("正常模式 (System)"), RestartTool::MODE_SYSTEM);
    m_restartModeCombo->addItem(QStringLiteral("恢复模式 (Recovery)"), RestartTool::MODE_RECOVERY);
    m_restartModeCombo->addItem(QStringLiteral("引导程序 (Fastboot)"), RestartTool::MODE_BOOTLOADER);
    m_restartModeCombo->addItem("Fastbootd", RestartTool::MODE_FASTBOOT);
    m_restartModeCombo->addItem("EDL", RestartTool::MODE_EDL);
    m_restartModeCombo->addItem(QStringLiteral("关机"), RestartTool::MODE_SHUTDOWN);

    m_restartButton = new QPushButton(QStringLiteral("重启设备"), this);
    m_restartButton->setEnabled(false);

    restartLayout->addRow(QStringLiteral("目标模式:"), m_restartModeCombo);
    restartLayout->addRow(m_restartButton);

    QLabel *modeHelp = new QLabel(this);
    modeHelp->setText("Fastbootd: Android 10+ user space Fastboot\nFastboot: traditional bootloader mode");
    modeHelp->setWordWrap(true);
    modeHelp->setStyleSheet("color: #666; font-size: 10px;");
    restartLayout->addRow(modeHelp);

    // tool selector
    QGroupBox *toolGroup = new QGroupBox(QStringLiteral("工具选择"), this);
    QVBoxLayout *toolSelLayout = new QVBoxLayout(toolGroup);

    m_toolSelector = new QListWidget(this);
    m_toolSelector->setSelectionMode(QAbstractItemView::SingleSelection);
    m_toolSelector->setMaximumHeight(80);
    m_toolSelector->addItem(QStringLiteral("设备信息"));
    m_toolSelector->addItem(QStringLiteral("刷机工具"));
    m_toolSelector->addItem(QStringLiteral("系统工具"));
    m_toolSelector->item(0)->setSelected(true);

    toolSelLayout->addWidget(m_toolSelector);
    mainLayout->addWidget(toolGroup);

    // bottom toolbar
    QHBoxLayout *toolLayout = new QHBoxLayout();
    m_refreshButton = new QPushButton(QStringLiteral("刷新设备"), this);

    toolLayout->addWidget(m_refreshButton);
    toolLayout->addStretch();

    // assemble
    mainLayout->addWidget(deviceGroup);
    mainLayout->addWidget(restartGroup);
    mainLayout->addWidget(toolGroup);
    mainLayout->addLayout(toolLayout);
    mainLayout->addStretch();
}

void ToolPanel::setupConnections()
{
    connect(m_deviceList, &QListWidget::itemSelectionChanged,
            this, &ToolPanel::onDeviceListSelectionChanged);
    connect(m_restartButton, &QPushButton::clicked,
            this, &ToolPanel::onRestartButtonClicked);
    connect(m_refreshButton, &QPushButton::clicked,
            this, &ToolPanel::onRefreshButtonClicked);
    connect(m_restartTool, &RestartTool::outputMessage,
            this, &ToolPanel::onRestartToolOutput);
    connect(m_toolSelector, &QListWidget::itemSelectionChanged,
            this, &ToolPanel::onToolSelectionChanged);
}

void ToolPanel::updateDeviceList(const QMap<QString, DeviceInfo> &devices)
{
    m_currentDevices = devices;
    m_deviceList->clear();

    for (const DeviceInfo &info : m_currentDevices) {
        QString displayText = QString("%1\n%2").arg(info.serialNumber).arg(info.model);

        QString modeInfo;
        switch (info.mode) {
        case DeviceDetector::MODE_ADB: modeInfo = " [ADB]"; break;
        case DeviceDetector::MODE_FASTBOOT: modeInfo = " [Fastboot]"; break;
        case DeviceDetector::MODE_FASTBOOTD: modeInfo = " [Fastbootd]"; break;
        case DeviceDetector::MODE_EDL_9008: modeInfo = " [EDL]"; break;
        case DeviceDetector::MODE_MTK_DA: modeInfo = " [MTK DA]"; break;
        default: modeInfo = " [unknown]"; break;
        }

        displayText += modeInfo;

        QListWidgetItem *item = new QListWidgetItem(displayText, m_deviceList);
        item->setData(Qt::UserRole, info.serialNumber);
    }

    if (m_currentDevices.isEmpty()) {
        QListWidgetItem *item = new QListWidgetItem(QStringLiteral("无设备连接"), m_deviceList);
        item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
        m_restartButton->setEnabled(false);
    }
}

QString ToolPanel::getSelectedDevice() const
{
    return m_currentSelectedDevice;
}

void ToolPanel::onDeviceListSelectionChanged()
{
    QList<QListWidgetItem*> selectedItems = m_deviceList->selectedItems();

    if (selectedItems.isEmpty()) {
        m_currentSelectedDevice = "";
        m_restartButton->setEnabled(false);
        emit deviceSelectionChanged("");
        return;
    }

    QListWidgetItem *item = selectedItems.first();

    if (item->text().contains(QStringLiteral("无设备连接"))) {
        m_currentSelectedDevice = "";
        m_restartButton->setEnabled(false);
        emit deviceSelectionChanged("");
        return;
    }

    m_currentSelectedDevice = item->data(Qt::UserRole).toString();

    if (m_currentDevices.contains(m_currentSelectedDevice)) {
        m_restartButton->setEnabled(true);
        emit deviceSelectionChanged(m_currentSelectedDevice);
    } else {
        m_restartButton->setEnabled(false);
        emit deviceSelectionChanged("");
    }
}

void ToolPanel::onRestartButtonClicked()
{
    if (m_currentSelectedDevice.isEmpty() || !m_currentDevices.contains(m_currentSelectedDevice)) {
        emit outputMessage(QStringLiteral("请先选择一个设备"), true);
        return;
    }

    const DeviceInfo &info = m_currentDevices[m_currentSelectedDevice];
    RestartTool::RestartMode targetMode = static_cast<RestartTool::RestartMode>(
        m_restartModeCombo->currentData().toInt());

    m_restartTool->restartDevice(
        m_currentSelectedDevice,
        static_cast<DeviceDetector::DeviceMode>(info.mode),
        targetMode
    );
}

void ToolPanel::onRefreshButtonClicked()
{
    emit refreshRequested();
}

void ToolPanel::onRestartToolOutput(const QString &message, bool isError)
{
    emit outputMessage(message, isError);
}

void ToolPanel::onToolSelectionChanged()
{
    QList<QListWidgetItem*> selected = m_toolSelector->selectedItems();
    if (selected.isEmpty()) return;

    int index = m_toolSelector->row(selected.first());
    emit toolSelected(index);
}
