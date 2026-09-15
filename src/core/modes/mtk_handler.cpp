#include "mtk_handler.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>
#include <QResource>
#include <QTemporaryDir>
#include <memory>

#include "core/modes/mtk_brom.h"
#include "core/modes/mtk_payload.h"   // bromFlashOnSession + 请求/回调类型

// JSON-RPC timeout defaults
static const int BRIDGE_START_TIMEOUT = 5000;
static const int CONNECT_TIMEOUT = 120000;
static const int SECCFG_TIMEOUT = 120000;
static const int PRINTGPT_TIMEOUT = 60000;
static const int READ_PARTITION_TIMEOUT = 300000;
static const int WRITE_PARTITION_TIMEOUT = 300000;
static const int RESET_TIMEOUT = 30000;
static const int DEFAULT_CMD_TIMEOUT = 120000;
static const int BRIDGE_RESPONSE_TIMEOUT = 2000;

MtkHandler::MtkHandler(QObject *parent)
    : QObject(parent)
    , m_initialized(false)
    , m_connected(false)
    , m_requestId(0)
    , m_process(nullptr)
{
}

MtkHandler::~MtkHandler()
{
    stopBridge();
}

bool MtkHandler::extractBridge()
{
#ifdef Q_OS_WIN
    QString resourcePath = QString(":/binaries/mtk_bridge/windows/x64/mtk_bridge.exe");
#elif defined(Q_OS_MACOS)
    QString resourcePath = QString(":/binaries/mtk_bridge/macos/x64/mtk_bridge");
#else
    QString resourcePath = QString(":/binaries/mtk_bridge/linux/x64/mtk_bridge");
#endif

    // Try Qt resource first (embedded via .qrc)
    QFile resFile(resourcePath);
    if (resFile.open(QIODevice::ReadOnly)) {
        QByteArray data = resFile.readAll();
        resFile.close();

        if (data.size() > 0) {
            // Write to temp file
            QTemporaryDir *tmpDir = new QTemporaryDir();
            if (tmpDir->isValid()) {
                QString outPath = tmpDir->filePath(getPlatformBinaryName());
                QFile outFile(outPath);
                if (outFile.open(QIODevice::WriteOnly)) {
                    outFile.write(data);
                    outFile.close();
                    outFile.setPermissions(QFile::ExeOwner | QFile::ReadOwner |
                                           QFile::ExeGroup | QFile::ReadGroup);
                    m_bridgePath = outPath;
                    return true;
                }
            }
            delete tmpDir;
        }
    }

    // Fallback: search filesystem paths (development mode)
    QString bridgeName = getPlatformBinaryName();
#ifdef Q_OS_WIN
    QString platformDir = "windows/x64";
#elif defined(Q_OS_MACOS)
    QString platformDir = "macos/x64";
#else
    QString platformDir = "linux/x64";
#endif

    QStringList searchPaths = {
        QCoreApplication::applicationDirPath() + "/../third_party/mtk_bridge/" + platformDir + "/" + bridgeName,
        QCoreApplication::applicationDirPath() + "/../../third_party/mtk_bridge/" + platformDir + "/" + bridgeName,
        QDir::currentPath() + "/third_party/mtk_bridge/" + platformDir + "/" + bridgeName,
        QCoreApplication::applicationDirPath() + "/" + bridgeName,
        QDir::currentPath() + "/" + bridgeName,
    };

    for (const QString &path : searchPaths) {
        if (QFileInfo::exists(path)) {
            m_bridgePath = path;
            return true;
        }
    }

    m_lastError = "mtk_bridge binary not found in Qt resources or filesystem";
    return false;
}

bool MtkHandler::initialize()
{
    if (!extractBridge()) {
        emit outputMessage("MTK: " + m_lastError, true);
        return false;
    }

    m_initialized = true;
    return true;
}

QString MtkHandler::getPlatformBinaryName() const
{
#ifdef Q_OS_WIN
    return "mtk_bridge.exe";
#else
    return "mtk_bridge";
#endif
}

