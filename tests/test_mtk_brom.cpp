#include <QtTest>
#include <memory>

#include "core/modes/mtk_brom.h"

// ---- MockUsbChannel：记录写入序列、预置读取队列（IBromUsb 注入）----
class MockUsbChannel : public mtkbrom::IBromUsb {
public:
    QByteArray writes;              // 全部写入字节（含空包）
    QList<QByteArray> reads;        // 按序弹出的读取响应；空表示返回空
    bool failOpen = false;
    QString openError;
    int pktSize = 0x400;

    bool open(QString *error) override
    {
        if (failOpen) {
            if (error) *error = openError;
            return false;
        }
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
        Q_UNUSED(timeoutMs)
        Q_UNUSED(error)
        if (reads.isEmpty()) { out.clear(); return false; }
        QByteArray r = reads.takeFirst();
        out = r.left(maxLen);
        return !r.isEmpty() || maxLen == 0;
    }
    int maxPacketSize() const override { return pktSize; }
    bool close() override { return true; }
};

// 注意：MockUsbChannel 所有权在 BromSession 构造时转移；
// 测试一律先保存裸指针 MockUsbChannel *m = usb.get() 再 move，后续经 m 访问。
class TestMtkBrom : public QObject {
    Q_OBJECT
private slots:
    // ---- 纯函数 ----
    void checksumXorsLittleEndianU16();
    void checksumOddLengthPadsZero();
    void handshakeEchoIsBitwiseNot();
    // ---- 握手 ----
    void handshakeSendsStartBytesAndVerifiesEcho();
    void handshakeNonBromPidPrefiresA0();
    void handshakeEchoMismatchFails();
    // ---- echo / 状态 ----
    void echoCmdVerifiesEchoByte();
    void echoCmdMismatchFails();
    void readStatusParsesBigEndian();
    // ---- get_target_config ----
    void targetConfigParsesBitfield();
    void targetConfigBadStatusFails();
    // ---- sendDa ----
    void sendDaFrameConstruction();
    void sendDaChunksAtPacketSize();
    void sendDaSlaRequiredFails();
    void sendDaChecksumMismatchStillSucceeds();
    // ---- jump ----
    void jumpDaSuccess();
    void jumpDaAddressMismatchFails();
    void jumpDa64SendsOneByte();
    // ---- 失败路径 ----
    void connectOpenFailure();
};

void TestMtkBrom::checksumXorsLittleEndianU16()
{
    // 对照 prepare_data()：00 01 02 03 → 0x0100 ^ 0x0302 = 0x0202
    QByteArray d("\x00\x01\x02\x03", 4);
    QCOMPARE(mtkbrom::BromSession::calcDaChecksum(d), quint32(0x0202));
}

void TestMtkBrom::checksumOddLengthPadsZero()
{
    // 奇数长度：AA BB CC → (0xBBAA) ^ (0x00CC) = 0xBBAA ^ 0x00CC = 0xBB66
    QByteArray d("\xAA\xBB\xCC", 3);
    QCOMPARE(mtkbrom::BromSession::calcDaChecksum(d), quint32(0xBB66));
}

void TestMtkBrom::handshakeEchoIsBitwiseNot()
{
    QCOMPARE(mtkbrom::BromSession::expectedHandshakeEcho(),
             QByteArray("\x5F\xF5\xAF\xFA", 4));
}

void TestMtkBrom::handshakeSendsStartBytesAndVerifiesEcho()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev); // 所有权转移；此后经 m 访问

    // 0x0003 属 BROM PID：不预发 A0，直接 A0 0A 50 05，回显取反
    m->reads << QByteArray("\x5F", 1) << QByteArray("\xF5", 1)
             << QByteArray("\xAF", 1) << QByteArray("\xFA", 1);
    QVERIFY(s.connect(nullptr));
    QCOMPARE(m->writes, QByteArray("\xA0\x0A\x50\x05", 4));
}

