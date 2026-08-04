#include <QtTest>
#include "image_engine/compression/compressor.h"
#include "image_engine/compression/zstd_wrapper.h"

class TestCompression : public QObject
{
    Q_OBJECT
private slots:
    void zstdRoundTrip();
    void zstdInvalidInput();
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

QTEST_APPLESS_MAIN(TestCompression)
#include "test_compression.moc"
