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
    // false = 检测失败或设备不支持该查询。此时不能把 isBootloaderUnlocked
    // 的默认值 false 当成"已锁定"展示给用户。
    bool isBootloaderStateKnown;
    bool isFastbootdMode;
    
    // 系统信息
    bool isRooted;              // 当前 ADB shell **实际**拿到 root（uid=0）
    // Root 代理通道状态：APK 内 agent 以 root 运行，通过 Noise 会话执行命令。
    // 与 isRooted 分开：adb shell 的 su 未授权时，代理通道仍可能可用。
    bool rootAgentConnected;
    bool rootAgentRoot;         // 代理通道实际执行 id 得到 uid=0
    // UI 门控用：adb shell 的 su 或 Root 代理通道任意一个能提权即可。
    bool hasRoot() const { return isRooted || rootAgentRoot; }
    // 检测到的 root 方案（KernelSU / Magisk / APatch ...）。
    // 已安装 ≠ 当前 shell 已授权：UI 要区分"未 Root"和"装了但 Shell 未授权"。
    QString rootManager;
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
