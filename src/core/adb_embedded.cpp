#include "adb_embedded.h"
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QDebug>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

AdbEmbedded::AdbEmbedded(QObject *parent) 
    : QObject(parent)
    , m_initialized(false)
{
}

AdbEmbedded::~AdbEmbedded()
{
    // 清理临时文件
    if (m_tempDir.isValid()) {
        m_tempDir.remove();
    }
}

AdbEmbedded& AdbEmbedded::instance()
{
    static AdbEmbedded instance;
    return instance;
}

bool AdbEmbedded::initialize()
{
    if (m_initialized) {
        return true;
    }

    qDebug() << "Initializing embedded ADB...";

    if (!extractEmbeddedTools()) {
        qWarning() << "Failed to extract embedded ADB tools";
        return false;
    }

    m_initialized = true;
    qDebug() << "Embedded ADB initialized successfully";
    return true;
}

bool AdbEmbedded::extractEmbeddedTools()
{
    // 确保临时目录有效
    if (!m_tempDir.isValid()) {
        qCritical() << "Cannot create temporary directory";
        return false;
    }

    QString platformSubdir;
    QString binaryExtension;
    
    // 根据平台设置路径
#ifdef Q_OS_WIN
    platformSubdir = "windows/x64";
    binaryExtension = ".exe";
#elif defined(Q_OS_MACOS)
    platformSubdir = "macos/x64";
    binaryExtension = "";
#else
    platformSubdir = "linux/x64";
    binaryExtension = "";
#endif

    // 要提取的文件列表
    QVector<QPair<QString, QString>> filesToExtract = {
        {QString("adb/%1/adb%2").arg(platformSubdir).arg(binaryExtension), "adb"},
        {QString("adb/%1/fastboot%2").arg(platformSubdir).arg(binaryExtension), "fastboot"}
    };

#ifdef Q_OS_WIN
    // Windows需要额外的DLL文件
    filesToExtract.append({
        {QString("adb/%1/AdbWinApi.dll").arg(platformSubdir), "AdbWinApi.dll"},
        {QString("adb/%1/AdbWinUsbApi.dll").arg(platformSubdir), "AdbWinUsbApi.dll"}
    });
#endif

    // 提取所有文件
    for (const auto &filePair : filesToExtract) {
        QString resourcePath = ":/binaries/" + filePair.first;
        QString outputPath = m_tempDir.path() + "/" + filePair.second;

        QFile resourceFile(resourcePath);
        if (!resourceFile.exists()) {
            qWarning() << "Resource file not found:" << resourcePath;
            return false;
        }

        // 复制文件到临时目录
        if (!resourceFile.copy(outputPath)) {
            qWarning() << "Failed to copy resource file to:" << outputPath 
                      << "Error:" << resourceFile.errorString();
            return false;
        }

        // 设置文件权限（非Windows系统）
#ifndef Q_OS_WIN
        if (chmod(outputPath.toUtf8().constData(), 0755) != 0) {
            qWarning() << "Failed to set executable permissions for:" << outputPath;
            return false;
        }
#endif

        qDebug() << "Extracted:" << outputPath;
    }

    // 设置ADB和Fastboot路径
    m_adbPath = m_tempDir.path() + "/adb";
    m_fastbootPath = m_tempDir.path() + "/fastboot";

#ifdef Q_OS_WIN
    m_adbPath += ".exe";
    m_fastbootPath += ".exe";
#endif

    // 验证文件是否存在且可执行
    if (!QFile::exists(m_adbPath) || !QFile::exists(m_fastbootPath)) {
        qCritical() << "Extracted binaries not found";
        return false;
    }

    // 启动ADB服务器
    QProcess adbProcess;
    adbProcess.start(m_adbPath, QStringList() << "start-server");
    if (!adbProcess.waitForFinished(5000)) {
        qWarning() << "Failed to start ADB server";
        // 继续执行，因为ADB可能已经在运行
    }

    QProcess fastbootTest;
    fastbootTest.setProgram(m_fastbootPath);
    fastbootTest.setArguments(QStringList() << "--version");
    fastbootTest.start();
    
    if (fastbootTest.waitForFinished(3000)) {
        qDebug() << "Fastboot test passed:" << fastbootTest.readAllStandardOutput();
    } else {
        qWarning() << "Fastboot test failed";
        return false;
    }
    
    return true;
}

QString AdbEmbedded::executeCommand(const QString &command, int timeout)
{
    if (!m_initialized && !initialize()) {
        return "Error: ADB not initialized";
    }

    QProcess process;
    process.setProgram(m_adbPath);
    
    // 解析命令参数
    QStringList arguments = command.split(' ', Qt::SkipEmptyParts);
    process.setArguments(arguments);


    process.start();
    
    if (!process.waitForFinished(timeout)) {
        process.kill();
        return "Error: Command timeout";
    }

    QString output = process.readAllStandardOutput();
    QString error = process.readAllStandardError();

    if (process.exitCode() != 0) {
        return "Error: " + error;
    }

    return output.isEmpty() ? "Success" : output.trimmed();
}

QString AdbEmbedded::getDeviceInfo(const QString &serial, const QString &prop)
{
    QString command;
    if (serial.isEmpty()) {
        command = QString("shell getprop %1").arg(prop);
    } else {
        command = QString("-s %1 shell getprop %2").arg(serial, prop);
    }
    
    return executeCommand(command).trimmed();
}

QString AdbEmbedded::getAdbPath() const
{
    return m_adbPath;
}

QString AdbEmbedded::getFastbootPath() const
{
    return m_fastbootPath;
}

QString AdbEmbedded::getPlatformBinaryName(const QString &baseName) const
{
#ifdef Q_OS_WIN
    return baseName + ".exe";
#else
    return baseName;
#endif
}