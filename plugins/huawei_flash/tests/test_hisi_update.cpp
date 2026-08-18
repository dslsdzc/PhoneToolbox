#include <QtTest>
#include <QTemporaryFile>
#include <memory>

#include <cstring>

#include <zlib.h>

#include "hisi_flash.h"
#include "hisi_update.h"
#include "update_app.h"

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
    // ---- F2-2: 命令层与刷写 ----
    void zlibCompressProduces781Header();
    void unlockFrame();
    void flashPartitionFrames();
    void rebootCommands();
    // ---- F2-3: update.app 解析 ----
    void parseUpdateAppEntries();
    void parseUpdateAppBadMagicFails();
    void parseUpdateAppHugeDataLenFails();
    void parseUpdateAppHeaderLenExceedsFails();
    // ---- F2-3: xloader 边界 ----
    void isXloaderPartitionNames();
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

void TestHisiUpdate::zlibCompressProduces781Header()
{
    // zlib 格式：0x78 0x01 头；解压后还原
    const QByteArray data("hello hisi", 10);
    const QByteArray comp = hisi::zlibCompress(data);
    QVERIFY(comp.size() >= 2);
    QCOMPARE(quint8(comp[0]), quint8(0x78));
    QCOMPARE(quint8(comp[1]), quint8(0x01));
    // 用 zlib uncompress 验证（校验 Deflate 流 + Adler32 尾；
    // qUncompress 需要 qCompress 的 4 字节长度前缀，不适用裸 zlib 流）
    QByteArray back(int(data.size()), Qt::Uninitialized);
    uLongf len = uLongf(back.size());
    const int res = ::uncompress(reinterpret_cast<Bytef *>(back.data()), &len,
                                 reinterpret_cast<const Bytef *>(comp.constData()),
                                 uLong(comp.size()));
    QCOMPARE(res, Z_OK);
    back.truncate(int(len));
    QCOMPARE(back, data);
}

void TestHisiUpdate::unlockFrame()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    hisi::HisiFlasher f(s);
    m->reads << QByteArray(hisi::HisiSession::kAckResponse);
    const QByteArray code("\x01\x02\x03\x04", 4);
    QVERIFY(f.unlock(code, nullptr));
    // 帧：0x7E | esc(0x0B + code + crcLE) | 0x7E
    QByteArray expect(1, '\x7E');
    QByteArray body(1, char(hisi::FRAME_UNLOCK));
    body += code;
    const quint16 crc = hisi::crc16X25(body);
    body.append(char(crc & 0xFF)).append(char((crc >> 8) & 0xFF));
    expect += hisi::escapePayload(body);
    expect += QByteArray(1, '\x7E');
    QCOMPARE(m->writes, expect);
}

void TestHisiUpdate::flashPartitionFrames()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    hisi::HisiFlasher f(s);
    // HEAD + DATA + TAIL 三个响应（成功）
    m->reads << QByteArray(hisi::HisiSession::kAckResponse)
             << QByteArray(hisi::HisiSession::kAckResponse)
             << QByteArray(hisi::HisiSession::kAckResponse);
    // 临时镜像文件（< 0x20000，单块）
    QTemporaryFile tmp;
    QVERIFY(tmp.open());
    tmp.write("test image data");
    tmp.flush();
    const QByteArray header(64, '\x00'); // 64B 分区头（含 fileSeq@20 全 0）
    QVERIFY(f.flashPartition(QStringLiteral("boot"), header, tmp.fileName(), nullptr, nullptr));
    // 三帧顺序：HEAD(0x41) → DATA(0x0F) → TAIL(0x43)
    QVERIFY(m->writes.contains('\x41'));
    QVERIFY(m->writes.contains('\x0F'));
    QVERIFY(m->writes.contains('\x43'));
    QVERIFY(m->writes.indexOf('\x41') < m->writes.indexOf('\x0F'));
    QVERIFY(m->writes.indexOf('\x0F') < m->writes.indexOf('\x43'));
    // DATA 帧定位：按 HEAD 帧帧长计算（不依赖 0x0F 是否恰为内容中首遇字节），
    // DATA 帧紧随 HEAD 帧（0x7E 0x0F 命令字节）
    const QByteArray headFrame = hisi::buildFrame(hisi::FRAME_HEAD, header);
    const int headStart = m->writes.indexOf(headFrame);
    QVERIFY(headStart >= 0);
    const int dataStart = headStart + headFrame.size();
    QCOMPARE(m->writes.at(dataStart), char('\x7E'));
    QCOMPARE(m->writes.at(dataStart + 1), char('\x0F'));
    // DATA 帧体（协议核实记录）：0x0F + (fileSeq+addr) BE32 + origLen BE32
    // 镜像 15B → origLen = 0x0F
    QCOMPARE(m->writes.mid(dataStart + 2, 4), QByteArray("\x00\x00\x00\x00", 4));
    QCOMPARE(m->writes.mid(dataStart + 6, 4), QByteArray("\x00\x00\x00\x0F", 4));
}

