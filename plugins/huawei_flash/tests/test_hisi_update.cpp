#include <QtTest>
#include <memory>

#include "hisi_update.h"

// MockUsbChannel：记录写入序列、预置读取队列（IUsbChannel 注入）
class MockUsbChannel : public hisi::IUsbChannel {
public:
    QByteArray writes;
    QList<QByteArray> reads;
    QByteArray stale; // 读缓冲残留（discardInput 清除；read 优先消费）
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
        if (!stale.isEmpty()) {
            out = stale.left(maxLen);
            stale.remove(0, out.size());
            return true;
        }
        if (reads.isEmpty()) { out.clear(); return false; }
        QByteArray r = reads.takeFirst();
        out = r.left(maxLen);
        return !r.isEmpty() || maxLen == 0;
    }
    bool discardInput(int maxLen, int timeoutMs, QString *error) override
    {
        Q_UNUSED(maxLen) Q_UNUSED(timeoutMs) Q_UNUSED(error)
        stale.clear();
        return true;
    }
    bool close() override { return true; }
};

class TestHisiUpdate : public QObject {
    Q_OBJECT
private slots:
    // ---- 纯函数 ----
    void crc16X25StandardVector();
    void crc16X25Empty();
    void escapePayloadTransforms();
    void buildFrameWrapsAndAppendsCrc();
    // ---- 握手 ----
    void handshakeFrameAndResponse();
    void handshakeRetriesOnMismatch();
    // ---- 命令 ----
    void sendCommandAckSuccess();
    void sendCommandDeviceError();
    void sendCommandTimeoutFails();
    void sendCommandDiscardsStaleInput();
    void readFrameDiscardsLeadingNoise();
    void connectOpenFailure();
};

void TestHisiUpdate::crc16X25StandardVector()
{
    // 标准 X25 测试向量："123456789" → 0x906E
    QByteArray d("123456789");
    QCOMPARE(hisi::crc16X25(d), quint16(0x906E));
}

void TestHisiUpdate::crc16X25Empty()
{
    // init 0xFFFF，空数据 → ~0xFFFF = 0x0000
    QCOMPARE(hisi::crc16X25(QByteArray()), quint16(0x0000));
}

void TestHisiUpdate::escapePayloadTransforms()
{
    // 0x7E → 0x7D 0x5E；0x7D → 0x7D 0x5D；其余原样
    QByteArray in("\x7E\x7D\x01", 3);
    QCOMPARE(hisi::escapePayload(in), QByteArray("\x7D\x5E\x7D\x5D\x01", 5));
}

void TestHisiUpdate::buildFrameWrapsAndAppendsCrc()
{
    // buildFrame(0x0A, 空)：payload = [0x0A]；CRC16-X25([0x0A]) 小端
    // 计算：crc16X25 of [0x0A] —— 手动算或按实现；此处断言结构：
    // 0x7E | escaped(cmd+crcLE) | 0x7E
    const QByteArray frame = hisi::buildFrame(hisi::FRAME_REBOOT, QByteArray());
    QVERIFY(frame.startsWith('\x7E'));
    QVERIFY(frame.endsWith('\x7E'));
    const quint16 crc = hisi::crc16X25(QByteArray(1, char(hisi::FRAME_REBOOT)));
    QByteArray expect("\x7E", 1);
    expect += hisi::escapePayload(QByteArray(1, char(hisi::FRAME_REBOOT))
                                      .append(char(crc & 0xFF)).append(char((crc >> 8) & 0xFF)));
    expect += QByteArray("\x7E", 1);
    QCOMPARE(frame, expect);
}

void TestHisiUpdate::handshakeFrameAndResponse()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    // 响应含前缀 7E 26 00 00 25 A7
    m->reads << QByteArray("\x7E\x26\x00\x00\x25\xA7\x00\x06\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x7E", 21);
    QVERIFY(s.connect(nullptr));
    // 握手帧：19B 命令 + CRC LE + 0x7E（无前导 0x7E）
    const quint16 crc = hisi::crc16X25(hisi::HisiSession::kHandshakeCommand);
    QByteArray expect = hisi::HisiSession::kHandshakeCommand;
    expect.append(char(crc & 0xFF)).append(char((crc >> 8) & 0xFF));
    expect.append('\x7E');
    QCOMPARE(m->writes, expect);
}

void TestHisiUpdate::handshakeRetriesOnMismatch()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    // 两次错误响应（含 7E 但不含预期前缀）+ 一次正确
    m->reads << QByteArray("\x7E\x03\x00\x00\x7E", 5)
             << QByteArray("\x7E\x03\x00\x00\x7E", 5)
             << QByteArray("\x7E\x26\x00\x00\x25\xA7\x7E", 7);
    QVERIFY(s.connect(nullptr));
    // 三次握手帧：19B 命令 + 2B CRC LE + 0x7E = 22B，3 帧共 66B，帧内容相同
    QCOMPARE(m->writes.size(), 66);
    QCOMPARE(m->writes.mid(0, 22), m->writes.mid(22, 22));
}

void TestHisiUpdate::sendCommandAckSuccess()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    m->reads << QByteArray(hisi::HisiSession::kAckResponse);
    QVERIFY(s.sendCommand(hisi::FRAME_REBOOT, QByteArray(), 0.3, nullptr));
    QVERIFY(m->writes.endsWith('\x7E'));
}

void TestHisiUpdate::sendCommandDeviceError()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    m->reads << QByteArray("\x7E\x03\x00\x00\x00\x00\x7E", 7); // 0x03 错误帧
    QString err;
    QVERIFY(!s.sendCommand(hisi::FRAME_REBOOT, QByteArray(), 0.3, &err));
    QVERIFY(err.contains("设备错误") || err.contains("0x03"));
}

void TestHisiUpdate::sendCommandTimeoutFails()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    // 无响应（队列空 → read 返回 false）
    QString err;
    QVERIFY(!s.sendCommand(hisi::FRAME_REBOOT, QByteArray(), 0.1, &err));
    QVERIFY(!err.isEmpty());
}

void TestHisiUpdate::sendCommandDiscardsStaleInput()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    m->stale = QByteArray("\x7E\x03\x00\x00\x00\x00\x7E", 7); // 上次命令的迟到错误帧
    m->reads << QByteArray(hisi::HisiSession::kAckResponse);  // 本命令的 ACK
    QVERIFY(s.sendCommand(hisi::FRAME_REBOOT, QByteArray(), 0.3, nullptr));
}

void TestHisiUpdate::readFrameDiscardsLeadingNoise()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    // 前导垃圾字节 + 有效 ACK 帧；drain 已清 stale，噪声随响应读入被 readFrame 过滤
    m->reads << QByteArray("\x00\x01\x02\x7E\x02\x6A\xD3\x7E", 8);
    QVERIFY(s.sendCommand(hisi::FRAME_REBOOT, QByteArray(), 0.3, nullptr));
}

void TestHisiUpdate::connectOpenFailure()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    m->failOpen = true;
    m->openError = QStringLiteral("无权限");
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    QString err;
    QVERIFY(!s.connect(&err));
    QVERIFY(err.contains("无权限"));
}

QTEST_APPLESS_MAIN(TestHisiUpdate)
#include "test_hisi_update.moc"
