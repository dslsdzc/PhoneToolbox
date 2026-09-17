#include "device_detector.h"
#include "adb_embedded.h"
#include "modes/edl_9008.h"
#include "core/odin/odin_libusb_transport.h"
#include "core/eub/samsung_mode.h"
#include "resource_monitor.h"
#include <libusb.h>
#include <QProcess>
#include <QStringList>
#include <QDebug>
#include <QTimer>
#include <QThread>
#include <QRegularExpression>

DeviceDetector::DeviceDetector(QObject *parent)
    : QObject(parent)
    , m_monitorTimer(new QTimer(this))
    , m_edlDetector(new EDL9008(this))
{
    m_monitorTimer->setInterval(2000);
    connect(m_monitorTimer, SIGNAL(timeout()), this, SLOT(checkDevices()));

    // H1 降级接入：整体 CPU 使用率 >80% 时设备轮询放慢为 5s（高负载降负），
    // 恢复（<70%）后回到 2s。context 为本对象 → 析构自动断开。
    connect(&ResourceMonitor::instance(), &ResourceMonitor::cpuHigh, this,
            [this](bool high, int) {
                m_monitorTimer->setInterval(high ? 5000 : 2000);
            });
}

DeviceDetector::~DeviceDetector()
{
    stopMonitoring();
}

void DeviceDetector::startMonitoring()
{
    // 确保ADB已初始化
    if (!AdbEmbedded::instance().initialize()) {
        qWarning() << "Failed to initialize ADB for device monitoring";
        return;
    }
    
    m_monitorTimer->start();
    qDebug() << "Device monitoring started";
}

void DeviceDetector::stopMonitoring()
{
    m_monitorTimer->stop();
    qDebug() << "Device monitoring stopped";
}

void DeviceDetector::checkDevices()
{
    detectConnectedDevices();
}

void DeviceDetector::detectConnectedDevices()
{
    QMap<QString, DeviceInfo> newDevices;

    // 检测Fastboot设备 - 添加异常处理
    QStringList fastbootDevices;
    if (detectFastbootDevices(fastbootDevices)) {
        for (const QString &deviceId : fastbootDevices) {
            try {
                DeviceMode mode = m_fastbootDeviceModes[deviceId] ? MODE_FASTBOOTD : MODE_FASTBOOT;
                DeviceInfo info = getFastbootDeviceInfo(deviceId);
                info.mode = mode;
                info.isFastbootdMode = (mode == MODE_FASTBOOTD);
                newDevices[deviceId] = info;
                if (!m_currentDevices.contains(deviceId)) {
                    qDebug() << "Fastboot device connected:" << deviceId;
                    QString formattedInfo = formatDeviceInfoForDisplay(info);
                    qDebug().noquote() << formattedInfo;
                    emit deviceConnected(info);
                } else if (m_currentDevices[deviceId].mode != mode) {
                    emit deviceModeChanged(deviceId, mode);
                    qDebug() << "Fastboot device mode changed:" << deviceId << "to" << mode;
                }
            } catch (const std::exception& e) {
                qWarning() << "Exception while processing Fastboot device" << deviceId << ":" << e.what();
            } catch (...) {
                qWarning() << "Unknown exception while processing Fastboot device" << deviceId;
            }
        }
    }
    
    // 检测EDL设备
    detectEDLDevices(newDevices);

    // 检测协议通道设备（F5）：MTK BROM / 华为 USB Update / 展锐
    detectProtocolDevices(newDevices);

    // 检测ADB设备
    QStringList adbDevices;
    if (detectADBDevices(adbDevices)) {
        for (const QString &deviceId : adbDevices) {
            DeviceMode mode = MODE_ADB;
            DeviceInfo info = getDeviceInfo(deviceId, mode);
            info.mode = mode;
            
            newDevices[deviceId] = info;
            
            if (!m_currentDevices.contains(deviceId)) {
                qDebug() << "ADB device connected:" << deviceId;
                // 输出格式化设备信息
                QString formattedInfo = formatDeviceInfoForDisplay(info);
                qDebug().noquote() << formattedInfo;
                emit deviceConnected(info);
            } else if (m_currentDevices[deviceId].mode != mode) {
                emit deviceModeChanged(deviceId, mode);
                qDebug() << "Device mode changed:" << deviceId << "to" << mode;
            }
        }
    }
    
    // 检查断开的设备
    for (auto it = m_currentDevices.begin(); it != m_currentDevices.end(); ++it) {
        if (!newDevices.contains(it.key())) {
            emit deviceDisconnected(it.key());
            qDebug() << "Device disconnected:" << it.key();
        }
    }
    
    m_currentDevices = newDevices;
}

