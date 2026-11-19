#include <QtTest>
#include <QElapsedTimer>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>

#include <sodium.h>

#include "core/root_agent/root_agent_client.h"

// 桌面端客户端 ↔ 本机 agent 的协议测试。
// 依赖 agent/build/phone_agent（先构建 agent/）；不存在时跳过，避免把夹具缺失
// 伪装成通过。
class TestRootAgentClient : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void execReturnsOutput();
    void detectRootOnNormalUser();
    void badTokenRejected();
    void execTimeout();
    void noiseSession();
    void pairAndNoiseSession();
    void pairExternal();

private:
    struct Agent {
        QProcess process;
        QTemporaryDir stateDir;
        quint16 port = 0;
        QString publicKeyHex;
        QString token = QStringLiteral("test-token-0123456789");
    };

    bool startAgent(Agent &agent);
    void stopAgent(Agent &agent);

    QString m_agentPath;
};

void TestRootAgentClient::initTestCase()
{
    m_agentPath = QStringLiteral(PT_SRC_ROOT "/agent/build/phone_agent");
    if (!QFile::exists(m_agentPath))
        QSKIP("agent/build/phone_agent 不存在；先构建 agent/");
    QVERIFY(sodium_init() >= 0);
}

bool TestRootAgentClient::startAgent(Agent &agent)
{
    if (!agent.stateDir.isValid())
        return false;

    agent.process.start(m_agentPath,
                        {QStringLiteral("--port"), QStringLiteral("0"),
                         QStringLiteral("--token"), agent.token,
                         QStringLiteral("--allow-plaintext"),
                         QStringLiteral("--pair-code"), QStringLiteral("123456"),
                         QStringLiteral("--state-dir"), agent.stateDir.path()});
    if (!agent.process.waitForStarted(3000))
        return false;

    bool gotPort = false;
    bool gotKey = false;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 3000 && (!gotPort || !gotKey)) {
        if (!agent.process.waitForReadyRead(200))
            continue;
        while (agent.process.canReadLine()) {
            const QByteArray line = agent.process.readLine().trimmed();
            if (line.startsWith("PORT=")) {
                agent.port = line.mid(5).toUShort();
                gotPort = agent.port != 0;
            } else if (line.startsWith("PUBKEY=")) {
                agent.publicKeyHex = QString::fromLatin1(line.mid(7));
                gotKey = !agent.publicKeyHex.isEmpty();
            }
        }
    }
    return gotPort && gotKey;
}

void TestRootAgentClient::stopAgent(Agent &agent)
{
    agent.process.terminate();
    if (!agent.process.waitForFinished(2000))
        agent.process.kill();
}

void TestRootAgentClient::execReturnsOutput()
{
    Agent agent;
    QVERIFY2(startAgent(agent), "agent 启动失败");

    RootAgentClient client;
    client.setToken(agent.token);
    client.setLocalPort(agent.port);

    QSignalSpy connected(&client, &RootAgentClient::connected);
    QSignalSpy finished(&client, &RootAgentClient::execFinished);
    client.connectToAgent(3000);
    QTRY_VERIFY_WITH_TIMEOUT(connected.count() == 1, 5000);

    client.exec({QStringLiteral("/bin/echo"), QStringLiteral("hello")}, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 5000);

    const QList<QVariant> args = finished.takeFirst();
    QCOMPARE(args.at(0).toInt(), 0);
    QCOMPARE(args.at(1).toString(), QStringLiteral("hello\n"));
    QCOMPARE(args.at(3).toBool(), false);

    stopAgent(agent);
}

void TestRootAgentClient::detectRootOnNormalUser()
{
    Agent agent;
    QVERIFY2(startAgent(agent), "agent 启动失败");

    RootAgentClient client;
    client.setToken(agent.token);
    client.setLocalPort(agent.port);

    QSignalSpy connected(&client, &RootAgentClient::connected);
    QSignalSpy root(&client, &RootAgentClient::rootDetected);
    client.connectToAgent(3000);
    QTRY_VERIFY_WITH_TIMEOUT(connected.count() == 1, 5000);

    client.detectRoot();
    QTRY_VERIFY_WITH_TIMEOUT(root.count() == 1, 5000);

    const QList<QVariant> args = root.takeFirst();
    QCOMPARE(args.at(0).toBool(), false); // 测试 agent 以普通用户运行
    QVERIFY(args.at(1).toString().contains(QStringLiteral("uid=")));

    stopAgent(agent);
}

void TestRootAgentClient::badTokenRejected()
{
    Agent agent;
    QVERIFY2(startAgent(agent), "agent 启动失败");

    RootAgentClient client;
    client.setToken(QStringLiteral("wrong-token"));
    client.setLocalPort(agent.port);

    QSignalSpy connected(&client, &RootAgentClient::connected);
    QSignalSpy errors(&client, &RootAgentClient::errorOccurred);
    client.connectToAgent(3000);

    QTRY_VERIFY_WITH_TIMEOUT(errors.count() >= 1, 5000);
    QCOMPARE(connected.count(), 0);

    stopAgent(agent);
}

void TestRootAgentClient::execTimeout()
{
    Agent agent;
    QVERIFY2(startAgent(agent), "agent 启动失败");

    RootAgentClient client;
    client.setToken(agent.token);
    client.setLocalPort(agent.port);

    QSignalSpy connected(&client, &RootAgentClient::connected);
    QSignalSpy finished(&client, &RootAgentClient::execFinished);
    client.connectToAgent(3000);
    QTRY_VERIFY_WITH_TIMEOUT(connected.count() == 1, 5000);

    client.exec({QStringLiteral("/bin/sh"), QStringLiteral("-c"),
                 QStringLiteral("sleep 2")},
                100);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 5000);

    const QList<QVariant> args = finished.takeFirst();
    QCOMPARE(args.at(3).toBool(), true);

    stopAgent(agent);
}

