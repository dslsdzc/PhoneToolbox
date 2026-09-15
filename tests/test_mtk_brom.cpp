#include <QtTest>
#include <memory>

#include "core/modes/mtk_brom.h"
#include "core/modes/mtk_emmc.h"
#include "mtk_test_helpers.h"

using mtktest::be32;

// ---- MockUsbChannel：记录写入序列、预置读取队列（IBromUsb 注入）----
class MockUsbChannel : public mtkbrom::IBromUsb {
public:
    QByteArray writes;              // 全部写入字节（含空包）
    QList<QByteArray> writeFrames;  // 逐笔（每次 write() 一笔，**含空包**）—— 帧级断言用
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
        writeFrames << data;
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
    // ---- get_hw_code / get_hw_sw_ver（D1-T4）----
    void getHwCodeSplits32BitReply();
    void getHwSwVerParsesBigEndianQuad();
    void getHwCodeShortReplyFails();
    void getHwSwVerShortReplyFails();
    // ---- get_bromver / get_blver（D1-T6）----
    void bromVerAndBlVerAreSingleByteReads();
    void bromVerFailsWhenDeviceSilent();
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
    // ---- F1-2: 内存协议 ----
    void readMemoryFrame();
    void writeMemoryFrame();
    // ---- F1-2: DA 存储 ----
    void daStorageRequiresActiveDa();
    void emmcReadFrameAndDataLoop();
    void emmcWriteFrameAndDataLoop();
    void listPartitionsParses60ByteEntries();
    void listPartitionsParses58ByteEntries();
    void listPartitionsParses4cByteEntries();
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

// 0xFD：回 4B >I = (hwCode<<16)|hwVer（mtk_preloader.py:190-191 的 regular mode 拆分）
void TestMtkBrom::getHwCodeSplits32BitReply()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    // echo 0xFD 后设备只回 4 字节 —— **无尾随状态字**（T4 裁决：上游 sendcmd(0xFD, 4)）
    m->reads << QByteArray("\xFD", 1) << QByteArray("\x67\x65\xCA\x00", 4);
    quint16 hwCode = 0, hwVer = 0;
    QString err;
    QVERIFY2(s.getHwCode(hwCode, hwVer, &err), qPrintable(err));
    QCOMPARE(hwCode, quint16(0x6765));
    QCOMPARE(hwVer, quint16(0xCA00));
    // Qt 6.11 的 QByteArray::first() 需参数 → 比整串（顺带断言除命令回显外没有别的写）
    QCOMPARE(m->writes, QByteArray("\xFD", 1));
    QVERIFY(m->reads.isEmpty()); // 恰好消费「echo + 4B」：若有人再加状态字读，这里会失败
}

// 0xFC：回 8B >HHHH = (hw_sub_code, hw_ver, sw_ver, 保留)（mtk_preloader.py:928-930）
void TestMtkBrom::getHwSwVerParsesBigEndianQuad()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    // 同样无尾随状态字（T4 裁决：上游 sendcmd(0xFC, 8) → unpack(">HHHH")）
    m->reads << QByteArray("\xFC", 1)
             << QByteArray("\x8A\x00\xCB\x01\x00\x35\x00\x00", 8);
    mtkbrom::HwSwVer out;
    QString err;
    QVERIFY2(s.getHwSwVer(out, &err), qPrintable(err));
    QCOMPARE(out.hwSubCode, quint16(0x8A00));
    QCOMPARE(out.hwVer, quint16(0xCB01));
    QCOMPARE(out.swVer, quint16(0x0035));
    QCOMPARE(m->writes, QByteArray("\xFC", 1));
    QVERIFY(m->reads.isEmpty());
}