bool DeviceDetector::detectADBDevices(QStringList &devices)
{
    QString output = AdbEmbedded::instance().executeCommand("devices -l");
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    
    for (const QString &line : lines) {
        if (line.contains("device") && !line.startsWith("List")) {
            QStringList parts = line.split(' ', Qt::SkipEmptyParts);
            if (parts.size() >= 2) {
                QString serial = parts[0];
                if (!serial.isEmpty()) {
                    devices.append(serial);
                }
            }
        }
    }
    
    return !devices.isEmpty();
}

DeviceDetector::DeviceMode DeviceDetector::detectDeviceMode(const QString &deviceId)
{
    // 尝试ADB命令
    QString adbResult = AdbEmbedded::instance().executeCommand(
        QString("-s %1 shell getprop ro.build.version.sdk").arg(deviceId));
    
    if (!adbResult.contains("Error")) {
        return MODE_ADB;
    }
    
    // 检测Fastboot模式
    QStringList fastbootDevices;
    if (detectFastbootDevices(fastbootDevices)) {
        for (const QString &fbDevice : fastbootDevices) {
            if (fbDevice == deviceId) {
                // 进一步检测是传统Fastboot还是Fastbootd
                return isFastbootdMode(deviceId) ? MODE_FASTBOOTD : MODE_FASTBOOT;
            }
        }
    }
    
    // 检测EDL模式 (USB枚举)
    EDLDeviceInfo edlDummy;
    if (m_edlDetector->detectDevice(edlDummy)) {
        return MODE_EDL_9008;
    }
    
    // 检测MTK DA模式
    if (detectMTKDAMode()) {
        return MODE_MTK_DA;
    }
    
    return MODE_UNKNOWN;
}

