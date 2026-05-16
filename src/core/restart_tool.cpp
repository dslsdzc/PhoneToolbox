#include "restart_tool.h"
#include "adb_embedded.h"
#include <QDebug>

RestartTool::RestartTool(QObject *parent) : QObject(parent)
{
}

QString RestartTool::restartDevice(const QString &deviceId, DeviceDetector::DeviceMode currentMode, RestartMode targetMode)
{
    emit outputMessage(QString("正在尝试重启设备 %1 到 %2 模式...")
                      .arg(deviceId)
                      .arg(getModeName(targetMode)));
    
    // 检查当前模式和目标模式的兼容性
    if (currentMode == DeviceDetector::MODE_FASTBOOTD && targetMode == MODE_FASTBOOT) {
        emit outputMessage("设备已在Fastbootd模式", false);
        return "Device already in Fastbootd mode";
    }
    
    bool hasRoot = false;
    if (currentMode == DeviceDetector::MODE_ADB) {
        hasRoot = checkRootPermission(deviceId);
    }
    
    QString command = getRestartCommand(deviceId, currentMode, targetMode, hasRoot);
    
    if (command.isEmpty()) {
        emit outputMessage("无法生成重启命令或模式不支持", true);
        return "Error: No command generated or mode not supported";
    }
    
    emit outputMessage(QString("执行命令: %1").arg(command));
    
    QString result;
    if (currentMode == DeviceDetector::MODE_ADB) {
        result = AdbEmbedded::instance().executeCommand(command);
    } else {
        // 对于Fastboot模式，我们需要直接执行fastboot命令
        QProcess process;
        QString fastbootPath = AdbEmbedded::instance().getFastbootPath();
        
        QStringList arguments;
        if (!deviceId.isEmpty()) {
            arguments << "-s" << deviceId;
        }
        arguments << command.split(' ', Qt::SkipEmptyParts);
        
        process.setProgram(fastbootPath);
        process.setArguments(arguments);
        
        emit outputMessage(QString("执行Fastboot命令: %1 %2").arg(fastbootPath).arg(arguments.join(" ")));
        
        process.start();
        if (!process.waitForFinished(10000)) {
            process.kill();
            result = "Error: Command timeout";
        } else {
            result = process.readAllStandardOutput() + process.readAllStandardError();
        }
    }
    
    emit outputMessage(QString("命令结果: %1").arg(result));
    
    if (result.contains("Error") || result.contains("error") || result.contains("failed")) {
        emit outputMessage("重启命令执行失败", true);
        
        // 提供特定错误的建议
        if (result.contains("no permissions")) {
            emit outputMessage("建议: 检查USB调试授权或尝试重新插拔设备", false);
        } else if (result.contains("device not found")) {
            emit outputMessage("建议: 设备可能已断开连接", false);
        } else if (result.contains("command not found")) {
            emit outputMessage("建议: 该设备不支持此重启命令", false);
        }
    } else {
        emit outputMessage("重启命令已发送");
        
        // 根据目标模式提供额外信息
        switch (targetMode) {
        case MODE_FASTBOOT:
            emit outputMessage("设备将重启到Fastbootd模式 (Android 10+ 用户空间Fastboot)");
            break;
        case MODE_BOOTLOADER:
            emit outputMessage("设备将重启到Fastboot模式 (传统引导程序)");
            break;
        case MODE_RECOVERY:
            emit outputMessage("设备将重启到恢复模式");
            break;
        case MODE_EDL:
            emit outputMessage("设备将进入EDL模式，请谨慎操作");
            break;
        }
    }
    
    return result;
}

bool RestartTool::checkRootPermission(const QString &deviceId)
{
    // 检查root权限
    QString result = AdbEmbedded::instance().executeCommand(
        QString("-s %1 shell su -c \"echo root\"").arg(deviceId));
    
    if (result.contains("root")) {
        emit outputMessage("设备具有Root权限");
        return true;
    } else {
        emit outputMessage("设备没有Root权限，尝试普通重启");
        return false;
    }
}