void TestMtkBrom::getHwCodeShortReplyFails()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\xFD", 1) << QByteArray("\x67\x65", 2); // 只回 2B（readExact 默认实现：短包即失败）
    // 预置非零：失败不得把半截数据（或 0）冒充成真值写回出参
    quint16 hwCode = 0x1111, hwVer = 0x2222;
    QString err;
    QVERIFY(!s.getHwCode(hwCode, hwVer, &err));
    QVERIFY(err.contains("精确读"));
    QCOMPARE(hwCode, quint16(0x1111));
    QCOMPARE(hwVer, quint16(0x2222));
}

void TestMtkBrom::getHwSwVerShortReplyFails()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\xFC", 1) << QByteArray("\x8A\x00\xCB\x01", 4); // 少回 4B（短包即失败）
    mtkbrom::HwSwVer out;
    out.hwSubCode = 0x1111; out.hwVer = 0x2222; out.swVer = 0x3333;
    QString err;
    QVERIFY(!s.getHwSwVer(out, &err));
    QVERIFY(err.contains("精确读"));
    QCOMPARE(out.hwSubCode, quint16(0x1111));
    QCOMPARE(out.hwVer, quint16(0x2222));
    QCOMPARE(out.swVer, quint16(0x3333));
}

// BROM 版本（mtk_preloader.py:657-662/664-673）：写 1B 命令 → 读 1B 值（**无回显校验**）
void TestMtkBrom::bromVerAndBlVerAreSingleByteReads()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\x05", 1) << QByteArray("\x02", 1);
    quint8 bromVer = 0;
    quint8 blVer = 0;
    QString err;
    QVERIFY2(s.getBromVer(bromVer, &err), qPrintable(err));
    QVERIFY2(s.getBlVer(blVer, &err), qPrintable(err));
    QCOMPARE(bromVer, quint8(0x05));
    QCOMPARE(blVer, quint8(0x02));
    QCOMPARE(m->writeFrames.size(), 2);
    QCOMPARE(m->writeFrames.at(0), QByteArray("\xFF", 1));      // GET_VERSION
    QCOMPARE(m->writeFrames.at(1), QByteArray("\xFE", 1));      // GET_BL_VER
}

// 读不到值 → 明确失败（不把 0 当版本号蒙过去）
void TestMtkBrom::bromVerFailsWhenDeviceSilent()
{
    auto usb = std::make_unique<MockUsbChannel>();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    quint8 v = 0;
    QString err;
    QVERIFY(!s.getBromVer(v, &err));
    QVERIFY(!err.isEmpty());
}

void TestMtkBrom::sendDaFrameConstruction()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    // 回显：0xD7 + addr + len + siglen（各 4B）；状态 0；上传响应 checksum(2B)+status(2B)
    m->reads << QByteArray(1, char(0xD7))
             << QByteArray("\x00\x40\x00\x00", 4)   // addr 回显 0x400000
             << QByteArray("\x00\x00\x00\x02", 4)   // len 回显
             << QByteArray("\x00\x00\x00\x00", 4)   // sig_len 回显
             << QByteArray("\x00\x00", 2)           // status = 0
             << QByteArray("\x00\x00\x00\x00", 4);  // u16 checksum + u16 status（rword(2)）
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
             << QByteArray("\x00\x00\x00\x00", 4);
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
             << QByteArray("\xDE\xAD\x00\x00", 4); // checksum 不符但 status 0
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
             << QByteArray(1, char(0x01)) // 0x01 标记回显
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

void TestMtkBrom::readMemoryFrame()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    // 对照 read()：echo 0xD1 → echo addr → echo dwords → status(2B) → 数据(8B) → status2(2B)
    m->reads << QByteArray(1, char(0xD1))
             << QByteArray("\x00\x10\x00\x00", 4)
             << QByteArray("\x00\x00\x00\x02", 4)
             << QByteArray("\x00\x00", 2)
             << QByteArray("\xDE\xAD\xBE\xEF\x01\x02\x03\x04", 8)
             << QByteArray("\x00\x00", 2);
    QByteArray out;
    QVERIFY(s.readMemory(0x00100000, 2, out, nullptr));
    QCOMPARE(out.size(), 8);
    QCOMPARE(out, QByteArray("\xDE\xAD\xBE\xEF\x01\x02\x03\x04", 8));
    QCOMPARE(m->writes,
             QByteArray("\xD1\x00\x10\x00\x00\x00\x00\x00\x02", 9));
}

