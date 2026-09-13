#include "flash_tool.h"
#include "adb_embedded.h"
#include "core/modes/spd_storage.h"
// Phase B Task 8：oppo-edl 通道 —— 计划层（目录 → FlashPlan）+ 会话编排 + 真机传输整链
#include "core/edl/edl_libusb_transport.h"
#include "core/edl/edl_session.h"
#include "core/edl/flash_plan.h"
#include "src/plugins/plugin_manager.h"
#include <libusb.h>
#include <QProcess>
#include <QFileInfo>
#include <QFile>
#include <QRegularExpression>
#include <QDir>
#include <QCoreApplication>
#include <QThread>
#include <QPair>

#ifdef Q_OS_WIN
static const char *kPlatformScript = "flash-all.bat";
static const char *kOtherScript = "flash-all.sh";
#else
static const char *kPlatformScript = "flash-all.sh";
static const char *kOtherScript = "flash-all.bat";
#endif

FlashTool::FlashTool(QObject *parent)
    : QObject(parent)
    , m_edlHandler(this)
    , m_mtkHandler(nullptr)
{
    connect(&m_edlHandler, &EDLHandler::outputMessage,
            this, &FlashTool::outputMessage);
    connect(&m_edlHandler, &EDLHandler::progress,
            this, &FlashTool::flashProgress);
}

QString FlashTool::executeFastboot(const QStringList &args, int timeoutMs)
{
    if (!AdbEmbedded::instance().initialize()) {
        return "Error: ADB/Fastboot not initialized";
    }

    QString fastbootPath = AdbEmbedded::instance().getFastbootPath();
    if (fastbootPath.isEmpty()) {
        return "Error: Fastboot path is empty";
    }

    QProcess process;
    process.setProgram(fastbootPath);
    process.setArguments(args);

    process.start();
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        return "Error: Command timeout";
    }

    return process.readAllStandardOutput() + process.readAllStandardError();
}

QString FlashTool::executeFastbootWithDevice(const QString &deviceId,
                                              const QStringList &extraArgs,
                                              int timeoutMs)
{
    QStringList args;
    if (!deviceId.isEmpty()) {
        args << "-s" << deviceId;
    }
    args << extraArgs;
    return executeFastboot(args, timeoutMs);
}

QStringList FlashTool::listPartitions(const QString &deviceId)
{
    QStringList partitions;
    QString output = executeFastbootWithDevice(deviceId, {"getvar", "all"}, 10000);

    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        // 格式: (bootloader) partition-type:<name>: <type>
        QRegularExpression re(R"(partition-type:(\w+):)");
        auto m = re.match(line);
        if (m.hasMatch()) {
            QString name = m.captured(1);
            if (!partitions.contains(name)) {
                partitions.append(name);
            }
        }
    }

    return partitions;
}