DeviceInfo DeviceDetector::getDeviceInfo(const QString &deviceId, DeviceMode mode)
{
    DeviceInfo info;
    info.serialNumber = deviceId;
    info.mode = mode;
    
    if (mode == MODE_ADB) {
        // 使用ADB获取详细信息
        info.manufacturer = AdbEmbedded::instance().getDeviceInfo(deviceId, "ro.product.manufacturer");
        info.model = AdbEmbedded::instance().getDeviceInfo(deviceId, "ro.product.model");
        info.deviceName = AdbEmbedded::instance().getDeviceInfo(deviceId, "ro.product.device");
        info.androidVersion = AdbEmbedded::instance().getDeviceInfo(deviceId, "ro.build.version.release");
        info.buildNumber = AdbEmbedded::instance().getDeviceInfo(deviceId, "ro.build.display.id");
        
        // 获取Android SDK版本
        info.sdkVersion = AdbEmbedded::instance().getDeviceInfo(deviceId, "ro.build.version.sdk");

        // 获取安全补丁级别
        info.securityPatch = AdbEmbedded::instance().getDeviceInfo(deviceId, "ro.build.version.security_patch");

        // 获取构建指纹
        info.buildFingerprint = AdbEmbedded::instance().getDeviceInfo(deviceId, "ro.build.fingerprint");
        
        // 检查Root状态（通过su -c id验证实际权限）
        QString rootCheck = AdbEmbedded::instance().executeCommand(
            QString("-s %1 shell su -c id").arg(deviceId));
        info.isRooted = rootCheck.contains("uid=0");
        
        // 获取网络信息
        info.imei = AdbEmbedded::instance().executeCommand(
            QString("-s %1 shell service call iphonesubinfo 1 | awk -F \"'\" '{print $2}' | sed '1 d' | tr -d '.' | awk '{print $1}'").arg(deviceId)).trimmed();
        
        info.wifiMac = AdbEmbedded::instance().getDeviceInfo(deviceId, "ro.boot.wifimacaddr");
        
        // 获取硬件信息
        info.cpuInfo = AdbEmbedded::instance().executeCommand(
            QString("-s %1 shell cat /proc/cpuinfo | grep -i processor | wc -l").arg(deviceId)).trimmed() + " 核心";
        
        QString ramInfo = AdbEmbedded::instance().executeCommand(
            QString("-s %1 shell cat /proc/meminfo | grep MemTotal").arg(deviceId)).trimmed();
        if (!ramInfo.isEmpty()) {
            info.ramSize = ramInfo.split(":").value(1).trimmed();
        }
        
        // 获取电池信息
        QString batteryLevel = AdbEmbedded::instance().executeCommand(
            QString("-s %1 shell dumpsys battery | grep level: | head -1").arg(deviceId)).trimmed();
        if (!batteryLevel.isEmpty()) {
            info.batteryHealth = batteryLevel.split(":").value(1).trimmed() + "%";
        }
        
    } else if (mode == MODE_FASTBOOT || mode == MODE_FASTBOOTD) {
        // 获取Fastboot设备信息
        info = getFastbootDeviceInfo(deviceId);
        info.mode = mode; // 确保模式正确设置
    }
    
    return info;
}

// 新增结构体存储设备ID和模式
struct FastbootDevice {
    QString id;
    bool isFastbootd;
};

bool DeviceDetector::detectFastbootDevices(QStringList &devices)
{
    QProcess fastbootProcess;
    QString fastbootPath = AdbEmbedded::instance().getFastbootPath();
    
    if (fastbootPath.isEmpty()) {
        qDebug() << "Fastboot path is empty, skipping detection";
        return false;
    }
    
    // 执行 `fastboot devices -l` 获取详细信息（包含模式）
    fastbootProcess.setProgram(fastbootPath);
    fastbootProcess.setArguments(QStringList() << "devices" << "-l");
    
    fastbootProcess.start();
    if (!fastbootProcess.waitForFinished(3000)) {
        fastbootProcess.kill();
        return false;
    }
    
    QString output = fastbootProcess.readAllStandardOutput();
    QString error = fastbootProcess.readAllStandardError();
    
    if (!error.isEmpty()) {
        qDebug() << "Fastboot error:" << error;
    }
    
    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    QList<FastbootDevice> fbDevices; // 存储设备ID和模式
    
    for (const QString &line : lines) {
        if (line.isEmpty() || line.startsWith("List of devices")) {
            continue;
        }
        
        // 解析格式：<设备ID>  <状态>  transport_id:<ID> [fastbootd]
        QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            QString serial = parts[0].trimmed();
            if (!serial.isEmpty() && !serial.contains("?")) {
                // 判断是否包含 fastbootd 标记
                bool isFastbootd = parts.contains("fastbootd");
                fbDevices.append({serial, isFastbootd});
                devices.append(serial); // 仍需填充原devices列表（兼容旧逻辑）
            }
        }
    }
    
    // 缓存设备模式（用于后续快速查询）
    m_fastbootDeviceModes.clear();
    for (const auto &device : fbDevices) {
        m_fastbootDeviceModes[device.id] = device.isFastbootd;
    }
    
    return !devices.isEmpty();
}

