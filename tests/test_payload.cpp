#include <QtTest>
#include "image_engine/wire_format.h"
#include "image_engine/payload_image.h"

class TestWire : public QObject
{
    Q_OBJECT
private slots:
    void parseVarint();
    void parseLengthDelimited();
    void encodeRoundTrip();
    void parseInvalid();            // Task 13: wire 层负向用例（Task 12 审查遗留补上）
};

void TestWire::parseVarint()
{
    // field 1 varint value 300 → 0x08 0xAC 0x02
    QByteArray d("\x08\xAC\x02", 3);
    bool ok = false;
    QList<pbwire::Field> fields = pbwire::parseMessage(d, ok);
    QVERIFY(ok);
    QCOMPARE(fields.size(), 1);
    QCOMPARE(fields[0].number, 1);
    QCOMPARE(fields[0].varint, 300ull);
}

void TestWire::parseLengthDelimited()
{
    // field 3 bytes "hi" → 0x1A 0x02 'h' 'i'
    QByteArray d("\x1A\x02hi", 4);
    bool ok = false;
    QList<pbwire::Field> fields = pbwire::parseMessage(d, ok);
    QVERIFY(ok);
    QCOMPARE(fields[0].number, 3);
    QCOMPARE(fields[0].bytes, QByteArray("hi"));
}

void TestWire::encodeRoundTrip()
{
    QByteArray msg = pbwire::encodeVarint(1, 300);
    QByteArray nested = pbwire::encodeBytes(3, QByteArray("hi"));
    QByteArray combined = msg + nested;
    bool ok = false;
    QList<pbwire::Field> fields = pbwire::parseMessage(combined, ok);
    QVERIFY(ok);
    QCOMPARE(fields.size(), 2);
    QCOMPARE(fields[0].varint, 300ull);
    QCOMPARE(fields[1].bytes, QByteArray("hi"));
}

void TestWire::parseInvalid()
{
    // 非法 wire 数据 → ok=false，不得崩溃
    bool ok = true;
    // varint 截断（0x80 无终止字节）
    QVERIFY(pbwire::parseMessage(QByteArray("\x08\x80", 2), ok).isEmpty());
    QVERIFY(!ok);
    // varint 超过 10 字节上限
    ok = true;
    QVERIFY(pbwire::parseMessage(QByteArray("\x08", 1) + QByteArray(11, '\x80'), ok).isEmpty());
    QVERIFY(!ok);
    // 长度字段超界
    ok = true;
    QVERIFY(pbwire::parseMessage(QByteArray("\x1A\x05hi", 4), ok).isEmpty());
    QVERIFY(!ok);
    // field number == 0
    ok = true;
    QVERIFY(pbwire::parseMessage(QByteArray("\x00", 1), ok).isEmpty());
    QVERIFY(!ok);
    // 未知 wireType (tag & 7 == 7)
    ok = true;
    QVERIFY(pbwire::parseMessage(QByteArray("\x0F", 1), ok).isEmpty());
    QVERIFY(!ok);
    // 64-bit 字段越界（仅 1 字节）
    ok = true;
    QVERIFY(pbwire::parseMessage(QByteArray("\x09", 1), ok).isEmpty());
    QVERIFY(!ok);
    // 32-bit 字段越界（仅 1 字节）
    ok = true;
    QVERIFY(pbwire::parseMessage(QByteArray("\x0D", 1), ok).isEmpty());
    QVERIFY(!ok);
}

// ---------- Task 13: payload manifest 解析 ----------

class TestPayload : public QObject
{
    Q_OBJECT
private slots:
    void detect();
    void parseMinimalManifest();
    void parseInvalid();
    void parseV1();
    void parseUnknownFields();
};

