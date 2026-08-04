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
    QByteArray s(1200, 0);
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

QTEST_APPLESS_MAIN(TestExt4)
#include "test_fs_ext4.moc"