void TestHisiUpdate::rebootCommands()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    hisi::HisiFlasher f(s);
    m->reads << QByteArray(hisi::HisiSession::kAckResponse)
             << QByteArray(hisi::HisiSession::kAckResponse);
    QVERIFY(f.reboot(nullptr));
    QVERIFY(m->writes.contains('\x0A'));
    QVERIFY(m->writes.contains('\x32'));
    // 顺序：REBOOT(0x0A) 先于 FORCE_REBOOT(0x32)
    QVERIFY(m->writes.indexOf('\x0A') < m->writes.indexOf('\x32'));
}

void TestHisiUpdate::parseUpdateAppEntries()
{
    // 构造 2 条条目：boot（0x10000 数据）+ system（0x20000 数据）
    QByteArray app;
    for (const char *name : {"boot", "system"}) {
        const quint32 headerLen = 98 + 8; // 98 固定 + 8 剩余
        QByteArray h(headerLen, '\0');
        h[0] = 0x55; h[1] = 0xAA; h[2] = 0x5A; h[3] = 0xA5;
        const quint32 len = headerLen;
        h[4] = char(len & 0xFF); h[5] = char((len >> 8) & 0xFF);
        h[6] = char((len >> 16) & 0xFF); h[7] = char((len >> 24) & 0xFF);
        // dataLength @24（magic 4 + headerLen 4 + 4 + 8 + 4）
        const quint32 dlen = QByteArray(name).size() == 4 ? 0x10000u : 0x20000u;
        h[24] = char(dlen & 0xFF); h[25] = char((dlen >> 8) & 0xFF);
        h[26] = char((dlen >> 16) & 0xFF); h[27] = char((dlen >> 24) & 0xFF);
        // 分区名 @60（32B NUL 结尾）
        memcpy(h.data() + 60, name, qMin<qsizetype>(strlen(name), 32));
        // fileSeq @20（大端）：boot=0, system=1
        h[20] = char(QByteArray(name).size() == 4 ? 0 : 1);
        app += h;
        app += QByteArray(int(dlen), char(0xAB));
    }
    QList<hisi::AppPartition> parts;
    QVERIFY(hisi::parseUpdateApp(app, parts, nullptr));
    QCOMPARE(parts.size(), 2);
    QCOMPARE(parts[0].header.size(), 106); // 98 固定 + 8 剩余：完整头透传
    QCOMPARE(parts[0].name, QStringLiteral("boot"));
    QCOMPARE(parts[0].data.size(), 0x10000);
    QCOMPARE(parts[1].name, QStringLiteral("system"));
    QCOMPARE(parts[1].data.size(), 0x20000);
}

void TestHisiUpdate::parseUpdateAppBadMagicFails()
{
    QByteArray app(200, '\x00');
    QString err;
    QList<hisi::AppPartition> parts;
    QVERIFY(!hisi::parseUpdateApp(app, parts, &err));
    QVERIFY(err.contains("magic"));
}

void TestHisiUpdate::parseUpdateAppHugeDataLenFails()
{
    // 数据长度字段 0xFFFFFFFF（恶意）→ 拒绝（> INT_MAX）
    QByteArray h(98, '\0');
    h[0] = 0x55; h[1] = 0xAA; h[2] = 0x5A; h[3] = 0xA5;
    const quint32 headerLen = 98;
    h[4] = char(headerLen & 0xFF); h[5] = char((headerLen >> 8) & 0xFF);
    h[6] = char((headerLen >> 16) & 0xFF); h[7] = char((headerLen >> 24) & 0xFF);
    h[24] = 0xFF; h[25] = 0xFF; h[26] = 0xFF; h[27] = 0xFF; // dataLen = 0xFFFFFFFF
    memcpy(h.data() + 60, "boot", 4);
    QByteArray app = h + QByteArray(10, '\x00'); // 数据区不足
    QList<hisi::AppPartition> parts;
    QString err;
    QVERIFY(!hisi::parseUpdateApp(app, parts, &err));
    QVERIFY(err.contains("越界"));
    // 错误串携带真实分区名（分区名在越界校验前已解析）
    QVERIFY(err.contains(QStringLiteral("boot")));
}

void TestHisiUpdate::parseUpdateAppHeaderLenExceedsFails()
{
    // headerLen 超出剩余字节 → 拒绝
    QByteArray h(98, '\0');
    h[0] = 0x55; h[1] = 0xAA; h[2] = 0x5A; h[3] = 0xA5;
    const quint32 headerLen = 0x1000; // 远超实际
    h[4] = char(headerLen & 0xFF); h[5] = char((headerLen >> 8) & 0xFF);
    h[6] = char((headerLen >> 16) & 0xFF); h[7] = char((headerLen >> 24) & 0xFF);
    QByteArray app = h;
    QList<hisi::AppPartition> parts;
    QString err;
    QVERIFY(!hisi::parseUpdateApp(app, parts, &err));
    QVERIFY(err.contains("头长"));
}

void TestHisiUpdate::isXloaderPartitionNames()
{
    QVERIFY(hisi::isXloaderPartition(QStringLiteral("xloader")));
    QVERIFY(hisi::isXloaderPartition(QStringLiteral("preloader")));
    QVERIFY(hisi::isXloaderPartition(QStringLiteral("xloader_a")));
    QVERIFY(!hisi::isXloaderPartition(QStringLiteral("boot")));
    QVERIFY(!hisi::isXloaderPartition(QStringLiteral("system")));
}

QTEST_APPLESS_MAIN(TestHisiUpdate)
#include "test_hisi_update.moc"
