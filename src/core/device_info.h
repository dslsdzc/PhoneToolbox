#ifndef DEVICE_INFO_H
#define DEVICE_INFO_H

#include <QString>
#include <QMap>
#include <QVariant>

// 前向声明
class DeviceDetector;

class DeviceInfo
{
public:
    DeviceInfo();
    
    // 基本信息
    QString serialNumber;
    QString manufacturer;
    QString model;
    QString deviceName;
    QString androidVersion;
    QString buildNumber;
    
    // 网络信息
    QString imei;
    QString meid;
    QString simState;
    QString networkType;
    QString wifiMac;
    QString bluetoothMac;
    
    // 硬件信息
    QString cpuInfo;
    QString ramSize;
    QString storageSize;
    QString screenResolution;
    QString batteryHealth;
    
    // Fastboot特定信息
    QString bootloaderVersion;
    QString basebandVersion;
    QString productName;
    QString variant;
    QString hwVersion;
    bool isBootloaderUnlocked;
    bool isFastbootdMode;
    
    // 系统信息
    bool isRooted;
    QString selinuxStatus;
    QString securityPatch;      // ro.build.version.security_patch, e.g. "2025-05-01"
    QString sdkVersion;         // ro.build.version.sdk, e.g. "34"
    QString buildFingerprint;   // ro.build.fingerprint
    
    int mode;  // 使用int而不是enum，避免包含问题
    
    QMap<QString, QVariant> toMap() const;
    QString toString() const;
    
    // Fastboot信息格式化
    QString toFastbootString() const;
};

#endif // DEVICE_INFO_H