void TestRootAgentClient::noiseSession()
{
    Agent agent;
    QVERIFY2(startAgent(agent), "agent 启动失败");

    uint8_t clientPublic[32];
    uint8_t clientPrivate[32];
    crypto_box_keypair(clientPublic, clientPrivate);
    char clientPublicHex[65];
    char clientPrivateHex[65];
    sodium_bin2hex(clientPublicHex, sizeof(clientPublicHex), clientPublic, 32);
    sodium_bin2hex(clientPrivateHex, sizeof(clientPrivateHex), clientPrivate, 32);

    QFile peers(agent.stateDir.filePath(QStringLiteral("peers.txt")));
    QVERIFY(peers.open(QIODevice::WriteOnly));
    peers.write(QStringLiteral("%1 test-client exec,push,pull\n")
                    .arg(QLatin1String(clientPublicHex))
                    .toUtf8());
    peers.close();

    RootAgentClient client;
    client.setLocalPort(agent.port);
    client.setIdentity(QString::fromLatin1(clientPrivateHex), agent.publicKeyHex);
    QVERIFY(client.hasIdentity());

    QSignalSpy connected(&client, &RootAgentClient::connected);
    QSignalSpy errors(&client, &RootAgentClient::errorOccurred);
    QSignalSpy finished(&client, &RootAgentClient::execFinished);

    client.connectToAgent(3000);
    QTRY_VERIFY_WITH_TIMEOUT(connected.count() == 1, 5000);
    QVERIFY2(errors.isEmpty(),
             errors.isEmpty() ? "" : qPrintable(errors.first().first().toString()));

    client.exec({QStringLiteral("/bin/echo"), QStringLiteral("noise")}, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 5000);

    const QList<QVariant> args = finished.takeFirst();
    QCOMPARE(args.at(0).toInt(), 0);
    QCOMPARE(args.at(1).toString(), QStringLiteral("noise\n"));

    stopAgent(agent);
}

void TestRootAgentClient::pairAndNoiseSession()
{
    Agent agent;
    QVERIFY2(startAgent(agent), "agent 启动失败");

    RootAgentClient client;
    client.setLocalPort(agent.port);

    QSignalSpy paired(&client, &RootAgentClient::pairingFinished);
    client.pair(QStringLiteral("123456"));
    QTRY_VERIFY_WITH_TIMEOUT(paired.count() == 1, 5000);

    const QList<QVariant> pairArgs = paired.takeFirst();
    QVERIFY2(pairArgs.at(0).toBool(),
             qPrintable(pairArgs.at(3).toString()));
    const QString clientPrivate = pairArgs.at(1).toString();
    const QString agentPublic = pairArgs.at(2).toString();
    QCOMPARE(clientPrivate.size(), 64);
    QCOMPARE(agentPublic.size(), 64);

    client.setIdentity(clientPrivate, agentPublic);
    QVERIFY(client.hasIdentity());

    QSignalSpy connected(&client, &RootAgentClient::connected);
    QSignalSpy finished(&client, &RootAgentClient::execFinished);
    client.connectToAgent(3000);
    QTRY_VERIFY_WITH_TIMEOUT(connected.count() == 1, 5000);

    client.exec({QStringLiteral("/bin/echo"), QStringLiteral("paired")}, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 5000);
    QCOMPARE(finished.takeFirst().at(1).toString(), QStringLiteral("paired\n"));

    stopAgent(agent);
}

void TestRootAgentClient::pairExternal()
{
    const QString portText = qEnvironmentVariable("ROOT_AGENT_PORT");
    const QString code = qEnvironmentVariable("ROOT_AGENT_CODE");
    if (portText.isEmpty() || code.isEmpty())
        QSKIP("set ROOT_AGENT_PORT / ROOT_AGENT_CODE to test a running device agent");

    RootAgentClient client;
    client.setLocalPort(portText.toUShort());

    QSignalSpy paired(&client, &RootAgentClient::pairingFinished);
    QSignalSpy pairErrors(&client, &RootAgentClient::errorOccurred);
    client.pair(code, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(paired.count() == 1, 15000);

    const QList<QVariant> pairArgs = paired.takeFirst();
    QVERIFY2(pairArgs.at(0).toBool(),
             qPrintable(QStringLiteral("配对失败：%1；errors=%2")
                            .arg(pairArgs.at(3).toString(),
                                 pairErrors.isEmpty()
                                     ? QStringLiteral("none")
                                     : pairErrors.first().first().toString())));

    client.setIdentity(pairArgs.at(1).toString(), pairArgs.at(2).toString());

    QSignalSpy connected(&client, &RootAgentClient::connected);
    QSignalSpy errors(&client, &RootAgentClient::errorOccurred);
    QSignalSpy root(&client, &RootAgentClient::rootDetected);

    client.connectToAgent(5000);
    QTRY_VERIFY_WITH_TIMEOUT(connected.count() == 1, 8000);

    client.detectRoot();
    QTRY_VERIFY_WITH_TIMEOUT(root.count() == 1, 8000);

    const QList<QVariant> rootArgs = root.takeFirst();
    QVERIFY2(rootArgs.at(0).toBool(),
             qPrintable(QStringLiteral("不是 root：%1").arg(rootArgs.at(1).toString())));
    QVERIFY2(rootArgs.at(1).toString().contains(QStringLiteral("uid=0")),
             qPrintable(rootArgs.at(1).toString()));
}

QTEST_GUILESS_MAIN(TestRootAgentClient)
#include "test_root_agent_client.moc"