void TestMtkBrom::writeMemoryFrame()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    // 对照 write()：echo 0xD4 → echo addr → echo count → status(2B,<=3) → 逐值 echo → status2(2B)
    m->reads << QByteArray(1, char(0xD4))
             << QByteArray("\x00\x10\x00\x00", 4)
             << QByteArray("\x00\x00\x00\x02", 4)
             << QByteArray("\x00\x00", 2)           // status <= 3
             << QByteArray("\x00\x00\x00\x01", 4)   // 值1 回显
             << QByteArray("\x00\x00\x00\x02", 4)   // 值2 回显
             << QByteArray("\x00\x00", 2);          // status2
    const QByteArray data("\x00\x00\x00\x01\x00\x00\x00\x02", 8);
    QVERIFY(s.writeMemory(0x00100000, data, nullptr));
    QCOMPARE(m->writes,
             QByteArray("\xD4\x00\x10\x00\x00\x00\x00\x00\x02"
                        "\x00\x00\x00\x01\x00\x00\x00\x02", 17));
}

void TestMtkBrom::daStorageRequiresActiveDa()
{
    auto usb = std::make_unique<MockUsbChannel>();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    mtkbrom::DaStorage st(s);
    QByteArray out;
    QString err;
    QVERIFY(!st.emmcRead(0, 512, out, 0x08, &err));
    QVERIFY(err.contains("DA"));
}

void TestMtkBrom::emmcReadFrameAndDataLoop()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    mtkbrom::DaStorage st(s);
    st.setDaActive(true);
    // switch_part: 0x60→ACK, parttype→ACK；读: 0xD6→ACK; 数据块(4B)+checksum(2B)→ACK
    m->reads << QByteArray(1, char(0x5A)) << QByteArray(1, char(0x5A))
             << QByteArray(1, char(0x5A))
             << QByteArray("\xAA\xBB\xCC\xDD", 4)
             << QByteArray("\x02\x9E", 2); // checksum = AA+BB+CC+DD = 0x29E
    QByteArray out;
    QVERIFY(st.emmcRead(0x1000, 4, out, 0x08, nullptr));
    QCOMPARE(out, QByteArray("\xAA\xBB\xCC\xDD", 4));
    QVERIFY(m->reads.isEmpty()); // 数据块 + checksum 均经读取队列按序消费
    // 完整帧布局（读方向数据不写回设备，数据/checksum 经 m->reads 返回）：
    //   [0] 0x60 [1] 0x08                  switch_part
    //   [2] 0xD6 [3] 0x0C [4] 0x02         READ_CMD 帧头
    //   [5..12] addr 8B BE [13..20] len 8B BE [21..24] packetsize 4B BE
    //   [25] 数据块循环 ACK
    QCOMPARE(m->writes.mid(0, 2), QByteArray("\x60\x08", 2));
    QCOMPARE(m->writes.mid(2, 3), QByteArray("\xD6\x0C\x02", 3));
    QCOMPARE(m->writes.mid(5, 8), QByteArray("\x00\x00\x00\x00\x00\x00\x10\x00", 8));
    QCOMPARE(m->writes.mid(13, 8), QByteArray("\x00\x00\x00\x00\x00\x00\x00\x04", 8));
    QCOMPARE(m->writes.mid(21, 4), QByteArray("\x00\x10\x00\x00", 4));
    QCOMPARE(m->writes.mid(25, 1), QByteArray(1, char(0x5A))); // 循环 ACK
    QCOMPARE(m->writes.size(), 26); // 帧 25B + 循环 ACK
}