bool MtkHandler::startBridge()
{
    if (m_process && m_process->state() == QProcess::Running)
        return true;

    if (!m_initialized && !initialize())
        return false;

    // Clean up old process
    stopBridge();

    m_process = new QProcess(this);
    m_readBuffer.clear();

    // Connect stdout for response reading
    QObject::connect(m_process, &QProcess::readyReadStandardOutput, this, [this]() {
        m_readBuffer.append(m_process->readAllStandardOutput());

        // Process complete JSON lines
        while (true) {
            int nlIdx = m_readBuffer.indexOf('\n');
            if (nlIdx < 0)
                break;

            QByteArray line = m_readBuffer.left(nlIdx).trimmed();
            m_readBuffer.remove(0, nlIdx + 1);

            if (line.isEmpty())
                continue;

            // Handle progress notifications
            QJsonDocument doc = QJsonDocument::fromJson(line);
            if (doc.isObject()) {
                QJsonObject obj = doc.object();
                if (obj.contains("method") && obj["method"].toString() == "__progress") {
                    QJsonObject params = obj["params"].toObject();
                    int pct = params["percent"].toInt();
                    QString msg = params["message"].toString();
                    if (!msg.isEmpty())
                        emit outputMessage("MTK: " + msg, false);
                    emit progress(pct);
                }
            }
        }
    });

    // Forward stderr as debug messages
    QObject::connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        // stderr from bridge is debug info, forward to output
        // (but don't flood the UI with Python logging)
    });

    // Handle process exit
    QObject::connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this](int exitCode, QProcess::ExitStatus status) {
        Q_UNUSED(status);
        if (exitCode != 0) {
            m_connected = false;
        }
    });

    m_process->setProgram(m_bridgePath);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    emit outputMessage("MTK: 启动 Bridge 进程...", false);
    m_process->start();

    if (!m_process->waitForStarted(BRIDGE_START_TIMEOUT)) {
        m_lastError = "无法启动 mtk_bridge: " + m_process->errorString();
        emit outputMessage("MTK: " + m_lastError, true);
        stopBridge();
        return false;
    }

    // Wait for the "ready" signal from bridge
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < BRIDGE_START_TIMEOUT) {
        if (m_process->waitForReadyRead(200)) {
            m_readBuffer.append(m_process->readAllStandardOutput());
            // Check for readiness: bridge prints "mtk_bridge: ready" to stderr
            if (m_process->readAllStandardError().contains("ready"))
                break;
        }
        if (m_process->state() != QProcess::Running)
            break;
    }

    emit outputMessage("MTK: Bridge 进程已就绪", false);
    return true;
}

void MtkHandler::stopBridge()
{
    if (m_process) {
        if (m_process->state() == QProcess::Running) {
            m_process->terminate();
            if (!m_process->waitForFinished(3000)) {
                m_process->kill();
                m_process->waitForFinished(2000);
            }
        }
        delete m_process;
        m_process = nullptr;
    }
    m_readBuffer.clear();
    m_connected = false;
}

QJsonObject MtkHandler::sendRequest(const QString &method, const QJsonObject &params,
                                     int timeoutMs)
{
    QJsonObject response;
    response["error"] = QJsonObject{{"code", -99}, {"message", "No response"}};

    if (!m_process || m_process->state() != QProcess::Running) {
        if (!startBridge()) {
            response["error"] = QJsonObject{{"code", -1}, {"message", m_lastError}};
            return response;
        }
    }

    int id = ++m_requestId;
    QJsonObject request;
    request["id"] = id;
    request["method"] = method;
    request["params"] = params;

    QByteArray requestData = QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n";

    // Write request to stdin
    m_process->write(requestData);
    if (!m_process->waitForBytesWritten(2000)) {
        response["error"] = QJsonObject{{"code", -2}, {"message", "Failed to write to bridge"}};
        return response;
    }

    // Read response (one JSON line per response)
    QByteArray responseLine;
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        // Check if we have a complete line in the buffer
        while (true) {
            int nlIdx = m_readBuffer.indexOf('\n');
            if (nlIdx < 0)
                break;

            QByteArray line = m_readBuffer.left(nlIdx).trimmed();
            m_readBuffer.remove(0, nlIdx + 1);

            if (line.isEmpty())
                continue;

            // Check for progress notifications
            QJsonDocument doc = QJsonDocument::fromJson(line);
            if (doc.isObject()) {
                QJsonObject obj = doc.object();

                // Progress events (unsolicited)
                if (obj.contains("method") && obj["method"].toString() == "__progress") {
                    QJsonObject p = obj["params"].toObject();
                    int pct = p["percent"].toInt();
                    QString msg = p["message"].toString();
                    if (!msg.isEmpty())
                        emit outputMessage("MTK: " + msg, false);
                    emit progress(pct);
                    continue;
                }

                // Response with matching ID
                if (obj.contains("id") && obj["id"].toInt() == id) {
                    return obj;
                }

                // Response without ID? keep it anyway
                if (obj.contains("result") || obj.contains("error")) {
                    return obj;
                }
            }
        }

        // Wait for more data
        if (!m_process->waitForReadyRead(200)) {
            if (m_process->state() != QProcess::Running)
                break;
        }
    }

    if (timer.elapsed() >= timeoutMs) {
        response["error"] = QJsonObject{{"code", -3}, {"message", "Request timed out"}};
    }

    return response;
}

