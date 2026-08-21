#include <QtTest>
#include <QTemporaryFile>
#include <memory>

#include "core/modes/spd_flash.h"
#include "core/modes/spd_storage.h"

// MockUsbChannel：记录写入序列、预置读取队列（IUsbChannel 注入）
class MockUsbChannel : public spd::IUsbChannel {
public:
    QByteArray writes;
    QList<QByteArray> reads;
    bool failOpen = false;
    QString openError;

    bool open(QString *error) override
    {
        if (failOpen) { if (error) *error = openError; return false; }
        return true;
    }
    bool write(const QByteArray &data, QString *error) override
    {
        Q_UNUSED(error)
        writes += data;
        return true;
    }
    bool read(QByteArray &out, int maxLen, int timeoutMs, QString *error) override
    {
        Q_UNUSED(timeoutMs) Q_UNUSED(error)
        if (reads.isEmpty()) { out.clear(); return false; }
        QByteArray r = reads.takeFirst();
        out = r.left(maxLen);
        return !r.isEmpty() || maxLen == 0;
    }
    bool close() override { return true; }
};

class TestSpdFlash : public QObject {
    Q_OBJECT
private slots:
    // ---- 纯函数 ----
    void sumChecksumVector();
    void crc16Vector();
    // ---- 枚举 ----
    void enumValues();
    // ---- 帧 ----
    void frameConstruction();
    void frameEscaping();
    // ---- 命令 ----
    void sendCommandAck();
    void sendCommandBadChecksumFails();
    void responseDataExtraction();
    void responseEmbedded0x7e();
    void responseTranscoded();
    void checkBaudFrame();
    void connectOpenFailure();
    void enumerateVid();
    // ---- F4-2: FDL 上传与存储 ----
    void uploadFdlSequence();
    void eraseFlashFrame();
    void readFlashResponse();
    void resetFrame();
    // ---- F4-2 审查修复：ACK 强制校验 + log 帧跳过 ----
    void uploadFdlRejectsNonAck();
    void logFrameSkipped();
    // ---- F4-3: 集成边界 ----
    void isFdlPartitionNames();
    // ---- F4-3: 分区写路径（行为观察核实后实现）----
    void erasePartitionFrame();
    void writePartitionSequence();
};

// 构造响应帧：type BE16 + len BE16 + data + checksum（行为观察帧布局）
QByteArray makeResponseFrame(quint16 type, const QByteArray &data)
{
    QByteArray body;
    body.append(char((type >> 8) & 0xFF)).append(char(type & 0xFF));
    body.append(char((data.size() >> 8) & 0xFF)).append(char(data.size() & 0xFF));
    body += data;
    const quint16 chk = spd::sumChecksum(body);
    body.append(char((chk >> 8) & 0xFF)).append(char(chk & 0xFF));
    QByteArray frame(1, '\x7E');
    frame += body;
    frame += QByteArray(1, '\x7E');
    return frame;
}

// 转义帧体（保留首尾 0x7E 分隔符不转义）：body 中 0x7E→7D 5E、0x7D→7D 5D
// （与发送侧一致，行为观察 TRANSCODE）
QByteArray escapeFrame(const QByteArray &rawFrame)
{
    QByteArray frame(1, '\x7E');
    for (int i = 1; i < rawFrame.size() - 1; ++i) {
        const quint8 b = quint8(rawFrame[i]);
        if (b == 0x7E) frame += QByteArray("\x7D\x5E", 2);
        else if (b == 0x7D) frame += QByteArray("\x7D\x5D", 2);
        else frame.append(char(b));
    }
    frame.append(char(0x7E));
    return frame;
}

void TestSpdFlash::sumChecksumVector()
{
    // 空数据：0 + 折叠 + ~ = 0xFFFF → BE 交换后仍是 0xFFFF
    QCOMPARE(spd::sumChecksum(QByteArray()), quint16(0xFFFF));
}

void TestSpdFlash::crc16Vector()
{
    // CRC-16/XMODEM（poly 0x11021 非反射，init 0）标准向量："123456789" → 0x31C3
    // （行为观察核实为 init 0；0x29B1 是 init 0xFFFF 的 CCITT-FALSE 变体）
    QCOMPARE(spd::crc16(QByteArray("123456789")), quint16(0x31C3));
}

