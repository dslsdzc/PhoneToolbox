#include <QtTest>
#include <memory>

#include "core/modes/spd_flash.h"

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
    // ---- 帧 ----
    void frameConstruction();
    void frameEscaping();
    // ---- 命令 ----
    void sendCommandAck();
    void sendCommandBadChecksumFails();
    void connectOpenFailure();
    void enumerateVid();
};

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
    // payload 1B 0x7E → 转义为 0x7D 0x5E（HDLC：0x7D 0x5D 是 0x7D 的转义）
    QVERIFY(m->writes.contains(QByteArray("\x7D\x5E", 2)));
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

QTEST_APPLESS_MAIN(TestSpdFlash)
#include "test_spd_flash.moc"