DeviceInfo DeviceDetector::getFastbootDeviceInfo(const QString &deviceId)
{
    DeviceInfo info;
    info.serialNumber = deviceId;
    info.mode = MODE_FASTBOOT;
    
    // 安全检查
    if (deviceId.isEmpty()) {
        qWarning() << "getFastbootDeviceInfo called with empty deviceId";
        return info;
    }
    
    qDebug() << "Getting Fastboot device info for:" << deviceId;
    
    try {
        // 获取基础设备信息
        info.productName = getFastbootVar("product", deviceId);
        qDebug() << "Product name:" << info.productName;
        
        info.variant = getFastbootVar("variant", deviceId);
        qDebug() << "Variant:" << info.variant;
        
        info.hwVersion = getFastbootVar("hw_version", deviceId);
        if (info.hwVersion.isEmpty()) {
            info.hwVersion = getFastbootVar("hw-version", deviceId);
        }
        qDebug() << "HW version:" << info.hwVersion;
        
        // 获取 Bootloader 版本
        info.bootloaderVersion = getFastbootVar("bootloader-version", deviceId);
        if (info.bootloaderVersion.isEmpty() || info.bootloaderVersion.contains("FAILED")) {
        info.bootloaderVersion = "无法获取";
        }
        qDebug() << "Bootloader version:" << info.bootloaderVersion;
        
        // 检测Bootloader锁状态
        QString bootloaderStatus = getBootloaderStatus(deviceId);
        info.isBootloaderUnlocked = (bootloaderStatus == "已解锁");
        qDebug() << "Bootloader status:" << bootloaderStatus;
        
        // 检测是否为Fastbootd模式
        info.isFastbootdMode = isFastbootdMode(deviceId);
        if (info.isFastbootdMode) {
            info.mode = MODE_FASTBOOTD;
        }
        qDebug() << "Fastbootd mode:" << info.isFastbootdMode;
        
        // 仅在Fastbootd模式下获取电池状态
        if (info.isFastbootdMode) {
            QString batteryStatus = getFastbootVar("battery-status", deviceId);
            if (batteryStatus == "low") {
                info.batteryHealth = "电量低";
            } else if (batteryStatus == "ok") {
                info.batteryHealth = "正常";
            } else if (!batteryStatus.isEmpty() && !batteryStatus.contains("Not found")) {
                info.batteryHealth = batteryStatus;
            } else {
                info.batteryHealth = "未知";
            }
        } else {
            info.batteryHealth = "传统Fastboot模式不支持电池检测";
        }
        qDebug() << "Battery health:" << info.batteryHealth;
        
        // 从产品名推断制造商
        if (!info.productName.isEmpty()) {
            QString productName = info.productName.toLower();
            if (productName.contains("nokia")) {
                info.manufacturer = "Nokia";
            } else if (productName.contains("xiaomi") || productName.contains("redmi")) {
                info.manufacturer = "Xiaomi";
            } else if (productName.contains("oneplus")) {
                info.manufacturer = "OnePlus";
            } else if (productName.contains("samsung")) {
                info.manufacturer = "Samsung";
            } else if (productName.contains("google")) {
                info.manufacturer = "Google";
            } else if (productName.contains("huawei") || productName.contains("honor")) {
                info.manufacturer = "Huawei";
            } else if (productName.contains("oppo")) {
                info.manufacturer = "OPPO";
            } else if (productName.contains("vivo")) {
                info.manufacturer = "vivo";
            } else if (productName.contains("realme")) {
                info.manufacturer = "Realme";
            } else {
                info.manufacturer = "未知";
            }
        } else {
            info.manufacturer = "未知";
        }
        
        info.model = info.productName.isEmpty() ? "未知" : info.productName;
        
        qDebug() << "Final device info - Manufacturer:" << info.manufacturer << "Model:" << info.model;
        
    } catch (const std::exception& e) {
        qWarning() << "Exception in getFastbootDeviceInfo:" << e.what();
    } catch (...) {
        qWarning() << "Unknown exception in getFastbootDeviceInfo";
    }
    
    return info;
}

