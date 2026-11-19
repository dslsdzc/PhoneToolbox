#include "root_agent_executor.h"

#include "core/root_agent/root_agent_client.h"

#include <QEventLoop>
#include <QTimer>

RootShellResult runRootAgentCommand(RootAgentClient *client,
                                    const QString &command, int timeoutMs)
{
    RootShellResult result;
    if (!client || !client->isConnected()) {
        // usedAgent=false 让 RootShell 回退 adb su。
        return result;
    }

    result.usedAgent = true;

    QEventLoop loop;
    bool finished = false;
    const auto finish = [&](int exitCode, const QString &out,
                            const QString &err, bool timedOut) {
        if (finished)
            return;
        finished = true;
        result.exitCode = exitCode;
        result.stdoutText = out;
        result.stderrText = err;
        result.timedOut = timedOut;
        loop.quit();
    };

    const auto finishedConnection =
        QObject::connect(client, &RootAgentClient::execFinished, &loop,
                         [&](int exitCode, const QString &out,
                             const QString &err, bool timedOut) {
                             finish(exitCode, out, err, timedOut);
                         });
    const auto errorConnection =
        QObject::connect(client, &RootAgentClient::errorOccurred, &loop,
                         [&](const QString &message) {
                             finish(-1, QString(), message, false);
                         });
    const auto disconnectedConnection =
        QObject::connect(client, &RootAgentClient::disconnected, &loop, [&]() {
            finish(-1, QString(), QStringLiteral("root agent disconnected"),
                   false);
        });

    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() {
        finish(-1, QString(), QStringLiteral("root agent timeout"), true);
    });

    client->execShell(command, timeoutMs);
    if (!finished) {
        timer.start(timeoutMs + 2000);
        loop.exec();
    }

    QObject::disconnect(finishedConnection);
    QObject::disconnect(errorConnection);
    QObject::disconnect(disconnectedConnection);

    if (result.timedOut)
        client->disconnectFromAgent();
    return result;
}