QList<FlashTool::PartitionInfo> FlashTool::getPartitionDetails(const QString &deviceId)
{
    QMap<QString, PartitionInfo> partMap;
    QString output = executeFastbootWithDevice(deviceId, {"getvar", "all"}, 10000);

    QStringList lines = output.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        // partition-type:<name>: <type>
        {
            QRegularExpression re(R"(partition-type:(\w+):\s*(.+))");
            auto m = re.match(line);
            if (m.hasMatch()) {
                QString name = m.captured(1);
                partMap[name].name = name;
                partMap[name].type = m.captured(2).trimmed();
                continue;
            }
        }
        // partition-size:<name>: 0x<hex>
        {
            QRegularExpression re(R"(partition-size:(\w+):\s*0x([0-9a-fA-F]+))");
            auto m = re.match(line);
            if (m.hasMatch()) {
                QString name = m.captured(1);
                bool ok;
                qint64 bytes = m.captured(2).toLongLong(&ok, 16);
                partMap[name].name = name;
                if (ok) {
                    if (bytes >= 1024 * 1024 * 1024)
                        partMap[name].size = QString("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 1);
                    else if (bytes >= 1024 * 1024)
                        partMap[name].size = QString("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', 0);
                    else if (bytes >= 1024)
                        partMap[name].size = QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
                    else
                        partMap[name].size = QString("%1 B").arg(bytes);
                }
                continue;
            }
        }
    }

    return partMap.values();
}

bool FlashTool::flashPartition(const QString &deviceId, const QString &partition,
                               const QString &imagePath)
{
    QFileInfo fi(imagePath);
    if (!fi.exists()) {
        emit outputMessage(QString("文件不存在: %1").arg(imagePath), true);
        return false;
    }

    emit outputMessage(QString("开始刷入 %1 到 %2...").arg(fi.fileName(), partition), false);
    emit flashProgress(10);

    QString result = executeFastbootWithDevice(deviceId, {"flash", partition, imagePath}, 60000);

    emit flashProgress(90);

    if (result.contains("FAILED") || result.contains("failed")) {
        emit outputMessage(QString("刷入失败: %1").arg(result), true);
        emit flashProgress(0);
        return false;
    }

    emit outputMessage(QString("刷入完成: %1 -> %2").arg(partition, fi.fileName()), false);
    emit flashProgress(100);
    return true;
}

bool FlashTool::erasePartition(const QString &deviceId, const QString &partition)
{
    emit outputMessage(QString("正在擦除 %1...").arg(partition), false);
    emit flashProgress(10);

    QString result = executeFastbootWithDevice(deviceId, {"erase", partition}, 30000);

    emit flashProgress(90);

    if (result.contains("FAILED") || result.contains("failed")) {
        emit outputMessage(QString("擦除失败: %1").arg(result), true);
        emit flashProgress(0);
        return false;
    }

    emit outputMessage(QString("擦除完成: %1").arg(partition), false);
    emit flashProgress(100);
    return true;
}

bool FlashTool::formatPartition(const QString &deviceId, const QString &partition)
{
    emit outputMessage(QString("正在格式化分区 %1...").arg(partition), false);
    emit flashProgress(10);

    QString result = executeFastbootWithDevice(deviceId, {"format", partition}, 60000);

    emit flashProgress(90);

    if (result.contains("FAILED") || result.contains("failed")) {
        emit outputMessage(QString("格式化失败: %1").arg(result), true);
        emit flashProgress(0);
        return false;
    }

    emit outputMessage(QString("分区格式化完成: %1").arg(partition), false);
    emit flashProgress(100);
    return true;
}

bool FlashTool::flashUpdate(const QString &deviceId, const QString &zipPath)
{
    QFileInfo fi(zipPath);
    if (!fi.exists()) {
        emit outputMessage(QString("文件不存在: %1").arg(zipPath), true);
        return false;
    }

    emit outputMessage(QString("开始更新设备: %1").arg(fi.fileName()), false);
    emit flashProgress(5);

    QString result = executeFastbootWithDevice(deviceId, {"update", zipPath}, 120000);

    if (result.contains("FAILED") || result.contains("failed")) {
        emit outputMessage(QString("更新失败: %1").arg(result), true);
        emit flashProgress(0);
        return false;
    }

    emit outputMessage("设备更新完成", false);
    emit flashProgress(100);
    return true;
}

bool FlashTool::adbSideload(const QString &deviceId, const QString &zipPath)
{
    QFileInfo fi(zipPath);
    if (!fi.exists()) {
        emit outputMessage(QString("文件不存在: %1").arg(zipPath), true);
        return false;
    }

    emit outputMessage("正在通过 ADB Sideload 刷入...", false);
    emit flashProgress(10);

    if (!AdbEmbedded::instance().initialize()) {
        emit outputMessage("ADB 初始化失败", true);
        return false;
    }

    QProcess process;
    process.setProgram(AdbEmbedded::instance().getAdbPath());
    QStringList args;
    if (!deviceId.isEmpty()) {
        args << "-s" << deviceId;
    }
    args << "sideload" << zipPath;
    process.setArguments(args);

    emit outputMessage(QString("执行: adb %1").arg(args.join(" ")), false);

    process.start();
    if (!process.waitForFinished(120000)) {
        process.kill();
        emit outputMessage("Sideload 超时", true);
        emit flashProgress(0);
        return false;
    }

    QString result = process.readAllStandardOutput() + process.readAllStandardError();
    emit outputMessage(result, false);
    emit flashProgress(100);
    return true;
}

bool FlashTool::runFlashScript(const QString &scriptPath, const QString &deviceId)
{
    QFileInfo fi(scriptPath);
    if (!fi.exists()) {
        emit outputMessage(QString("脚本不存在: %1").arg(scriptPath), true);
        return false;
    }

    QString scriptName = fi.fileName().toLower();
    bool isWindows = false;
#ifdef Q_OS_WIN
    isWindows = true;
#endif

    // 平台检查：当前 OS 与脚本类型是否匹配
    if (isWindows && scriptName.endsWith(".sh")) {
        emit outputMessage("检测到 .sh 脚本，当前为 Windows 平台。建议使用 flash-all.bat", true);
    } else if (!isWindows && scriptName.endsWith(".bat")) {
        emit outputMessage("检测到 .bat 脚本，当前为非 Windows 平台。建议使用 flash-all.sh", true);
    }

    emit outputMessage(QString("运行刷机脚本: %1").arg(fi.filePath()), false);
    emit flashProgress(5);

    QProcess process;
    QStringList args;

#ifdef Q_OS_WIN
    args << "/c" << fi.filePath();
    process.setProgram("cmd");
#else
    // 确保脚本可执行
    QFile::setPermissions(scriptPath,
        fi.permissions() | QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther);
    args << fi.filePath();
    process.setProgram("/bin/sh");
#endif

    // 传入设备ID作为环境变量
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!deviceId.isEmpty()) {
        env.insert("ANDROID_SERIAL", deviceId);
    }
    process.setProcessEnvironment(env);
    process.setArguments(args);

    process.start();
    if (!process.waitForFinished(180000)) { // 3分钟超时
        process.kill();
        emit outputMessage("刷机脚本执行超时", true);
        emit flashProgress(0);
        return false;
    }

    QString output = process.readAllStandardOutput();
    QString error = process.readAllStandardError();

    if (!output.isEmpty())
        emit outputMessage(output, false);
    if (!error.isEmpty())
        emit outputMessage(error, true);

    int exitCode = process.exitCode();
    if (exitCode != 0) {
        emit outputMessage(QString("脚本执行失败 (退出码: %1)").arg(exitCode), true);
        emit flashProgress(0);
        return false;
    }

    emit outputMessage("刷机脚本执行完成", false);
    emit flashProgress(100);
    return true;
}

QString FlashTool::executeAdb(const QString &deviceId, const QStringList &args,
                               int timeoutMs)
{
    if (!AdbEmbedded::instance().initialize()) {
        return "Error: ADB not initialized";
    }

    QString adbPath = AdbEmbedded::instance().getAdbPath();
    if (adbPath.isEmpty()) {
        return "Error: ADB path is empty";
    }

    QProcess process;
    process.setProgram(adbPath);
    QStringList fullArgs;
    if (!deviceId.isEmpty()) {
        fullArgs << "-s" << deviceId;
    }
    fullArgs << args;
    process.setArguments(fullArgs);

    emit outputMessage(QString("执行: adb %1").arg(fullArgs.join(" ")), false);

    process.start();
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        return "Error: Command timeout";
    }

    QString output = process.readAllStandardOutput();
    QString error = process.readAllStandardError();
    if (process.exitCode() != 0) {
        return "Error: " + error;
    }
    return output;
}