QString DeviceDetector::getBootloaderStatus(const QString &deviceId)
{
    // 方法1: 检查oem device-info (小米等品牌)
    QString oemInfo = executeFastbootCommand("oem device-info", deviceId);
    if (oemInfo.contains("Device unlocked: true") || oemInfo.contains("unlocked: true")) {
        return "已解锁";
    } else if (oemInfo.contains("Device unlocked: false") || oemInfo.contains("unlocked: false")) {
        return "已锁定";
    }
    
    // 方法2: 检查oem get-bootinfo (华为等品牌)
    QString bootInfo = executeFastbootCommand("oem get-bootinfo", deviceId);
    if (bootInfo.contains("unlocked") || bootInfo.contains("UNLOCKED")) {
        return "已解锁";
    } else if (bootInfo.contains("locked") || bootInfo.contains("LOCKED")) {
        return "已锁定";
    }
    
    // 方法3: 检查getvar unlocked状态
    QString unlockedStatus = getFastbootVar("unlocked", deviceId);
    if (unlockedStatus == "yes") {
        return "已解锁";
    } else if (unlockedStatus == "no") {
        return "已锁定";
    }
    
    // 方法4: 通过oem unlocking状态判断
    QString oemUnlocking = getFastbootVar("oem unlocking", deviceId);
    if (!oemUnlocking.isEmpty()) {
        return oemUnlocking == "yes" ? "可解锁" : "不可解锁";
    }
    
    return "未知";
}

// 改进 isFastbootdMode 检测
bool DeviceDetector::isFastbootdMode(const QString &deviceId)
{
    // 直接从缓存获取，避免重复执行命令
    if (m_fastbootDeviceModes.contains(deviceId)) {
        return m_fastbootDeviceModes[deviceId];
    }
    
    // 兼容处理：若缓存未命中，执行原逻辑作为 fallback
    QString result = executeFastbootCommand("getvar is-userspace", deviceId);
    return result.contains("is-userspace: yes") || result.contains("fastbootd");
}

QString DeviceDetector::getFastbootVar(const QString &varName, const QString &deviceId)
{
    if (varName.isEmpty() || deviceId.isEmpty()) {
        return "";
    }

    // 执行 fastboot 命令获取变量
    QString command = QString("getvar %1").arg(varName);
    QString result = executeFastbootCommand(command, deviceId);

    // 过滤命令结果（关键处理）
    QStringList lines = result.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        // 跳过包含 "Finished" 或 "FAILED" 的状态行
        if (line.contains("Finished") || line.contains("FAILED")) {
            continue;
        }
        // 提取变量值（格式如 "battery-status: 50%"）
        if (line.startsWith(varName + ":")) {
            QString value = line.split(":", Qt::SkipEmptyParts).value(1).trimmed();
            return value;
        }
    }

    // 若未找到有效数据，返回友好提示
    return "设备不支持此信息";
}

QString DeviceDetector::executeFastbootCommand(const QString &command, const QString &deviceId)
{
    if (!AdbEmbedded::instance().initialize()) {
        return "Error: ADB/Fastboot not initialized";
    }

    QProcess process;
    QString fastbootPath = AdbEmbedded::instance().getFastbootPath();
    
    QStringList arguments;
    if (!deviceId.isEmpty()) {
        arguments << "-s" << deviceId;
    }
    
    // 拆分命令参数
    arguments << command.split(' ', Qt::SkipEmptyParts);
    
    process.setProgram(fastbootPath);
    process.setArguments(arguments);
    
    process.start();
    if (!process.waitForFinished(5000)) {
        process.kill();
        return "Error: Fastboot command timed out";
    }
    
    QString output = process.readAllStandardOutput();
    QString error = process.readAllStandardError();
    
    return output + error;
}

