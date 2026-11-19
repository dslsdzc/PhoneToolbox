#include "root_agent_client.h"

#include "secure/noise_channel.h"

#include <sodium.h>

#include <QAbstractSocket>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

namespace {
constexpr int kMaxFrame = 4 * 1024 * 1024;
constexpr char kFrameJson = 0x01;

QString toHex(const uint8_t *data, size_t length)
{
    char buffer[129];
    sodium_bin2hex(buffer, sizeof(buffer), data, length);
    return QString::fromLatin1(buffer);
}

bool fromHex(const QString &hex, uint8_t *out, size_t length)
{
    if (hex.size() != int(length * 2))
        return false;
    const QByteArray bytes = hex.toLatin1();
    return sodium_hex2bin(out, length, bytes.constData(), size_t(bytes.size()),
                          NULL, NULL, NULL) == 0;
}

void pairMac(const uint8_t key[CPACE_KEY_LEN], const uint8_t client[32],
             const uint8_t agent[32], uint8_t out[32])
{
    crypto_auth_hmacsha256_state state;
    crypto_auth_hmacsha256_init(&state, key, CPACE_KEY_LEN);
    crypto_auth_hmacsha256_update(&state, (const unsigned char *)"pair-confirm", 12);
    crypto_auth_hmacsha256_update(&state, client, 32);
    crypto_auth_hmacsha256_update(&state, agent, 32);
    crypto_auth_hmacsha256_final(&state, out);
}
} // namespace

RootAgentClient::RootAgentClient(QObject *parent)
    : QObject(parent)
{
    if (sodium_init() < 0)
        qWarning("libsodium initialization failed");

    m_connectTimer = new QTimer(this);
    m_connectTimer->setSingleShot(true);
    connect(m_connectTimer, &QTimer::timeout, this, [this]() {
        emit errorOccurred(QStringLiteral("连接 agent 超时"));
        cleanupSocket();
    });

    m_handshakeTimer = new QTimer(this);
    m_handshakeTimer->setSingleShot(true);
    connect(m_handshakeTimer, &QTimer::timeout, this, [this]() {
        emit errorOccurred(QStringLiteral("agent 握手超时"));
        onSocketDisconnected();
    });

    m_execTimer = new QTimer(this);
    m_execTimer->setSingleShot(true);
    connect(m_execTimer, &QTimer::timeout, this, &RootAgentClient::onExecTimeout);
}

RootAgentClient::~RootAgentClient()
{
    disconnectFromAgent();
    stopForward();
}

void RootAgentClient::setIdentity(const QString &clientPrivateHex,
                                  const QString &agentPublicHex)
{
    m_clientPrivateHex = clientPrivateHex.trimmed();
    m_agentPublicHex = agentPublicHex.trimmed();
}

bool RootAgentClient::hasIdentity() const
{
    return m_clientPrivateHex.size() == 64 && m_agentPublicHex.size() == 64;
}

void RootAgentClient::pair(const QString &pairCode, int timeoutMs,
                           bool allowDangerous)
{
    if (m_localPort == 0) {
        emit pairingFinished(false, QString(), QString(),
                             QStringLiteral("尚未建立 adb forward"));
        return;
    }
    if (pairCode.trimmed().isEmpty()) {
        emit pairingFinished(false, QString(), QString(),
                             QStringLiteral("配对码为空"));
        return;
    }

    m_pairing = true;
    m_allowDangerous = allowDangerous;
    m_pairPhase = 0;
    m_pairCode = pairCode.trimmed();
    m_pairAgentPublicHex.clear();
    crypto_box_keypair(m_pairClientPublic, m_pairClientPrivate);
    connectToAgent(timeoutMs);
}

void RootAgentClient::sendPairJson(const QJsonObject &object)
{
    const QByteArray json = QJsonDocument(object).toJson(QJsonDocument::Compact);
    sendRawFrame(0x10, json);
}

void RootAgentClient::failPairing(const QString &error)
{
    m_pairing = false;
    emit pairingFinished(false, QString(), QString(), error);
    cleanupSocket();
}

