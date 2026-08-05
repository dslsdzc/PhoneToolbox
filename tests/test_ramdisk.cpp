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
    void lz4LegacyRepeatMagic();
    void lz4LegacyLgTrailer();
    void invalidLgTrailerFails();
    void hugeCompSizeFails();
    void lz4FrameRoundTrip();
    void detectXz();
    void xzRoundTrip();
    void detectLzma();
    void detectLzmaDeclaredSize();
    void lzmaRoundTrip();
    void detectUnknown();
    void rawPassthrough();
    void corruptLz4LegacyFails();
    void corruptGzipFails();
    void corruptXzFails();
    void corruptLzmaFails();
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

void TestRamdisk::lz4LegacyRepeatMagic()
{
    // LG 设备 ramdisk：块间重复魔数（magiskboot 两代解码器均跳过魔数字），
    // 修复前会被当作 >8MiB 的 LG 流尾 → 静默截断输出
    QByteArray a("first block payload abcdefghijklmnop");
    QByteArray b("second block payload 0123456789");
    QByteArray ca = patcher::compressRamdisk(a, "lz4");
    QByteArray cb = patcher::compressRamdisk(b, "lz4");
    // [magic][块1][magic][块2]：每段去掉各自开头的魔数，块间再插一个魔数
    QByteArray stream("\x02\x21\x4c\x18", 4);
    stream.append(ca.mid(4));
    stream.append("\x02\x21\x4c\x18", 4);
    stream.append(cb.mid(4));
    QByteArray out;
    QString err;
    QVERIFY(patcher::decompressRamdisk(stream, out, &err));
    QCOMPARE(out, a + b);
}

void TestRamdisk::lz4LegacyLgTrailer()
{
    // 合法 LG 变体：流尾 4B LE 总未压缩大小（> 单块压缩上限 → LG 分支）
    QByteArray data("lg ramdisk content with trailer");
    QByteArray stream = patcher::compressRamdisk(data, "lz4");
    stream.append('\x00').append('\x00').append('\x00').append('\x01'); // 0x01000000 (16 MiB)
    QByteArray out;
    QString err;
    QVERIFY(patcher::decompressRamdisk(stream, out, &err));
    QCOMPARE(out, data);
}

void TestRamdisk::invalidLgTrailerFails()
{
    // 块后插入伪 LG 流尾 + 尾随垃圾：必须失败而非静默丢弃垃圾
    QByteArray data("payload before fake trailer");
    QByteArray stream = patcher::compressRamdisk(data, "lz4");
    stream.append('\x00').append('\x00').append('\x00').append('\x01'); // 伪流尾 16 MiB
    stream.append("trailing garbage");
    QByteArray out;
    QString err;
    QVERIFY(!patcher::decompressRamdisk(stream, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestRamdisk::hugeCompSizeFails()
{
    // compSize >= 2^31：int 截断为负的防护（必须失败，不得越界/死循环）
    QByteArray stream("\x02\x21\x4c\x18", 4);
    stream.append('\xff').append('\xff').append('\xff').append('\xff'); // 0xFFFFFFFF
    stream.append("data");
    QByteArray out;
    QString err;
    QVERIFY(!patcher::decompressRamdisk(stream, out, &err));
    QVERIFY(!err.isEmpty());
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

void TestRamdisk::detectLzmaDeclaredSize()
{
    // 审查 Minor：声明未压缩大小的合法 lzma-alone。magiskboot v25.2 判据
    //（属性 0x5D + 大小字段 MSB ∈ {0xFF, 0x00}）应识别 —— 修复前要求大小
    // 恒为 8×FF，声明大小流被误判 raw，ramdisk 被当未压缩原样处理而损坏。
    QByteArray data("ramdisk with declared-size lzma-alone header");
    QByteArray comp = patcher::compressRamdisk(data, "lzma");
    QVERIFY(comp.size() >= 13);
    // 改写 8B LE 未压缩大小字段为实际大小（<2^56 → MSB=0x00）
    const quint64 sz = static_cast<quint64>(data.size());
    for (int i = 0; i < 8; ++i)
        comp[5 + i] = char((sz >> (8 * i)) & 0xFF);
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(comp, fmt));
    QCOMPARE(fmt, "lzma");
    QByteArray out;
    QString err;
    QVERIFY(patcher::decompressRamdisk(comp, out, &err));
    QCOMPARE(out, data);
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

void TestRamdisk::corruptGzipFails()
{
    // 损坏/截断的 gzip 帧必须失败并给出错误，不得静默返回部分数据
    QByteArray data("ramdisk payload for gzip corruption tests");
    const QByteArray comp = patcher::compressRamdisk(data, "gzip");
    QVERIFY(!comp.isEmpty());

    QByteArray out;
    QString err;

    // 截断（尾部 trailer 被切掉）
    QVERIFY(!patcher::decompressRamdisk(comp.left(comp.size() - 5), out, &err));
    QVERIFY(!err.isEmpty());

    // 载荷字节翻转 → inflate/CRC32 校验失败
    QByteArray corrupt = comp;
    const int mid = corrupt.size() / 2;
    corrupt[mid] = char(corrupt[mid] ^ 0xFF);
    err.clear();
    QVERIFY(!patcher::decompressRamdisk(corrupt, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestRamdisk::corruptXzFails()
{
    QByteArray data("ramdisk payload for xz corruption tests 0123456789");
    const QByteArray comp = patcher::compressRamdisk(data, "xz");
    QVERIFY(!comp.isEmpty());

    QByteArray out;
    QString err;

    // 截断：xz footer（尾部 12 字节）缺失 → 必须失败
    QVERIFY(!patcher::decompressRamdisk(comp.left(comp.size() - 6), out, &err));
    QVERIFY(!err.isEmpty());

    // 载荷损坏
    QByteArray corrupt = comp;
    const int mid = corrupt.size() / 2;
    corrupt[mid] = char(corrupt[mid] ^ 0x01);
    err.clear();
    QVERIFY(!patcher::decompressRamdisk(corrupt, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestRamdisk::corruptLzmaFails()
{
    QByteArray data("ramdisk payload for lzma-alone corruption tests");
    const QByteArray comp = patcher::compressRamdisk(data, "lzma");
    QVERIFY(!comp.isEmpty());

    QByteArray out;
    QString err;

    // 截断：压缩数据不完整（保留 13 字节头以通过格式检测）
    QVERIFY(!patcher::decompressRamdisk(comp.left(comp.size() - 4), out, &err));
    QVERIFY(!err.isEmpty());

    // 载荷字节翻转 → LZMA_DATA_ERROR
    QByteArray corrupt = comp;
    const int tail = corrupt.size() - 3;
    corrupt[tail] = char(corrupt[tail] ^ 0xFF);
    err.clear();
    QVERIFY(!patcher::decompressRamdisk(corrupt, out, &err));
    QVERIFY(!err.isEmpty());
}

QTEST_APPLESS_MAIN(TestRamdisk)
#include "test_ramdisk.moc"