bool FlashTool::dumpPartition(const QString &deviceId, int deviceMode,
                               const QString &partition, const QString &outputPath)
{
    Q_UNUSED(deviceMode)
    emit outputMessage(QString("开始读取分区: %1").arg(partition), false);
    emit flashProgress(10);

    QString devPath = QString("/dev/block/bootdevice/by-name/%1").arg(partition);
    QString altDevPath = QString("/dev/block/by-name/%1").arg(partition);
    QString tempFile = QString("/sdcard/%1_dump.img").arg(partition);
    QString ddResult;

    // 根据模式选择策略
    int adbMode = 1; // DeviceDetector::MODE_ADB
    int fastbootMode = 2; // DeviceDetector::MODE_FASTBOOT
    int fastbootdMode = 3; // DeviceDetector::MODE_FASTBOOTD
    int recoveryMode = 6; // DeviceDetector::MODE_RECOVERY

    if (deviceMode == fastbootMode || deviceMode == fastbootdMode) {
        // Fastboot 模式: 没有 ADB，无法直接读取
        emit outputMessage("Fastboot 模式下无 ADB，无法直接读取分区", true);
        emit outputMessage("请先使用「启动临时镜像」boot 一个 Recovery 镜像", false);
        emit flashProgress(0);
        return false;
    }

    if (deviceMode == adbMode) {
        // ADB 模式: 需要 root
        emit outputMessage("ADB 模式，检测 root 权限...", false);
        QString rootCheck = executeAdb(deviceId, {"shell", "su", "-c", "id"}, 5000);
        if (rootCheck.contains("uid=0")) {
            emit outputMessage("已获取 root 权限", false);
            ddResult = executeAdb(deviceId, {
                "shell", "su", "-c",
                QString("dd if=%1 of=%2 bs=1048576").arg(devPath, tempFile)
            }, 120000);
        } else {
            emit outputMessage("无 root 权限，尝试直接读取...", false);
            ddResult = executeAdb(deviceId, {
                "shell", "dd", QString("if=%1").arg(devPath),
                QString("of=%1").arg(tempFile), "bs=1048576"
            }, 120000);
        }
    } else if (deviceMode == recoveryMode) {
        // Recovery 模式: block 设备通常可读
        emit outputMessage("Recovery 模式，直接读取分区...", false);
        ddResult = executeAdb(deviceId, {
            "shell", "dd", QString("if=%1").arg(devPath),
            QString("of=%1").arg(tempFile), "bs=1048576"
        }, 120000);
    } else {
        // 其他模式 (ADB 可用的)
        emit outputMessage("尝试读取分区...", false);
        ddResult = executeAdb(deviceId, {
            "shell", "dd", QString("if=%1").arg(devPath),
            QString("of=%1").arg(tempFile), "bs=1048576"
        }, 120000);
    }

    if (ddResult.startsWith("Error")) {
        // 尝试备选路径
        emit outputMessage("尝试备选分区路径...", false);
        ddResult = executeAdb(deviceId, {
            "shell", "dd", QString("if=%1").arg(altDevPath),
            QString("of=%1").arg(tempFile), "bs=1048576"
        }, 120000);
    }

    if (ddResult.startsWith("Error")) {
        emit outputMessage(QString("读取分区失败: %1").arg(ddResult), true);
        emit flashProgress(0);
        return false;
    }
    emit flashProgress(50);

    // Step 2: pull file
    QString pullResult = executeAdb(deviceId, {"pull", tempFile, outputPath}, 120000);
    if (pullResult.startsWith("Error")) {
        emit outputMessage(QString("拉取镜像失败: %1").arg(pullResult), true);
        emit flashProgress(0);
        return false;
    }
    emit flashProgress(80);

    // Step 3: cleanup temp file on device
    executeAdb(deviceId, {"shell", "su", "-c", QString("rm %1").arg(tempFile)}, 5000);

    emit outputMessage(QString("分区已保存到: %1").arg(outputPath), false);
    emit flashProgress(100);
    return true;
}

bool FlashTool::fastbootDumpMem(const QString &deviceId, const QString &address,
                                 const QString &size, const QString &outputPath)
{
    if (!AdbEmbedded::instance().initialize()) {
        emit outputMessage("ADB/Fastboot 未初始化", true);
        return false;
    }

    QString fastbootPath = AdbEmbedded::instance().getFastbootPath();
    if (fastbootPath.isEmpty()) {
        emit outputMessage("Fastboot 路径为空", true);
        return false;
    }

    emit outputMessage(QString("内存转储: addr=%1 size=%2").arg(address, size), false);
    emit outputMessage("这可能需要较长时间，请耐心等待...", false);
    emit flashProgress(10);

    QStringList args;
    if (!deviceId.isEmpty()) {
        args << "-s" << deviceId;
    }
    args << "oem" << "dump-mem" << address << size;

    QProcess process;
    process.setProgram(fastbootPath);
    process.setArguments(args);
    process.start();

    if (!process.waitForFinished(300000)) { // 5分钟超时
        process.kill();
        emit outputMessage("内存转储超时", true);
        emit flashProgress(0);
        return false;
    }

    emit flashProgress(60);

    QByteArray rawData = process.readAllStandardOutput();
    QString errOutput = process.readAllStandardError();

    if (process.exitCode() != 0) {
        // 有些设备输出在 stderr 但命令成功
        if (rawData.isEmpty() && !errOutput.contains("FAILED")) {
            rawData = errOutput.toUtf8();
        }
        if (rawData.isEmpty()) {
            emit outputMessage(QString("转储失败: %1").arg(errOutput), true);
            emit flashProgress(0);
            return false;
        }
    }

    // 保存到文件
    QFile outFile(outputPath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        emit outputMessage("无法创建输出文件", true);
        emit flashProgress(0);
        return false;
    }

    // 去除 fastboot 的文本前缀 (bootloader)
    QByteArray cleanData;
    QList<QByteArray> lines = rawData.split('\n');
    for (const QByteArray &line : lines) {
        int idx = line.indexOf(") ");
        if (idx >= 0 && line.indexOf("(bootloader)") >= 0) {
            cleanData.append(line.mid(idx + 2));
        } else {
            cleanData.append(line);
        }
        cleanData.append('\n');
    }

    qint64 written = outFile.write(cleanData);
    outFile.close();

    if (written <= 0) {
        emit outputMessage("写入文件失败", true);
        emit flashProgress(0);
        return false;
    }

    emit outputMessage(QString("内存转储已保存: %1 (%2 bytes)")
                       .arg(outputPath).arg(written), false);
    emit flashProgress(100);
    return true;
}

bool FlashTool::fastbootBoot(const QString &deviceId, const QString &imagePath)
{
    QFileInfo fi(imagePath);
    if (!fi.exists()) {
        emit outputMessage(QString("镜像不存在: %1").arg(imagePath), true);
        return false;
    }

    emit outputMessage(QString("启动临时镜像: %1").arg(fi.fileName()), false);
    emit flashProgress(10);

    QString result = executeFastbootWithDevice(deviceId, {"boot", imagePath}, 60000);

    if (result.contains("FAILED") || result.contains("failed")) {
        emit outputMessage(QString("启动失败: %1").arg(result), true);
        emit flashProgress(0);
        return false;
    }

    emit outputMessage("设备已从临时镜像启动", false);
    emit flashProgress(100);
    return true;
}