void RootAgentClient::handlePairMessage(const QJsonObject &object)
{
    const QString op = object.value(QStringLiteral("op")).toString();

    if (op == QStringLiteral("error")) {
        failPairing(object.value(QStringLiteral("message")).toString());
        return;
    }

    if (m_pairPhase == 1 && op == QStringLiteral("pair_response")) {
        uint8_t response[CPACE_RESPONSE_LEN];
        uint8_t agentPublic[32];
        if (!fromHex(object.value(QStringLiteral("response")).toString(),
                     response, sizeof(response))
            || !fromHex(object.value(QStringLiteral("agent_static")).toString(),
                        agentPublic, sizeof(agentPublic))) {
            failPairing(QStringLiteral("pair_response 格式错误"));
            return;
        }

        uint8_t clientKey[CPACE_KEY_LEN];
        uint8_t serverKey[CPACE_KEY_LEN];
        if (cpace_client_finish(&m_pairCtx, response, clientKey, serverKey) != 0) {
            failPairing(QStringLiteral("CPace 校验失败（配对码不对？）"));
            return;
        }

        uint8_t mac[32];
        pairMac(clientKey, m_pairClientPublic, agentPublic, mac);

        m_pairAgentPublicHex = toHex(agentPublic, sizeof(agentPublic));
        m_pairPhase = 2;

        QJsonObject confirm;
        confirm[QStringLiteral("op")] = QStringLiteral("pair_confirm");
        confirm[QStringLiteral("mac")] = toHex(mac, sizeof(mac));
        sendPairJson(confirm);
        return;
    }

    if (m_pairPhase == 2 && op == QStringLiteral("pair_ok")) {
        m_pairing = false;
        const QString clientPrivateHex =
            toHex(m_pairClientPrivate, sizeof(m_pairClientPrivate));
        const QString agentPublicHex = m_pairAgentPublicHex;
        // 先关闭配对连接再通知；否则回调里新建的 Noise 连接会被随后到来的
        // disconnectFromAgent() 一起断掉，表现为“配对成功但必须重启手机端服务”。
        cleanupSocket();
        emit pairingFinished(true, clientPrivateHex, agentPublicHex, QString());
        return;
    }

    if (m_pairPhase != 0)
        failPairing(QStringLiteral("意外的配对响应"));
}

bool RootAgentClient::startForward(const QString &adbPath, quint16 devicePort,
                                   QString *error)
{
    if (adbPath.isEmpty() || devicePort == 0) {
        if (error)
            *error = QStringLiteral("adb 路径或设备端口为空");
        return false;
    }

    stopForward();

    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        if (error)
            *error = QStringLiteral("找不到空闲本地端口");
        return false;
    }
    m_localPort = probe.serverPort();
    probe.close();

    m_adb = new QProcess(this);
    QStringList args;
    if (!m_serial.isEmpty())
        args << "-s" << m_serial;
    args << QStringLiteral("forward")
         << QStringLiteral("tcp:%1").arg(m_localPort)
         << QStringLiteral("tcp:%1").arg(devicePort);

    m_adb->start(adbPath, args);
    if (!m_adb->waitForFinished(5000)) {
        if (error)
            *error = QStringLiteral("adb forward 超时");
        stopForward();
        return false;
    }
    if (m_adb->exitCode() != 0) {
        if (error) {
            *error = QString::fromLocal8Bit(m_adb->readAllStandardError()).trimmed();
            if (error->isEmpty())
                *error = QStringLiteral("adb forward 失败");
        }
        stopForward();
        return false;
    }
    return true;
}

void RootAgentClient::stopForward()
{
    if (!m_adb)
        return;

    if (m_localPort != 0) {
        QProcess remove;
        QStringList args;
        if (!m_serial.isEmpty())
            args << "-s" << m_serial;
        args << QStringLiteral("forward") << QStringLiteral("--remove")
             << QStringLiteral("tcp:%1").arg(m_localPort);
        remove.start(m_adb->program(), args);
        remove.waitForFinished(3000);
    }

    m_adb->deleteLater();
    m_adb = nullptr;
    m_localPort = 0;
}

void RootAgentClient::connectToAgent(int timeoutMs)
{
    if (m_localPort == 0) {
        emit errorOccurred(QStringLiteral("尚未建立 adb forward"));
        return;
    }

    if (m_socket) {
        // cleanupSocket() resets pairing state; preserve the CPace intent that
        // pair() set before calling us.
        const bool pairing = m_pairing;
        cleanupSocket();
        m_pairing = pairing;
    }

    m_buffer.clear();
    m_authenticated = false;
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected,
            this, &RootAgentClient::onSocketConnected);
    connect(m_socket, &QTcpSocket::readyRead,
            this, &RootAgentClient::onReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred,
            this, &RootAgentClient::onSocketError);
    connect(m_socket, &QTcpSocket::disconnected,
            this, &RootAgentClient::onSocketDisconnected);

    m_connectTimer->start(timeoutMs);
    m_socket->connectToHost(QHostAddress::LocalHost, m_localPort);
}