bool DeviceDetector::detectEDLDevices(QMap<QString, DeviceInfo> &newDevices)
{
    QList<EDLDeviceInfo> edlDevices = m_edlDetector->listDevices();
    for (const EDLDeviceInfo &edlInfo : edlDevices) {
        QString deviceId = QString("EDL_%1:%2@%3.%4")
            .arg(edlInfo.vid, 4, 16, QChar('0'))
            .arg(edlInfo.pid, 4, 16, QChar('0'))
            .arg(edlInfo.busNumber)
            .arg(edlInfo.deviceAddress);

        DeviceInfo info;
        info.serialNumber = deviceId;
        info.mode = MODE_EDL_9008;
        info.manufacturer = m_edlDetector->describeDevice(edlInfo);
        info.model = "EDL 9008 Mode";

        newDevices[deviceId] = info;

        if (!m_currentDevices.contains(deviceId)) {
            qDebug().noquote() << "EDL device connected:" << m_edlDetector->describeDevice(edlInfo);
            emit deviceConnected(info);
        }
    }
    return !edlDevices.isEmpty();
}

void DeviceDetector::detectProtocolDevices(QMap<QString, DeviceInfo> &newDevices)
{
    // 协议通道设备检测（F5）：libusb VID/PID 快查三个自研协议通道。
    // 设备 ID 用 usb-<bus>-<addr>；模式映射：
    //   0x0E8D:0x0003 → MTK BROM；0x12D1（DBAdapter，任意 PID）→ 华为 USB Update；
    //   0x1782（任意 PID）→ 展锐。
    struct ProtoId { int vid; int pid; DeviceMode mode; };
    const ProtoId ids[] = {
        { 0x0E8D, 0x0003, MODE_MTK_BROM },
        { 0x12D1, -1,     MODE_HUAWEI_USB_UPDATE },
        { 0x1782, -1,     MODE_SPD },
    };
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != LIBUSB_SUCCESS)
        return;
    libusb_set_option(ctx, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    libusb_device **list = nullptr;
    const ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) { libusb_exit(ctx); return; }
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS)
            continue;

        // 三星（Phase C/E）：判据是**纯函数**（odin_libusb_transport.h 的 isOdinDevice）——
        // VID 0x04E8 + 接口类 0x0A(CDC_DATA) + 批量 in/out（Thor/odin4 现行做法），老 PID 兜底。
        // **认领顺序（EUB 先于 Odin）由 eub::samsungModeFor 固化并单测覆盖（core/eub/samsung_mode.h
        // 头注释 / facts §E2）**——本分支只按三态结论分派，不再自己判优先级。
        // 只对三星 VID 深挖描述符：每 2s 一轮的枚举里，其余厂商不必多读一次配置描述符。
        if (desc.idVendor == 0x04E8) {
            QList<quint8> classes;
            // 与 odin_libusb_transport.cpp 的 inspectDevice 同规则（**CDC 路径**上）：
            // 同一接口内 bulk in/out 齐备才算。注意老 PID 兜底（0x6601/0x685D/0x68C3）**不看描述符**、
            // 由纯函数直接命中 —— 那条路径上"检测认领 ⊆ open() 可接受"不成立（需真机才能暴露，实际机型极少）。
            bool hasBulkInOut = false;
            libusb_config_descriptor *cfg = nullptr;
            if (libusb_get_active_config_descriptor(list[i], &cfg) == LIBUSB_SUCCESS && cfg) {
                for (int ic = 0; ic < int(cfg->bNumInterfaces); ++ic) {
                    for (int a = 0; a < int(cfg->interface[ic].num_altsetting); ++a) {
                        const libusb_interface_descriptor *alt = &cfg->interface[ic].altsetting[a];
                        classes << quint8(alt->bInterfaceClass);
                        bool epIn = false, epOut = false;
                        for (int e = 0; e < int(alt->bNumEndpoints); ++e) {
                            const libusb_endpoint_descriptor *ep = &alt->endpoint[e];
                            if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) != LIBUSB_TRANSFER_TYPE_BULK)
                                continue;
                            if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN) epIn = true;
                            else                                                epOut = true;
                        }
                        if (epIn && epOut)
                            hasBulkInOut = true;
                    }
                }
                libusb_free_config_descriptor(cfg);
            }
            const eub::SamsungMode sm =
                eub::samsungModeFor(desc.idVendor, desc.idProduct, classes, hasBulkInOut);
            if (sm != eub::SamsungMode::NotSamsung) {
                // 认领顺序（EUB 先于 Odin）由 samsungModeFor 固化并单测覆盖（samsung_mode.h 头注释）
                const DeviceMode claimed = (sm == eub::SamsungMode::Eub) ? MODE_SAMSUNG_EUB
                                                                        : MODE_SAMSUNG_ODIN;
                const QString devId = QStringLiteral("usb-%1-%2")
                                          .arg(libusb_get_bus_number(list[i]))
                                          .arg(libusb_get_device_address(list[i]));
                DeviceInfo info;
                info.serialNumber = devId;
                info.mode = claimed;
                info.model = getModeDisplayName(claimed);
                newDevices[devId] = info;
                // emit 模式与兄弟分支一致：新设备 → deviceConnected；同 ID 模式变化 → deviceModeChanged
                if (!m_currentDevices.contains(devId)) {
                    qDebug() << "Samsung protocol device connected:" << devId;
                    emit deviceConnected(info);
                } else if (m_currentDevices[devId].mode != claimed) {
                    emit deviceModeChanged(devId, claimed);
                    qDebug() << "Samsung protocol device mode changed:" << devId
                             << "to" << getModeDisplayName(claimed);
                }
                continue;                       // 已认领，不再走下面的 PID 表
            }
        }

        for (const ProtoId &id : ids) {
            if (desc.idVendor != id.vid)
                continue;
            if (id.pid != -1 && desc.idProduct != id.pid)
                continue;
            const QString devId = QStringLiteral("usb-%1-%2")
                                      .arg(libusb_get_bus_number(list[i]))
                                      .arg(libusb_get_device_address(list[i]));
            DeviceInfo info;
            info.serialNumber = devId;
            info.mode = id.mode;
            info.model = getModeDisplayName(id.mode); // 与 detectEDLDevices 构造模式一致（列表第二行显示）
            newDevices[devId] = info;

            // emit 模式与 detectEDLDevices 一致：新设备 → deviceConnected；
            // 同 ID 模式变化 → deviceModeChanged（与 fastboot/ADB 分支一致）
            if (!m_currentDevices.contains(devId)) {
                qDebug() << "Protocol device connected:" << devId;
                emit deviceConnected(info);
            } else if (m_currentDevices[devId].mode != id.mode) {
                emit deviceModeChanged(devId, id.mode);
                qDebug() << "Protocol device mode changed:" << devId
                         << "to" << getModeDisplayName(id.mode);
            }
            break;
        }
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
}