void TestMtkBrom::handshakeNonBromPidPrefiresA0()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x6000; // Preloader（非 BROM PID）
    mtkbrom::BromSession s(std::move(usb), dev);
    m->reads << QByteArray("\x5F", 1) << QByteArray("\xF5", 1)
             << QByteArray("\xAF", 1) << QByteArray("\xFA", 1);
    QVERIFY(s.connect(nullptr));
    // 预发 0xA0 在 startcmd 之前
    QCOMPARE(m->writes, QByteArray("\xA0\xA0\x0A\x50\x05", 5));
}

void TestMtkBrom::handshakeEchoMismatchFails()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    m->reads << QByteArray("\x5F", 1) << QByteArray("\xFF", 1); // 第二个字节回显错误
    QString err;
    QVERIFY(!s.connect(&err));
    QVERIFY(err.contains("握手"));
}

void TestMtkBrom::echoCmdVerifiesEchoByte()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    m->reads << QByteArray(1, char(0xFD)); // GET_HW_CODE 回显
    QVERIFY(s.echoCmd(mtkbrom::CMD_GET_HW_CODE, nullptr));
    QCOMPARE(m->writes, QByteArray(1, char(0xFD)));
}

void TestMtkBrom::echoCmdMismatchFails()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    m->reads << QByteArray(1, char(0xA5)); // 回显 NACK
    QString err;
    QVERIFY(!s.echoCmd(mtkbrom::CMD_GET_HW_CODE, &err));
    QVERIFY(err.contains("回显"));
}

void TestMtkBrom::readStatusParsesBigEndian()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    m->reads << QByteArray("\x00\xFF", 2);
    quint16 st = 0;
    QVERIFY(s.readStatus(st, nullptr));
    QCOMPARE(st, quint16(0x00FF));
}

void TestMtkBrom::targetConfigParsesBitfield()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    // config = 0x93（SBC+SLA+cert+cmdC8），status = 0
    m->reads << QByteArray(1, char(0xD8)) << QByteArray("\x00\x00\x00\x93\x00\x00", 6);
    mtkbrom::TargetConfig cfg;
    QVERIFY(s.getTargetConfig(cfg, nullptr));
    QCOMPARE(m->writes, QByteArray(1, char(0xD8)));
    QVERIFY(cfg.sbc); QVERIFY(cfg.sla); QVERIFY(!cfg.daa); QVERIFY(!cfg.epp);
    QVERIFY(cfg.cert); QVERIFY(!cfg.memread); QVERIFY(!cfg.memwrite); QVERIFY(cfg.cmdC8);
}

void TestMtkBrom::targetConfigBadStatusFails()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    m->reads << QByteArray(1, char(0xD8)) << QByteArray("\x00\x00\x00\x00\x01\x00", 6);
    mtkbrom::TargetConfig cfg;
    QString err;
    QVERIFY(!s.getTargetConfig(cfg, &err)); // status 0x0100 > 0xFF → 失败
    QVERIFY(err.contains("状态"));
}

void TestMtkBrom::sendDaFrameConstruction()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    // 回显：0xD7 + addr + len + siglen（各 4B）；状态 0；上传响应 checksum(4B)+status(2B)
    m->reads << QByteArray(1, char(0xD7))
             << QByteArray("\x00\x40\x00\x00", 4)   // addr 回显 0x400000
             << QByteArray("\x00\x00\x00\x02", 4)   // len 回显
             << QByteArray("\x00\x00\x00\x00", 4)   // sig_len 回显
             << QByteArray("\x00\x00", 2)           // status = 0
             << QByteArray("\x00\x00\x00\x00\x00\x00", 6); // checksum + status
    const QByteArray da("\xAA\xBB", 2); // 校验和 0xBBAA
    QVERIFY(s.sendDa(0x400000, 2, 0, da, nullptr));
    // 帧：0xD7 | addr(4B BE) | len(4B BE) | siglen(4B BE) | data，共 15 字节。
    // 空包（0x2000 对齐/收尾）在 QByteArray 层面不可见（mock 无法表达零长度
    // 传输），此处断言数据帧本身。
    QCOMPARE(m->writes,
             QByteArray("\xD7\x00\x40\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\xAA\xBB", 15));
}

