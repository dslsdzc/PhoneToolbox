#include "device_info_panel.h"
#include <QVBoxLayout>
#include <QGroupBox>
#include <QTextEdit>
#include <QFrame>
#include <QPushButton>
#include <QScrollBar>

DeviceInfoPanel::DeviceInfoPanel(QWidget *parent)
    : QWidget(parent)
    , m_infoDisplay(nullptr)
{
    setupUI();
}

void DeviceInfoPanel::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(5, 5, 5, 5);

    QGroupBox *infoGroup = new QGroupBox("设备详细信息", this);
    QVBoxLayout *groupLayout = new QVBoxLayout(infoGroup);

    m_infoDisplay = new QTextEdit(this);
    m_infoDisplay->setReadOnly(true);
    m_infoDisplay->setFrameStyle(QFrame::NoFrame);
    m_infoDisplay->setMinimumHeight(200);
    m_infoDisplay->setStyleSheet(
        "QTextEdit { background: transparent; border: none; }");
    m_infoDisplay->setHtml(buildEmptyHtml());

    groupLayout->addWidget(m_infoDisplay);

    mainLayout->addWidget(infoGroup);
}

void DeviceInfoPanel::updateDeviceInfo(const DeviceInfo &info)
{
    // 动态刷新（电池/CPU 等）不应把用户正在看的位置弹回顶部。
    const int scroll = m_infoDisplay->verticalScrollBar()->value();
    m_info = info;
    m_infoDisplay->setHtml(buildInfoHtml(info));
    m_infoDisplay->verticalScrollBar()->setValue(scroll);
}

QString DeviceInfoPanel::escapeHtml(const QString &text) const
{
    QString escaped = text;
    escaped.replace("&", "&amp;");
    escaped.replace("<", "&lt;");
    escaped.replace(">", "&gt;");
    return escaped;
}

QString DeviceInfoPanel::buildEmptyHtml()
{
    return
        "<html><body style='font-size:13px; color:palette(text);'>"
        "<table width='100%' style='border-collapse:collapse;'>"
        "<tr><td width='30%' style='padding:0px 3px; color:palette(text);'>序列号:</td>"
        "<td style='padding:0px 3px;'>未连接</td></tr>"
        "<tr><td style='padding:0px 3px; color:palette(text);'>型号:</td>"
        "<td style='padding:0px 3px;'>未连接</td></tr>"
        "<tr><td style='padding:0px 3px; color:palette(text);'>制造商:</td>"
        "<td style='padding:0px 3px;'>未连接</td></tr>"
        "<tr><td style='padding:0px 3px; color:palette(text);'>Android版本:</td>"
        "<td style='padding:0px 3px;'>未连接</td></tr>"
        "<tr><td style='padding:0px 3px; color:palette(text);'>Bootloader:</td>"
        "<td style='padding:0px 3px;'>未连接</td></tr>"
        "<tr><td style='padding:0px 3px; color:palette(text);'>当前模式:</td>"
        "<td style='padding:0px 3px;'>未连接</td></tr>"
        "<tr><td style='padding:0px 3px; color:palette(text);'>Root状态:</td>"
        "<td style='padding:0px 3px;'>未连接</td></tr>"
        "<tr><td style='padding:0px 3px; color:palette(text);'>电池状态:</td>"
        "<td style='padding:0px 3px;'>未连接</td></tr>"
        "</table></body></html>";
}



void DeviceInfoPanel::clearDeviceInfo()
{
    m_info = DeviceInfo();
    m_infoDisplay->setHtml(buildEmptyHtml());
}

