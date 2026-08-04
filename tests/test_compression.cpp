#include <QtTest>
#include "image_engine/compression/brotli_wrapper.h"
#include "image_engine/compression/bzip2_wrapper.h"
#include "image_engine/compression/compressor.h"
#include "image_engine/compression/lz4_wrapper.h"
#include "image_engine/compression/xz_wrapper.h"
#include "image_engine/compression/zstd_wrapper.h"

class TestCompression : public QObject
{
    Q_OBJECT
private slots:
    void zstdRoundTrip();
    void zstdInvalidInput();
    void lz4RoundTrip();
    void lz4InvalidInput();
    void lz4Truncated();
    void bzip2RoundTrip();
    void bzip2InvalidInput();
    void xzRoundTrip();
    void xzInvalidInput();
    void xzStandardRoundTrip();
    void brotliRoundTrip();
    void brotliInvalidInput();
    void brotliTruncated();
    void gzipRoundTrip();
    void gzipInvalidInput();
    void gzipTruncated();
    void gzipForgedIsize();
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
    QByteArray data(1024 * 1024, 'a'); // 1MB 可压缩内容，触发多块/多迭代解压路径
    QByteArray comp = imgcomp::lz4Compress(data);
    QVERIFY(!comp.isEmpty());
    QCOMPARE(imgcomp::lz4Decompress(comp), data);
}

void TestCompression::lz4InvalidInput() { QVERIFY(imgcomp::lz4Decompress("garbage").isEmpty()); }

void TestCompression::lz4Truncated()
{
    QByteArray data(1024 * 1024, 'a');
    QByteArray comp = imgcomp::lz4Compress(data);
    QVERIFY(imgcomp::lz4Decompress(comp.left(comp.size() - 1)).isEmpty()); // 截断末字节（endMark/校验和）
}

void TestCompression::bzip2RoundTrip()
{
    QByteArray data("bzip2 payload with repeated repeated repeated content");
    QByteArray comp = imgcomp::bzip2Compress(data);
    QVERIFY(!comp.isEmpty());
    QCOMPARE(imgcomp::bzip2Decompress(comp), data);
}

void TestCompression::bzip2InvalidInput() { QVERIFY(imgcomp::bzip2Decompress("garbage").isEmpty()); }

void TestCompression::xzRoundTrip()
{
    QByteArray data("xz payload with some repetition repetition repetition");
    QByteArray comp = imgcomp::xzCompress(data);
    QVERIFY(!comp.isEmpty());
    QCOMPARE(imgcomp::xzDecompress(comp), data);
}

void TestCompression::xzInvalidInput() { QVERIFY(imgcomp::xzDecompress("garbage").isEmpty()); }

void TestCompression::xzStandardRoundTrip()
{
    // 标准 xz 流：由系统 `printf 'standard xz payload with repetition repetition repetition' | xz -c`
    // 离线生成后嵌入（测试运行时无系统命令依赖），验证与标准 xz 格式互操作、无自定义容器后缀。
    static const unsigned char stdStream[] = {
        0xfd, 0x37, 0x7a, 0x58, 0x5a, 0x00, 0x00, 0x04, 0xe6, 0xd6, 0xb4, 0x46,
        0x04, 0xc0, 0x30, 0x39, 0x21, 0x01, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x9f, 0x96, 0x66, 0xff, 0xe0, 0x00, 0x38, 0x00,
        0x28, 0x5d, 0x00, 0x39, 0x9d, 0x08, 0x46, 0x94, 0x48, 0xf6, 0x80, 0x6a,
        0xb2, 0xb5, 0x06, 0x82, 0x3a, 0x22, 0x4e, 0x54, 0xfe, 0x93, 0xa5, 0x7b,
        0x76, 0xfc, 0x94, 0xd7, 0x06, 0xf8, 0xd3, 0xf2, 0x50, 0x53, 0xd9, 0xfa,
        0x8a, 0xe2, 0x5f, 0x4f, 0x53, 0x80, 0x00, 0x00, 0x63, 0x83, 0x74, 0x4b,
        0x2b, 0x2c, 0x57, 0x47, 0x00, 0x01, 0x4c, 0x39, 0x2a, 0x3d, 0x4f, 0x23,
        0x1f, 0xb6, 0xf3, 0x7d, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x59, 0x5a
    };
    QByteArray comp(reinterpret_cast<const char *>(stdStream), sizeof(stdStream));
    QByteArray data("standard xz payload with repetition repetition repetition");
    QCOMPARE(imgcomp::xzDecompress(comp), data);
}