bool DeviceDetector::detectMTKDAMode()
{
    // MTK DA模式检测
    // 需要通过USB设备枚举和特定命令检测
    // 简化实现，返回false
    qDebug() << "MTK DA mode detection not implemented yet";
    return false;
}

// ==================== 设备信息显示改进 ====================

QString DeviceDetector::formatDeviceInfoForDisplay(const DeviceInfo &info) const
{
    QString displayText;
    
    displayText += "设备已连接:\n";
    
    DeviceMode deviceMode = static_cast<DeviceMode>(info.mode);

    if (info.mode == MODE_FASTBOOT || info.mode == MODE_FASTBOOTD) {
        displayText += "Fastboot设备信息:\n";
    } else if (info.mode == MODE_ADB) {
        displayText += "ADB设备信息:\n";
    } else {
        displayText += "未知设备信息:\n";
    }
    
    // 序列号
    displayText += QString("   序列号: %1\n").arg(formatValue(info.serialNumber));
    
    // 产品型号
    displayText += QString("   产品型号: %1\n").arg(formatValue(info.productName));
    
    // 设备变体 - 特殊处理，过滤无关信息
    QString variant = info.variant;
    if (variant.contains("Finished") || variant.contains("Total time")) {
        variant = "未知";
    }
    displayText += QString("   设备变体: %1\n").arg(formatValue(variant));
    
    // 硬件版本
    displayText += QString("   硬件版本: %1\n").arg(formatValue(info.hwVersion));
    
    // Bootloader版本
    displayText += QString("   Bootloader版本: %1\n").arg(formatValue(info.bootloaderVersion));
    
    // BL锁状态
    QString lockStatus = info.isBootloaderUnlocked ? 
        QString("%1 已解锁").arg(getBootloaderStatusIcon(true)) : 
        QString("%1 已锁定").arg(getBootloaderStatusIcon(false));
    displayText += QString("   BL锁状态: %1\n").arg(lockStatus);
    
    // 运行模式
    displayText += QString("   运行模式: %1\n").arg(getModeDisplayName(deviceMode));
    
    // 电池状态
    displayText += QString("   电池状态: %1\n").arg(formatValue(info.batteryHealth));
    
    // 制造商
    displayText += QString("   制造商: %1\n").arg(formatValue(info.manufacturer));
    
    // 对于ADB设备，显示额外信息
    if (info.mode == MODE_ADB) {
        if (!info.model.isEmpty()) {
            displayText += QString("   型号: %1\n").arg(formatValue(info.model));
        }
        if (!info.androidVersion.isEmpty()) {
            displayText += QString("   Android版本: %1\n").arg(formatValue(info.androidVersion));
        }
        if (!info.buildNumber.isEmpty()) {
            displayText += QString("   构建版本: %1\n").arg(formatValue(info.buildNumber));
        }
        if (!info.imei.isEmpty() && info.imei != "未知") {
            displayText += QString("   IMEI: %1\n").arg(formatValue(info.imei));
        }
        if (!info.cpuInfo.isEmpty() && info.cpuInfo != "未知") {
            displayText += QString("   CPU: %1\n").arg(formatValue(info.cpuInfo));
        }
        if (!info.ramSize.isEmpty() && info.ramSize != "未知") {
            displayText += QString("   内存: %1\n").arg(formatValue(info.ramSize));
        }
    }
    
    return displayText;
}

