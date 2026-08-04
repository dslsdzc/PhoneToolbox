#include <QtTest>
#include "image_engine/compression/compressor.h"
#include "image_engine/compression/lz4_wrapper.h"
#include "image_engine/compression/zstd_wrapper.h"

class TestCompression : public QObject
{
    Q_OBJECT
private slots:
    void zstdRoundTrip();
    void zstdInvalidInput();
    void lz4RoundTrip();
    void lz4InvalidInput();
    void dispatchRoundTrip();
};

void TestCompression::zstdRoundTrip()
{
    QByteArray data("hello zstd world, repeat. hello zstd world, repeat."); // 足够长以触发压缩
    QByteArray comp = imgcomp::zstdCompress(data);
    QVERIFY(!comp.isEmpty());
    QVERIFY(comp != data);
    QCOMPARE(imgcomp::zstdDecompress(comp), data);
}

void TestCompression::zstdInvalidInput() { QVERIFY(imgcomp::zstdDecompress("garbage").isEmpty()); }

void TestCompression::lz4RoundTrip()
{
    QByteArray data("lz4 frame payload, padding padding padding padding padding");
    QByteArray comp = imgcomp::lz4Compress(data);
    QVERIFY(!comp.isEmpty());
    QCOMPARE(imgcomp::lz4Decompress(comp), data);
}

void TestCompression::lz4InvalidInput() { QVERIFY(imgcomp::lz4Decompress("garbage").isEmpty()); }

void TestCompression::dispatchRoundTrip()
{
    QByteArray data("dispatch test data, dispatch test data.");
    QCOMPARE(imgcomp::decompress(imgcomp::Type::Zstd, imgcomp::compress(imgcomp::Type::Zstd, data)), data);
    QVERIFY(imgcomp::compress(imgcomp::Type::None, data).isEmpty());
    QVERIFY(imgcomp::decompress(imgcomp::Type::None, data).isEmpty());
}

QTEST_APPLESS_MAIN(TestCompression)
#include "test_compression.moc"