void TestCompression::brotliRoundTrip()
{
    // 小负载（单块）与 1MB（触发流式多块/多迭代解压路径；一次性 API 缓冲
    // 不足返回 ERROR，此用例防止回归到仅能解小负载的实现）
    QByteArray small("brotli payload repeated repeated repeated");
    QByteArray comp = imgcomp::brotliCompress(small);
    QVERIFY(!comp.isEmpty());
    QCOMPARE(imgcomp::brotliDecompress(comp), small);
    QByteArray big(1024 * 1024, 'a');
    QByteArray bigComp = imgcomp::brotliCompress(big);
    QVERIFY(!bigComp.isEmpty());
    QCOMPARE(imgcomp::brotliDecompress(bigComp), big);
}

void TestCompression::brotliInvalidInput() { QVERIFY(imgcomp::brotliDecompress("garbage").isEmpty()); }

void TestCompression::brotliTruncated()
{
    QByteArray data(1024 * 1024, 'a');
    QByteArray comp = imgcomp::brotliCompress(data);
    QVERIFY(!comp.isEmpty());
    QVERIFY(imgcomp::brotliDecompress(comp.left(comp.size() - 1)).isEmpty()); // 截断末字节（流尾标记缺失）
    QVERIFY(imgcomp::brotliDecompress(comp.left(comp.size() / 2)).isEmpty());  // 流被切
}

void TestCompression::gzipRoundTrip()
{
    QByteArray data("gzip payload repeated repeated repeated");
    QByteArray comp = imgcomp::gzipCompress(data);
    QVERIFY(!comp.isEmpty());
    QCOMPARE(imgcomp::gzipDecompress(comp), data);
}

void TestCompression::gzipInvalidInput() { QVERIFY(imgcomp::gzipDecompress("garbage").isEmpty()); }

void TestCompression::gzipTruncated()
{
    QByteArray data(1024 * 1024, 'a');
    QByteArray comp = imgcomp::gzipCompress(data);
    QVERIFY(!comp.isEmpty());
    QVERIFY(imgcomp::gzipDecompress(comp.left(comp.size() / 2)).isEmpty());        // deflate 流被切
    QVERIFY(imgcomp::gzipDecompress(comp.left(comp.size() - 8)).isEmpty());        // 仅 trailer（CRC32+ISIZE 8 字节）被切
}

void TestCompression::gzipForgedIsize()
{
    // 伪造尾部 ISIZE 为 ~2GiB：不得按 ISIZE 大分配/崩溃。直接分配路径因
    // ISIZE 超输入 1024 倍被拒 → 回退流式；流式下 zlib 自行校验 trailer
    // ISIZE 与解出大小不符 → Z_DATA_ERROR → 必须返回空。
    QByteArray data("forged isize payload payload payload");
    QByteArray comp = imgcomp::gzipCompress(data);
    QVERIFY(!comp.isEmpty());
    QByteArray forged = comp;
    const int tail = forged.size() - 4;
    forged[tail] = char(0xff); forged[tail + 1] = char(0xff);
    forged[tail + 2] = char(0xff); forged[tail + 3] = char(0x7f); // 0x7fffffff ≈ 2GiB
    QVERIFY(imgcomp::gzipDecompress(forged).isEmpty());
}

void TestCompression::dispatchRoundTrip()
{
    QByteArray data("dispatch test data, dispatch test data.");
    QCOMPARE(imgcomp::decompress(imgcomp::Type::Zstd, imgcomp::compress(imgcomp::Type::Zstd, data)), data);
    QCOMPARE(imgcomp::decompress(imgcomp::Type::Xz, imgcomp::compress(imgcomp::Type::Xz, data)), data);
    QCOMPARE(imgcomp::decompress(imgcomp::Type::Brotli, imgcomp::compress(imgcomp::Type::Brotli, data)), data);
    QCOMPARE(imgcomp::decompress(imgcomp::Type::Gzip, imgcomp::compress(imgcomp::Type::Gzip, data)), data);
    QVERIFY(imgcomp::compress(imgcomp::Type::None, data).isEmpty());
    QVERIFY(imgcomp::decompress(imgcomp::Type::None, data).isEmpty());
}

QTEST_APPLESS_MAIN(TestCompression)
#include "test_compression.moc"
