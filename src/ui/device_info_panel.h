#ifndef DEVICE_INFO_PANEL_H
#define DEVICE_INFO_PANEL_H

#include <QWidget>
#include <QTextEdit>
#include "core/device_detector.h"

class DeviceInfoPanel : public QWidget
{
    Q_OBJECT

public:
    explicit DeviceInfoPanel(QWidget *parent = nullptr);
    void updateDeviceInfo(const DeviceInfo &info);
    void clearDeviceInfo();

private:
    void setupUI();
    QString buildInfoHtml(const DeviceInfo &info);
    QString buildEmptyHtml();
    QString escapeHtml(const QString &text) const;
    QString row(const QString &label, const QString &value) const;

    QTextEdit *m_infoDisplay;
};

#endif // DEVICE_INFO_PANEL_H