void RootAgentClient::disconnectFromAgent()
{
    m_connectTimer->stop();
    m_handshakeTimer->stop();
    m_execTimer->stop();
    cleanupSocket();
    emit disconnected();
}

void RootAgentClient::onSocketConnected()
{
    m_connectTimer->stop();
    // The TCP socket may be accepted while the peer never finishes CPace or
    // Noise; do not let the UI stay in "connecting" forever.
    m_handshakeTimer->start(10000);

    if (m_pairing) {
        static const char ad[] = "PhoneToolbox/root-agent/1";
        const QByteArray code = m_pairCode.toUtf8();
        uint8_t public_data[CPACE_PUBLIC_DATA_LEN];
        if (cpace_client_start(&m_pairCtx, code.constData(), size_t(code.size()),
                               "desktop", "agent", ad, sizeof(ad) - 1,
                               public_data) != 0) {
            failPairing(QStringLiteral("CPace 初始化失败"));
            return;
        }

        QJsonObject start;
        start[QStringLiteral("op")] = QStringLiteral("pair_start");
        start[QStringLiteral("public_data")] = toHex(public_data, sizeof(public_data));
        start[QStringLiteral("client_static")] = toHex(m_pairClientPublic,
                                                      sizeof(m_pairClientPublic));
        start[QStringLiteral("client_name")] = QStringLiteral("PhoneToolbox");
        start[QStringLiteral("capabilities")] = m_allowDangerous
            ? QStringLiteral("exec,push,pull,dangerous")
            : QStringLiteral("exec,push,pull");
        sendPairJson(start);
        m_pairPhase = 1;
        return;
    }

    if (hasIdentity()) {
        uint8_t private_key[32];
        uint8_t public_key[32];
        const QByteArray private_hex = m_clientPrivateHex.toLatin1();
        const QByteArray agent_hex = m_agentPublicHex.toLatin1();
        if (sodium_hex2bin(private_key, sizeof(private_key),
                           private_hex.constData(), 64, NULL, NULL, NULL) != 0
            || sodium_hex2bin(public_key, sizeof(public_key),
                              agent_hex.constData(), 64, NULL, NULL, NULL) != 0) {
            emit errorOccurred(QStringLiteral("Noise 身份密钥格式错误"));
            cleanupSocket();
            return;
        }

        static const char prologue[] = "PhoneToolbox/root-agent/1";
        noise_channel channel;
        char error[256] = {0};
        if (noise_channel_init(&channel, 1, private_key, public_key,
                               prologue, sizeof(prologue) - 1,
                               error, sizeof(error)) != 0) {
            emit errorOccurred(QString::fromUtf8(error));
            cleanupSocket();
            return;
        }

        uint8_t msg1[NOISE_MAX_HANDSHAKE_MSG];
        size_t msg1_length = 0;
        if (noise_channel_write_handshake(&channel, msg1, &msg1_length,
                                          sizeof(msg1), error, sizeof(error)) != 0) {
            noise_channel_free(&channel);
            emit errorOccurred(QString::fromUtf8(error));
            cleanupSocket();
            return;
        }

        m_noise = new noise_channel(channel);
        m_handshaking = true;
        sendRawFrame(0x11, QByteArray(reinterpret_cast<const char *>(msg1),
                                      int(msg1_length)));
        return;
    }

    // 明文 token 只用于开发模式。
    QJsonObject auth;
    auth[QStringLiteral("op")] = QStringLiteral("auth");
    auth[QStringLiteral("token")] = m_token;
    sendJson(auth);
}

void RootAgentClient::onSocketError()
{
    if (m_socket)
        emit errorOccurred(m_socket->errorString());
    onSocketDisconnected();
}

void RootAgentClient::onSocketDisconnected()
{
    const bool hadSocket = (m_socket != nullptr);
    cleanupSocket();
    if (hadSocket)
        emit disconnected();
}

