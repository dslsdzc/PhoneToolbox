#ifndef DEVICE_DETECTOR_H
#define DEVICE_DETECTOR_H

#include <QObject>
#include <QTimer>
#include <QMap>
#include <QString>
#include "device_info.h"

class EDL9008;
class QThread;
struct EDLDeviceInfo;

class DeviceDetector : public QObject
{
    Q_OBJECT

public:
    enum DeviceMode {
        MODE_UNKNOWN = 0,
        MODE_ADB = 1,
        MODE_FASTBOOT = 2,
        MODE_FASTBOOTD = 3,
        MODE_EDL_9008 = 4,
        MODE_MTK_DA = 5,
        MODE_RECOVERY = 6,
        MODE_MTK_BROM = 7,          // MTK BROM 直刷（F1）
        MODE_HUAWEI_USB_UPDATE = 8, // 华为 Kirin USB Update（F2 插件）
        MODE_SPD = 9,               // 展锐 ResearchDownload（F4）
        MODE_SAMSUNG_ODIN = 10,     // 三星 Odin 下载模式（Phase C）
        MODE_SAMSUNG_EUB = 11       // 三星 Exynos EUB（USB-Boot）救援模式
    };

    explicit DeviceDetector(QObject *parent = nullptr);
    ~DeviceDetector();

    void startMonitoring();
    void stopMonitoring();

    // Fastboot相关方法
    bool detectFastbootDevices(QStringList &devices);
    DeviceInfo getFastbootDeviceInfo(const QString &deviceId);
    QString executeFastbootCommand(const QString &command, const QString &deviceId = "");
    QString formatDeviceInfoForDisplay(const DeviceInfo &info) const;
    QString getBootloaderStatusIcon(bool isUnlocked) const;
    QString getModeDisplayName(DeviceMode mode) const;
    // 触发一次即时探测。探测在后台线程执行，本调用立即返回（H2）。
    void forceRefresh();
    QMap<QString, bool> m_fastbootDeviceModes; 

signals:
    void deviceConnected(const DeviceInfo &info);
    // 后台线程采集到电池等动态字段变化后通知 UI；UI 在槽里更新。
    void deviceInfoUpdated(const DeviceInfo &info);
    void deviceDisconnected(const QString &serial);
    void deviceModeChanged(const QString &serial, DeviceMode newMode);

private slots:
    void checkDevices();

private:
    QTimer *m_monitorTimer;
    QThread *m_thread = nullptr;   // 探测工作线程（首次 startMonitoring 时创建）
    bool m_probing = false;        // 工作线程内防轮询堆积
    QMap<QString, DeviceInfo> m_currentDevices;
    QMap<QString, qint64> m_dynamicRefreshAt;
    
    void detectConnectedDevices();
    DeviceMode detectDeviceMode(const QString &deviceId);
    DeviceInfo getDeviceInfo(const QString &deviceId, DeviceMode mode);
    
    // Fastboot特定检测
    QString getBootloaderStatus(const QString &deviceId);
    bool isFastbootdMode(const QString &deviceId);
    QString getFastbootVar(const QString &varName, const QString &deviceId);
    
    // 特定模式检测
    bool detectEDLDevices(QMap<QString, DeviceInfo> &newDevices);
    void detectProtocolDevices(QMap<QString, DeviceInfo> &newDevices); // F5：MTK BROM / 华为 USB Update / 展锐
    bool detectMTKDAMode();
    bool detectADBDevices(QStringList &devices);

    QString formatValue(const QString &value) const;

    EDL9008 *m_edlDetector;
};

#endif // DEVICE_DETECTOR_H