void TestMtkBrom::emmcWriteFrameAndDataLoop()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    mtkbrom::DaStorage st(s);
    st.setDaActive(true);
    // 0x62 → ACK；循环：发 ACK → 块 → checksum → 读 CONT
    m->reads << QByteArray(1, char(0x5A))
             << QByteArray(1, char(0x69)); // CONT_CHAR
    const QByteArray data("\x01\x02", 2);
    QVERIFY(st.emmcWrite(0x1000, data, 0x08, nullptr));
    // 帧头：0x62 | 0x02(EMMC) | 0x08 | addr 8B BE | len 8B BE | packetsize 4B BE
    QCOMPARE(m->writes.mid(0, 1), QByteArray("\x62", 1));
    QCOMPARE(m->writes.mid(1, 2), QByteArray("\x02\x08", 2));
    QCOMPARE(m->writes.mid(3, 8), QByteArray("\x00\x00\x00\x00\x00\x00\x10\x00", 8)); // addr
    QCOMPARE(m->writes.mid(11, 8), QByteArray("\x00\x00\x00\x00\x00\x00\x00\x02", 8)); // len
    QCOMPARE(m->writes.mid(19, 4), QByteArray("\x00\x10\x00\x00", 4)); // packetsize
    // 数据循环：前置 ACK(23) | 512B 块(24..535：01 02 + 510 个 00，512 对齐补零)
    //            | checksum 2B(536..537) = 逐字节和 0x0003
    QCOMPARE(m->writes.at(23), char(0x5A)); // 前置 ACK
    QCOMPARE(m->writes.mid(24, 2), data);
    QCOMPARE(m->writes.size(), 24 + 512 + 2);
    QCOMPARE(m->writes.mid(536, 2), QByteArray("\x00\x03", 2));
}

void TestMtkBrom::listPartitionsParses60ByteEntries()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    mtkbrom::DaStorage st(s);
    st.setDaActive(true);
    // 构造 partdata：0x60B 条目（partdata[0x48]==0xFF 路径），2 条
    QByteArray pd(0xC0, '\x00');
    pd[0x48] = char(0xFF);
    memcpy(pd.data(), "boot", 4);
    // 条目1: name="boot" size=0x10000 offset=0；条目2: name="system" size=0x20000 offset=0x10000
    // 0x60B 条目布局: name[0..0x40), size@0x40 <Q(小端), flags@0x48 <Q, offset@0x50 <Q
    memcpy(pd.data() + 0x40, "\x00\x00\x01\x00\x00\x00\x00\x00", 8);
    memcpy(pd.data() + 0x60, "system", 6);
    memcpy(pd.data() + 0xA0, "\x00\x00\x02\x00\x00\x00\x00\x00", 8);
    memcpy(pd.data() + 0xB0, "\x00\x00\x01\x00\x00\x00\x00\x00", 8);
    // read_pmt 协议：写 0xA5 → 读 ack → 读 length → 发 ACK（无读）→ 读数据 → 发 ACK
    m->reads << QByteArray(1, char(0x5A))
             << QByteArray("\x00\x00\x00\xC0", 4)
             << pd;
    QList<mtkbrom::EmPartition> parts;
    QVERIFY(st.listPartitions(parts, nullptr));
    QCOMPARE(parts.size(), 2);
    QCOMPARE(parts[0].name, QStringLiteral("boot"));
    QCOMPARE(parts[0].sizeBytes, quint64(0x10000));
    QCOMPARE(parts[1].name, QStringLiteral("system"));
    QCOMPARE(parts[1].offsetBytes, quint64(0x10000));
}

