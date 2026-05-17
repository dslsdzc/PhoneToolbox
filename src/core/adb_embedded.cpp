#include "adb_embedded.h"
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QDebug>
#include <QUrl>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

AdbEmbedded::AdbEmbedded(QObject *parent)
    : QObject(parent)
{
}

AdbEmbedded::~AdbEmbedded()
{
    if (m_tempDir.isValid())
        m_tempDir.remove();
}

AdbEmbedded& AdbEmbedded::instance()
{
    static AdbEmbedded instance;
    return instance;
}

bool AdbEmbedded::initialize()
{
    if (m_initialized)
        return true;

    qDebug() << "AdbEmbedded: initializing...";

    // 1. 尝试使用系统 ADB
    if (findSystemAdb()) {
        qDebug() << "AdbEmbedded: using system ADB at" << m_adbPath;
        m_initialized = true;
        return true;
    }

    // 2. 下载 platform-tools
    qDebug() << "AdbEmbedded: system ADB not found, downloading...";
    if (downloadPlatformTools()) {
        qDebug() << "AdbEmbedded: downloaded platform-tools to" << m_adbPath;
        m_initialized = true;
        return true;
    }

    qCritical() << "AdbEmbedded: all methods failed";
    return false;
}

bool AdbEmbedded::findSystemAdb()
{
    // 先检查环境变量 ANDROID_HOME/platform-tools
    QString androidHome = qEnvironmentVariable("ANDROID_HOME");
    if (!androidHome.isEmpty()) {
        QString candidate = androidHome + "/platform-tools/adb";
#ifdef Q_OS_WIN
        candidate += ".exe";
#endif
        if (QFile::exists(candidate)) {
            m_adbPath = candidate;
            m_fastbootPath = androidHome + "/platform-tools/fastboot";
#ifdef Q_OS_WIN
            m_fastbootPath += ".exe";
#endif
            m_useSystemAdb = true;
            return true;
        }
    }

    // 再查 PATH
    QString systemAdb = QStandardPaths::findExecutable("adb");
    if (!systemAdb.isEmpty()) {
        m_adbPath = systemAdb;

        // 同目录找 fastboot
        QDir dir = QFileInfo(systemAdb).absoluteDir();
        QString fb = dir.absolutePath() + "/fastboot";
#ifdef Q_OS_WIN
        fb += ".exe";
#endif
        if (QFile::exists(fb))
            m_fastbootPath = fb;

        m_useSystemAdb = true;
        return true;
    }

    return false;
}

bool AdbEmbedded::downloadPlatformTools()
{
    if (!m_tempDir.isValid()) {
        qWarning() << "downloadPlatformTools: temp dir invalid";
        return false;
    }

    // 选择平台
    QString platformKey;
#ifdef Q_OS_WIN
    platformKey = "windows";
#elif defined(Q_OS_MACOS)
    platformKey = "darwin";
#else
    platformKey = "linux";
#endif

    QString url = QString("https://dl.google.com/android/repository/platform-tools-latest-%1.zip")
                      .arg(platformKey);
    QString zipPath = m_tempDir.path() + "/platform-tools.zip";

    qDebug() << "AdbEmbedded: downloading" << url;

    // 下载
    QNetworkAccessManager mgr;
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(60000);

    QNetworkReply *reply = mgr.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "downloadPlatformTools: network error" << reply->errorString();
        reply->deleteLater();
        return false;
    }

    QByteArray data = reply->readAll();
    reply->deleteLater();

    if (data.isEmpty()) {
        qWarning() << "downloadPlatformTools: empty response";
        return false;
    }

    QFile f(zipPath);
    if (!f.open(QIODevice::WriteOnly)) {
        qWarning() << "downloadPlatformTools: cannot write" << zipPath;
        return false;
    }
    f.write(data);
    f.close();

    qDebug() << "AdbEmbedded: downloaded" << data.size() << "bytes, extracting...";

    // 解压
    if (!extractZip(zipPath, m_tempDir.path())) {
        qWarning() << "downloadPlatformTools: extract failed";
        return false;
    }

    // 清理 zip
    QFile::remove(zipPath);

    m_downloaded = true;
    return true;
}

bool AdbEmbedded::extractZip(const QString &zipPath, const QString &destDir)
{
    QString extractDir = destDir + "/platform-tools";

#ifdef Q_OS_WIN
    // Windows: 用 PowerShell
    QProcess p;
    p.start("powershell", {
        "-NoProfile",
        "-Command",
        QString("Expand-Archive -Path '%1' -DestinationPath '%2' -Force")
            .arg(zipPath, destDir)
    });
    if (!p.waitForFinished(120000)) {
        p.kill();
        qWarning() << "extractZip: PowerShell timeout";
        return false;
    }
    if (p.exitCode() != 0) {
        qWarning() << "extractZip: PowerShell error:" << p.readAllStandardError();
        return false;
    }
#else
    // Linux / macOS: 用 unzip
    QProcess p;
    p.start("unzip", {"-o", zipPath, "-d", destDir});
    if (!p.waitForFinished(120000)) {
        p.kill();
        qWarning() << "extractZip: unzip timeout";
        return false;
    }
    if (p.exitCode() != 0) {
        qWarning() << "extractZip: unzip error:" << p.readAllStandardError();
        return false;
    }
#endif

    // 设置可执行权限
#ifdef Q_OS_WIN
    m_adbPath = extractDir + "/adb.exe";
    m_fastbootPath = extractDir + "/fastboot.exe";
#else
    m_adbPath = extractDir + "/adb";
    m_fastbootPath = extractDir + "/fastboot";
    chmod(m_adbPath.toUtf8().constData(), 0755);
    chmod(m_fastbootPath.toUtf8().constData(), 0755);
#endif

    if (!QFile::exists(m_adbPath)) {
        qWarning() << "extractZip: adb not found after extraction";
        return false;
    }

    return true;
}

QString AdbEmbedded::executeCommand(const QString &command, int timeout)
{
    if (!m_initialized && !initialize())
        return "Error: ADB not initialized";

    QProcess process;
    process.setProgram(m_adbPath);
    QStringList arguments = command.split(' ', Qt::SkipEmptyParts);
    process.setArguments(arguments);
    process.start();

    if (!process.waitForFinished(timeout)) {
        process.kill();
        return "Error: Command timeout";
    }

    QString output = process.readAllStandardOutput();
    QString error = process.readAllStandardError();

    if (process.exitCode() != 0)
        return "Error: " + error;

    return output.isEmpty() ? "Success" : output.trimmed();
}

QString AdbEmbedded::getDeviceInfo(const QString &serial, const QString &prop)
{
    QString command;
    if (serial.isEmpty())
        command = QString("shell getprop %1").arg(prop);
    else
        command = QString("-s %1 shell getprop %2").arg(serial, prop);

    return executeCommand(command).trimmed();
}

QString AdbEmbedded::getAdbPath() const { return m_adbPath; }
QString AdbEmbedded::getFastbootPath() const { return m_fastbootPath; }