void TestSpdFlash::enumValues()
{
    // 行为观察：响应默认 ACK=0x80（命令后强制校验）、READ_FLASH 响应 0x93、
    // POWER_OFF=0x17、log 帧 0xFF（接收循环跳过）
    QCOMPARE(int(spd::BSL_REP_ACK), 0x80);
    QCOMPARE(int(spd::BSL_REP_READ_FLASH), 0x93);
    QCOMPARE(int(spd::BSL_REP_LOG), 0xFF);
    QCOMPARE(int(spd::BSL_CMD_POWER_OFF), 0x17);
}

void TestSpdFlash::frameConstruction()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    // CONNECT 帧：0x7E | type(00 00) + len(00 00) + checksum | 0x7E
    m->reads << QByteArray("\x7E\x00\x00\x00\x00\xFF\xFF\x7E", 8); // 响应帧
    QByteArray reply;
    QVERIFY(s.sendCommand(spd::BSL_CMD_CONNECT, QByteArray(), reply, 64, nullptr));
    // 发送帧：0x7E + 00 00 (type) + 00 00 (len) + checksum(sum of 4B) + 0x7E
    // sumChecksum(00 00 00 00)：0 + 0 → 0 → ~ = 0xFFFF → BE 交换 0xFFFF
    QCOMPARE(m->writes, QByteArray("\x7E\x00\x00\x00\x00\xFF\xFF\x7E", 8));
}

void TestSpdFlash::frameEscaping()
{
    // 转义模式：payload 含 0x7E → 0x7D 0x5E（TRANSCODE，0x7E^0x20）
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0, /*transcode=*/true);
    m->reads << QByteArray("\x7E\x00\x00\x00\x00\xFF\xFF\x7E", 8);
    QByteArray reply;
    QVERIFY(s.sendCommand(spd::BSL_CMD_CONNECT, QByteArray(1, char(0x7E)), reply, 64, nullptr));
    // 完整帧：0x7E + type(00 00) + len(00 01) + 转义 payload(7D 5E) + checksum(81 FE) + 0x7E
    // （checksum 81 FE：sum(00 00 00 01 7E)=0x017E → ~0xFE81 → 交换 0x81FE）
    QCOMPARE(m->writes, QByteArray("\x7E\x00\x00\x00\x01\x7D\x5E\x81\xFE\x7E", 10));
}

void TestSpdFlash::sendCommandAck()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    // 响应：type=0x01 (START_DATA 确认), len=0, checksum（00 01 00 00 求和 0x0100 → ~0xFEFF → 交换 0xFFFE）
    m->reads << QByteArray("\x7E\x00\x01\x00\x00\xFF\xFE\x7E", 8);
    QByteArray reply;
    QVERIFY(s.sendCommand(spd::BSL_CMD_START_DATA, QByteArray(8, '\0'), reply, 64, nullptr));
}

void TestSpdFlash::sendCommandBadChecksumFails()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    m->reads << QByteArray("\x7E\x00\x01\x00\x00\x00\x00\x7E", 8); // checksum 错
    QByteArray reply;
    QString err;
    QVERIFY(!s.sendCommand(spd::BSL_CMD_START_DATA, QByteArray(8, '\0'), reply, 64, &err));
    QVERIFY(err.contains("checksum"));
}

void TestSpdFlash::responseDataExtraction()
{
    // 响应带数据（READ_FLASH 风格）：reply 剥离 type/len/checksum，仅返回数据区
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    const QByteArray data("\xAA\xBB\xCC\xDD", 4);
    m->reads << makeResponseFrame(spd::BSL_REP_READ_FLASH, data);
    QByteArray reply;
    QVERIFY(s.sendCommand(spd::BSL_CMD_READ_FLASH, QByteArray(12, '\0'), reply, 64, nullptr));
    QCOMPARE(reply, data);
}

void TestSpdFlash::responseEmbedded0x7e()
{
    // H1：非转义模式，数据区含 0x7E —— 响应须按 len 字段闭合（len+6），不得提前截断
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    const QByteArray data("\xAA\x7E\xCC\xDD", 4);
    m->reads << makeResponseFrame(spd::BSL_REP_READ_FLASH, data);
    QByteArray reply;
    QVERIFY(s.sendCommand(spd::BSL_CMD_READ_FLASH, QByteArray(12, '\0'), reply, 64, nullptr));
    QCOMPARE(reply, data);
}

