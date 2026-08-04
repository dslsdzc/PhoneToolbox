#include <QtTest>
#include "image_engine/fs/erofs_reader.h"

// EROFS superblock 布局按 Linux 内核 fs/erofs/erofs_fs.h 核对（v5.10~v6.x 及
// erofs-utils master 一致，mkfs.erofs 输出即此布局）:
//   EROFS_SUPER_OFFSET = 1024（偏移 0 为保留区）
//   magic 0xE0F5E1E2 小端落盘 E2 E1 F5 E0 @1024 (4B)
//   feature_compat  LE32 @1032 | blkszbits u8 @1036 (blockSize = 1<<blkszbits)
//   root_nid        LE16 @1038 (48BIT 特性时改用 rootnid_8b LE64 @1136)
//   feature_incompat LE32 @1104 | available_compr_algs LE16 @1108 (bit0 = LZ4;
//   algs==0 为 legacy 布局，内核按仅有 LZ4 处理)
class TestErofs : public QObject
{
    Q_OBJECT
private slots:
    void detect();
    void parseSuper();
    void parseSuperLz4Flag();
    void parseSuper48Bit();
    void invalidInput();
};

static void put16(QByteArray &d, int off, quint32 v)
{
    d[off] = char(v & 0xFF);
    d[off + 1] = char((v >> 8) & 0xFF);
}
static void put32(QByteArray &d, int off, quint32 v)
{
    for (int i = 0; i < 4; ++i) d[off + i] = char((v >> (i * 8)) & 0xFF);
}
static void put64(QByteArray &d, int off, quint64 v)
{
    for (int i = 0; i < 8; ++i) d[off + i] = char((v >> (i * 8)) & 0xFF);
}
static void putMagic(QByteArray &d)
{
    d[1024] = char(0xE2); d[1025] = char(0xE1); d[1026] = char(0xF5); d[1027] = char(0xE0);
}

void TestErofs::detect()
{
    QByteArray s(1028, 0);
    putMagic(s);
    QVERIFY(imgerofs::isErofs(s));
    QVERIFY(!imgerofs::isErofs(QByteArray("CrAU")));
    QVERIFY(!imgerofs::isErofs(QByteArray(1024, 0))); // 截断，读不到完整魔数
}

void TestErofs::parseSuper()
{
    QByteArray s(1144, 0);
    putMagic(s);
    s[1036] = char(12);   // blkszbits = 4096
    put16(s, 1038, 2);    // root_nid
    imgerofs::SuperBlock sb;
    QVERIFY(imgerofs::parseSuper(s, sb));
    QCOMPARE(sb.blockSize, 4096u);
    QCOMPARE(sb.rootNid, 2ull);
}

void TestErofs::parseSuperLz4Flag()
{
    QByteArray s(1144, 0);
    putMagic(s);
    s[1036] = char(12);
    put16(s, 1038, 2);
    imgerofs::SuperBlock sb;
    put16(s, 1108, 0x0001);  // available_compr_algs: bit0 = LZ4
    QVERIFY(imgerofs::parseSuper(s, sb));
    QVERIFY(sb.isLz4);
    put16(s, 1108, 0x0000);  // legacy 布局（algs==0）: 唯一可用算法即 LZ4
    QVERIFY(imgerofs::parseSuper(s, sb));
    QVERIFY(sb.isLz4);
    put16(s, 1108, 0x0002);  // bit0 清零: 无 LZ4
    QVERIFY(imgerofs::parseSuper(s, sb));
    QVERIFY(!sb.isLz4);
}

void TestErofs::parseSuper48Bit()
{
    // EROFS_FEATURE_INCOMPAT_48BIT(0x80) 置位时 root_nid 取 rootnid_8b @1136
    QByteArray s(1144, 0);
    putMagic(s);
    s[1036] = char(12);
    put32(s, 1104, 0x00000080);
    put64(s, 1136, 0x123456789ull);
    imgerofs::SuperBlock sb;
    QVERIFY(imgerofs::parseSuper(s, sb));
    QCOMPARE(sb.rootNid, 0x123456789ull);
}

void TestErofs::invalidInput()
{
    imgerofs::SuperBlock sb;
    QVERIFY(!imgerofs::parseSuper(QByteArray(), sb));          // 空输入
    QVERIFY(!imgerofs::parseSuper(QByteArray(1028, 0), sb));   // 魔数错误
    QByteArray s(1144, 0);
    putMagic(s);
    s[1036] = char(3);   // blkszbits 过小
    QVERIFY(!imgerofs::parseSuper(s, sb));
    s[1036] = char(17);  // blkszbits 过大
    QVERIFY(!imgerofs::parseSuper(s, sb));
}

QTEST_APPLESS_MAIN(TestErofs)
#include "test_fs_erofs.moc"