// ==================== Public API ====================

bool MtkHandler::connect()
{
    if (m_connected) {
        emit outputMessage("MTK: 已经连接", false);
        return true;
    }

    emit outputMessage("MTK: 尝试连接设备...", false);
    emit progress(10);

    QJsonObject resp = sendRequest("connect", QJsonObject{}, CONNECT_TIMEOUT);

    if (resp.contains("error")) {
        QString err = resp["error"].toObject()["message"].toString();
        m_lastError = err;
        emit outputMessage("MTK: 连接失败 - " + err, true);
        emit progress(0);
        return false;
    }

    QString chip = resp["result"].toObject()["chip"].toString();
    emit outputMessage(QString("MTK: 设备已连接 - %1").arg(chip), false);
    m_connected = true;
    emit progress(100);
    return true;
}

void MtkHandler::disconnect()
{
    if (m_process && m_process->state() == QProcess::Running) {
        QJsonObject resp = sendRequest("disconnect", QJsonObject{}, 5000);
        Q_UNUSED(resp);
    }
    m_connected = false;
}

bool MtkHandler::seccfgUnlock()
{
    if (!m_connected) {
        m_lastError = "MTK 未连接";
        return false;
    }

    emit outputMessage("MTK: 解锁 Bootloader (seccfg)...", false);
    emit progress(10);

    QJsonObject params;
    params["lock"] = false;
    QJsonObject resp = sendRequest("seccfg", params, SECCFG_TIMEOUT);

    if (resp.contains("result")) {
        emit outputMessage("MTK: Bootloader 已解锁", false);
        emit progress(100);
        return true;
    }

    QString err = resp["error"].toObject()["message"].toString();
    m_lastError = err;
    emit outputMessage("MTK: 解锁失败 - " + err, true);
    emit progress(0);
    return false;
}

bool MtkHandler::seccfgLock()
{
    if (!m_connected) {
        m_lastError = "MTK 未连接";
        return false;
    }

    emit outputMessage("MTK: 回锁 Bootloader (seccfg)...", false);
    emit progress(10);

    QJsonObject params;
    params["lock"] = true;
    QJsonObject resp = sendRequest("seccfg", params, SECCFG_TIMEOUT);

    if (resp.contains("result")) {
        emit outputMessage("MTK: Bootloader 已回锁", false);
        emit progress(100);
        return true;
    }

    QString err = resp["error"].toObject()["message"].toString();
    m_lastError = err;
    emit outputMessage("MTK: 回锁失败 - " + err, true);
    emit progress(0);
    return false;
}

QList<MtkPartition> MtkHandler::listPartitions()
{
    QList<MtkPartition> parts;

    if (!m_connected) {
        emit outputMessage("MTK 未连接", true);
        return parts;
    }

    emit outputMessage("MTK: 读取分区表...", false);
    emit progress(20);

    QJsonObject resp = sendRequest("printgpt", QJsonObject{}, PRINTGPT_TIMEOUT);

    if (resp.contains("error")) {
        QString err = resp["error"].toObject()["message"].toString();
        emit outputMessage("MTK: 读取分区表失败 - " + err, true);
        emit progress(0);
        return parts;
    }

    QJsonObject result = resp["result"].toObject();
    QJsonArray partitions = result["partitions"].toArray();

    for (const QJsonValue &val : partitions) {
        QJsonObject p = val.toObject();
        MtkPartition mp;
        mp.name = p["name"].toString();
        mp.offset = static_cast<quint64>(p["offset"].toDouble());
        mp.length = static_cast<quint64>(p["length"].toDouble());
        parts.append(mp);
    }

    emit outputMessage(QString("MTK: 检测到 %1 个分区").arg(parts.size()), false);
    emit progress(100);
    return parts;
}

