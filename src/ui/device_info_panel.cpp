#include "device_info_panel.h"
#include <QVBoxLayout>
#include <QGroupBox>
#include <QTextEdit>
#include <QFrame>

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

void DeviceInfoPanel::updateDeviceInfo(const DeviceInfo &info)
{
    m_infoDisplay->setHtml(buildInfoHtml(info));
}

void DeviceInfoPanel::clearDeviceInfo()
{
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
        if (!blText.isEmpty() && blText != "未知") {
            blText += info.isBootloaderUnlocked ? " (已解锁)" : " (已锁定)";
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

    // Root状态
    {
        QString rootStatus;
        if (info.isRooted) {
            rootStatus = "已Root";
        } else {
            QString av = info.androidVersion;
            if (av.contains("10") || av.contains("11") ||
                av.contains("12") || av.contains("13")) {
                rootStatus = "未Root (Android 10+ 建议使用Magisk)";
            } else {
                rootStatus = "未Root (可考虑使用SuperSU或Magisk)";
            }
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