void RootAgentClient::onReadyRead()
{
    if (!m_socket)
        return;

    m_buffer.append(m_socket->readAll());

    while (m_buffer.size() >= 4) {
        const quint32 length =
            (quint32(quint8(m_buffer.at(0))) << 24)
            | (quint32(quint8(m_buffer.at(1))) << 16)
            | (quint32(quint8(m_buffer.at(2))) << 8)
            | quint32(quint8(m_buffer.at(3)));

        if (length == 0 || length > quint32(kMaxFrame)) {
            emit errorOccurred(QStringLiteral("agent 返回了非法帧长度"));
            cleanupSocket();
            return;
        }
        if (m_buffer.size() < int(4 + length))
            break;

        QByteArray payload = m_buffer.mid(4, int(length));
        m_buffer.remove(0, int(4 + length));

        if (m_handshaking) {
            if (payload.isEmpty() || quint8(payload.at(0)) != 0x11) {
                emit errorOccurred(QStringLiteral("Noise 握手帧类型错误"));
                cleanupSocket();
                return;
            }
            char error[256] = {0};
            if (!m_noise
                || noise_channel_read_handshake(
                       m_noise,
                       reinterpret_cast<const uint8_t *>(payload.constData()) + 1,
                       size_t(payload.size() - 1), error, sizeof(error)) != 0
                || noise_channel_finish_handshake(m_noise, error, sizeof(error)) != 0) {
                emit errorOccurred(QString::fromUtf8(error));
                cleanupSocket();
                return;
            }
            m_handshaking = false;
            m_authenticated = true;
            m_handshakeTimer->stop();
            emit connected();
            continue;
        }

        if (m_noise) {
            if (payload.isEmpty() || quint8(payload.at(0)) != 0x12) {
                emit errorOccurred(QStringLiteral("Noise 传输帧类型错误"));
                cleanupSocket();
                return;
            }
            QByteArray plain(payload.size(), '\0');
            size_t plain_length = 0;
            char error[128] = {0};
            if (noise_channel_decrypt(
                    m_noise,
                    reinterpret_cast<const uint8_t *>(payload.constData()) + 1,
                    size_t(payload.size() - 1),
                    reinterpret_cast<uint8_t *>(plain.data()), &plain_length,
                    size_t(plain.size()), error, sizeof(error)) != 0) {
                emit errorOccurred(QString::fromUtf8(error));
                cleanupSocket();
                return;
            }
            plain.resize(int(plain_length));
            payload = plain;
        }

        if (payload.isEmpty() || quint8(payload.at(0)) != kFrameJson)
            continue;

        QJsonParseError parseError;
        const QJsonDocument document =
            QJsonDocument::fromJson(payload.mid(1), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject())
            continue;

        if (m_pairing)
            handlePairMessage(document.object());
        else
            handleMessage(document.object());
    }
}

void RootAgentClient::handleMessage(const QJsonObject &object)
{
    const QString op = object.value(QStringLiteral("op")).toString();

    if (op == QStringLiteral("auth_ok")) {
        m_authenticated = true;
        m_connectTimer->stop();
        m_handshakeTimer->stop();
        emit connected();
        return;
    }

    if (op == QStringLiteral("error")) {
        const QString code = object.value(QStringLiteral("code")).toString();
        const QString message = object.value(QStringLiteral("message")).toString();
        emit errorOccurred(QStringLiteral("%1: %2").arg(code, message));
        if (!m_authenticated)
            cleanupSocket();
        return;
    }

    if (op == QStringLiteral("stdout") || op == QStringLiteral("stderr")) {
        if (!m_execInProgress)
            return;
        const QByteArray bytes =
            QByteArray::fromBase64(object.value(QStringLiteral("data")).toString().toLatin1());
        if (op == QStringLiteral("stdout"))
            m_execStdout += QString::fromUtf8(bytes);
        else
            m_execStderr += QString::fromUtf8(bytes);
        return;
    }

    if (op == QStringLiteral("exit")) {
        if (!m_execInProgress)
            return;
        const bool timedOut = object.value(QStringLiteral("timed_out")).toBool();
        const bool canceled = object.value(QStringLiteral("canceled")).toBool();
        finishExec(object.value(QStringLiteral("code")).toInt(-1),
                   timedOut || canceled);
        return;
    }
}

void RootAgentClient::exec(const QStringList &argv, int timeoutMs)
{
    if (!m_authenticated) {
        emit errorOccurred(QStringLiteral("尚未连接到 agent"));
        return;
    }
    if (argv.isEmpty()) {
        emit errorOccurred(QStringLiteral("命令为空"));
        return;
    }
    if (m_execInProgress) {
        emit errorOccurred(QStringLiteral("上一条命令还没结束"));
        return;
    }

    m_execInProgress = true;
    m_execId = ++m_nextExecId;
    m_execStdout.clear();
    m_execStderr.clear();

    QJsonArray arguments;
    for (const QString &argument : argv)
        arguments.append(argument);

    QJsonObject object;
    object[QStringLiteral("op")] = QStringLiteral("exec");
    object[QStringLiteral("id")] = m_execId;
    object[QStringLiteral("argv")] = arguments;
    object[QStringLiteral("timeout_ms")] = timeoutMs;
    sendJson(object);

    // 客户端兜底：agent 侧就算没回，也不能让 UI 永远等。
    m_execTimer->start(timeoutMs + 5000);
}

