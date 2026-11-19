#include "root_agent_installer.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QThread>

RootAgentInstaller::RootAgentInstaller(QObject *parent)
    : QObject(parent)
{
}

void RootAgentInstaller::start(const QString &serial, const QString &adbPath)
{
    QThread *thread = QThread::create([this, serial, adbPath]() {
        QString packageId;
        QString error;
        const bool ok = run(serial, adbPath, &packageId, &error);
        emit finished(ok, packageId, error);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

QString RootAgentInstaller::findTemplateApk() const
{
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/root-agent-tools/template.apk"),
        appDir + QStringLiteral("/../android/root-agent/app/build/outputs/apk/release/app-release.apk"),
        QDir::currentPath() + QStringLiteral("/android/root-agent/app/build/outputs/apk/release/app-release.apk"),
    };
    for (const QString &candidate : candidates) {
        if (QFileInfo(candidate).isFile())
            return candidate;
    }
    return QString();
}

bool RootAgentInstaller::runProcess(const QStringList &programAndArgs,
                                    int timeoutMs, QString *output,
                                    QString *error) const
{
    if (programAndArgs.isEmpty())
        return false;

    QProcess process;
    process.setProgram(programAndArgs.first());
    process.setArguments(programAndArgs.mid(1));
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start();
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        if (error)
            *error = QStringLiteral("命令超时：%1").arg(programAndArgs.first());
        return false;
    }

    const QString text = QString::fromLocal8Bit(process.readAll());
    if (output)
        *output = text;
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            QString tail = text.trimmed();
            if (tail.size() > 800)
                tail = tail.right(800);
            *error = tail.isEmpty()
                ? QStringLiteral("命令失败：%1").arg(programAndArgs.first())
                : tail;
        }
        return false;
    }
    return true;
}

bool RootAgentInstaller::run(const QString &serial, const QString &adbPath,
                             QString *packageId, QString *error)
{
    if (serial.isEmpty() || adbPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("没有选中设备或 adb 不可用");
        return false;
    }

    const QString templateApk = findTemplateApk();
    if (templateApk.isEmpty()) {
        if (error)
            *error = QStringLiteral("找不到模板 APK");
        return false;
    }

    // 开源项目使用固定包名；模板 APK 已签名，直接覆盖安装即可。
    // 不再为每个设备重新生成随机包名/重打包，启动和安装都更快。
    const QString package = QStringLiteral("com.phonetoolbox.agent");
    if (packageId)
        *packageId = package;

    emit logMessage(QStringLiteral("安装固定包名 APK：%1").arg(package));

    QString output;
    if (!runProcess({adbPath, QStringLiteral("-s"), serial,
                     QStringLiteral("install"), QStringLiteral("-r"), templateApk},
                    180000, &output, error))
        return false;

    emit logMessage(QStringLiteral("安装完成，打开一次 APK 以查看配对码…"));
    runProcess({adbPath, QStringLiteral("-s"), serial, QStringLiteral("shell"),
                QStringLiteral("am"), QStringLiteral("start"),
                QStringLiteral("-n"),
                QStringLiteral("%1/com.local.service.MainActivity").arg(package)},
               15000, &output, nullptr);

    return true;
}