void TestMtkBrom::sendDaChunksAtPacketSize()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    m->pktSize = 64;
    mtkbrom::BromSession s(std::move(usb), dev);
    m->reads << QByteArray(1, char(0xD7))
             << QByteArray("\x00\x00\x00\x00", 4) << QByteArray("\x00\x00\x00\x80", 4)
             << QByteArray("\x00\x00\x00\x00", 4) << QByteArray("\x00\x00", 2)
             << QByteArray("\x00\x00\x00\x00\x00\x00", 6);
    const QByteArray da(128, '\x01');
    QVERIFY(s.sendDa(0, 128, 0, da, nullptr));
    // 帧头 13B（0xD7 + addr + len + siglen）后跟 128 字节数据
    QCOMPARE(m->writes.mid(13), da);
}

void TestMtkBrom::sendDaSlaRequiredFails()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    m->reads << QByteArray(1, char(0xD7))
             << QByteArray("\x00\x00\x00\x00", 4) << QByteArray("\x00\x00\x00\x00", 4)
             << QByteArray("\x00\x00\x00\x00", 4)
             << QByteArray("\x1D\x0D", 2); // SLA 需要
    QString err;
    QVERIFY(!s.sendDa(0, 0, 0, QByteArray(), &err));
    QVERIFY(err.contains("SLA"));
}

void TestMtkBrom::sendDaChecksumMismatchStillSucceeds()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    m->reads << QByteArray(1, char(0xD7))
             << QByteArray("\x00\x00\x00\x00", 4) << QByteArray("\x00\x00\x00\x01", 4)
             << QByteArray("\x00\x00\x00\x00", 4) << QByteArray("\x00\x00", 2)
             << QByteArray("\xDE\xAD\xBE\xEF\x00\x00", 6); // checksum 不符但 status 0
    QVERIFY(s.sendDa(0, 1, 0, QByteArray("\x01", 1), nullptr)); // 警告不终止
}

void TestMtkBrom::jumpDaSuccess()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    m->reads << QByteArray(1, char(0xD5))
             << QByteArray("\x40\x00\x00\x00", 4) // 地址回显
             << QByteArray("\x00\x00", 2);        // status 0
    QVERIFY(s.jumpDa(0x40000000, nullptr));
    QCOMPARE(m->writes, QByteArray("\xD5\x40\x00\x00\x00", 5));
}

void TestMtkBrom::jumpDaAddressMismatchFails()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    m->reads << QByteArray(1, char(0xD5))
             << QByteArray("\x00\x00\x00\x00", 4); // 地址回显不符
    QString err;
    QVERIFY(!s.jumpDa(0x40000000, &err));
    QVERIFY(err.contains("地址"));
}

void TestMtkBrom::jumpDa64SendsOneByte()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    m->reads << QByteArray(1, char(0xDE))
             << QByteArray("\x40\x00\x00\x00", 4)
             << QByteArray("\x00\x00", 2);
    QVERIFY(s.jumpDa64(0x40000000, nullptr));
    // 0xDE | addr | 0x01（64 位标记）
    QCOMPARE(m->writes, QByteArray("\xDE\x40\x00\x00\x00\x01", 6));
}

void TestMtkBrom::connectOpenFailure()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    m->failOpen = true;
    m->openError = QStringLiteral("无权限");
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    QString err;
    QVERIFY(!s.connect(&err));
    QVERIFY(err.contains("无权限"));
}

QTEST_APPLESS_MAIN(TestMtkBrom)
#include "test_mtk_brom.moc"