QStringList FlashTool::listPartitionsAdb(const QString &deviceId)
{
    QStringList partitions;

    // 方式1: ls /dev/block/by-name/
    QString result = executeAdb(deviceId, {"shell", "ls", "/dev/block/by-name/"}, 10000);
    if (!result.startsWith("Error")) {
        QStringList lines = result.split('\n', Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            QString name = line.trimmed();
            if (!name.isEmpty() && !name.contains("/")) {
                partitions.append(name);
            }
        }
    }

    // 如果上面没结果，尝试 /dev/block/bootdevice/by-name/
    if (partitions.isEmpty()) {
        result = executeAdb(deviceId, {"shell", "ls", "/dev/block/bootdevice/by-name/"}, 10000);
        if (!result.startsWith("Error")) {
            QStringList lines = result.split('\n', Qt::SkipEmptyParts);
            for (const QString &line : lines) {
                QString name = line.trimmed();
                if (!name.isEmpty() && !name.contains("/")) {
                    partitions.append(name);
                }
            }
        }
    }

    return partitions;
}

QList<FlashTool::PartitionInfo> FlashTool::getPartitionDetailsAdb(const QString &deviceId)
{
    QList<PartitionInfo> result;

    // 读取 /proc/partitions 获取大小
    QString procResult = executeAdb(deviceId, {"shell", "cat", "/proc/partitions"}, 10000);
    if (procResult.startsWith("Error")) {
        // 降级为只用分区名
        QStringList names = listPartitionsAdb(deviceId);
        for (const QString &name : names) {
            PartitionInfo pi;
            pi.name = name;
            result.append(pi);
        }
        return result;
    }

    // 解析 /proc/partitions: major minor blocks name
    QMap<QString, qint64> partSizes;
    QStringList lines = procResult.split('\n', Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        QStringList fields = line.split(' ', Qt::SkipEmptyParts);
        if (fields.size() >= 4) {
            bool ok;
            qint64 blocks = fields[2].toLongLong(&ok);
            QString name = fields[3].trimmed();
            if (ok && blocks > 0 && !name.isEmpty()) {
                partSizes[name] = blocks * 512; // blocks are 512-byte sectors
            }
        }
    }

    // 获取分区名列表
    QStringList names = listPartitionsAdb(deviceId);
    for (const QString &name : names) {
        PartitionInfo pi;
        pi.name = name;
        if (partSizes.contains(name)) {
            qint64 bytes = partSizes[name];
            if (bytes >= 1024 * 1024 * 1024)
                pi.size = QString("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 1);
            else if (bytes >= 1024 * 1024)
                pi.size = QString("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', 0);
            else if (bytes >= 1024)
                pi.size = QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 0);
            else
                pi.size = QString("%1 B").arg(bytes);
        }
        result.append(pi);
    }

    return result;
}

bool FlashTool::adbFlashPartition(const QString &deviceId, const QString &partition,
                                   const QString &imagePath)
{
    QFileInfo fi(imagePath);
    if (!fi.exists()) {
        emit outputMessage(QString("文件不存在: %1").arg(imagePath), true);
        return false;
    }

    emit outputMessage(QString("ADB 刷入 %1 到 %2...").arg(fi.fileName(), partition), false);
    emit flashProgress(10);

    // Step 1: push image to device
    QString remotePath = QString("/sdcard/%1_flash.img").arg(partition);
    QString pushResult = executeAdb(deviceId, {"push", imagePath, remotePath}, 60000);
    if (pushResult.startsWith("Error")) {
        emit outputMessage(QString("推送镜像失败: %1").arg(pushResult), true);
        emit flashProgress(0);
        return false;
    }
    emit flashProgress(40);

    // Step 2: dd write
    QString devPath = QString("/dev/block/bootdevice/by-name/%1").arg(partition);
    QString ddResult = executeAdb(deviceId, {
        "shell", "su", "-c",
        QString("dd if=%1 of=%2 bs=1048576").arg(remotePath, devPath)
    }, 120000);

    if (ddResult.startsWith("Error")) {
        emit outputMessage(QString("刷入失败: %1").arg(ddResult), true);
        emit flashProgress(0);
        return false;
    }
    emit flashProgress(80);

    // Step 3: sync & cleanup
    executeAdb(deviceId, {"shell", "su", "-c", "sync"}, 10000);
    executeAdb(deviceId, {"shell", "su", "-c", QString("rm %1").arg(remotePath)}, 5000);

    emit outputMessage(QString("ADB 刷入完成: %1 -> %2").arg(partition, fi.fileName()), false);
    emit flashProgress(100);
    return true;
}

int FlashTool::checkBootloaderStatus(const QString &deviceId)
{
    QString result = executeFastbootWithDevice(deviceId, {"getvar", "unlocked"}, 5000);
    if (result.contains("unlocked: yes"))
        return 1;
    if (result.contains("unlocked: no"))
        return 0;

    // 尝试 fastboot oem device-info
    result = executeFastbootWithDevice(deviceId, {"oem", "device-info"}, 5000);
    if (result.contains("Device unlocked: true"))
        return 1;
    if (result.contains("Device unlocked: false"))
        return 0;

    // 无法确定
    emit outputMessage("无法检测 Bootloader 解锁状态", true);
    return -1;
}

// ==================== Bootloader 解锁/回锁 ====================

bool FlashTool::unlockBootloader(const QString &deviceId)
{
    emit outputMessage("正在尝试解锁 Bootloader...", false);
    emit flashProgress(10);

    // 策略: 按兼容性从高到低尝试
    struct { QStringList args; QString desc; } attempts[] = {
        { {"flashing", "unlock"}, "fastboot flashing unlock" },
        { {"oem", "unlock"},      "fastboot oem unlock" },
        { {"oem", "unlock-go"},   "fastboot oem unlock-go" },
    };

    for (const auto &attempt : attempts) {
        emit outputMessage(QString("尝试: %1").arg(attempt.desc), false);
        QString result = executeFastbootWithDevice(deviceId, attempt.args, 15000);

        if (!result.contains("FAILED") && !result.contains("failed")) {
            emit outputMessage(attempt.desc + " 执行成功", false);
            emit flashProgress(80);

            // 等待几秒让设备状态更新
            QThread::msleep(2000);

            // 验证解锁是否生效
            int status = checkBootloaderStatus(deviceId);
            if (status == 1) {
                emit outputMessage("Bootloader 已成功解锁！", false);
                emit flashProgress(100);
                return true;
            }

            emit outputMessage("命令已执行，但解锁状态未确认", true);
            emit flashProgress(100);
            return true;
        }

        emit outputMessage(QString("%1 失败: %2").arg(attempt.desc, result.trimmed()), true);
    }

    emit outputMessage("所有解锁尝试均失败。设备可能不支持解锁或已被厂商封锁。", true);
    emit flashProgress(0);
    return false;
}

bool FlashTool::lockBootloader(const QString &deviceId)
{
    emit outputMessage("正在尝试回锁 Bootloader...", false);
    emit flashProgress(10);

    struct { QStringList args; QString desc; } attempts[] = {
        { {"flashing", "lock"}, "fastboot flashing lock" },
        { {"oem", "lock"},      "fastboot oem lock" },
    };

    for (const auto &attempt : attempts) {
        emit outputMessage(QString("尝试: %1").arg(attempt.desc), false);
        QString result = executeFastbootWithDevice(deviceId, attempt.args, 15000);

        if (!result.contains("FAILED") && !result.contains("failed")) {
            emit outputMessage(attempt.desc + " 执行成功", false);
            emit flashProgress(80);

            QThread::msleep(2000);

            int status = checkBootloaderStatus(deviceId);
            if (status == 0) {
                emit outputMessage("Bootloader 已成功回锁！", false);
                emit flashProgress(100);
                return true;
            }

            emit outputMessage("命令已执行，但回锁状态未确认", true);
            emit flashProgress(100);
            return true;
        }

        emit outputMessage(QString("%1 失败: %2").arg(attempt.desc, result.trimmed()), true);
    }

    emit outputMessage("所有回锁尝试均失败。", true);
    emit flashProgress(0);
    return false;
}

// ==================== FRP 清除 ====================

bool FlashTool::eraseFRP(const QString &deviceId, int deviceMode)
{
    bool fastbootMode = (deviceMode == 2 || deviceMode == 3); // Fastboot / Fastbootd
    bool adbMode = (deviceMode == 1);   // ADB (需要 Root)
    bool edlMode = (deviceMode == 4);
    bool mtkMode = (deviceMode == 5);

    if (!fastbootMode && !adbMode && !edlMode && !mtkMode) {
        emit outputMessage("FRP 清除需要 Fastboot / ADB Root / EDL / MTK 模式", true);
        emit flashProgress(0);
        return false;
    }

    emit outputMessage("正在清除 FRP...", false);
    emit flashProgress(10);

    if (adbMode) {
        // ADB Root: 通过 shell dd 清空 frp 分区
        QStringList possibleNames = {"frp", "protect_frp", "frp_raw"};
        for (const QString &name : possibleNames) {
            emit outputMessage(QString("尝试清除分区: %1").arg(name), false);
            // 先检查分区是否存在
            QString check = executeAdb(deviceId,
                {"shell", "ls", "/dev/block/by-name/" + name}, 5000);
            if (check.contains("No such file") || check.contains("Permission denied") ||
                check.trimmed().isEmpty() || check.contains("not found")) {
                emit outputMessage(QString("分区 %1 不存在").arg(name), true);
                continue;
            }
            // dd 写零
            QString result = executeAdb(deviceId,
                {"shell", "su", "-c",
                 QString("dd if=/dev/zero of=/dev/block/by-name/%1 bs=1M count=1 2>/dev/null").arg(name)},
                30000);
            if (result.contains("error") || result.contains("not found") ||
                result.contains("denied")) {
                // 尝试无需 su 的路径（部分系统）
                result = executeAdb(deviceId,
                    {"shell", "dd", "if=/dev/zero",
                     QString("of=/dev/block/by-name/%1").arg(name),
                     "bs=1M", "count=1"}, 30000);
            }
            if (!result.contains("error") && !result.trimmed().isEmpty()) {
                emit outputMessage(QString("FRP 分区 %1 已清空").arg(name), false);
                emit flashProgress(100);
                return true;
            }
        }
        // 尝试挂载 persist 并删除 frp 文件
        emit outputMessage("尝试通过 persist 分区清除 FRP...", false);
        QStringList persistCmds = {
            "shell", "su", "-c",
            "mount -o rw,remount /persist 2>/dev/null; "
            "rm -f /persist/frp/* 2>/dev/null; "
            "rm -f /persist/data/frp/* 2>/dev/null; "
            "rm -f /persist/frp 2>/dev/null; "
            "dd if=/dev/zero of=/persist/frp bs=1M count=1 2>/dev/null; "
            "sync; echo FRP_CLEARED"
        };
        QString result = executeAdb(deviceId, persistCmds, 15000);
        if (result.contains("FRP_CLEARED")) {
            emit outputMessage("FRP 已通过 persist 清除", false);
            emit flashProgress(100);
            return true;
        }

        emit outputMessage("FRP 清除失败（设备可能未 Root）", true);
        emit flashProgress(0);
        return false;
    }

    if (fastbootMode) {
        // Fastboot: fastboot erase frp (可能使用不同分区名)
        QStringList possibleNames = {"frp", "protect_frp", "frp_raw"};
        for (const QString &name : possibleNames) {
            emit outputMessage(QString("尝试擦除分区: %1").arg(name), false);
            if (erasePartition(deviceId, name)) {
                emit outputMessage("FRP 已成功清除", false);
                emit flashProgress(100);
                return true;
            }
        }
        emit outputMessage("所有 FRP 分区擦除尝试均失败（可能需要解锁 Bootloader）", true);
        emit flashProgress(0);
        return false;
    }

    if (edlMode) {
        // Phase B Task 8 前置（Task 7 审查交接）：Firehose 只枚举得出 **LUN**（listPartitions 的
        // 产物是 "lun<N>"），拿不到 GPT 分区名 —— 按 "frp"/"protect_frp"/"frp_raw" 找分区在本路径上
        // **必然不命中**（旧实现只会走到"EDL 未找到 FRP 分区"，等于静默失败）。
        // 不猜、不静默：明确告知改走刷写计划（rawprogram 的 label=frp 条目带 LUN/起始扇区/扇区数，
        // 计划方式能精确定位该分区）。
        emit outputMessage(QStringLiteral(
            "EDL 模式无法按分区名定位 FRP：Firehose 只回报 lun<N>，不含 GPT 分区名。"
            "请改用「EDL 刷写计划…」方式清除 —— 计划中 label=frp 的条目可定位该分区"), true);
        emit flashProgress(0);
        return false;
    }

    // MTK: 找到 frp 分区后写零（EDL 已在上面明确拒绝，见其注释）
    QString frpName;
    quint64 frpSize = 0;

    if (mtkMode) {
        auto parts = mtkListPartitions();
        for (const auto &p : parts) {
            QString lower = p.name.toLower();
            if (lower == "frp" || lower == "protect_frp" || lower == "frp_raw") {
                frpName = p.name;
                frpSize = p.length;
                break;
            }
        }
        if (frpName.isEmpty()) {
            emit outputMessage("MTK 未找到 FRP 分区", true);
            emit flashProgress(0);
            return false;
        }
    }

    if (frpSize == 0) {
        emit outputMessage("FRP 分区大小无效，使用默认大小 1MB", false);
        frpSize = 1024 * 1024;
    }

    emit outputMessage(QString("找到 FRP 分区: %1 (%2 bytes)")
                       .arg(frpName).arg(frpSize), false);
    emit flashProgress(40);

    // 创建全零临时文件
    QString tmpPath = QDir::tempPath() + "/frp_zero_erase_" + QString::number(QCoreApplication::applicationPid());
    QFile tmpFile(tmpPath);
    if (!tmpFile.open(QIODevice::WriteOnly)) {
        emit outputMessage("无法创建临时文件", true);
        emit flashProgress(0);
        return false;
    }

    // 写入所需大小的零
    QByteArray zeros(65536, 0); // 64KB chunks
    quint64 written = 0;
    while (written < frpSize) {
        quint64 chunk = qMin(static_cast<quint64>(zeros.size()), frpSize - written);
        tmpFile.write(zeros.constData(), static_cast<qint64>(chunk));
        written += chunk;
    }
    tmpFile.close();

    emit flashProgress(60);

    bool ok = false;
    if (mtkMode) {
        // EDL 分支已不存在：EDL 在入口即明确拒绝（Firehose 无分区名，见上面的注释）
        emit outputMessage("MTK: 写入零到 FRP 分区...", false);
        ok = mtkWritePartition(frpName, tmpPath);
    }

    // 清理临时文件
    tmpFile.remove();

    if (ok) {
        emit outputMessage("FRP 已成功清除", false);
        emit flashProgress(100);
    } else {
        emit outputMessage("FRP 清除失败", true);
        emit flashProgress(0);
    }

    return ok;
}

// ==================== EDL Mode ====================

bool FlashTool::edlConnect(const QString &programmerPath)
{
    return m_edlHandler.connectSahara(programmerPath);
}

bool FlashTool::edlDisconnect()
{
    m_edlHandler.disconnect();
    return true;
}

bool FlashTool::edlIsConnected() const
{
    return m_edlHandler.isConnected();
}

QList<EDLPartition> FlashTool::edlListPartitions()
{
    if (!m_edlHandler.firehoseConnect()) {
        emit outputMessage("EDL Firehose 配置失败", true);
        return {};
    }
    return m_edlHandler.listPartitions();
}

bool FlashTool::edlReadPartition(const EDLPartition &part, const QString &outputPath)
{
    return m_edlHandler.readPartition(part, outputPath);
}

bool FlashTool::edlWritePartition(const EDLPartition &part, const QString &imagePath)
{
    return m_edlHandler.writePartition(part, imagePath);
}

// ==================== MTK Mode ====================

bool FlashTool::mtkInitialize()
{
    if (!m_mtkHandler) {
        m_mtkHandler = new MtkHandler(this);
        connect(m_mtkHandler, &MtkHandler::outputMessage,
                this, &FlashTool::outputMessage);
        connect(m_mtkHandler, &MtkHandler::progress,
                this, &FlashTool::flashProgress);
    }
    return m_mtkHandler->initialize();
}

bool FlashTool::mtkConnect()
{
    if (!mtkInitialize()) return false;
    return m_mtkHandler->connect();
}

bool FlashTool::mtkDisconnect()
{
    if (m_mtkHandler) {
        m_mtkHandler->disconnect();
    }
    return true;
}

bool FlashTool::mtkIsConnected() const
{
    return m_mtkHandler && m_mtkHandler->isConnected();
}

bool FlashTool::mtkSeccfgUnlock()
{
    if (!m_mtkHandler || !m_mtkHandler->isConnected()) return false;
    return m_mtkHandler->seccfgUnlock();
}

bool FlashTool::mtkSeccfgLock()
{
    if (!m_mtkHandler || !m_mtkHandler->isConnected()) return false;
    return m_mtkHandler->seccfgLock();
}

QList<MtkPartition> FlashTool::mtkListPartitions()
{
    if (!m_mtkHandler || !m_mtkHandler->isConnected()) return {};
    return m_mtkHandler->listPartitions();
}

bool FlashTool::mtkReadPartition(const QString &partition, const QString &outputPath)
{
    if (!m_mtkHandler || !m_mtkHandler->isConnected()) return false;
    return m_mtkHandler->readPartition(partition, outputPath);
}

bool FlashTool::mtkWritePartition(const QString &partition, const QString &imagePath)
{
    if (!m_mtkHandler || !m_mtkHandler->isConnected()) return false;
    return m_mtkHandler->writePartition(partition, imagePath);
}

bool FlashTool::mtkReadAllPartitions(const QString &directory)
{
    if (!m_mtkHandler || !m_mtkHandler->isConnected()) return false;
    return m_mtkHandler->readAllPartitions(directory);
}

// ==================== F5: 整包刷写分派 ====================

// 多设备防护（F5 终审）：三个协议层均取枚举首个设备（mtk_handler.cpp runBromFlash
// 的 devs.first() / spd_storage.cpp runSpdFlash / hisi_flash.cpp runHisiFlash 的首个枚举）
// ——多台同厂商设备时刷写目标与用户所选条目未必一致。完整设备选择器
// （bus-addr 直通协议层）为后续任务；此处仅做分派层计数警告。
// 直接用 libusb 按 VID 计数（libusb 已链接）——不引入华为插件头（插件法务隔离保持），
// 三种 VID 统一处理。返回匹配 VID 的设备数；枚举失败返回 -1（不阻断刷写，仅无法计数）。
static int countDevicesOfVid(int vid, QString *error)
{
    libusb_context *ctx = nullptr;
    int ret = libusb_init(&ctx);
    if (ret != LIBUSB_SUCCESS) {
        if (error)
            *error = QStringLiteral("libusb_init 失败: %1").arg(QLatin1String(libusb_error_name(ret)));
        return -1;
    }
    libusb_set_option(ctx, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    libusb_device **list = nullptr;
    const ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) {
        if (error) *error = QStringLiteral("libusb_get_device_list 失败");
        libusb_exit(ctx);
        return -1;
    }
    int n = 0;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) == LIBUSB_SUCCESS &&
            desc.idVendor == vid)
            ++n;
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return n;
}

