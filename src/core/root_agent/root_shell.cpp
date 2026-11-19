#include "root_shell.h"

#include "core/adb_embedded.h"

#include <QProcess>

#include <utility>

QString RootShellResult::combinedText() const
{
    QString text = stdoutText;
    if (!stderrText.isEmpty()) {
        if (!text.isEmpty() && !text.endsWith(QLatin1Char('\n')))
            text += QLatin1Char('\n');
        text += stderrText;
    }
    return text.trimmed();
}

RootShell &RootShell::instance()
{
    static RootShell shell;
    return shell;
}

void RootShell::registerExecutor(const QString &serial, Executor executor)
{
    if (serial.isEmpty() || !executor)
        return;
    m_executors[serial] = std::move(executor);
}

void RootShell::unregisterExecutor(const QString &serial)
{
    m_executors.remove(serial);
}

bool RootShell::hasExecutor(const QString &serial) const
{
    return m_executors.contains(serial);
}

RootShellResult RootShell::run(const QString &serial, const QString &command,
                               int timeoutMs)
{
    RootShellResult result;
    if (serial.isEmpty() || command.trimmed().isEmpty()) {
        result.stderrText = QStringLiteral("empty device or command");
        return result;
    }

    const Executor executor = m_executors.value(serial);
    if (executor) {
        result = executor(command, timeoutMs);
        if (result.usedAgent)
            return result;
    }

    // 没有可用的代理连接：回退旧的 adb shell su -c。
    QString adb = AdbEmbedded::instance().getAdbPath();
    if (adb.isEmpty()) {
        AdbEmbedded::instance().initialize();
        adb = AdbEmbedded::instance().getAdbPath();
    }
    if (adb.isEmpty()) {
        result.stderrText = QStringLiteral("adb not found");
        return result;
    }

    QProcess process;
    process.start(adb, {QStringLiteral("-s"), serial, QStringLiteral("shell"),
                        QStringLiteral("su"), QStringLiteral("-c"), command});
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(1000);
        result.timedOut = true;
        result.exitCode = -1;
        return result;
    }

    result.exitCode = process.exitCode();
    result.stdoutText = QString::fromLocal8Bit(process.readAllStandardOutput());
    result.stderrText = QString::fromLocal8Bit(process.readAllStandardError());
    return result;
}