static QByteArray buildMinimalPayload()
{
    // InstallOperation: type=1(REPLACE), data_offset=2, data_length=3
    QByteArray op = pbwire::encodeVarint(1, 0) + pbwire::encodeVarint(2, 0)
                  + pbwire::encodeVarint(3, 8);
    // PartitionUpdate: partition_name=1("boot"), operations=8(op)
    QByteArray part = pbwire::encodeBytes(1, QByteArray("boot")) + pbwire::encodeMessage(8, op);
    // Manifest: block_size=3(4096), partitions=13(part)
    QByteArray manifest = pbwire::encodeVarint(3, 4096) + pbwire::encodeMessage(13, part);
    QByteArray payload;
    payload.append("CrAU");
    auto put64 = [&](quint64 v) {
        for (int i = 0; i < 8; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put64(2);                      // version
    put64(static_cast<quint64>(manifest.size())); // manifest_size
    auto put32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put32(0);                      // metadata_signature_size
    payload.append(manifest);
    payload.append(QByteArray(8, '\xAB')); // blob
    return payload;
}

// v1: 无 metadata_signature_size，manifest 数据起始 = 20
static QByteArray buildV1Payload()
{
    QByteArray manifest = pbwire::encodeVarint(3, 8192);
    QByteArray payload;
    payload.append("CrAU");
    auto put64 = [&](quint64 v) {
        for (int i = 0; i < 8; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put64(1);                      // version = 1
    put64(static_cast<quint64>(manifest.size()));
    payload.append(manifest);
    return payload;
}

// manifest/partition/op 三层各插入未知字段 → 按 protobuf 语义跳过
static QByteArray buildPayloadWithUnknownFields()
{
    QByteArray op = pbwire::encodeVarint(1, 0) + pbwire::encodeVarint(2, 0)
                  + pbwire::encodeVarint(3, 8)
                  + pbwire::encodeVarint(99, 12345);            // 未知字段
    QByteArray part = pbwire::encodeBytes(1, QByteArray("boot"))
                    + pbwire::encodeMessage(8, op)
                    + pbwire::encodeVarint(99, 1);              // 未知字段
    QByteArray manifest = pbwire::encodeVarint(3, 4096)
                        + pbwire::encodeMessage(13, part)
                        + pbwire::encodeVarint(99, 1);          // 未知字段
    QByteArray payload;
    payload.append("CrAU");
    auto put64 = [&](quint64 v) {
        for (int i = 0; i < 8; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put64(2);
    put64(static_cast<quint64>(manifest.size()));
    auto put32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put32(0);
    payload.append(manifest);
    return payload;
}

// manifest 内嵌套消息截断（头部尺寸自洽）→ false
static QByteArray buildTruncatedInnerPayload()
{
    QByteArray op = pbwire::encodeVarint(1, 0) + pbwire::encodeVarint(2, 0)
                  + pbwire::encodeVarint(3, 8);
    QByteArray badPart = pbwire::encodeBytes(1, QByteArray("boot"))
                       + pbwire::encodeMessage(8, op.left(3));  // ops 内容截断
    QByteArray manifest = pbwire::encodeVarint(3, 4096) + pbwire::encodeMessage(13, badPart);
    QByteArray payload;
    payload.append("CrAU");
    auto put64 = [&](quint64 v) {
        for (int i = 0; i < 8; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put64(2);
    put64(static_cast<quint64>(manifest.size()));
    auto put32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put32(0);
    payload.append(manifest);
    return payload;
}

void TestPayload::detect()
{
    QVERIFY(imgpayload::isPayload(QByteArray("CrAU")));
    QVERIFY(!imgpayload::isPayload(QByteArray("ANDROID!")));
}

void TestPayload::parseMinimalManifest()
{
    QByteArray p = buildMinimalPayload();
    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(p, info));
    QCOMPARE(info.blockSize, 4096ull);
    QCOMPARE(info.partitions.size(), 1);
    QCOMPARE(info.partitions[0].name, "boot");
    QCOMPARE(info.partitions[0].ops.size(), 1);
    QCOMPARE(info.partitions[0].ops[0].type, 0);
    QCOMPARE(info.partitions[0].ops[0].dataLength, 8ull);
}

void TestPayload::parseInvalid()
{
    imgpayload::PayloadInfo info;
    // 空 / 过短（< 20 字节）/ 坏魔数
    QVERIFY(!imgpayload::parseManifest(QByteArray(), info));
    QVERIFY(!imgpayload::parseManifest(QByteArray("CrAU"), info));
    QVERIFY(!imgpayload::parseManifest(QByteArray("ANDROID!") + QByteArray(16, 0), info));
    // manifest_size 超界（篡改为全 0xFF → 越界拒绝）
    QByteArray p = buildMinimalPayload();
    p.replace(12, 8, QByteArray(8, '\xFF'));
    QVERIFY(!imgpayload::parseManifest(p, info));
    // manifest 内部嵌套消息截断 → false
    QVERIFY(!imgpayload::parseManifest(buildTruncatedInnerPayload(), info));
}

void TestPayload::parseV1()
{
    // v1: 无 metadata_signature_size，manifest 数据起始 = 20
    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(buildV1Payload(), info));
    QCOMPARE(info.blockSize, 8192ull);
    QCOMPARE(info.partitions.size(), 0);
    QCOMPARE(info.manifestRaw, pbwire::encodeVarint(3, 8192));
}

void TestPayload::parseUnknownFields()
{
    // 未知字段（protobuf 语义: 跳过）→ 解析成功且已知字段不受影响
    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(buildPayloadWithUnknownFields(), info));
    QCOMPARE(info.blockSize, 4096ull);
    QCOMPARE(info.partitions.size(), 1);
    QCOMPARE(info.partitions[0].name, "boot");
    QCOMPARE(info.partitions[0].ops.size(), 1);
    QCOMPARE(info.partitions[0].ops[0].type, 0);
    QCOMPARE(info.partitions[0].ops[0].dataLength, 8ull);
}

// 双测试类（TestWire + TestPayload）共用主函数
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    int status = 0;
    {
        TestWire tc;
        status |= QTest::qExec(&tc, argc, argv);
    }
    {
        TestPayload tc;
        status |= QTest::qExec(&tc, argc, argv);
    }
    return status;
}
#include "test_payload.moc"