// programmer 探测（oppo-edl 通道）：解包产物目录内的 prog_*firehose*.{elf,mbn,bin}。
// 真包两种命名都覆盖（prog_ufs_firehose_*.elf / prog_firehose_*.mbn —— buildPlanFromDir 也按前者
// 判 storageType）。大小写不敏感；按文件名排序（QDir::Name）保证"取首个"可复现。
static QStringList findProgrammersInDir(const QString &dir)
{
    QDir d(dir);
    const QStringList names = d.entryList(
        {QStringLiteral("*.elf"), QStringLiteral("*.mbn"), QStringLiteral("*.bin")},
        QDir::Files, QDir::Name);
    QStringList hits;
    for (const QString &name : names) {
        const QString lower = name.toLower();
        if (lower.startsWith(QLatin1String("prog")) && lower.contains(QLatin1String("firehose")))
            hits << d.absoluteFilePath(name);
    }
    return hits;
}

QString FlashTool::resolveProgrammer(const QString &planDir, const QString &explicitPath,
                                     QStringList *messages, QString *error)
{
    if (!explicitPath.isEmpty())
        return explicitPath;        // 显式指定即信任；打不开/为空由调用方在读取时报错

    const QStringList found = findProgrammersInDir(planDir);
    if (found.isEmpty()) {
        if (error)
            *error = QStringLiteral("未在 %1 内找到 programmer（prog_*firehose*.{elf,mbn,bin}）。"
                                    "请在「EDL 刷写计划…」入口重新选择包含 programmer 的目录，"
                                    "或显式提供 programmerPath").arg(planDir);
        return QString();
    }
    if (found.size() > 1 && messages)
        *messages << QStringLiteral("目录内有 %1 个 programmer 候选，使用首个：%2")
                         .arg(found.size()).arg(QFileInfo(found.first()).fileName());
    return found.first();
}