bool MtkHandler::readPartition(const QString &partition, const QString &outputPath)
{
    if (!m_connected) {
        emit outputMessage("MTK 未连接", true);
        return false;
    }

    emit outputMessage(QString("MTK: 读取分区 %1").arg(partition), false);
    emit progress(10);

    QJsonObject params;
    params["partition"] = partition;
    params["output_path"] = outputPath;
    QJsonObject resp = sendRequest("read_partition", params, READ_PARTITION_TIMEOUT);

    if (resp.contains("result")) {
        emit outputMessage(QString("MTK: 分区 %1 已保存到 %2").arg(partition, outputPath), false);
        emit progress(100);
        return true;
    }

    QString err = resp["error"].toObject()["message"].toString();
    m_lastError = err;
    emit outputMessage("MTK: 读取失败 - " + err, true);
    emit progress(0);
    return false;
}

bool MtkHandler::writePartition(const QString &partition, const QString &imagePath)
{
    if (!m_connected) {
        emit outputMessage("MTK 未连接", true);
        return false;
    }

    QFileInfo fi(imagePath);
    if (!fi.exists()) {
        emit outputMessage(QString("文件不存在: %1").arg(imagePath), true);
        return false;
    }

    emit outputMessage(QString("MTK: 写入分区 %1 <- %2").arg(partition, fi.fileName()), false);
    emit progress(10);

    QJsonObject params;
    params["partition"] = partition;
    params["image_path"] = imagePath;
    QJsonObject resp = sendRequest("write_partition", params, WRITE_PARTITION_TIMEOUT);

    if (resp.contains("result")) {
        emit outputMessage("MTK: 写入完成", false);
        emit progress(100);
        return true;
    }

    QString err = resp["error"].toObject()["message"].toString();
    m_lastError = err;
    emit outputMessage("MTK: 写入失败 - " + err, true);
    emit progress(0);
    return false;
}

bool MtkHandler::readAllPartitions(const QString &directory)
{
    if (!m_connected) {
        emit outputMessage("MTK 未连接", true);
        return false;
    }

    emit outputMessage(QString("MTK: 备份全部分区到 %1").arg(directory), false);
    emit progress(5);

    QJsonObject params;
    params["directory"] = directory;
    QJsonObject resp = sendRequest("read_all_partitions", params, 600000);

    if (resp.contains("result")) {
        emit outputMessage("MTK: 全部分区备份完成", false);
        emit progress(100);
        return true;
    }

    QString err = resp["error"].toObject()["message"].toString();
    m_lastError = err;
    emit outputMessage("MTK: 备份失败 - " + err, true);
    emit progress(0);
    return false;
}

bool MtkHandler::resetDevice()
{
    if (!m_connected) {
        emit outputMessage("MTK 未连接", true);
        return false;
    }

    emit outputMessage("MTK: 重置设备...", false);

    QJsonObject resp = sendRequest("reset", QJsonObject{}, RESET_TIMEOUT);

    if (resp.contains("result")) {
        emit outputMessage("MTK: 重置命令已发送", false);
        m_connected = false;
        return true;
    }

    QString err = resp["error"].toObject()["message"].toString();
    emit outputMessage("MTK: 重置失败 - " + err, true);
    return false;
}

// ==================== F1-3: BROM 直刷路由（D1-T9 落地） ====================

// 本函数只剩真机段（枚举 / libusb 打开 / 握手）；刷写主体在 mtkbrom::bromFlashOnSession
// （mtk_payload.cpp —— 抽出它是为了能被离线用例覆盖，见 T9 审查 I1）。
// 诚实边界：本段离线不可验证。失败即停、**不复位**（与 Phase B/C 同口径）。
// 不改动现有 mtk_bridge JSON-RPC 主路径（上方 MtkHandler 成员方法保持原行为）。
bool runBromFlash(const mtkbrom::BromFlashRequest &req, const mtkbrom::BromLogFn &log,
                  const mtkbrom::BromProgressFn &progress, QString *error)
{
    QList<mtkbrom::BromDevice> devs;
    if (!mtkbrom::enumerateUsb(devs, error))
        return false;
    if (devs.isEmpty()) {
        if (error) *error = QStringLiteral("未检测到 MTK BROM 设备（VID 0x0E8D:0x0003 等）");
        return false;
    }
    std::unique_ptr<mtkbrom::IBromUsb> usb;
    if (!mtkbrom::openLibusbUsb(devs.first(), usb, error))
        return false;
    mtkbrom::BromSession session(std::move(usb), devs.first());
    if (!session.connect(error))
        return false;
    return mtkbrom::bromFlashOnSession(session, req, log, progress, error);
}