void TestSpdFlash::responseTranscoded()
{
    // H1：转义模式响应 —— 接收侧反转义（7D 5E→7E、7D 5D→7D）后 checksum 通过
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0, /*transcode=*/true);
    const QByteArray data("\x7E\x7D\x41\x42", 4);
    m->reads << escapeFrame(makeResponseFrame(spd::BSL_REP_READ_FLASH, data));
    QByteArray reply;
    QVERIFY(s.sendCommand(spd::BSL_CMD_READ_FLASH, QByteArray(12, '\0'), reply, 64, nullptr));
    QCOMPARE(reply, data);
}

void TestSpdFlash::checkBaudFrame()
{
    // H2：CHECK_BAUD 特判帧 —— 整帧为 len 个 0x7E（无 header/len/checksum 结构）
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    m->reads << QByteArray("\x7E\x00\x00\x00\x00\xFF\xFF\x7E", 8);
    QByteArray reply;
    QVERIFY(s.sendCommand(spd::BSL_CMD_CHECK_BAUD, QByteArray(4, char(0xAA)), reply, 64, nullptr));
    QCOMPARE(m->writes, QByteArray(4, char(0x7E)));
}

void TestSpdFlash::connectOpenFailure()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    m->failOpen = true;
    m->openError = QStringLiteral("无权限");
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    QString err;
    QVERIFY(!s.connect(&err));
    QVERIFY(err.contains("无权限"));
}

void TestSpdFlash::enumerateVid()
{
    // 纯函数级：enumerateUsb 依赖 libusb 设备列表，无设备环境返回空列表不失败
    QList<QPair<int, int>> devs;
    QString err;
    QVERIFY(spd::enumerateUsb(devs, &err)); // 无设备时 true + 空列表
    Q_UNUSED(devs)
}

void TestSpdFlash::uploadFdlSequence()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    spd::SpdFlasher f(s);
    // 临时 FDL 文件（3 字节 → 单块 MIDST）
    QTemporaryFile tmp;
    QVERIFY(tmp.open());
    tmp.write("fdl");
    tmp.flush();
    // 响应队列：START_DATA 确认 + MIDST_DATA 确认 + END_DATA 确认 + EXEC_DATA 确认
    // （真实 ACK：type=0x80；sendAndExpectAck 强制校验）
    const QByteArray ack = makeResponseFrame(spd::BSL_REP_ACK, QByteArray());
    m->reads << ack << ack << ack << ack;
    QVERIFY(f.uploadFdl(tmp.fileName(), 0x40004000, true, nullptr));
    // 帧序列：START_DATA(0x01, addr+size BE32) → MIDST_DATA(0x02, 3B) → END(0x03) → EXEC(0x04)
    QVERIFY(m->writes.contains('\x01'));
    QVERIFY(m->writes.contains('\x02'));
    QVERIFY(m->writes.contains('\x03'));
    QVERIFY(m->writes.contains('\x04'));
}

void TestSpdFlash::eraseFlashFrame()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    spd::SpdFlasher f(s);
    m->reads << makeResponseFrame(spd::BSL_REP_ACK, QByteArray());
    QVERIFY(f.eraseFlash(0x1000, 0x100, nullptr));
    // ERASE_FLASH(0x0A)：addr BE32 + size BE32
    QVERIFY(m->writes.contains('\x0A'));
}

void TestSpdFlash::readFlashResponse()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    spd::SpdFlasher f(s);
    // READ_FLASH 响应：type=BSL_REP_READ_FLASH(0x93) + len + 4B 数据
    const QByteArray data("\xAA\xBB\xCC\xDD", 4);
    m->reads << makeResponseFrame(spd::BSL_REP_READ_FLASH, data);
    QByteArray out;
    QVERIFY(f.readFlash(0x1000, 4, 0, out, nullptr));
    QCOMPARE(out, data);
}

void TestSpdFlash::resetFrame()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    spd::SpdFlasher f(s);
    m->reads << makeResponseFrame(spd::BSL_REP_ACK, QByteArray());
    QVERIFY(f.resetDevice(nullptr));
    QVERIFY(m->writes.contains('\x05'));
}

