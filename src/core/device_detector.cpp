#include "device_detector.h"
#include "adb_embedded.h"
#include "device_io_busy.h"
#include "modes/edl_9008.h"
#include "core/odin/odin_libusb_transport.h"
#include "core/eub/samsung_mode.h"
#include "core/modes/mtk_brom.h"   // 协议通道 MTK 白名单判据（与协议层 kUsbIds 同源）
#include "resource_monitor.h"
#include <libusb.h>
#include <QProcess>
#include <QStringList>
#include <QDebug>
#include <QDateTime>
#include <QTimer>
#include <QThread>
#include <QScopeGuard>
#include <QRegularExpression>

namespace {

// `adb shell getprop` 的标准输出：[key]: [value]
bool parsePropDump(const QString &dump, QMap<QString, QString> &props)
{
    static const QRegularExpression re(QStringLiteral("^\\[([^\\]]+)\\]: \\[(.*)\\]$"));
    int hits = 0;
    const QStringList lines = dump.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QRegularExpressionMatch match = re.match(line.trimmed());
        if (!match.hasMatch())
            continue;
        props.insert(match.captured(1), match.captured(2));
        ++hits;
    }
    return hits > 0;
}

// adb 失败时返回 "Error..."；空输出历史上曾被返回成 "Success"。
// 这两类都不是设备数据，不能原样塞进界面（否则会显示成
// "Error: Command timeout 核心" / "Success%" 这种用户看不懂的东西）。
QString cleanCommandOutput(const QString &raw)
{
    const QString text = raw.trimmed();
    if (text.startsWith(QStringLiteral("Error")))
        return QString();
    if (text == QStringLiteral("Success"))
        return QString();
    return text;
}

QString readAdbBatteryLevel(const QString &deviceId)
{
    const QString raw = AdbEmbedded::instance().executeCommand(
        QString("-s %1 shell dumpsys battery | grep level: | head -1").arg(deviceId),
        5000);
    const QString output = cleanCommandOutput(raw);
    if (output.isEmpty())
        return QString();
    const QString value = output.split(QLatin1Char(':')).value(1).trimmed();
    return value.isEmpty() ? QString() : value + QStringLiteral("%");
}

} // namespace

DeviceDetector::DeviceDetector(QObject *parent)
    : QObject(parent)
    , m_monitorTimer(new QTimer(this))
    , m_edlDetector(new EDL9008(this))
{
    // 探测搬到工作线程后，设备信号是跨线程投递的 —— 参数类型必须注册。
    qRegisterMetaType<DeviceInfo>("DeviceInfo");
    qRegisterMetaType<DeviceDetector::DeviceMode>("DeviceDetector::DeviceMode");

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
    if (!m_thread)
        return; // 从未启动：对象与 timer / EDL 探测器都在主线程，正常析构

    if (m_thread->isRunning()) {
        QThread *const ownerThread = QThread::currentThread();
        // 在工作线程里停 timer，并把对象（连同 m_monitorTimer / m_edlDetector）
        // 一起移回调用线程。moveToThread 会移动 children；否则 ~QObject 会在主线程
        // 删除仍属于工作线程的 QTimer/EDL 探测器，Qt 会报
        // "Timers cannot be stopped from another thread"，也是未定义行为。
        QMetaObject::invokeMethod(this, [this, ownerThread]() {
            m_monitorTimer->stop();
            moveToThread(ownerThread);
        }, Qt::BlockingQueuedConnection);
        m_thread->quit();
        m_thread->wait();
    }

    delete m_thread;
    m_thread = nullptr;
}

void DeviceDetector::startMonitoring()
{
    // 确保ADB已初始化
    if (!AdbEmbedded::instance().initialize()) {
        qWarning() << "Failed to initialize ADB for device monitoring";
        return;
    }

    if (!m_thread) {
        m_thread = new QThread();
        m_thread->setObjectName(QStringLiteral("DeviceDetector"));
        // 对象连同 timer / EDL 探测器整体搬进工作线程：探测链上的 adb、fastboot、
        // libusb 调用全是阻塞式的，留在 UI 线程会周期性冻结界面（H2）。
        moveToThread(m_thread);
        m_thread->start();
    }

    QMetaObject::invokeMethod(this, [this]() {
        m_monitorTimer->start();
        checkDevices();
    }, Qt::QueuedConnection);

    qDebug() << "Device monitoring started (worker thread)";
}