bool FlashTool::isPackageChannelMode(DeviceDetector::DeviceMode mode)
{
    // oppo-edl（9008）**不在**此列：EDL 的分区列表是真实条目，分区刷写/读取都按 lun<N> 工作。
    // 见 flash_tool.h 的注释与 tests/test_pipeline.cpp 的 edlKeepsPartitionFlashPath。
    return !flashChannelForMode(mode).isEmpty() && mode != DeviceDetector::MODE_EDL_9008;
}

QString FlashTool::flashChannelForMode(DeviceDetector::DeviceMode mode)
{
    switch (mode) {
    case DeviceDetector::MODE_MTK_BROM:
        return QStringLiteral("mtk-brom");
    case DeviceDetector::MODE_HUAWEI_USB_UPDATE:
        return QStringLiteral("huawei-usb-update");
    case DeviceDetector::MODE_SPD:
        return QStringLiteral("spd");
    case DeviceDetector::MODE_EDL_9008:
        return QStringLiteral("oppo-edl");
    default:
        return QString();
    }
}

bool FlashTool::flashFullPackage(const QString &deviceId, DeviceDetector::DeviceMode mode,
                                 const QVariantMap &params, QString *error)
{
    const QString channel = flashChannelForMode(mode);
    if (channel.isEmpty()) {
        if (error) *error = QStringLiteral("模式 %1 无协议通道").arg(int(mode));
        return false;
    }
    if (channel == QStringLiteral("mtk-brom")) {
        // F1 通道：DA 二进制路径由 params 提供，读入字节交 runBromFlash
        // （F1 签名为 QByteArray daBinary——路径→字节转换在此完成）。
        const QString daPath = params.value(QStringLiteral("daPath")).toString();
        if (daPath.isEmpty()) {
            if (error) *error = QStringLiteral("缺少 DA 二进制路径（mtk-brom 通道）");
            return false;
        }
        QFile daFile(daPath);
        if (!daFile.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("无法读取 DA 二进制: %1").arg(daPath);
            return false;
        }
        const QByteArray daBinary = daFile.readAll();
        daFile.close();
        if (daBinary.isEmpty()) {
            if (error) *error = QStringLiteral("DA 文件为空");
            return false;
        }
        // 分区列表（分区名→镜像路径）由 F5-3 FlashPanel 构建并经 params 传入——
        // 结构待接线，先空列表（诚实边界，详见 F5-2 报告）。
        const QList<QPair<QString, QByteArray>> partitions;
        // 多设备防护：BROM 层取枚举首个设备（devs.first()）——多台 MTK 同连时警告
        const int mtkCount = countDevicesOfVid(0x0E8D, nullptr);
        if (mtkCount > 1)
            emit outputMessage(QStringLiteral(
                "检测到 %1 台同厂商设备，将刷写首个枚举设备（完整设备选择器为后续任务）")
                .arg(mtkCount), false);
        emit outputMessage(QStringLiteral("MTK BROM 刷写通道：%1").arg(deviceId), false);
        return runBromFlash(daBinary, partitions, error);
    }
    if (channel == QStringLiteral("huawei-usb-update")) {
        // F2 插件通道：经 PluginManager 运行时加载（法务隔离保持——删除插件文件即完整移除）。
        // 已知行为（F5-1 交接）：VID 通配检测（0x12D1 任意 PID）会让华为手机同时以 ADB
        // 模式出现；本分派按模式键控不受影响，但 F5-3 UI 不得对 ADB 设备提供该协议通道。
        // 进度：插件 execute 契约无 progress 参数（huawei_flash_plugin.cpp），华为通道进度不接线。
        const QString updateApp = params.value(QStringLiteral("updateApp")).toString();
        if (updateApp.isEmpty()) {
            if (error) *error = QStringLiteral("缺少 update.app 路径（huawei-usb-update 通道）");
            return false;
        }
        auto plugins = PluginManager::instance()
                           .byCapability(QStringLiteral("huawei-usb-update.flash"));
        if (plugins.isEmpty()) {
            if (error) *error = QStringLiteral("未找到华为刷写插件（plugins/ 目录缺失或未加载）");
            return false;
        }
        QVariantMap capParams;
        capParams.insert(QStringLiteral("updateApp"), updateApp);
        // 多设备防护：hisi_flash.cpp 取枚举首个设备——多台华为同连时警告
        // （raw libusb VID 计数，不经插件 API——法务隔离保持）
        const int huaweiCount = countDevicesOfVid(0x12D1, nullptr);
        if (huaweiCount > 1)
            emit outputMessage(QStringLiteral(
                "检测到 %1 台同厂商设备，将刷写首个枚举设备（完整设备选择器为后续任务）")
                .arg(huaweiCount), false);
        emit outputMessage(QStringLiteral("华为 USB Update 刷写通道：%1").arg(deviceId), false);
        return plugins.first()->execute(
            QStringLiteral("huawei-usb-update.flash"), capParams, error);
    }
    if (channel == QStringLiteral("spd")) {
        // F4 通道：FDL 二进制由用户提供（诚实边界）
        // 已知行为（F5-1）：0x1782 为 VID-only 通配 —— 正常 ADB 模式的展锐
        // 手机会双重列出（ADB + 展锐模式）；F5-3 UI 不得对 ADB 设备提供该
        // 协议通道（PID 收窄为后续任务）
        const QString pacPath = params.value(QStringLiteral("pacPath")).toString();
        const QString fdl1 = params.value(QStringLiteral("fdl1Path")).toString();
        const QString fdl2 = params.value(QStringLiteral("fdl2Path")).toString();
        if (pacPath.isEmpty()) {
            if (error) *error = QStringLiteral("缺少 pac 路径（spd 通道）");
            return false;
        }
        // 多设备防护：spd_storage.cpp 取枚举首个设备——多台展锐同连时警告
        const int spdCount = countDevicesOfVid(0x1782, nullptr);
        if (spdCount > 1)
            emit outputMessage(QStringLiteral(
                "检测到 %1 台同厂商设备，将刷写首个枚举设备（完整设备选择器为后续任务）")
                .arg(spdCount), false);
        emit outputMessage(QStringLiteral("展锐刷写通道：%1").arg(deviceId), false);
        // SPD 进度接线（F5 终审）：runSpdFlash 的 progress 回调（分区名, 百分比）
        // 转发到 FlashTool::flashProgress 信号（与 EDLHandler::progress 同款转发模式）
        return spd::runSpdFlash(pacPath, fdl1, fdl2,
            [this](const QString &name, int percent) {
                Q_UNUSED(name)
                emit flashProgress(percent);
            }, error);
    }
    if (channel == QStringLiteral("oppo-edl")) {
        // Phase B 通道（Task 8）：解包产物目录（rawprogram*/patch*/settings.xml）→ FlashPlan →
        // Sahara 引导（programmer）→ 重枚举 → configure → getstorageinfo → validatePlan →
        // 逐条目写 → reset。**设备须停留在 9008（Sahara）状态**：run() 会自己 open 设备并走
        // Sahara，若用户已用「连接 EDL」把设备带到 Firehose，请改走 EdlSession::writePlan 那类
        // 会话内入口（Task 7 的 EDLHandler 路径），本通道不适用。
        const QString planDir = params.value(QStringLiteral("planDir")).toString();
        if (planDir.isEmpty()) {
            // 文案指向 UI 入口：只有「EDL 刷写计划…」会带 planDir 进来，用户需要知道去哪（Task 8 审查）
            if (error)
                *error = QStringLiteral("缺少解包产物目录（oppo-edl 通道的 planDir）—— "
                                        "整包刷写请用界面的「EDL 刷写计划…」入口");
            return false;
        }
        edl::FlashPlan plan;
        QString planErr;
        if (!edl::buildPlanFromDir(planDir, plan, &planErr)) {
            // buildPlanFromDir 的 *error 已带文件名级诊断（中文），直接透传
            if (error)
                *error = QStringLiteral("构建刷写计划失败：%1")
                             .arg(planErr.isEmpty() ? planDir : planErr);
            return false;
        }
        for (const QString &w : plan.warnings)
            emit outputMessage(QStringLiteral("[计划] %1").arg(w), false);

        QStringList progNotes;
        QString progErr;
        const QString programmerPath = resolveProgrammer(
            planDir, params.value(QStringLiteral("programmerPath")).toString(), &progNotes, &progErr);
        for (const QString &note : progNotes)
            emit outputMessage(note, false);
        if (programmerPath.isEmpty()) {
            if (error)
                *error = progErr.isEmpty()
                    ? QStringLiteral("无法确定 programmer（oppo-edl 通道）") : progErr;
            return false;
        }
        QFile progFile(programmerPath);
        if (!progFile.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("无法读取 programmer: %1").arg(programmerPath);
            return false;
        }
        const QByteArray programmer = progFile.readAll();
        progFile.close();
        if (programmer.isEmpty()) {
            if (error) *error = QStringLiteral("programmer 文件为空: %1").arg(programmerPath);
            return false;
        }

        // 多设备防护：LibusbEdlTransport::open 取首个匹配的 9008 设备（PID 表见其头文件）
        // —— 多台高通设备同连时警告（与既有三通道同款分派层计数警告）
        const int qcomCount = countDevicesOfVid(0x05C6, nullptr);
        if (qcomCount > 1)
            emit outputMessage(QStringLiteral(
                "检测到 %1 台同厂商设备，将刷写首个枚举设备（完整设备选择器为后续任务）")
                .arg(qcomCount), false);
        emit outputMessage(QStringLiteral("OPPO EDL 刷写通道：%1（计划 %2 条条目，programmer %3）")
                               .arg(deviceId)
                               .arg(plan.entries.size())
                               .arg(QFileInfo(programmerPath).fileName()), false);

        // 会话进度 → 既有两条信号。日志只在 (stage, detail) 变化时落一条：write 阶段每块都回调
        // （与 EDLHandler 的转发口径一致），百分比始终转发。
        edl::LibusbEdlTransport transport;
        QString lastProgressKey;
        edl::EdlSession session(transport, [this, &lastProgressKey](const edl::SessionProgress &p) {
            const QString key = p.stage + QLatin1Char('\x1f') + p.detail;
            if (key != lastProgressKey) {
                lastProgressKey = key;
                if (!p.detail.isEmpty())
                    emit outputMessage(QStringLiteral("[%1] %2").arg(p.stage, p.detail), false);
            }
            emit flashProgress(p.percent);
        });

        edl::FlashOptions opt;      // 默认：边写边校验（条目无 sha256 时跳过）、不做刷前全量校验
        QString runErr;
        if (!session.run(plan, programmer, opt, &runErr)) {
            if (error) *error = runErr.isEmpty() ? QStringLiteral("EDL 刷写失败") : runErr;
            return false;
        }
        emit flashProgress(100);
        emit outputMessage(QStringLiteral("EDL 刷写完成（设备已复位）"), false);
        return true;
    }
    // 不可达：flashChannelForMode 仅返回上述四字面量或空串（空串已在上方拒绝），
    // 保留裸 return 以满足编译器的全路径返回检查。
    return false;
}