void TestMtkBrom::listPartitionsParses58ByteEntries()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    mtkbrom::DaStorage st(s);
    st.setDaActive(true);
    // 0x58B 条目：判定走 mask_flags 路径，且 getLe32(0x48) 必须落在 (0,0xA) ——
    // 对 0x58 条目而言 0x48 是 offset@0x48 的低 4B，故 offset 须 < 0xA（真机常见值）；
    // pd[0x48] = 0x01 ≠ 0xFF，maskFlags = 1 → 0x58。
    QByteArray pd(0xB0, '\x00');
    memcpy(pd.data(), "boot", 4);
    // 条目1: name="boot" size@0x40 <Q(小端)=0x10000 offset@0x48 <Q=0x1；
    // 条目2 @0x58: name="system" size@0x98 <Q=0x20000 offset@0xA0 <Q=0x10001
    memcpy(pd.data() + 0x40, "\x00\x00\x01\x00\x00\x00\x00\x00", 8);
    memcpy(pd.data() + 0x48, "\x01\x00\x00\x00\x00\x00\x00\x00", 8);
    memcpy(pd.data() + 0x58, "system", 6);
    memcpy(pd.data() + 0x98, "\x00\x00\x02\x00\x00\x00\x00\x00", 8);
    memcpy(pd.data() + 0xA0, "\x01\x00\x01\x00\x00\x00\x00\x00", 8);
    m->reads << QByteArray(1, char(0x5A))
             << QByteArray("\x00\x00\x00\xB0", 4)
             << pd;
    QList<mtkbrom::EmPartition> parts;
    QVERIFY(st.listPartitions(parts, nullptr));
    QCOMPARE(parts.size(), 2);
    QCOMPARE(parts[0].name, QStringLiteral("boot"));
    QCOMPARE(parts[0].sizeBytes, quint64(0x10000));
    QCOMPARE(parts[0].offsetBytes, quint64(0x1));
    QCOMPARE(parts[1].name, QStringLiteral("system"));
    QCOMPARE(parts[1].sizeBytes, quint64(0x20000));
    QCOMPARE(parts[1].offsetBytes, quint64(0x10001));
}

void TestMtkBrom::listPartitionsParses4cByteEntries()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    mtkbrom::DaStorage st(s);
    st.setDaActive(true);
    // 0x4C 条目（32-bit）：判定走 mask_flags 路径，flags@0x48=0 → maskFlags=0 ∉(0,0xA) → 0x4C；
    // 布局: name@0(0x40) + size@0x40(4B) + offset@0x44(4B) + flags@0x48(4B)。
    // size/offset 均为 4B 字段，此前用 getLe64 会把相邻字段合并进高 32 位（审查 F2）。
    QByteArray pd(0x98, '\x00');
    memcpy(pd.data(), "boot", 4);
    // 条目1: name="boot" size@0x40 <I=0x10000 offset@0x44 <I=0x10000 flags@0x48 <I=0；
    // 条目2 @0x4C: name="system" size@0x8C <I=0x20000 offset@0x90 <I=0x20000
    memcpy(pd.data() + 0x40, "\x00\x00\x01\x00", 4);
    memcpy(pd.data() + 0x44, "\x00\x00\x01\x00", 4);
    memcpy(pd.data() + 0x4C, "system", 6);
    memcpy(pd.data() + 0x8C, "\x00\x00\x02\x00", 4);
    memcpy(pd.data() + 0x90, "\x00\x00\x02\x00", 4);
    m->reads << QByteArray(1, char(0x5A))
             << QByteArray("\x00\x00\x00\x98", 4)
             << pd;
    QList<mtkbrom::EmPartition> parts;
    QVERIFY(st.listPartitions(parts, nullptr));
    QCOMPARE(parts.size(), 2);
    QCOMPARE(parts[0].name, QStringLiteral("boot"));
    QCOMPARE(parts[0].sizeBytes, quint64(0x10000));
    QCOMPARE(parts[0].offsetBytes, quint64(0x10000));
    QCOMPARE(parts[1].name, QStringLiteral("system"));
    QCOMPARE(parts[1].sizeBytes, quint64(0x20000));
    QCOMPARE(parts[1].offsetBytes, quint64(0x20000));
}

QTEST_APPLESS_MAIN(TestMtkBrom)
#include "test_mtk_brom.moc"