void DeviceDetector::stopMonitoring()
{
    qDebug() << "Device monitoring stopped";

    if (!m_thread || !m_thread->isRunning()) {
        m_monitorTimer->stop();
        return;
    }
    if (QThread::currentThread() == m_thread) {
        m_monitorTimer->stop();
        return;
    }
    // 等正在执行的一轮探测结束再停 timer —— 避免探测结果落在已销毁的接收者上。
    QMetaObject::invokeMethod(this, [this]() { m_monitorTimer->stop(); },
                              Qt::BlockingQueuedConnection);
}

void DeviceDetector::forceRefresh()
{
    if (!m_thread || !m_thread->isRunning()) {
        checkDevices();
        return;
    }
    QMetaObject::invokeMethod(this, [this]() { checkDevices(); }, Qt::QueuedConnection);
}

void DeviceDetector::checkDevices()
{
    if (m_probing)
        return; // 上一轮还没跑完：跳过本轮，不堆积
    m_probing = true;
    // 异常也不能把 m_probing 永久卡在 true（那会让设备检测从此停摆）。
    const auto guard = qScopeGuard([this]() { m_probing = false; });
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
                // 设备详情是静态的：只在首次出现或模式变化时采集，轮询只做轻量发现。
                DeviceInfo info;
                const auto cached = m_currentDevices.constFind(deviceId);
                if (cached != m_currentDevices.constEnd() && cached->mode == mode) {
                    info = cached.value();
                } else {
                    info = getFastbootDeviceInfo(deviceId);
                }
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
            // 同 fastboot：缓存命中时不再重跑 13 条 adb 命令；
            // 但电池等会变化的字段每 5 秒轻量刷新一次。
            DeviceInfo info;
            const auto cached = m_currentDevices.constFind(deviceId);
            if (cached != m_currentDevices.constEnd() && cached->mode == mode) {
                info = cached.value();
                const qint64 now = QDateTime::currentMSecsSinceEpoch();
                if (now - m_dynamicRefreshAt.value(deviceId, 0) >= 5000) {
                    m_dynamicRefreshAt[deviceId] = now;
                    const QString oldBattery = info.batteryHealth;
                    const QString battery = readAdbBatteryLevel(deviceId);
                    if (!battery.isEmpty())
                        info.batteryHealth = battery;
                    if (info.batteryHealth != oldBattery)
                        emit deviceInfoUpdated(info);
                }
            } else {
                info = getDeviceInfo(deviceId, mode);
            }
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
    
    // 检查断开的设备。
    // 正在被写操作占用的设备（刷机/擦除/push）不因本轮没枚举到就报断开：
    // 刷机重启、模式切换都会让设备短暂消失，过早发 disconnected 会清空面板。
    // 保留原条目，等写操作结束后的下一轮再判。
    for (auto it = m_currentDevices.begin(); it != m_currentDevices.end(); ++it) {
        if (newDevices.contains(it.key()))
            continue;
        if (deviceio::isBusy(it.key())) {
            newDevices.insert(it.key(), it.value());
            continue;
        }
        emit deviceDisconnected(it.key());
        qDebug() << "Device disconnected:" << it.key();
    }
    
    m_currentDevices = newDevices;
}

