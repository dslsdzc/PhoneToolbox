#ifndef ROOT_AGENT_CLIENT_H
#define ROOT_AGENT_CLIENT_H

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include "secure/cpace_pair.h"

class QProcess;
class QTcpSocket;
class QTimer;
struct noise_channel;

// 桌面端 ↔ Android root agent 的协议客户端。
//
// 传输层：设置 Noise 身份后走 Noise_IK；未设置时明文 localhost + token
// 仅用于开发模式。上层 exec/push/pull 不关心传输细节。
class RootAgentClient : public QObject
{
    Q_OBJECT

public:
    explicit RootAgentClient(QObject *parent = nullptr);
    ~RootAgentClient() override;

    void setSerial(const QString &serial) { m_serial = serial; }
    void setToken(const QString &token) { m_token = token; }
    // 设置 Noise 长期身份：客户端 Curve25519 私钥 + agent 公钥（均为 hex）。
    // 两者都非空时连接走 Noise_IK；否则退回明文 token（仅开发模式）。
    void setIdentity(const QString &clientPrivateHex, const QString &agentPublicHex);
    bool hasIdentity() const;
    quint16 localPort() const { return m_localPort; }
    // 测试/直连用：绕过 adb forward，直接连一个已知本地端口。
    void setLocalPort(quint16 port) { m_localPort = port; }

    // 建立 adb forward；返回 false 时填 error。
    bool startForward(const QString &adbPath, quint16 devicePort, QString *error);
    void stopForward();

    void connectToAgent(int timeoutMs = 5000);
    void disconnectFromAgent();
    bool isConnected() const { return m_authenticated; }

    // 首次配对：用 APK 显示的短码跑 CPace，成功后把客户端私钥/agent 公钥
    // 通过 pairingFinished 交给调用方保存。连接必须已经 adb forward 到端口。
    void pair(const QString &pairCode, int timeoutMs = 10000,
              bool allowDangerous = false);

    // 执行一条命令（argv 直执行）。完成后发 execFinished。
    void exec(const QStringList &argv, int timeoutMs = 30000);
    // 走 agent 的 shell 通道（/bin/sh -c）。agent 会按 dangerous 能力授权，
    // 未授权时直接拒绝，调用方不应改用 adb su 绕过。
    void execShell(const QString &command, int timeoutMs = 30000);

    // 通过 agent 判断 root：执行 id，检查 uid=0。
    void detectRoot();

signals:
    void connected();
    void disconnected();
    void errorOccurred(const QString &message);
    void logMessage(const QString &message);

    void execFinished(int exitCode, const QString &stdoutText,
                      const QString &stderrText, bool timedOut);
    void rootDetected(bool isRoot, const QString &detail);
    void pairingFinished(bool ok, const QString &clientPrivateHex,
                         const QString &agentPublicHex, const QString &error);

private slots:
    void onSocketConnected();
    void onReadyRead();
    void onSocketError();
    void onExecTimeout();

private:
    void sendJson(const QJsonObject &object);
    void sendPayload(const QByteArray &payload);
    void sendRawFrame(quint8 type, const QByteArray &data);
    void handleMessage(const QJsonObject &object);
    void finishExec(int exitCode, bool timedOut);
    void cleanupSocket();

    QTcpSocket *m_socket = nullptr;
    QProcess *m_adb = nullptr;
    QTimer *m_connectTimer = nullptr;
    QTimer *m_execTimer = nullptr;

    QString m_serial;
    QString m_token;
    quint16 m_localPort = 0;
    QByteArray m_buffer;
    bool m_authenticated = false;

    bool m_execInProgress = false;
    int m_execId = 0;
    int m_nextExecId = 0;
    QString m_execStdout;
    QString m_execStderr;

    bool m_rootProbe = false;

    struct noise_channel *m_noise = nullptr;
    bool m_handshaking = false;
    QString m_clientPrivateHex;
    QString m_agentPublicHex;

    bool m_pairing = false;
    bool m_allowDangerous = false;
    int m_pairPhase = 0; // 1 = 等 pair_response，2 = 等 pair_ok
    cpace_client_ctx m_pairCtx{};
    uint8_t m_pairClientPrivate[32] = {0};
    uint8_t m_pairClientPublic[32] = {0};
    QString m_pairCode;
    QString m_pairAgentPublicHex;

    void handlePairMessage(const QJsonObject &object);
    void failPairing(const QString &error);
    void sendPairJson(const QJsonObject &object);
};

#endif // ROOT_AGENT_CLIENT_H