void RootAgentClient::execShell(const QString &command, int timeoutMs)
{
    if (!m_authenticated) {
        emit errorOccurred(QStringLiteral("尚未连接到 agent"));
        return;
    }
    if (command.trimmed().isEmpty()) {
        emit errorOccurred(QStringLiteral("命令为空"));
        return;
    }
    if (m_execInProgress) {
        emit errorOccurred(QStringLiteral("上一条命令还没结束"));
        return;
    }

    m_execInProgress = true;
    m_execId = ++m_nextExecId;
    m_execStdout.clear();
    m_execStderr.clear();

    QJsonObject object;
    object[QStringLiteral("op")] = QStringLiteral("exec");
    object[QStringLiteral("id")] = m_execId;
    object[QStringLiteral("shell")] = true;
    object[QStringLiteral("command")] = command;
    object[QStringLiteral("timeout_ms")] = timeoutMs;
    sendJson(object);

    m_execTimer->start(timeoutMs + 5000);
}

void RootAgentClient::detectRoot()
{
    m_rootProbe = true;
    // 用 PATH 里的 id：Android 的 /system/bin 和本机测试环境都能命中，
    // 不把路径写死成 /system/bin/id。
    exec({QStringLiteral("id")}, 5000);
}

void RootAgentClient::cancelExec(int execId)
{
    if (!m_authenticated || execId <= 0)
        return;
    QJsonObject object;
    object[QStringLiteral("op")] = QStringLiteral("cancel");
    object[QStringLiteral("id")] = execId;
    sendJson(object);
}

void RootAgentClient::sendJson(const QJsonObject &object)
{
    const QByteArray json = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray payload;
    payload.reserve(json.size() + 1);
    payload.append(kFrameJson);
    payload.append(json);
    sendPayload(payload);
}

void RootAgentClient::sendPayload(const QByteArray &payload)
{
    if (payload.isEmpty())
        return;

    if (!m_noise) {
        sendRawFrame(quint8(payload.at(0)), payload.mid(1));
        return;
    }

    QByteArray cipher(payload.size() + NOISE_TAG_LEN, '\0');
    size_t cipher_length = 0;
    char error[128] = {0};
    if (noise_channel_encrypt(m_noise,
                              reinterpret_cast<const uint8_t *>(payload.constData()),
                              size_t(payload.size()),
                              reinterpret_cast<uint8_t *>(cipher.data()),
                              &cipher_length, size_t(cipher.size()),
                              error, sizeof(error)) != 0) {
        emit errorOccurred(QString::fromUtf8(error));
        return;
    }
    cipher.resize(int(cipher_length));
    sendRawFrame(0x12, cipher);
}

void RootAgentClient::sendRawFrame(quint8 type, const QByteArray &data)
{
    if (!m_socket || m_socket->state() != QAbstractSocket::ConnectedState)
        return;

    const quint32 length = quint32(data.size() + 1);
    QByteArray frame;
    frame.reserve(int(length) + 4);
    frame.append(char((length >> 24) & 0xff));
    frame.append(char((length >> 16) & 0xff));
    frame.append(char((length >> 8) & 0xff));
    frame.append(char(length & 0xff));
    frame.append(char(type));
    frame.append(data);
    m_socket->write(frame);
}

void RootAgentClient::finishExec(int exitCode, bool timedOut)
{
    m_execTimer->stop();
    m_execInProgress = false;

    const QString stdoutText = m_execStdout;
    const QString stderrText = m_execStderr;
    m_execStdout.clear();
    m_execStderr.clear();

    if (m_rootProbe) {
        m_rootProbe = false;
        emit rootDetected(stdoutText.contains(QStringLiteral("uid=0")),
                          stdoutText.trimmed());
    }

    emit execFinished(exitCode, stdoutText, stderrText, timedOut);
}

void RootAgentClient::onExecTimeout()
{
    emit errorOccurred(QStringLiteral("命令超时（客户端兜底）"));
    cancelExec(m_execId);
    finishExec(-1, true);
}

void RootAgentClient::cleanupSocket()
{
    m_connectTimer->stop();
    m_handshakeTimer->stop();
    m_execTimer->stop();
    m_authenticated = false;
    m_handshaking = false;
    m_pairing = false;
    m_pairPhase = 0;
    m_execInProgress = false;
    m_buffer.clear();
    m_execStdout.clear();
    m_execStderr.clear();

    if (m_noise) {
        noise_channel_free(m_noise);
        delete m_noise;
        m_noise = nullptr;
    }

    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
}