QString DeviceInfoPanel::buildInfoHtml(const DeviceInfo &info)
{
    QString html;
    html += "<html><body style='font-size:13px; color:palette(text);'>";
    html += "<table width='100%' style='border-collapse:collapse;'>";

    // 序列号
    html += row("序列号:", info.serialNumber);

    // 型号
    html += row("型号:", info.model);

    // 制造商
    html += row("制造商:", info.manufacturer);

    // Android版本 + 兼容性信息
    {
        QString versionText = info.androidVersion;
        if (!versionText.isEmpty()) {
            if (versionText.contains("17") || versionText.contains("16") ||
                versionText.contains("15") || versionText.contains("14") ||
                versionText.contains("13") || versionText.contains("12") ||
                versionText.contains("11") || versionText.contains("10")) {
                versionText += " (支持Fastbootd)";
            } else if (versionText.contains("9") || versionText.contains("8")) {
                versionText += " (仅传统Fastboot)";
            } else {
                versionText += " (旧版本Android)";
            }
        }
        html += row("Android版本:", versionText);
    }

    // Bootloader
    {
        QString blText = info.bootloaderVersion;
        if (blText.compare(QStringLiteral("unknown"), Qt::CaseInsensitive) == 0)
            blText.clear();

        if (info.isBootloaderStateKnown) {
            const QString state = info.isBootloaderUnlocked
                ? QStringLiteral("已解锁")
                : QStringLiteral("已锁定");
            blText = blText.isEmpty() ? state
                                      : QStringLiteral("%1 (%2)").arg(blText, state);
        } else if (!blText.isEmpty()) {
            blText += QStringLiteral(" (未知)");
        }
        html += row("Bootloader:", blText);
    }

    // 当前模式
    {
        QString modeStr;
        switch (info.mode) {
        case DeviceDetector::MODE_ADB:        modeStr = "ADB模式"; break;
        case DeviceDetector::MODE_FASTBOOT:   modeStr = "Fastboot模式"; break;
        case DeviceDetector::MODE_FASTBOOTD:  modeStr = "Fastbootd模式"; break;
        case DeviceDetector::MODE_EDL_9008:   modeStr = "EDL 9008模式"; break;
        case DeviceDetector::MODE_MTK_DA:     modeStr = "MTK DA模式"; break;
        default:                              modeStr = "未知模式"; break;
        }
        html += row("当前模式:", modeStr);
    }

    // Root状态：adb shell 的 su 与 Root 代理通道分开显示，避免把
    // “代理能提权”误当成“adb shell 能直接 su”。
    {
        QString rootStatus;
        if (info.isRooted) {
            rootStatus = info.rootManager.isEmpty()
                ? QStringLiteral("已Root")
                : QStringLiteral("已Root (%1)").arg(info.rootManager);
        } else if (info.rootAgentRoot) {
            rootStatus = QStringLiteral("已Root（Root 代理通道）");
        } else if (info.rootAgentConnected) {
            rootStatus = QStringLiteral("Root 代理已连接（未提权）");
        } else {
            rootStatus = QStringLiteral("未Root");
        }
        html += row("Root状态:", rootStatus);
    }

    // 电池状态
    html += row("电池状态:", info.batteryHealth);

    // CPU信息
    if (!info.cpuInfo.isEmpty()) {
        html += row("CPU信息:", info.cpuInfo);
    }

    // 存储信息
    if (!info.storageSize.isEmpty()) {
        html += row("存储空间:", info.storageSize);
    }

    // 安全补丁级别
    if (!info.securityPatch.isEmpty()) {
        html += row("安全补丁:", info.securityPatch);
    }

    // SDK版本
    if (!info.sdkVersion.isEmpty()) {
        html += row("SDK版本:", info.sdkVersion);
    }

    html += "</table></body></html>";
    return html;
}

QString DeviceInfoPanel::row(const QString &label, const QString &value) const
{
    QString escapedValue = value.isEmpty() || value == "未知" ? "未知" : escapeHtml(value);
    return QString(
        "<tr><td width='30%%' style='padding:0px 3px; color:palette(text); vertical-align:top;'>%1</td>"
        "<td style='padding:0px 3px;'>%2</td></tr>"
    ).arg(escapeHtml(label), escapedValue);
}