QString RestartTool::getModeName(RestartMode mode) const
{
    switch (mode) {
    case MODE_SYSTEM: return "正常模式";
    case MODE_RECOVERY: return "恢复模式";
    case MODE_BOOTLOADER: return "引导程序";
    case MODE_FASTBOOT: return "Fastboot模式";
    case MODE_EDL: return "EDL模式";
    case MODE_SHUTDOWN: return "关机";
    default: return "未知模式";
    }
}

QString RestartTool::getRestartCommand(const QString &deviceId, DeviceDetector::DeviceMode currentMode, RestartMode targetMode, bool hasRoot)
{
    QString command;
    
    if (currentMode == DeviceDetector::MODE_ADB) {
        // ADB模式下的重启命令
        switch (targetMode) {
        case MODE_SYSTEM:
            command = QString("-s %1 reboot").arg(deviceId);
            break;
        case MODE_RECOVERY:
            if (hasRoot) {
                command = QString("-s %1 shell su -c \"reboot recovery\"").arg(deviceId);
            } else {
                command = QString("-s %1 reboot recovery").arg(deviceId);
            }
            break;
        case MODE_BOOTLOADER:
            if (hasRoot) {
                command = QString("-s %1 shell su -c \"reboot bootloader\"").arg(deviceId);
            } else {
                command = QString("-s %1 reboot bootloader").arg(deviceId);
            }
            break;
        case MODE_FASTBOOT:
            // 从ADB重启到Fastbootd
            if (hasRoot) {
                command = QString("-s %1 shell su -c \"reboot fastboot\"").arg(deviceId);
            } else {
                // 尝试使用 reboot fastboot，如果不支持则使用 reboot bootloader
                command = QString("-s %1 reboot fastboot").arg(deviceId);
            }
            break;
        case MODE_EDL:
            if (hasRoot) {
                command = QString("-s %1 shell su -c \"reboot edl\"").arg(deviceId);
            } else {
                command = QString("-s %1 reboot edl").arg(deviceId);
            }
            break;
        case MODE_SHUTDOWN:
            if (hasRoot) {
                command = QString("-s %1 shell su -c \"reboot -p\"").arg(deviceId);
            } else {
                command = QString("-s %1 shell reboot -p").arg(deviceId);
            }
            break;
        }
    } else if (currentMode == DeviceDetector::MODE_FASTBOOT) {
        // 传统Fastboot模式下的重启命令
        switch (targetMode) {
        case MODE_SYSTEM:
            command = "reboot";
            break;
        case MODE_RECOVERY:
            command = "reboot-recovery";
            break;
        case MODE_BOOTLOADER:
            command = "reboot-bootloader";
            break;
        case MODE_FASTBOOT:
            // 从传统Fastboot重启到Fastbootd
            command = "reboot-fastboot";
            break;
        case MODE_SHUTDOWN:
            command = "shutdown";
            break;
        case MODE_EDL:
            emit outputMessage("传统Fastboot模式不支持直接重启到EDL", true);
            break;
        }
    } else if (currentMode == DeviceDetector::MODE_FASTBOOTD) {
        // Fastbootd模式下的重启命令
        switch (targetMode) {
        case MODE_SYSTEM:
            command = "reboot";
            break;
        case MODE_RECOVERY:
            command = "reboot-recovery";
            break;
        case MODE_BOOTLOADER:
            // 从Fastbootd重启到传统Fastboot
            command = "reboot-bootloader";
            break;
        case MODE_FASTBOOT:
            // 已经在Fastbootd模式
            emit outputMessage("设备已在Fastbootd模式", false);
            break;
        case MODE_SHUTDOWN:
            command = "shutdown";
            break;
        case MODE_EDL:
            emit outputMessage("Fastbootd模式不支持直接重启到EDL", true);
            break;
        }
    }
    
    return command;
}