bool DeviceDetector::detectADBDevices(QStringList &devices)
{
    // 5s 足够：设备列表只是本地 adb server 查询。旧实现用默认 30s，
    // adb server 卡住时会让关闭窗口/停止监控等上整整半分钟。
    const QString output = AdbEmbedded::instance().executeCommand("devices -l", 5000);
    const QStringList lines = output.split('\n', Qt::SkipEmptyParts);

    for (const QString &line : lines) {
        // 只认第二列恰好是 device 的行。旧实现用 contains("device")，
        // 会把 unauthorized/offline 之外、但描述里带 device 字样的行也吞进来。
        const QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() < 2 || parts.at(1) != QStringLiteral("device"))
            continue;
        const QString serial = parts.at(0);
        if (!serial.isEmpty())
            devices.append(serial);
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
        AdbEmbedded &adb = AdbEmbedded::instance();

        // 一次 getprop dump 拿全部属性（旧实现每条属性一次进程，共 8 条）。
        // dump 解析失败（异常输出/老设备）时回退到逐条查询，行为不回退。
        QMap<QString, QString> propDump;
        const QString dump = adb.executeCommand(
            QStringList() << "-s" << deviceId << "shell" << "getprop", 10000);
        const bool dumpFailed = dump.startsWith(QStringLiteral("Error"));
        const bool haveDump = !dumpFailed && parsePropDump(dump, propDump);
        const auto prop = [&](const char *key) -> QString {
            if (haveDump)
                return propDump.value(QString::fromLatin1(key)).trimmed();
            if (dumpFailed)
                return QString(); // 设备无响应：不再逐条重试（避免 8×超时雪崩）
            // 逐属性回退只给单条 5s：宁可让字段空着，也不要在 UI 线程消失后
            // 还让关闭窗口等上 8×30s。
            return cleanCommandOutput(adb.executeCommand(
                QStringList() << "-s" << deviceId << "shell" << "getprop"
                              << QString::fromLatin1(key), 5000));
        };

        info.manufacturer = prop("ro.product.manufacturer");
        info.model = prop("ro.product.model");
        info.deviceName = prop("ro.product.device");
        info.androidVersion = prop("ro.build.version.release");
        info.buildNumber = prop("ro.build.display.id");

        // 获取Android SDK版本
        info.sdkVersion = prop("ro.build.version.sdk");

        // 获取安全补丁级别
        info.securityPatch = prop("ro.build.version.security_patch");

        // 获取构建指纹
        info.buildFingerprint = prop("ro.build.fingerprint");

        // Bootloader 版本与解锁状态：ADB 模式下原本完全没采集，
        // 详情页只能显示“未知”。ro.boot.verifiedbootstate 等属性不需要 root。
        QString bootloaderVersion = prop("ro.bootloader");
        if (bootloaderVersion.isEmpty()
            || bootloaderVersion.compare(QStringLiteral("unknown"), Qt::CaseInsensitive) == 0) {
            bootloaderVersion = prop("ro.boot.bootloader");
        }
        info.bootloaderVersion = bootloaderVersion;

        const QString verifiedBoot =
            prop("ro.boot.verifiedbootstate").toLower();
        const QString flashLocked = prop("ro.boot.flash.locked");
        const QString vbmetaState =
            prop("ro.boot.vbmeta.device_state").toLower();
        const QString secureBootLock =
            prop("ro.secureboot.lockstate").toLower();

        bool bootloaderKnown = false;
        bool bootloaderUnlocked = false;
        if (verifiedBoot == QStringLiteral("orange")
            || verifiedBoot == QStringLiteral("yellow")) {
            bootloaderKnown = true;
            bootloaderUnlocked = true;
        } else if (verifiedBoot == QStringLiteral("green")) {
            bootloaderKnown = true;
            bootloaderUnlocked = false;
        } else if (flashLocked == QStringLiteral("0")) {
            bootloaderKnown = true;
            bootloaderUnlocked = true;
        } else if (flashLocked == QStringLiteral("1")) {
            bootloaderKnown = true;
            bootloaderUnlocked = false;
        } else if (vbmetaState == QStringLiteral("unlocked")) {
            bootloaderKnown = true;
            bootloaderUnlocked = true;
        } else if (vbmetaState == QStringLiteral("locked")) {
            bootloaderKnown = true;
            bootloaderUnlocked = false;
        } else if (secureBootLock == QStringLiteral("unlocked")) {
            bootloaderKnown = true;
            bootloaderUnlocked = true;
        } else if (secureBootLock == QStringLiteral("locked")) {
            bootloaderKnown = true;
            bootloaderUnlocked = false;
        }
        info.isBootloaderStateKnown = bootloaderKnown;
        info.isBootloaderUnlocked = bootloaderUnlocked;

        // ── Root 检测（三态，别再把"装了 KernelSU 但 Shell 未授权"说成未 Root）──
        // 1) adb root / shell 本身已经是 root
        bool shellRoot = false;
        const QString idOut = adb.executeCommand(
            QStringList() << "-s" << deviceId << "shell" << "id", 3000);
        if (idOut.contains(QStringLiteral("uid=0")))
            shellRoot = true;

        // 2) 实际提权判定：逐个尝试常见 su 路径与语法，只有返回 uid=0 才算 root。
        //    **不拿"装了 KernelSU/Magisk"当判定依据** —— 那是方案痕迹，不是当前权限。
        if (!shellRoot) {
            // 先试最常见的直接形式（也便于假 adb 夹具精确匹配）。
            const QString direct = adb.executeCommand(
                QStringList() << "-s" << deviceId << "shell" << "su" << "-c" << "id",
                2000);
            if (direct.contains(QStringLiteral("uid=0")))
                shellRoot = true;
        }
        if (!shellRoot) {
            const QStringList suAttempts = {
                QStringLiteral("su 0 id"),
                QStringLiteral("su root -c id"),
                QStringLiteral("/system/bin/su -c id"),
                QStringLiteral("/system/xbin/su -c id"),
                QStringLiteral("/sbin/su -c id"),
                QStringLiteral("/debug_ramdisk/su -c id"),
            };
            for (const QString &command : suAttempts) {
                // 2s：su 不存在会立刻返回；等待授权弹窗的场景也不该拖住首连。
                const QString out = adb.executeCommand(
                    QStringList() << "-s" << deviceId << "shell" << "sh" << "-c"
                                  << command, 2000);
                if (out.contains(QStringLiteral("uid=0"))) {
                    shellRoot = true;
                    break;
                }
            }
        }

        // 3) root 方案仅作**展示信息**，不参与 isRooted 判定。
        QStringList rootManagers;
        const QString packages = adb.executeCommand(
            QStringList() << "-s" << deviceId << "shell" << "pm" << "list" << "packages",
            5000);
        if (packages.contains(QStringLiteral("me.weishu.kernelsu")))
            rootManagers << QStringLiteral("KernelSU");
        if (packages.contains(QStringLiteral("com.topjohnwu.magisk")))
            rootManagers << QStringLiteral("Magisk");
        if (packages.contains(QStringLiteral("me.bmax.apatch")))
            rootManagers << QStringLiteral("APatch");
        if (packages.contains(QStringLiteral("eu.chainfire.supersu")))
            rootManagers << QStringLiteral("SuperSU");

        info.isRooted = shellRoot;
        info.rootManager = rootManagers.join(QStringLiteral(" / "));
        
        // 获取网络信息
        info.imei = cleanCommandOutput(adb.executeCommand(
            QString("-s %1 shell service call iphonesubinfo 1 | awk -F \"'\" '{print $2}' | sed '1 d' | tr -d '.' | awk '{print $1}'").arg(deviceId), 5000));
        
        info.wifiMac = prop("ro.boot.wifimacaddr");
        
        // 获取硬件信息
        const QString cores = cleanCommandOutput(adb.executeCommand(
            QString("-s %1 shell cat /proc/cpuinfo | grep -i processor | wc -l").arg(deviceId), 5000));
        if (!cores.isEmpty())
            info.cpuInfo = cores + QStringLiteral(" 核心");

        QString ramInfo = cleanCommandOutput(adb.executeCommand(
            QString("-s %1 shell cat /proc/meminfo | grep MemTotal").arg(deviceId), 5000));
        if (!ramInfo.isEmpty()) {
            const QString ramValue = ramInfo.split(":").value(1).trimmed();
            if (!ramValue.isEmpty())
                info.ramSize = ramValue;
        }
        
        // 获取电池信息
        QString batteryLevel = cleanCommandOutput(adb.executeCommand(
            QString("-s %1 shell dumpsys battery | grep level: | head -1").arg(deviceId), 5000));
        if (!batteryLevel.isEmpty()) {
            const QString levelValue = batteryLevel.split(":").value(1).trimmed();
            if (!levelValue.isEmpty())
                info.batteryHealth = levelValue + QStringLiteral("%");
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
        
        // 检测Bootloader锁状态（三态：已解锁 / 已锁定 / 未知）
        QString bootloaderStatus = getBootloaderStatus(deviceId);
        info.isBootloaderStateKnown =
            (bootloaderStatus == QStringLiteral("已解锁")
             || bootloaderStatus == QStringLiteral("已锁定"));
        info.isBootloaderUnlocked = (bootloaderStatus == QStringLiteral("已解锁"));
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
    
    // 方法4: 通过oem unlocking状态判断。
    // 只认明确的 yes/no：getFastbootVar 现在对"未返回"返回空串，
    // 不能再把任何非空值都当成设备支持（旧实现会误报"不可解锁"）。
    QString oemUnlocking = getFastbootVar("oem unlocking", deviceId);
    if (oemUnlocking == "yes")
        return "可解锁";
    if (oemUnlocking == "no")
        return "不可解锁";

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

    // 未找到有效数据返回空串。调用方用 isEmpty() 判断"设备不支持/未返回"。
    //
    // 旧实现返回"设备不支持此信息"这种非空文案，导致三个用户可见的错误：
    //   1) hw_version 未命中时，hw-version 的 fallback 永远不执行；
    //   2) getBootloaderStatus 把"非空"当成"设备支持"，于是返回"不可解锁"；
    //   3) 产品名/型号直接显示成这句提示。
    return QString();
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
    //   MTK 白名单（BROM + preloader，判据见下）→ MTK BROM；
    //   0x12D1（DBAdapter，任意 PID）→ 华为 USB Update；0x1782（任意 PID）→ 展锐。
    //
    // MTK **不在下面这张表里**：它的判据由协议层单点提供（mtkbrom::isKnownMtkUsbId，与 kUsbIds 同源）。
    // 检测层再抄一份 PID 表就会与协议层漂移 —— 本仓已多次吃亏（漏一个 PID = 设备进不了门）。
    struct ProtoId { int vid; int pid; DeviceMode mode; };
    const ProtoId ids[] = {
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
                // 用 switch 而不是三元：将来给 SamsungMode 添第四态时，-Wswitch 会在**编译期**
                // 点出这里未覆盖，三元则把它静默算成 Odin（用户被引到一条注定失败的刷写链上）。
                DeviceMode claimed;
                switch (sm) {
                case eub::SamsungMode::Eub:        claimed = MODE_SAMSUNG_EUB;  break;
                case eub::SamsungMode::Odin:       claimed = MODE_SAMSUNG_ODIN; break;
                case eub::SamsungMode::NotSamsung: continue;   // 上面的 if 已排除；防御性早退
                }
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

        // 认领一台设备。emit 语义与 detectEDLDevices 一致：新设备 → deviceConnected；
        // 同 ID 模式变化 → deviceModeChanged（与 fastboot/ADB 分支一致）。
        // （上面的三星分支有自己的日志前缀，仍走它自己那段。）
        const auto claim = [&](DeviceMode mode) {
            const QString devId = QStringLiteral("usb-%1-%2")
                                      .arg(libusb_get_bus_number(list[i]))
                                      .arg(libusb_get_device_address(list[i]));
            DeviceInfo info;
            info.serialNumber = devId;
            info.mode = mode;
            info.model = getModeDisplayName(mode); // 与 detectEDLDevices 构造模式一致（列表第二行显示）
            newDevices[devId] = info;

            if (!m_currentDevices.contains(devId)) {
                qDebug() << "Protocol device connected:" << devId;
                emit deviceConnected(info);
            } else if (m_currentDevices[devId].mode != mode) {
                emit deviceModeChanged(devId, mode);
                qDebug() << "Protocol device mode changed:" << devId
                         << "to" << getModeDisplayName(mode);
            }
        };

        // MTK（W6）：BROM 与 preloader **同一模式、同一张白名单** —— 不新增模式（协议层对两者是同一条链：
        // 握手字节完全相同，唯一差别是 isBromPid() 决定要不要预发一个 0xA0）。身份（BROM 还是 preloader）
        // 是**握手之后**由 GET_BL_VER 回值判定的事实，检测层不猜、模式文案也不把它说成 BROM。
        // 与既有行为的关系：原表里那一行（BROM 的 VID:PID）本就是 kUsbIds 的子集，故 BROM 那台照旧命中；
        // preloader PID 由此第一次进门。三星（0x04E8，在上方分支）与华为/展锐（下方表）的判据都不与其交叠。
        if (mtkbrom::isKnownMtkUsbId(desc.idVendor, desc.idProduct)) {
            claim(MODE_MTK_BROM);
            continue;
        }

        for (const ProtoId &id : ids) {
            if (desc.idVendor != id.vid)
                continue;
            if (id.pid != -1 && desc.idProduct != id.pid)
                continue;
            claim(id.mode);
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
    
    // BL锁状态（三态：未知时如实显示，不把默认 false 当成"已锁定"）
    QString lockStatus;
    if (!info.isBootloaderStateKnown)
        lockStatus = QStringLiteral("未知");
    else if (info.isBootloaderUnlocked)
        lockStatus = QStringLiteral("%1 已解锁").arg(getBootloaderStatusIcon(true));
    else
        lockStatus = QStringLiteral("%1 已锁定").arg(getBootloaderStatusIcon(false));
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
        // W6：白名单同时收 BROM 与 preloader（检测层按 VID/PID 认，见 detectProtocolDevices），
        // 而**身份要握手后由 GET_BL_VER 判定** —— 故这里不许只写 "MTK BROM"（那是把 preloader
        // 说成 BROM）。设备列表第二行就是这句（detectProtocolDevices 的 info.model）。
        // src/ui/flash_panel.cpp 有同款 switch 的**本地副本**（仓内约定两侧文案一致）——W6-T1 移交后
        // 已同步（2026-09-20，W2-T5 一并办）；两侧一致性由 tests/test_risk_gate_wiring.cpp 钉住。
        return QStringLiteral("MTK BROM / Preloader");
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
