#include <QtTest>
#include <memory>

#include "core/modes/mtk_brom.h"
#include "core/modes/mtk_emmc.h"
#include "core/modes/mtk_payload.h"

// MockUsbChannel 与 test_mtk_brom.cpp 相同（复制；测试间不共享 TU）
class MockUsbChannel : public mtkbrom::IBromUsb {
public:
    QByteArray writes;
    QList<QByteArray> reads;
    bool failOpen = false;
    QString openError;
    int pktSize = 0x400;

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
    int maxPacketSize() const override { return pktSize; }
    bool close() override { return true; }
};

class TestMtkPayload : public QObject {
    Q_OBJECT
private slots:
    void patchPreloaderSecurityReplacesPatterns();
    void patchPreloaderSecurityNoMatchReturnsFalse();
    void sendPayloadFrames();
    void sendPayloadSlaFails();
    void flashPartitionWritesAtPartitionOffset();
    void flashPartitionUnknownNameFails();
};

void TestMtkPayload::patchPreloaderSecurityReplacesPatterns()
{
    // SBC 修补全部 5 个核心模式（GPLv3 子模块来源标注，见 mtk_payload.cpp kPatches）：
    // 每个模式：输入 fromHex(from) → 修补后 == fromHex(to)，且返回 true
    struct Case { const char *hexFrom; const char *hexTo; };
    const Case kCases[] = {
        {"10B50C680268", "10B5012010BD"},                        // ram blacklist
        {"08B5104B7B441B681B68", "00207047000000000000"},        // seclib_sec_usbdl_enabled
        {"5072656C6F61646572205374617274", "50617463686564204C205374617274"}, // Patched loader msg
        {"F0B58BB002AE20250C460746", "002070470000000000205374617274"},       // sec_img_auth
        {"FFC0F3400008BD", "FF4FF0000008BD"},                    // get_vfy_policy
    };
    for (const Case &c : kCases) {
        QByteArray pl = QByteArray::fromHex(c.hexFrom);
        QVERIFY(mtkbrom::patchPreloaderSecurity(pl));
        QCOMPARE(pl, QByteArray::fromHex(c.hexTo));
    }
}

void TestMtkPayload::patchPreloaderSecurityNoMatchReturnsFalse()
{
    QByteArray pl = QByteArray::fromHex("DEADBEEF");
    QVERIFY(!mtkbrom::patchPreloaderSecurity(pl));
    QCOMPARE(pl, QByteArray::fromHex("DEADBEEF")); // 不变
}

void TestMtkPayload::sendPayloadFrames()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    // connect（握手）+ sendDa 帧 + jumpDa
    m->reads << QByteArray("\x5F", 1) << QByteArray("\xF5", 1)
             << QByteArray("\xAF", 1) << QByteArray("\xFA", 1);
    QVERIFY(s.connect(nullptr));
    m->writes.clear();

    // sendDa（规格 §2.4）：0xD7 + addr(0x0 默认) + len + siglen(0) + status + resp
    m->reads << QByteArray(1, char(0xD7))
             << QByteArray("\x00\x00\x00\x00", 4)
             << QByteArray("\x00\x00\x00\x02", 4)
             << QByteArray("\x00\x00\x00\x00", 4)
             << QByteArray("\x00\x00", 2)
             << QByteArray("\x00\x00\x00\x00", 4); // u16 checksum + u16 status（rword(2)）
    // jumpDa（规格 §2.4 步骤 7）：0xD5 + addr 回显 + status 0
    m->reads << QByteArray(1, char(0xD5))
             << QByteArray("\x00\x00\x00\x00", 4)
             << QByteArray("\x00\x00", 2);

    const QByteArray da("\xAA\xBB", 2);
    QVERIFY(mtkbrom::sendPayload(s, da, nullptr));
    // 帧前缀 + 数据 + 空包 + jumpDa 帧
    QVERIFY(m->writes.startsWith(
        QByteArray("\xD7\x00\x00\x00\x00\x00\x00\x00\x02\x00\x00\x00\x00\xAA\xBB", 15)));
    QVERIFY(m->writes.endsWith(QByteArray("\xD5\x00\x00\x00\x00", 5)));
}

void TestMtkPayload::sendPayloadSlaFails()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    m->reads << QByteArray("\x5F", 1) << QByteArray("\xF5", 1)
             << QByteArray("\xAF", 1) << QByteArray("\xFA", 1);
    QVERIFY(s.connect(nullptr));
    // SEND_DA 状态 0x1D0D（规格 §2.4 步骤 5：SLA 挑战）
    m->reads << QByteArray(1, char(0xD7))
             << QByteArray("\x00\x00\x00\x00", 4)
             << QByteArray("\x00\x00\x00\x02", 4)
             << QByteArray("\x00\x00\x00\x00", 4)
             << QByteArray("\x1D\x0D", 2); // SLA 需要
    QString err;
    QVERIFY(!mtkbrom::sendPayload(s, QByteArray("\xAA\xBB", 2), &err));
    QVERIFY(err.contains("SLA"));
}

void TestMtkPayload::flashPartitionWritesAtPartitionOffset()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    mtkbrom::DaStorage st(s);
    st.setDaActive(true);
    // listPartitions（规格 §3.6 PMT）：ack + len + partdata(1 条 0x60B, name=boot, offset=0x1000)
    QByteArray pd(0x60, '\0');
    pd[0x48] = char(0xFF);
    memcpy(pd.data(), "boot", 4);
    // 0x60B 条目字段小端：0x1000 = "\x00\x10\x00\x00\x00\x00\x00\x00"
    memcpy(pd.data() + 0x40, "\x00\x10\x00\x00\x00\x00\x00\x00", 8); // size
    memcpy(pd.data() + 0x50, "\x00\x10\x00\x00\x00\x00\x00\x00", 8); // offset
    m->reads << QByteArray(1, char(0x5A))
             << QByteArray("\x00\x00\x00\x60", 4)
             << pd;
    // emmcWrite（规格 §3.5 Legacy）：0x62 → ACK → CONT
    m->reads << QByteArray(1, char(0x5A))
             << QByteArray(1, char(0x69));
    const QByteArray img("\x01\x02", 2);
    QVERIFY(mtkbrom::flashPartition(s, st, QStringLiteral("boot"), img, nullptr));
    // 帧含 addr=0x1000
    QVERIFY(m->writes.contains(QByteArray("\x62\x02\x08\x00\x00\x00\x00\x00\x00\x10\x00", 11)));
}

void TestMtkPayload::flashPartitionUnknownNameFails()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    mtkbrom::DaStorage st(s);
    st.setDaActive(true);
    QByteArray pd(0x60, '\0');
    pd[0x48] = char(0xFF);
    memcpy(pd.data(), "boot", 4);
    m->reads << QByteArray(1, char(0x5A))
             << QByteArray("\x00\x00\x00\x60", 4)
             << pd;
    QString err;
    QVERIFY(!mtkbrom::flashPartition(s, st, QStringLiteral("missing"), QByteArray(), &err));
    QVERIFY(err.contains("未找到分区"));
}

QTEST_APPLESS_MAIN(TestMtkPayload)
#include "test_mtk_payload.moc"