void TestSpdFlash::uploadFdlRejectsNonAck()
{
    // H3：设备错误响应（0x84 OPERATION_FAILED）不得被当作成功——
    // 每步强制校验 ACK 0x80（行为观察 send_and_check 语义）
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    spd::SpdFlasher f(s);
    QTemporaryFile tmp;
    QVERIFY(tmp.open());
    tmp.write("fdl");
    tmp.flush();
    m->reads << makeResponseFrame(0x84, QByteArray()); // BSL_REP_OPERATION_FAILED
    QString err;
    QVERIFY(!f.uploadFdl(tmp.fileName(), 0x40004000, false, &err));
    QVERIFY(err.contains("ACK"));
    QVERIFY(err.contains("0084"));
}

void TestSpdFlash::logFrameSkipped()
{
    // H3：设备先发 BSL_REP_LOG(0xFF) 帧 —— 接收须跳过 log 帧继续读，
    // 直至真实响应帧（行为观察 recv_msg 循环语义）
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    m->reads << makeResponseFrame(spd::BSL_REP_LOG, QByteArray("log"))
             << makeResponseFrame(spd::BSL_REP_ACK, QByteArray()); // 真实 ACK
    QByteArray reply;
    quint16 rtype = 0;
    QVERIFY(s.sendCommand(spd::BSL_CMD_START_DATA, QByteArray(8, '\0'), reply, 64,
                          nullptr, &rtype));
    QCOMPARE(rtype, spd::BSL_REP_ACK); // 返回的是 ACK 帧而非 log 帧
    QVERIFY(m->reads.isEmpty());       // 两帧均已消费
}

void TestSpdFlash::isFdlPartitionNames()
{
    QVERIFY(spd::isFdlPartition(QStringLiteral("fdl1")));
    QVERIFY(spd::isFdlPartition(QStringLiteral("fdl2")));
    QVERIFY(spd::isFdlPartition(QStringLiteral("fdl")));
    QVERIFY(!spd::isFdlPartition(QStringLiteral("boot")));
    QVERIFY(!spd::isFdlPartition(QStringLiteral("system")));
}

void TestSpdFlash::erasePartitionFrame()
{
    // 按名擦除（行为观察 erase_partition）：ERASE_FLASH(0x0A) + 分区选择包
    // （name 36×UTF-16LE + size LE32=0，载荷 76B）→ ACK
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    spd::SpdFlasher f(s);
    m->reads << makeResponseFrame(spd::BSL_REP_ACK, QByteArray());
    QVERIFY(f.erasePartition(QStringLiteral("boot"), nullptr));
    // 选择包：name "boot" UTF-16LE（b 00 o 00 o 00 t 00）+ 补零至 36 单元 + size LE32=0
    QByteArray pkt(76, '\0');
    pkt[0] = 'b'; pkt[2] = 'o'; pkt[4] = 'o'; pkt[6] = 't';
    QCOMPARE(m->writes, makeResponseFrame(spd::BSL_CMD_ERASE_FLASH, pkt));
}

void TestSpdFlash::writePartitionSequence()
{
    // 按名写分区（行为观察 load_partition）：START_DATA(0x01) + 分区选择包 → ACK →
    // MIDST_DATA×N → END_DATA → ACK（分区写无 EXEC_DATA）
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    spd::SpdSession s(std::move(usb), 0x1782, 0);
    spd::SpdFlasher f(s);
    m->reads << makeResponseFrame(spd::BSL_REP_ACK, QByteArray())
             << makeResponseFrame(spd::BSL_REP_ACK, QByteArray())
             << makeResponseFrame(spd::BSL_REP_ACK, QByteArray());
    QVERIFY(f.writePartition(QStringLiteral("boot"), QByteArray("pkg", 3), nullptr));
    // 选择包：name "boot" UTF-16LE + 补零 + size LE32=3（载荷 76B，非 64 位模式）
    QByteArray pkt(76, '\0');
    pkt[0] = 'b'; pkt[2] = 'o'; pkt[4] = 'o'; pkt[6] = 't';
    pkt[72] = char(3);
    QByteArray expect;
    expect += makeResponseFrame(spd::BSL_CMD_START_DATA, pkt);
    expect += makeResponseFrame(spd::BSL_CMD_MIDST_DATA, QByteArray("pkg", 3));
    expect += makeResponseFrame(spd::BSL_CMD_END_DATA, QByteArray());
    QCOMPARE(m->writes, expect);
}

QTEST_APPLESS_MAIN(TestSpdFlash)
#include "test_spd_flash.moc"
