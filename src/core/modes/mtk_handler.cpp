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
#include <utility>   // std::as_const（遍历 Qt 容器，不得用 qAsConst）

#include "core/modes/mtk_brom.h"
#include "core/modes/mtk_chip_table.h"   // lookupChip / DaMode / damodeName（代际判定）
#include "core/modes/mtk_da_file.h"
#include "core/modes/mtk_emmc.h"
#include "core/modes/mtk_payload.h"
#include "core/modes/mtk_preloader_fetch.h"
#include "core/mtk_flash_plan.h"

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

namespace {

// 设备实读分区表 → 计划层参照表（**写入判据以设备为准**；预览期的 scatter 到这里可能对不上）
QList<mtkplan::PartitionRef> toPartitionRefs(const QList<mtkbrom::EmPartition> &parts)
{
    QList<mtkplan::PartitionRef> out;
    out.reserve(parts.size());
    for (const mtkbrom::EmPartition &p : std::as_const(parts)) {
        mtkplan::PartitionRef r;
        r.name = p.name;
        r.sizeBytes = p.sizeBytes;
        out << r;
    }
    return out;
}

} // namespace

// 诚实边界：枚举/打开/握手段（libusb 真机路径）离线不可验证；被验证的是它下面两层
// （bromBringUpDa 逐帧 + 计划层）。失败即停、**不复位**（与 Phase B/C 同口径）。
// 不改动现有 mtk_bridge JSON-RPC 主路径（上方 MtkHandler 成员方法保持原行为）。
bool runBromFlash(const mtkbrom::BromFlashRequest &req, const mtkbrom::BromLogFn &log,
                  const mtkbrom::BromProgressFn &progress, QString *error)
{
    auto say = [&log](const QString &m) { if (log) log(m, false); };
    auto warn = [&log](const QString &m) { if (log) log(m, true); };

    if (req.daFile.isEmpty()) {
        if (error) *error = QStringLiteral("DA 文件为空");
        return false;
    }
    if (req.imagePaths.isEmpty()) {
        if (error) *error = QStringLiteral("未选择任何镜像文件（mtk-brom 通道）");
        return false;
    }

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
    mtkbrom::TargetConfig cfg;
    if (session.getTargetConfig(cfg, nullptr) && (cfg.sla || cfg.daa)) {
        if (error) *error = QStringLiteral("设备启用 SLA/DAA 认证，暂不支持（RSA 响应自研为后续任务）");
        return false;
    }

    // hwcode / 版本 → DA 条目选择（5 元组；判不出 → 明确报错，不猜）
    // ⚠️ 版本口径（T4 实施期核上游 `mtk_preloader.py:174-232`）：
    //   • 0xFD 的**低 16 位 hwver** 只用于"关看门狗"等前置动作（`setreg_disablewatchdogtimer(hwcode, hwver)`）
    //     —— 本实现不做那些前置动作，故 hwVer 只读出、不参与判定；
    //   • 非 IoT 芯片的 `hwver/swver` **以 0xFC 为准**（上游在 0xFC 段把两者**清零后重填**：
    //     `hwver = 0; swver = 0; if res != -1: hw_sub_code = res[0]; hwver = res[1]; swver = res[2]`）；
    //   • 0xFC 失败（-1）时上游**不清零就往下走** → hwver/swver 保持 0 → 版本过滤维**旁路**（`or … == 0`）
    //     → 取首个候选。故这里**不因 0xFC 失败而中止**：记告警 + 用 0/0（与上游同姿态；IoT 读 A2 寄存器的
    //     另一条路我们已在芯片表 iot 位处拒绝）。
    quint16 hwCode = 0, hwVer = 0;
    if (!session.getHwCode(hwCode, hwVer, error))
        return false;
    mtkbrom::HwSwVer sw;
    if (!session.getHwSwVer(sw, error)) {
        warn(QStringLiteral("读取 0xFC（hw/sw 版本）失败：%1 —— 版本过滤维按上游口径旁路（hwver/swver = 0）")
                 .arg(error ? *error : QString()));
        if (error) error->clear();
        sw = mtkbrom::HwSwVer{};
    }
    quint8 bromVer = 0;
    quint8 blVer = 0;
    if (!session.getBromVer(bromVer, error))          // stage2 配置要写这两个值（顺序敏感）
        return false;
    if (!session.getBlVer(blVer, error))
        return false;
    say(QStringLiteral("芯片 hw_code=0x%1（hw_ver=0x%2，sw_ver=0x%3；BROM 0x%4 / BL 0x%5）")
            .arg(hwCode, 4, 16, QLatin1Char('0')).arg(sw.hwVer, 4, 16, QLatin1Char('0'))
            .arg(sw.swVer, 4, 16, QLatin1Char('0'))
            .arg(bromVer, 2, 16, QLatin1Char('0')).arg(blVer, 2, 16, QLatin1Char('0')));

    // 代际判定（spec §1）：表外芯片 / 非 LEGACY 代 → **明确报错，不猜**（D2/D3 才做另两代）
    const mtkbrom::ChipInfo *chip = mtkbrom::lookupChip(hwCode);
    if (!chip) {
        if (error) *error = QStringLiteral("芯片表未收录 hw_code=0x%1 —— 判不出 DA 代际（不猜）；"
                                           "补 tools/gen_mtk_chip_table.py 对应表项并重新生成后可支持")
                                .arg(hwCode, 4, 16, QLatin1Char('0'));
        return false;
    }
    if (chip->damode != mtkbrom::DaMode::Legacy) {
        if (error) *error = QStringLiteral("本设备 hw_code=0x%1 属 %2 代 —— Phase D1 只支持 LEGACY（D2/D3 另做）")
                                .arg(hwCode, 4, 16, QLatin1Char('0')).arg(mtkbrom::damodeName(chip->damode));
        return false;
    }
    if (chip->iot) {   // Task 2 审查 ⚠️：IoT 芯片在 LEGACY 里 region 映射不同（上游 upload_da1 的 iot 分支）
        if (error) *error = QStringLiteral("本设备 hw_code=0x%1 是 IoT 芯片（上游 iot=True）—— D1 未实现 IoT 的 "
                                           "region 映射（region[0]/region[1]），明确拒绝（不按手机映射硬刷）")
                                .arg(hwCode, 4, 16, QLatin1Char('0'));
        return false;
    }
    say(QStringLiteral("代际判定：%1（chip_dacode=0x%2）")
            .arg(mtkbrom::damodeName(chip->damode)).arg(chip->dacode, 4, 16, QLatin1Char('0')));

    mtkbrom::DaFile daFile;
    if (!mtkbrom::parseDaFile(req.daFile, daFile, error))
        return false;
    if (daFile.isV6) {                       // DA 条目自带 v6 = XML 代（spec §1：v6 强制 XML）
        if (error) *error = QStringLiteral("DA 文件是 v6（XML 代）—— Phase D1 只支持 LEGACY（D2/D3 另做）");
        return false;
    }
    mtkbrom::DaSelection sel;
    QStringList selWarn;
    if (!mtkbrom::selectDaEntry(daFile, hwCode, sw.hwVer, sw.swVer, &selWarn, sel, error)) {
        const QString why = error ? *error : QString();            // 先取值再改写（别自引用）
        if (error) *error = QStringLiteral("DA 条目选择失败（%1）：%2").arg(req.daLabel, why);
        return false;
    }
    for (const QString &w : std::as_const(selWarn))
        warn(w);
    say(QStringLiteral("DA 条目：DA1 %1 字节 @0x%2；DA2 %3 字节 @0x%4")
            .arg(sel.da1Bytes.size()).arg(sel.da1.startAddr, 8, 16, QLatin1Char('0'))
            .arg(sel.da2Bytes.size()).arg(sel.da2.startAddr, 8, 16, QLatin1Char('0')));

    // preloader 两路径（显式 > 自动导入 > 网络(默认关) > 跳过）
    // 注：**跳过 preloader 是否致命由 DA1 决定**（errorcode == 0xBC3 时必须要有）——
    // 判定在 bromBringUpDa 里，这里不预先告警（否则 errorcode==0 的设备会被无谓地吓一跳）。
    mtkbrom::PreloaderResult pre;
    if (!mtkbrom::resolvePreloader(req.preloader, req.preloaderSources, req.downloader, pre, error))
        return false;
    for (const QString &line : std::as_const(pre.log))
        say(line);

    QStringList bringLog;
    if (!mtkbrom::bromBringUpDa(session, sel, hwCode, bromVer, blVer, pre, &bringLog, error))
        return false;
    for (const QString &line : std::as_const(bringLog))
        say(line);

    // 设备分区表 → 计划（**写入判据以设备为准**；预览期的对照表到这里可能对不上）
    mtkbrom::DaStorage storage(session);
    storage.setDaActive(true);
    QList<mtkbrom::EmPartition> deviceParts;
    if (!storage.listPartitions(deviceParts, error))
        return false;
    if (deviceParts.isEmpty()) {
        if (error) *error = QStringLiteral("设备分区表为空（read_pmt 无条目）—— 拒绝在未知分区表上写入");
        return false;
    }
    mtkplan::MtkFlashPlan plan;
    if (!mtkplan::buildMtkPlan(toPartitionRefs(deviceParts), req.imagePaths, plan, error))
        return false;
    for (const QString &w : std::as_const(plan.warnings))
        warn(w);
    if (plan.entries.isEmpty()) {
        if (error) *error = QStringLiteral("按设备分区表没有任何可写入的镜像（见上方告警）");
        return false;
    }
    say(QStringLiteral("计划：%1 个分区，合计 %2 字节").arg(plan.entries.size()).arg(plan.totalBytes));

    // 逐分区写（失败即停；错误含分区名与已写字节 —— 由 flashPartition 填）
    quint64 written = 0;
    for (const mtkplan::PlanEntry &e : std::as_const(plan.entries)) {
        QFile f(e.imagePath);
        if (!f.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("无法读取镜像：%1").arg(e.imagePath);
            return false;
        }
        const QByteArray image = f.readAll();
        f.close();
        if (image.size() != qsizetype(e.imageSize)) {
            if (error) *error = QStringLiteral("镜像 %1 读入字节数与计划不符（%2 != %3）")
                                    .arg(e.imagePath).arg(image.size()).arg(e.imageSize);
            return false;
        }
        say(QStringLiteral("写入分区 %1（%2 字节）…").arg(e.partition).arg(image.size()));
        if (!mtkbrom::flashPartition(session, storage, e.partition, image, error))
            return false;
        written += quint64(image.size());
        if (progress)
            progress(written, plan.totalBytes);
    }

    // FINISH 收尾：失败只告警（数据已落盘）
    QString finishErr;
    if (storage.finishFlash(0, &finishErr))
        say(QStringLiteral("FINISH（0xD9）收尾完成"));
    else
        warn(QStringLiteral("FINISH 收尾失败（数据已写入）：%1").arg(finishErr));
    return true;
}

// 【弃用桩 —— **T10 接线时必须删除**】见 mtk_handler.h。
// 存在的唯一理由：flash_tool.cpp 的旧调用点要到 T10 才改，T9 删净会打断主程序 + test_pipeline
// 构建（计划裁决 A，见 .superpowers/sdd/progress.md 的 T9 条目）。**不含任何刷写逻辑**。
bool runBromFlash(const QByteArray &daBinary,
                  const QList<QPair<QString, QByteArray>> &partitions,
                  QString *error)
{
    Q_UNUSED(daBinary)
    Q_UNUSED(partitions)
    if (error)
        *error = QStringLiteral("旧 runBromFlash 入口已废弃：请改用 runBromFlash(BromFlashRequest)"
                                "（flash_tool 通道接线属 Task 10）");
    return false;
}
