#include <QtTest>
#include "image_engine/sin_image.h"

class TestSin : public QObject
{
    Q_OBJECT
private slots:
    void detect();
    void parseAddrBlock();
};

static QByteArray buildSinAddr()
{
    // 头: 0x03 "SIN" + 头长(15) + type(0x24)
    QByteArray hdr(15, 0);
    hdr[0] = char(0x03);
    hdr.replace(1, 3, "SIN");
    auto put32 = [&](QByteArray &d, int off, quint32 v) {
        d[off] = char(v); d[off + 1] = char(v >> 8); d[off + 2] = char(v >> 16); d[off + 3] = char(v >> 24);
    };
    put32(hdr, 4, 15);
    put32(hdr, 8, 0x24);
    // SinDataHeader
    QByteArray dataHdr(16, 0);
    dataHdr.replace(0, 4, "MMCF");
    put32(dataHdr, 4, 16);
    dataHdr.replace(8, 4, "GPTP");
    put32(dataHdr, 12, 0x18);
    // 后续 GPTGUID(16) 省略 —— 用 "ADDR" 块直接开头简化
    // ADDR 块头 0x44 字节
    QByteArray blk(0x44, 0);
    blk.replace(0, 4, "ADDR");
    put32(blk, 4, 0x44);
    auto put64 = [&](QByteArray &d, int off, quint64 v) {
        for (int i = 0; i < 8; ++i) d[off + i] = char((v >> (i * 8)) & 0xFF);
    };
    put64(blk, 12, 8);    // dataStart
    put64(blk, 20, 4096); // dataLength
    put64(blk, 28, 1024); // dataDest
    QByteArray sin = hdr + dataHdr + QByteArray(16, 0) + blk + QByteArray(4096, '\x44');
    return sin;
}

void TestSin::detect()
{
    QVERIFY(imgsin::isSinV3(QByteArray("\x03SIN", 4)));
    QVERIFY(!imgsin::isSinV3(QByteArray("CrAU")));
}

void TestSin::parseAddrBlock()
{
    QByteArray sin = buildSinAddr();
    QList<imgsin::BlockDesc> blocks;
    QString err;
    QVERIFY(imgsin::parseSin(sin, blocks, &err));
    QCOMPARE(blocks.size(), 1);
    QCOMPARE(blocks[0].dataDest, 1024ull);
    QVERIFY(!blocks[0].compressed);
}

QTEST_APPLESS_MAIN(TestSin)
#include "test_sin.moc"
