#include <QtTest>
#include "image_engine/fs/ext4_reader.h"

// 说明：brief 原构造把字段写在 0/4/24/88，但内核布局（fs/ext4/ext4.h
// struct ext4_super_block）中 superblock 位于文件偏移 1024，字段应落在
// 1024/1028/1048/1112（s_magic @1080=1024+56 不变）。已按 e2fsprogs
// mke2fs 真实镜像 xxd 核对（inodes_count=2048@1024、log_block_size=2@1048、
// inode_size=256@1112、magic 53 ef@1080、first_ino=11@1108），据此修正偏移。
class TestExt4 : public QObject
{
    Q_OBJECT
private slots:
    void detect();
    void parseSuper();
    void parseSuper64Bit();
    void invalidInput();
};

void TestExt4::detect()
{
    QByteArray s(1082, 0);
    s[1080] = char(0x53); s[1081] = char(0xEF);
    QVERIFY(imgext4::isExt4(s));
    QVERIFY(!imgext4::isExt4(QByteArray("CrAU")));
}

void TestExt4::parseSuper()
{
    // superblock: magic@1080, inodes_count@1024, blocks_count_lo@1028,
    //             log_block_size@1048, inode_size@1112
    // 缓冲 1400 ≥ 边界守卫 1384（parseSuper 要求镜像 ≥ 1384 字节），
    // 使字段校验（而非长度守卫）生效
    QByteArray s(1400, 0);
    s[1080] = char(0x53); s[1081] = char(0xEF);
    auto put32 = [&](int off, quint32 v) { s[off] = char(v); s[off + 1] = char(v >> 8); s[off + 2] = char(v >> 16); s[off + 3] = char(v >> 24); };
    put32(1024, 32);    // inodes_count
    put32(1028, 4096);  // blocks_count_lo
    s[1048] = char(2);  // log_block_size → 4096
    put32(1112, 256);   // inode_size
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(s, sb));
    QCOMPARE(sb.blockSize, 4096u);
    QCOMPARE(sb.inodeCount, 32ull);
    QCOMPARE(sb.inodeSize, 256u);
}

void TestExt4::parseSuper64Bit()
{
    // EXT4_FEATURE_INCOMPAT_64BIT (0x80) @1120 时并入 s_blocks_count_hi @1360
    QByteArray s(1400, 0);
    s[1080] = char(0x53); s[1081] = char(0xEF);
    auto put32 = [&](int off, quint32 v) { s[off] = char(v); s[off + 1] = char(v >> 8); s[off + 2] = char(v >> 16); s[off + 3] = char(v >> 24); };
    put32(1024, 32);    // inodes_count
    put32(1028, 4096);  // blocks_count_lo
    s[1048] = char(2);  // log_block_size → 4096
    put32(1112, 256);   // inode_size
    put32(1120, 0x80);  // s_feature_incompat |= 64BIT
    put32(1360, 3);     // s_blocks_count_hi
    imgext4::SuperBlock sb;
    QVERIFY(imgext4::parseSuper(s, sb));
    QCOMPARE(sb.blockSize, 4096u);
    QCOMPARE(sb.blockCount, (quint64(3) << 32) | 4096ull);
}

void TestExt4::invalidInput()
{
    auto put32 = [&](QByteArray &d, int off, quint32 v) { d[off] = char(v); d[off + 1] = char(v >> 8); d[off + 2] = char(v >> 16); d[off + 3] = char(v >> 24); };
    auto makeValid = [&](int size) {
        QByteArray d(size, 0);
        d[1080] = char(0x53); d[1081] = char(0xEF);
        put32(d, 1024, 32); put32(d, 1028, 4096);
        d[1048] = char(2); put32(d, 1112, 256);
        return d;
    };
    imgext4::SuperBlock sb;

    // 负向用例缓冲均用 1400（≥ 1384 边界守卫），确保是字段校验而非长度守卫拒绝
    // 坏 magic → false
    QByteArray badMagic = makeValid(1400);
    badMagic[1080] = char(0x54);
    QVERIFY(!imgext4::isExt4(badMagic));
    QVERIFY(!imgext4::parseSuper(badMagic, sb));
    // 短输入（无 magic 空间/零填充）→ false
    QVERIFY(!imgext4::parseSuper(QByteArray(1082, 0), sb));
    // log_block_size > 6 → false
    QByteArray logBig = makeValid(1400);
    logBig[1048] = char(7);
    QVERIFY(!imgext4::parseSuper(logBig, sb));
    // inode_size < 128 → false
    QByteArray inoSmall = makeValid(1400);
    put32(inoSmall, 1112, 64);
    QVERIFY(!imgext4::parseSuper(inoSmall, sb));
    // inode_size > blockSize (4096) → false
    QByteArray inoBig = makeValid(1400);
    put32(inoBig, 1112, 8192);
    QVERIFY(!imgext4::parseSuper(inoBig, sb));
    // 截断输入 [1082, 1363]：magic 有效但字段读取终点不足 → false，不得崩溃
    QByteArray trunc1363(1363, 0);
    trunc1363[1080] = char(0x53); trunc1363[1081] = char(0xEF);
    put32(trunc1363, 1024, 32); put32(trunc1363, 1028, 4096);
    trunc1363[1048] = char(2);
    QVERIFY(imgext4::isExt4(trunc1363));
    QVERIFY(!imgext4::parseSuper(trunc1363, sb));
    QByteArray trunc1113(1113, 0);
    trunc1113[1080] = char(0x53); trunc1113[1081] = char(0xEF);
    QVERIFY(!imgext4::parseSuper(trunc1113, sb));
}

QTEST_APPLESS_MAIN(TestExt4)
#include "test_fs_ext4.moc"