QString DeviceDetector::getBootloaderStatusIcon(bool isUnlocked) const
{
    return isUnlocked ? "已解锁" : "已锁定";
}

QString DeviceDetector::getModeDisplayName(DeviceMode mode) const
{
    switch (mode) {
    case MODE_ADB: return "ADB";
    case MODE_FASTBOOT: return "Fastboot";
    case MODE_FASTBOOTD: return "Fastbootd";
    case MODE_EDL_9008: return "EDL 9008";
    case MODE_MTK_DA: return "MTK DA";
    case MODE_RECOVERY: return "Recovery";
    case MODE_MTK_BROM:
        return QStringLiteral("MTK BROM");
    case MODE_HUAWEI_USB_UPDATE:
        return QStringLiteral("华为 USB Update");
    case MODE_SPD:
        return QStringLiteral("展锐");
    case MODE_SAMSUNG_ODIN:
        return QStringLiteral("三星 (Odin)");
    case MODE_SAMSUNG_EUB:
        return QStringLiteral("三星 EUB (Exynos)");
    default: return "未知";
    }
}

QString DeviceDetector::formatValue(const QString &value) const
{
    if (value.isEmpty() || 
        value.contains("Error") || 
        value.contains("not found") ||
        value.contains("Finished") ||
        value.contains("Total time") ||
        value == "unknown") {
        return "未知";
    }
    return value;
}
