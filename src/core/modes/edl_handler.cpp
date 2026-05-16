#include "edl_handler.h"
#include <libusb.h>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QThread>
#include <QDataStream>

// Known EDL VID/PID
static const uint16_t EDL_SAHARA_VID = 0x05c6;
static const uint16_t EDL_SAHARA_PID = 0x9008;
static const uint16_t EDL_FIREHOSE_PID = 0x9025; // after programmer load
static const uint16_t EDL_900E = 0x900e;

// Sahara endpoints
static const int EDP_OUT = 0x01;
static const int EDP_IN  = 0x82;

// Firehose endpoints (after mode switch)
static const int FH_OUT = 0x02;
static const int FH_IN  = 0x83;

static const int TIMEOUT_MS = 10000;

EDLHandler::EDLHandler(QObject *parent)
    : QObject(parent)
    , m_ctx(nullptr)
    , m_devHandle(nullptr)
    , m_outEp(EDP_OUT)
    , m_inEp(EDP_IN)
    , m_connected(false)
    , m_firehoseMode(false)
    , m_vid(0)
    , m_pid(0)
{
}

EDLHandler::~EDLHandler()
{
    disconnect();
}

// ==================== USB helpers ====================

bool EDLHandler::openUSB()
{
    if (m_ctx) closeUSB();

    int ret = libusb_init(&m_ctx);
    if (ret != 0) {
        m_lastError = QString("libusb_init: %1").arg(libusb_error_name(ret));
        return false;
    }

    libusb_device **list = nullptr;
    ssize_t count = libusb_get_device_list(m_ctx, &list);
    if (count < 0) {
        m_lastError = "libusb_get_device_list failed";
        return false;
    }

    bool found = false;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device *dev = list[i];
        libusb_device_descriptor desc;
        libusb_get_device_descriptor(dev, &desc);

        uint16_t targetPid = m_firehoseMode ? EDL_FIREHOSE_PID : EDL_SAHARA_PID;
        if (desc.idVendor == EDL_SAHARA_VID &&
            (desc.idProduct == targetPid || desc.idProduct == EDL_900E)) {

            ret = libusb_open(dev, &m_devHandle);
            if (ret == 0) {
                m_vid = desc.idVendor;
                m_pid = desc.idProduct;
                found = true;
                emit outputMessage(QString("已打开 EDL 设备 %1:%2")
                                   .arg(m_vid, 4, 16, QChar('0'))
                                   .arg(m_pid, 4, 16, QChar('0')), false);
                break;
            }
        }
    }

    libusb_free_device_list(list, 1);

    if (!found) {
        m_lastError = QString("未找到 %1 EDL 设备")
                      .arg(m_firehoseMode ? "Firehose" : "Sahara");
        libusb_exit(m_ctx);
        m_ctx = nullptr;
        return false;
    }

    return true;
}

void EDLHandler::closeUSB()
{
    if (m_devHandle) {
        libusb_release_interface(m_devHandle, m_firehoseMode ? 1 : 0);
        libusb_close(m_devHandle);
        m_devHandle = nullptr;
    }
    if (m_ctx) {
        libusb_exit(m_ctx);
        m_ctx = nullptr;
    }
}

bool EDLHandler::claimInterface()
{
    if (!m_devHandle) return false;

    int iface = m_firehoseMode ? 1 : 0;
    int ret = libusb_claim_interface(m_devHandle, iface);
    if (ret != 0) {
        ret = libusb_detach_kernel_driver(m_devHandle, iface);
        if (ret == 0 || ret == LIBUSB_ERROR_NOT_FOUND) {
            ret = libusb_claim_interface(m_devHandle, iface);
        }
    }
    if (ret != 0) {
        m_lastError = QString("claim_interface(%1): %2")
                      .arg(iface).arg(libusb_error_name(ret));
        return false;
    }
    return true;
}

// ==================== Sahara Protocol ====================

