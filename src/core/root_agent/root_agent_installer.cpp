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

RootAgentInstaller::~RootAgentInstaller()
{
    // The worker captures `this`; never let it outlive the installer.
    if (m_thread) {
        QThread *thread = m_thread;
        m_thread = nullptr;
        thread->requestInterruption();
        thread->wait();
    }
}

void RootAgentInstaller::start(const QString &serial, const QString &adbPath)
{
    if (m_thread && m_thread->isRunning()) {
        emit finished(false, QString(), QStringLiteral("上一次安装仍在进行"));
        return;
    }

    QThread *thread = QThread::create([this, serial, adbPath]() {
        QString packageId;
        QString error;
        const bool ok = run(serial, adbPath, &packageId, &error);
        emit finished(ok, packageId, error);
    });
    m_thread = thread;
    thread->setParent(this);
    connect(thread, &QThread::finished, this, [this, thread]() {
        if (m_thread == thread)
            m_thread = nullptr;
    });
    thread->start();
}

QString RootAgentInstaller::findTemplateApk(bool legacy) const
{
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
    if (legacy) {
        candidates = {
            appDir + QStringLiteral("/root-agent-tools/legacy-agent.apk"),
            appDir + QStringLiteral("/../android/legacy-agent/build/PhoneToolbox-Agent-legacy.apk"),
            QDir::currentPath() + QStringLiteral("/android/legacy-agent/build/PhoneToolbox-Agent-legacy.apk"),
        };
    } else {
        candidates = {
            appDir + QStringLiteral("/root-agent-tools/template.apk"),
            appDir + QStringLiteral("/../android/root-agent/app/build/outputs/apk/release/app-release.apk"),
            QDir::currentPath() + QStringLiteral("/android/root-agent/app/build/outputs/apk/release/app-release.apk"),
        };
    }
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

    QString sdkText;
    runProcess({adbPath, QStringLiteral("-s"), serial, QStringLiteral("shell"),
                QStringLiteral("getprop"), QStringLiteral("ro.build.version.sdk")},
               5000, &sdkText, nullptr);
    const int sdk = sdkText.trimmed().toInt();
    const bool legacy = sdk > 0 && sdk < 21;

    const QString templateApk = findTemplateApk(legacy);
    if (templateApk.isEmpty()) {
        if (error)
            *error = legacy
                ? QStringLiteral("找不到 legacy 模板 APK（Android 1.0–4.4）")
                : QStringLiteral("找不到模板 APK");
        return false;
    }

    // 开源项目使用固定包名；模板 APK 已签名，直接覆盖安装即可。
    // 不再为每个设备重新生成随机包名/重打包，启动和安装都更快。
    const QString package = QStringLiteral("com.phonetoolbox.agent");
    if (packageId)
        *packageId = package;

    emit logMessage(legacy
        ? QStringLiteral("检测到 Android API %1，安装 legacy 静态代理 APK…").arg(sdk)
        : QStringLiteral("安装固定包名 APK：%1").arg(package));

    QString output;
    if (!runProcess({adbPath, QStringLiteral("-s"), serial,
                     QStringLiteral("install"), QStringLiteral("-r"), templateApk},
                    180000, &output, error))
        return false;

    if (legacy) {
        // Open the on-device screen so the user can read the pair code there.
        emit logMessage(QStringLiteral("安装完成，打开手机端配对界面…"));
        runProcess({adbPath, QStringLiteral("-s"), serial, QStringLiteral("shell"),
                    QStringLiteral("am"), QStringLiteral("start"),
                    QStringLiteral("-n"),
                    QStringLiteral("%1/.LegacyActivity").arg(package)},
                   15000, &output, nullptr);
        return true;
    }

    emit logMessage(QStringLiteral("安装完成，打开一次 APK 以查看配对码…"));
    runProcess({adbPath, QStringLiteral("-s"), serial, QStringLiteral("shell"),
                QStringLiteral("am"), QStringLiteral("start"),
                QStringLiteral("-n"),
                QStringLiteral("%1/com.local.service.MainActivity").arg(package)},
               15000, &output, nullptr);

    return true;
}
