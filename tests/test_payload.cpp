#include <QtTest>
#include "image_engine/wire_format.h"

class TestWire : public QObject
{
    Q_OBJECT
private slots:
    void parseVarint();
    void parseLengthDelimited();
    void encodeRoundTrip();
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

QTEST_APPLESS_MAIN(TestWire)
#include "test_payload.moc"