// All Sahara packets: cmd(LE32) + length(LE32) + [payload]
bool EDLHandler::sendSaharaCmd(SaharaCmd cmd, const QByteArray &payload)
{
    if (!m_devHandle) return false;

    quint32 totalLen = 8 + payload.size();
    QByteArray pkt;
    QDataStream ds(&pkt, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << (quint32)cmd << totalLen;
    if (!payload.isEmpty())
        pkt.append(payload);

    int transferred = 0;
    int ret = libusb_bulk_transfer(m_devHandle, m_outEp,
                reinterpret_cast<unsigned char*>(pkt.data()), pkt.size(),
                &transferred, TIMEOUT_MS);
    if (ret != 0) {
        m_lastError = QString("Sahara send cmd 0x%1 failed: %2")
                      .arg(cmd, 2, 16, QChar('0'))
                      .arg(libusb_error_name(ret));
        return false;
    }
    return true;
}

bool EDLHandler::recvSaharaPacket(SaharaCmd &outCmd, QByteArray &outPayload, int timeoutMs)
{
    if (!m_devHandle) return false;

    unsigned char hdr[8];
    int transferred = 0;
    int ret = libusb_bulk_transfer(m_devHandle, m_inEp,
                hdr, 8, &transferred, timeoutMs);
    if (ret != 0) {
        m_lastError = QString("Sahara recv header failed: %1")
                      .arg(libusb_error_name(ret));
        return false;
    }
    if (transferred < 8) {
        m_lastError = "Sahara recv: short header";
        return false;
    }

    quint32 cmd = hdr[0] | ((quint32)hdr[1] << 8) | ((quint32)hdr[2] << 16) | ((quint32)hdr[3] << 24);
    quint32 len = hdr[4] | ((quint32)hdr[5] << 8) | ((quint32)hdr[6] << 16) | ((quint32)hdr[7] << 24);
    outCmd = static_cast<SaharaCmd>(cmd);

    if (len <= 8)
        return true; // no payload

    quint32 payloadLen = len - 8;
    outPayload.resize(payloadLen);
    int totalRead = 0;
    while (totalRead < (int)payloadLen) {
        ret = libusb_bulk_transfer(m_devHandle, m_inEp,
                    reinterpret_cast<unsigned char*>(outPayload.data() + totalRead),
                    payloadLen - totalRead, &transferred, timeoutMs);
        if (ret != 0) {
            m_lastError = QString("Sahara recv payload failed: %1")
                          .arg(libusb_error_name(ret));
            return false;
        }
        totalRead += transferred;
    }
    return true;
}

bool EDLHandler::recvHelloReq(SaharaHello &hello, int timeoutMs)
{
    // HELLO_REQ: cmd=0x01, len=0x30, then 10 more uint32
    SaharaCmd cmd;
    QByteArray payload;
    if (!recvSaharaPacket(cmd, payload, timeoutMs))
        return false;

    if (cmd != SAHARA_HELLO_REQ) {
        m_lastError = QString("Expected HELLO_REQ(0x01), got 0x%1")
                      .arg(cmd, 2, 16, QChar('0'));
        return false;
    }

    if (payload.size() < 40) {
        m_lastError = "HELLO_REQ payload too short";
        return false;
    }

    QDataStream ds(payload);
    ds.setByteOrder(QDataStream::LittleEndian);
    quint32 v[10]; // after cmd+len: version, version_supported, cmd_packet_length, mode, reserved[6]
    for (int i = 0; i < 10; i++)
        ds >> v[i];

    hello.version = v[0];
    hello.versionSupported = v[1];
    hello.cmdPacketLength = v[2];
    hello.mode = v[3];
    for (int i = 0; i < 6; i++)
        hello.reserved[i] = v[4 + i];

    return true;
}

bool EDLHandler::sendHelloResp(quint32 mode, quint32 version)
{
    QByteArray payload;
    QDataStream ds(&payload, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << version;           // version
    ds << (quint32)1;        // version_supported
    ds << (quint32)0;        // cmd_packet_length
    ds << mode;              // mode
    ds << (quint32)0;        // reserved[0]
    ds << (quint32)1;        // reserved[1]
    ds << (quint32)2;        // reserved[2]
    ds << (quint32)3;        // reserved[3]
    ds << (quint32)4;        // reserved[4]
    ds << (quint32)5;        // reserved[5]
    // Total: 10 uint32 = 40 bytes payload + 8 header = 48 bytes (0x30)

    return sendSaharaCmd(SAHARA_HELLO_RSP, payload);
}

bool EDLHandler::recvReadData(SaharaReadData &rd, int timeoutMs)
{
    // READ_DATA: cmd=0x03, then image_id, data_offset, data_len
    SaharaCmd cmd;
    QByteArray payload;
    if (!recvSaharaPacket(cmd, payload, timeoutMs))
        return false;

    if (cmd == SAHARA_END_TRANSFER) {
        // Device is done sending data requests
        if (payload.size() >= 8) {
            QDataStream ds(payload);
            ds.setByteOrder(QDataStream::LittleEndian);
            quint32 id, status;
            ds >> id >> status;
            m_lastError = QString("END_TRANSFER: id=0x%1 status=0x%2")
                          .arg(id, 2, 16, QChar('0'))
                          .arg(status, 2, 16, QChar('0'));
        }
        return false;
    }

    if (cmd != SAHARA_READ_DATA &&
        cmd != SAHARA_64BIT_MEMORY_READ_DATA &&
        cmd != SAHARA_HELLO_REQ) {
        m_lastError = QString("Expected READ_DATA(0x03), got 0x%1")
                      .arg(cmd, 2, 16, QChar('0'));
        return false;
    }

    // If we get another HELLO_REQ, the device wants to re-handshake
    if (cmd == SAHARA_HELLO_REQ) {
        m_lastError = "Unexpected HELLO_REQ during data transfer";
        return false;
    }

    if (payload.size() < 12) {
        m_lastError = "READ_DATA payload too short";
        return false;
    }

    QDataStream ds(payload);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds >> rd.imageId >> rd.dataOffset >> rd.dataLen;

    // Check for 64-bit packet (12 bytes for 3 x uint32 vs 24 bytes for 3 x uint64)
    if (cmd == SAHARA_64BIT_MEMORY_READ_DATA && payload.size() >= 24) {
        // 64-bit: image_id(Q), data_offset(Q), data_len(Q)
        // Re-parse as 64-bit
        QDataStream ds64(payload);
        ds64.setByteOrder(QDataStream::LittleEndian);
        quint64 id64, off64, len64;
        ds64 >> id64 >> off64 >> len64;
        rd.imageId = (quint32)id64;
        rd.dataOffset = (quint32)off64;
        rd.dataLen = (quint32)len64;
    }

    return true;
}

bool EDLHandler::recvEndTransfer(SaharaEndTransfer &et, int timeoutMs)
{
    SaharaCmd cmd;
    QByteArray payload;
    if (!recvSaharaPacket(cmd, payload, timeoutMs))
        return false;

    if (cmd != SAHARA_END_TRANSFER) {
        m_lastError = QString("Expected END_TRANSFER(0x04), got 0x%1")
                      .arg(cmd, 2, 16, QChar('0'));
        return false;
    }

    if (payload.size() >= 8) {
        QDataStream ds(payload);
        ds.setByteOrder(QDataStream::LittleEndian);
        ds >> et.imageId >> et.imageTxStatus;
    }
    return true;
}

bool EDLHandler::sendDoneReq()
{
    // DONE_REQ: cmd=0x05, len=0x08, no payload
    return sendSaharaCmd(SAHARA_DONE_REQ);
}

bool EDLHandler::recvDoneResp(SaharaEndTransfer &et, int timeoutMs)
{
    SaharaCmd cmd;
    QByteArray payload;
    if (!recvSaharaPacket(cmd, payload, timeoutMs))
        return false;

    if (cmd != SAHARA_DONE_RSP) {
        m_lastError = QString("Expected DONE_RSP(0x06), got 0x%1")
                      .arg(cmd, 2, 16, QChar('0'));
        return false;
    }

    if (payload.size() >= 4) {
        QDataStream ds(payload);
        ds.setByteOrder(QDataStream::LittleEndian);
        ds >> et.imageTxStatus;
    }
    et.imageId = 0;
    return true;
}

bool EDLHandler::sendSwitchMode(SaharaMode mode)
{
    QByteArray payload;
    QDataStream ds(&payload, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds << (quint32)mode;
    return sendSaharaCmd(SAHARA_SWITCH_MODE, payload);
}

bool EDLHandler::serveProgrammer(const QString &programmerPath)
{
    QFile prog(programmerPath);
    if (!prog.open(QIODevice::ReadOnly)) {
        m_lastError = QString("无法打开 programmer: %1").arg(programmerPath);
        return false;
    }
    QByteArray progData = prog.readAll();
    prog.close();

    emit outputMessage(QString("Programmer 大小: %1 字节").arg(progData.size()), false);

    while (true) {
        SaharaReadData rd;
        if (!recvReadData(rd)) {
            // Check if we got END_TRANSFER instead
            SaharaCmd cmd;
            QByteArray payload;
            if (recvSaharaPacket(cmd, payload, TIMEOUT_MS)) {
                if (cmd == SAHARA_END_TRANSFER) {
                    SaharaEndTransfer et;
                    if (payload.size() >= 8) {
                        QDataStream ds(payload);
                        ds.setByteOrder(QDataStream::LittleEndian);
                        ds >> et.imageId >> et.imageTxStatus;
                    }
                    if (et.imageTxStatus == SAHARA_STATUS_SUCCESS) {
                        emit outputMessage("Programmer 传输完成", false);
                        return true;
                    } else {
                        m_lastError = QString("Programmer 传输失败: status=0x%1")
                                      .arg(et.imageTxStatus, 2, 16, QChar('0'));
                        return false;
                    }
                } else if (cmd == SAHARA_CMD_READY) {
                    emit outputMessage("Sahara CMD_READY received", false);
                    return true;
                } else if (cmd == SAHARA_DONE_RSP) {
                    emit outputMessage("Sahara DONE_RSP received", false);
                    return true;
                }
            }
            return false;
        }

        emit outputMessage(QString("Sahara 请求: id=0x%1 offset=0x%2 len=%3")
                           .arg(rd.imageId, 2, 16, QChar('0'))
                           .arg(rd.dataOffset, 2, 16, QChar('0'))
                           .arg(rd.dataLen), false);

        // Serve requested data from programmer
        qint64 totalLen = progData.size();
        if (rd.dataOffset + rd.dataLen > (quint32)totalLen) {
            m_lastError = QString("Programmer 请求越界: offset=0x%1 len=%2, total=%3")
                          .arg(rd.dataOffset, 2, 16, QChar('0'))
                          .arg(rd.dataLen)
                          .arg(totalLen);
            return false;
        }

        QByteArray chunk = progData.mid(rd.dataOffset, rd.dataLen);
        int transferred = 0;
        int ret = libusb_bulk_transfer(m_devHandle, m_outEp,
                    reinterpret_cast<unsigned char*>(chunk.data()), chunk.size(),
                    &transferred, TIMEOUT_MS);
        if (ret != 0) {
            m_lastError = QString("发送 programmer 数据失败: %1")
                          .arg(libusb_error_name(ret));
            return false;
        }
    }
}

// ==================== Firehose ====================

bool EDLHandler::sendFirehoseXml(const QString &xml, int timeoutMs)
{
    if (!m_devHandle) return false;

    QByteArray data = xml.toUtf8();
    QByteArray pkt;
    quint32 len = data.size();
    pkt.append((char)(len & 0xff));
    pkt.append((char)((len >> 8) & 0xff));
    pkt.append((char)((len >> 16) & 0xff));
    pkt.append((char)((len >> 24) & 0xff));
    pkt.append(data);

    int transferred = 0;
    int ret = libusb_bulk_transfer(m_devHandle, m_outEp,
                reinterpret_cast<unsigned char*>(pkt.data()), pkt.size(),
                &transferred, timeoutMs);

    if (ret != 0) {
        m_lastError = QString("firehose 发送失败: %1").arg(libusb_error_name(ret));
        return false;
    }
    return true;
}

QString EDLHandler::recvFirehoseResponse(int timeoutMs)
{
    if (!m_devHandle) return {};

    unsigned char buf[4096];
    int transferred = 0;
    int ret = libusb_bulk_transfer(m_devHandle, m_inEp,
                buf, sizeof(buf), &transferred, timeoutMs);

    if (ret != 0) {
        m_lastError = QString("firehose 接收失败: %1").arg(libusb_error_name(ret));
        return {};
    }

    // First 4 bytes are length
    QByteArray raw(reinterpret_cast<char*>(buf + 4), transferred - 4);
    return QString::fromUtf8(raw);
}

bool EDLHandler::waitFirehoseDone(int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();

    while (!timer.hasExpired(timeoutMs)) {
        QString resp = recvFirehoseResponse(2000);
        if (resp.isEmpty()) continue;

        if (resp.contains("response")) {
            if (resp.contains("OK") || resp.contains("value=\"")) continue;
        }
        if (resp.contains("<log")) continue;
        return true;
    }
    return false;
}

// ==================== High-level API ====================

bool EDLHandler::connectSahara(const QString &programmerPath)
{
    emit outputMessage("EDL: 连接 Sahara 模式...", false);
    emit progress(5);

    m_firehoseMode = false;
    m_outEp = EDP_OUT;
    m_inEp = EDP_IN;

    if (!openUSB()) {
        emit outputMessage("EDL: " + m_lastError, true);
        return false;
    }

    if (!claimInterface()) {
        emit outputMessage("EDL: " + m_lastError, true);
        closeUSB();
        return false;
    }

    // Step 1: Receive HELLO_REQ from device
    emit outputMessage("等待 Sahara HELLO...", false);
    SaharaHello hello;
    if (!recvHelloReq(hello)) {
        emit outputMessage("EDL: " + m_lastError, true);
        closeUSB();
        return false;
    }

    emit outputMessage(QString("Sahara v%1, mode=0x%2")
                       .arg(hello.version)
                       .arg(hello.mode, 2, 16, QChar('0')), false);
    emit progress(15);

    // Step 2: Send HELLO_RESP with IMAGE_TX_PENDING mode
    if (!sendHelloResp(SAHARA_MODE_IMAGE_TX_PENDING, hello.version)) {
        emit outputMessage("EDL: HELLO_RESP 发送失败", true);
        closeUSB();
        return false;
    }
    emit outputMessage("Sahara 握手完成, 开始加载 programmer...", false);
    emit progress(25);

    // Step 3: Serve programmer data chunks as requested by device
    if (!serveProgrammer(programmerPath)) {
        emit outputMessage("EDL: " + m_lastError, true);
        closeUSB();
        return false;
    }
    emit progress(50);

    // Step 4: Send DONE_REQ and wait for DONE_RSP
    if (!sendDoneReq()) {
        emit outputMessage("EDL: DONE_REQ 发送失败", true);
        closeUSB();
        return false;
    }

    SaharaEndTransfer doneResp;
    if (!recvDoneResp(doneResp, TIMEOUT_MS)) {
        emit outputMessage("EDL: DONE_RSP 接收失败: " + m_lastError, true);
        closeUSB();
        return false;
    }

    if (doneResp.imageTxStatus != SAHARA_STATUS_SUCCESS) {
        emit outputMessage(QString("EDL: DONE 状态错误: 0x%1")
                           .arg(doneResp.imageTxStatus, 2, 16, QChar('0')), true);
        closeUSB();
        return false;
    }

    emit outputMessage("Programmer 加载成功", false);
    emit progress(60);

    // Step 5: Close Sahara device and wait for Firehose re-enumeration
    closeUSB();

    emit outputMessage("等待设备切换到 Firehose 模式...", false);
    QThread::msleep(3000);

    m_firehoseMode = true;
    m_outEp = FH_OUT;
    m_inEp = FH_IN;

    int retries = 15;
    while (retries-- > 0) {
        if (openUSB()) {
            if (claimInterface()) {
                m_connected = true;
                emit outputMessage("Firehose 模式连接成功", false);
                emit progress(70);
                return true;
            }
            closeUSB();
        }
        QThread::msleep(2000);
    }

    emit outputMessage("EDL: 设备未切换到 Firehose 模式", true);
    return false;
}

void EDLHandler::disconnect()
{
    if (m_connected) {
        sendFirehoseXml("<?xml version=\"1.0\"?>\n<data>\n<power value=\"reset\"/>\n</data>");
    }
    m_connected = false;
    m_firehoseMode = false;
    closeUSB();
}

bool EDLHandler::firehoseConnect()
{
    if (!m_connected) {
        emit outputMessage("未连接 EDL 设备", true);
        return false;
    }

    QString config = "<?xml version=\"1.0\"?>\n"
                     "<data>\n"
                     "<configure MemoryName=\"emmc\" Verbose=\"0\" "
                     "AlwaysValidate=\"0\" MaxPayloadSizeToTargetInBytes=\"1048576\" "
                     "ZLPAwareHost=\"1\" SkipStorageInit=\"0\"/>\n"
                     "</data>";

    if (!sendFirehoseXml(config)) {
        emit outputMessage("Firehose 配置失败: " + m_lastError, true);
        return false;
    }

    emit outputMessage("Firehose 会话已配置", false);
    return true;
}

QList<EDLPartition> EDLHandler::listPartitions()
{
    QList<EDLPartition> parts;
    if (!m_connected) return parts;

    QString query = "<?xml version=\"1.0\"?>\n<data>\n<partition>\n</data>";
    if (!sendFirehoseXml(query)) {
        emit outputMessage("分区查询失败", true);
        return parts;
    }

    QString resp = recvFirehoseResponse(30000);
    QStringList lines = resp.split('\n', Qt::SkipEmptyParts);

    for (const QString &line : lines) {
        if (!line.contains("<partition")) continue;

        EDLPartition p;
        p.sectorSize = 512;
        p.startSector = 0;

        {
            int si = line.indexOf("label=\"");
            if (si >= 0) {
                si += 7;
                int ei = line.indexOf('\"', si);
                if (ei > si) p.name = line.mid(si, ei - si);
            }
        }
        {
            int si = line.indexOf("size=\"");
            if (si >= 0) {
                si += 6;
                int ei = line.indexOf('\"', si);
                if (ei > si) {
                    p.numSectors = line.mid(si, ei - si).toULongLong(nullptr, 16);
                }
            }
        }
        if (!p.name.isEmpty())
            parts.append(p);
    }

    if (parts.isEmpty()) {
        EDLPartition raw;
        raw.name = "rawstorage";
        raw.startSector = 0;
        raw.numSectors = 0;
        raw.sectorSize = 512;
        parts.append(raw);
    }

    emit outputMessage(QString("检测到 %1 个分区").arg(parts.size()), false);
    return parts;
}

bool EDLHandler::readPartition(const EDLPartition &part, const QString &outputPath)
{
    if (!m_connected) {
        emit outputMessage("EDL 未连接", true);
        return false;
    }

    emit outputMessage(QString("读取分区: %1").arg(part.name), false);
    emit progress(50);

    QString xml = QString(
        "<?xml version=\"1.0\"?>\n<data>\n"
        "<read SECTOR_SIZE_IN_BYTES=\"%1\" "
        "num_sectors=\"%2\" "
        "start_sector=\"%3\" "
        "filename=\"%4\"/>\n</data>")
        .arg(part.sectorSize)
        .arg(part.numSectors)
        .arg(part.startSector)
        .arg(part.name + "_dump.bin");

    if (!sendFirehoseXml(xml, 60000)) {
        emit outputMessage("读取命令发送失败: " + m_lastError, true);
        return false;
    }

    QFile outFile(outputPath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        emit outputMessage("无法创建输出文件", true);
        return false;
    }

    unsigned char buf[65536];
    quint64 totalRead = 0;
    quint64 expectedBytes = part.numSectors * part.sectorSize;
    QElapsedTimer timer;
    timer.start();

    while (!timer.hasExpired(120000) &&
           (expectedBytes == 0 || totalRead < expectedBytes)) {
        int transferred = 0;
        int ret = libusb_bulk_transfer(m_devHandle, m_inEp,
                    buf, sizeof(buf), &transferred, 5000);
        if (ret == LIBUSB_ERROR_TIMEOUT) continue;
        if (ret != 0) {
            if (totalRead > 0) break;
            emit outputMessage("读取失败: " + QString(libusb_error_name(ret)), true);
            outFile.close();
            return false;
        }
        if (transferred > 0) {
            outFile.write(reinterpret_cast<char*>(buf), transferred);
            totalRead += transferred;
        }
    }

    outFile.close();
    emit outputMessage(QString("已读取 %1 字节").arg(totalRead), false);
    emit progress(100);
    return true;
}

bool EDLHandler::writePartition(const EDLPartition &part, const QString &imagePath)
{
    QFileInfo fi(imagePath);
    if (!fi.exists()) {
        emit outputMessage(QString("文件不存在: %1").arg(imagePath), true);
        return false;
    }

    if (!m_connected) {
        emit outputMessage("EDL 未连接", true);
        return false;
    }

    return writeRaw(fi.fileName(), part.startSector,
                    part.numSectors, part.sectorSize);
}

bool EDLHandler::writeRaw(const QString &filename, quint64 startSector,
                           quint64 numSectors, quint64 sectorSize)
{
    emit outputMessage(QString("写入分区: %1").arg(filename), false);
    emit progress(50);

    QString xml = QString(
        "<?xml version=\"1.0\"?>\n<data>\n"
        "<program SECTOR_SIZE_IN_BYTES=\"%1\" "
        "num_partition_sectors=\"%2\" "
        "start_sector=\"%3\" "
        "filename=\"%4\"/>\n</data>")
        .arg(sectorSize)
        .arg(numSectors)
        .arg(startSector)
        .arg(filename);

    if (!sendFirehoseXml(xml)) {
        emit outputMessage("写入命令发送失败: " + m_lastError, true);
        return false;
    }

    if (!waitFirehoseDone(120000)) {
        emit outputMessage("写入未确认", true);
        return false;
    }

    emit outputMessage("写入完成", false);
    emit progress(100);
    return true;
}
