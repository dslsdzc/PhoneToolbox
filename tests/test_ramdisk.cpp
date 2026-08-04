#include <QtTest>
#include "root_patcher/ramdisk_utils.h"
#include "image_engine/compression/compressor.h"

class TestRamdisk : public QObject
{
    Q_OBJECT
private slots:
    void detectGzip();
    void gzipRoundTrip();
    void detectLz4Legacy();
    void detectLz4Frame();
    void lz4LegacyRoundTrip();
    void lz4LegacyMultiBlock();
    void lz4FrameRoundTrip();
    void detectXz();
    void xzRoundTrip();
    void detectLzma();
    void lzmaRoundTrip();
    void detectUnknown();
    void rawPassthrough();
    void corruptLz4LegacyFails();
};

void TestRamdisk::detectGzip()
{
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(QByteArray("\x1f\x8b", 2), fmt));
    QCOMPARE(fmt, "gzip");
}

void TestRamdisk::gzipRoundTrip()
{
    QByteArray data("ramdisk content content content");
    QByteArray comp = imgcomp::gzipCompress(data);
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(comp, fmt));
    QCOMPARE(fmt, "gzip");
    QByteArray out;
    QString err;
    QVERIFY(patcher::decompressRamdisk(comp, out, &err));
    QCOMPARE(out, data);
    QByteArray re = patcher::compressRamdisk(out, "gzip");
    QVERIFY(patcher::decompressRamdisk(re, out, &err));
    QCOMPARE(out, data);
}

void TestRamdisk::detectLz4Legacy()
{
    // lz4 legacy 头: 魔数 0x184C2102 小端 \x02\x21\x4c\x18
    QByteArray head("\x02\x21\x4c\x18", 4);
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(head, fmt));
    QCOMPARE(fmt, "lz4");
}

void TestRamdisk::detectLz4Frame()
{
    // lz4 frame 头: 魔数 0x184D2204 小端 \x04\x22\x4d\x18
    QByteArray head("\x04\x22\x4d\x18", 4);
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(head, fmt));
    QCOMPARE(fmt, "lz4");
}

void TestRamdisk::lz4LegacyRoundTrip()
{
    QByteArray data("ramdisk content content content");
    QByteArray comp = patcher::compressRamdisk(data, "lz4");
    // legacy 输出须为魔数开头的块流
    QVERIFY(comp.size() > 4);
    QCOMPARE(comp.left(4), QByteArray("\x02\x21\x4c\x18", 4));
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(comp, fmt));
    QCOMPARE(fmt, "lz4");
    QByteArray out;
    QString err;
    QVERIFY(patcher::decompressRamdisk(comp, out, &err));
    QCOMPARE(out, data);
    QByteArray re = patcher::compressRamdisk(out, "lz4");
    QVERIFY(patcher::decompressRamdisk(re, out, &err));
    QCOMPARE(out, data);
}

void TestRamdisk::lz4LegacyMultiBlock()
{
    // 超过 8 MiB 单块上限（与 magiskboot/liblz4 一致的 0x800000），须拆成多块并依次解压
    QByteArray data;
    const int block = 0x800000;
    data.reserve(block + 100);
    while (data.size() < block + 100)
        data.append("multi-block ramdisk payload 0123456789");
    data.truncate(block + 100);
    QByteArray comp = patcher::compressRamdisk(data, "lz4");
    QVERIFY(comp.size() > 4);
    QByteArray out;
    QString err;
    QVERIFY(patcher::decompressRamdisk(comp, out, &err));
    QCOMPARE(out, data);
}

void TestRamdisk::lz4FrameRoundTrip()
{
    QByteArray data("ramdisk content as lz4 frame");
    QByteArray comp = imgcomp::compress(imgcomp::Type::Lz4, data);
    QVERIFY(!comp.isEmpty());
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(comp, fmt));
    QCOMPARE(fmt, "lz4");
    QByteArray out;
    QString err;
    QVERIFY(patcher::decompressRamdisk(comp, out, &err));
    QCOMPARE(out, data);
}

void TestRamdisk::detectXz()
{
    // xz 容器魔数: FD 37 7A 58 5A 00
    QByteArray head("\xfd\x37\x7a\x58\x5a\x00", 6);
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(head, fmt));
    QCOMPARE(fmt, "xz");
}

void TestRamdisk::xzRoundTrip()
{
    QByteArray data("ramdisk content for xz test 1234567890");
    QByteArray comp = patcher::compressRamdisk(data, "xz");
    QVERIFY(!comp.isEmpty());
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(comp, fmt));
    QCOMPARE(fmt, "xz");
    QByteArray out;
    QString err;
    QVERIFY(patcher::decompressRamdisk(comp, out, &err));
    QCOMPARE(out, data);
}

void TestRamdisk::detectLzma()
{
    // lzma-alone 头: 属性 0x5D + 字典 8MiB(小端 00 00 80 00) + 未知未压缩大小 8×FF
    QByteArray head(14, 0);
    head[0] = '\x5d';
    head[1] = '\x00'; head[2] = '\x00'; head[3] = '\x80'; head[4] = '\x00';
    for (int i = 5; i < 13; ++i)
        head[i] = '\xff';
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(head, fmt));
    QCOMPARE(fmt, "lzma");
}

void TestRamdisk::lzmaRoundTrip()
{
    QByteArray data("ramdisk content for lzma-alone test");
    QByteArray comp = patcher::compressRamdisk(data, "lzma");
    QVERIFY(!comp.isEmpty());
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(comp, fmt));
    QCOMPARE(fmt, "lzma");
    QByteArray out;
    QString err;
    QVERIFY(patcher::decompressRamdisk(comp, out, &err));
    QCOMPARE(out, data);
}

void TestRamdisk::detectUnknown()
{
    QString fmt;
    QVERIFY(!patcher::detectRamdiskFormat(QByteArray("plain data"), fmt));
}

void TestRamdisk::rawPassthrough()
{
    QByteArray raw("plain unmodified ramdisk data");
    QByteArray out;
    QString err;
    QVERIFY(patcher::decompressRamdisk(raw, out, &err));
    QCOMPARE(out, raw);
}

void TestRamdisk::corruptLz4LegacyFails()
{
    // 魔数正确但块被截断
    QByteArray truncated("\x02\x21\x4c\x18", 4);
    truncated.append('\x64').append('\x00').append('\x00').append('\x00'); // compSize=100
    truncated.append(QByteArray(10, 'x'));
    QByteArray out;
    QString err;
    QVERIFY(!patcher::decompressRamdisk(truncated, out, &err));
    QVERIFY(!err.isEmpty());

    // 魔数正确但压缩数据损坏
    QByteArray corrupt("\x02\x21\x4c\x18", 4);
    corrupt.append('\x0a').append('\x00').append('\x00').append('\x00'); // compSize=10
    corrupt.append(QByteArray(10, '\x00'));
    err.clear();
    QVERIFY(!patcher::decompressRamdisk(corrupt, out, &err));
    QVERIFY(!err.isEmpty());

    // 块大小为零（畸形）
    QByteArray zeroBlock("\x02\x21\x4c\x18", 4);
    zeroBlock.append(QByteArray(4, '\x00'));
    err.clear();
    QVERIFY(!patcher::decompressRamdisk(zeroBlock, out, &err));
    QVERIFY(!err.isEmpty());
}

QTEST_APPLESS_MAIN(TestRamdisk)
#include "test_ramdisk.moc"
