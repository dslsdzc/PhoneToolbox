#include <QtTest>
#include "image_engine/super_image.h"

class TestSuper : public QObject
{
    Q_OBJECT
private slots:
    void detect();
    void parseMinimal();
    void extractLinear();
    void zeroExtent();
    void rejectSectorOverflow();
    void rejectHugeLinear();
    void rejectBadEntrySize();
};

static QByteArray put32(quint32 v)
{
    QByteArray b(4, 0);
    for (int i = 0; i < 4; ++i) b[i] = char((v >> (i * 8)) & 0xFF);
    return b;
}
static QByteArray put64(quint64 v)
{
    QByteArray b(8, 0);
    for (int i = 0; i < 8; ++i) b[i] = char((v >> (i * 8)) & 0xFF);
    return b;
}

// 最小 super: geometry 主副本区 4096 + 几何块 4096 + metadata(头 128 + 表 140B) + 分区数据
// 布局对照 AOSP metadata_format.h / images.cpp（lp metadata 布局已由 spec 验证）：
//   geometry 主副本区 [0, 4096) → 几何块 "gDla" [4096, 8192) → metadata 头+表 [8192, 8460)
//   数据区从 metadata 区之后按 logical_block_size(4096) 对齐起始（扇区 24 = 字节 12288）
static QByteArray buildMinimalSuper(quint32 extentType = 0,      // 0=LINEAR 1=ZERO
                                    quint64 numSectors = 8,
                                    quint64 targetData = 24,
                                    quint32 partEntrySize = 52,
                                    quint32 extEntrySize = 24)
{
    // geometry（AOSP LpMetadataGeometry 固定 52B；块本身占 4096B）
    QByteArray geom(4096, 0);
    geom.replace(0, 4, put32(0x616c4467));
    geom.replace(4, 4, put32(52));   // struct_size
    geom.replace(40, 4, put32(4096)); // metadata_max_size
    geom.replace(44, 4, put32(1));    // slot_count
    geom.replace(48, 4, put32(4096)); // logical_block_size
    // metadata: 头 128 + partitions 表(1×52) + extents 表(1×24) + block_devices 表(1×64)
    QByteArray partitions(52, 0);
    // 注意: QByteArray::replace(pos, len, after) 是"用 after 替换 [pos, pos+len) 区间"，
    // 会改变数组长度。此处仅替换 name 的实际字节数(6)，保持 52B 定长条目不变。
    partitions.replace(0, 6, "system");
    partitions.replace(40, 4, put32(0));  // first_extent_index
    partitions.replace(44, 4, put32(1));  // num_extents
    QByteArray extents(24, 0);
    extents.replace(0, 8, put64(numSectors)); // num_sectors
    extents.replace(8, 4, put32(extentType)); // target_type: 0=LINEAR 1=ZERO
    extents.replace(12, 8, put64(targetData)); // target_data: 数据起点扇区（metadata 区之后按 4096 对齐）
    extents.replace(20, 4, put32(0));  // target_source: block device 0
    // block_devices 表 (1×64)
    QByteArray bd(64, 0);
    bd.replace(24, 8, put64(4096));    // size 占位
    const quint32 tablesSize = 52 + 24 + 64;
    QByteArray hdr(128, 0);
    hdr.replace(0, 4, put32(0x414C5030));
    hdr.replace(4, 2, QByteArray("\x0a\x00", 2));  // major 10
    hdr.replace(6, 2, QByteArray("\x00\x00", 2));  // minor 0
    hdr.replace(8, 4, put32(128));     // header_size
    hdr.replace(44, 4, put32(tablesSize)); // tables_size
    hdr.replace(80, 4, put32(0));      // partitions offset (相对 header 尾)
    hdr.replace(84, 4, put32(1));      // num partitions
    hdr.replace(88, 4, put32(partEntrySize)); // entry size
    hdr.replace(92, 4, put32(52));     // extents offset
    hdr.replace(96, 4, put32(1));      // num extents
    hdr.replace(100, 4, put32(extEntrySize)); // entry size
    hdr.replace(104, 4, put32(52 + 24)); // groups offset
    hdr.replace(108, 4, put32(0));     // num groups (默认组省略)
    hdr.replace(116, 4, put32(52 + 24)); // block_devices offset
    hdr.replace(120, 4, put32(1));     // num block devices
    hdr.replace(124, 4, put32(64));    // entry size
    QByteArray img;
    img.append(QByteArray(4096, 0));  // geometry 主副本区
    img.append(geom);
    img.append(hdr).append(partitions).append(extents).append(bd);
    // metadata 区结束于 4096+4096+128+140 = 8460B；按 logical_block_size 对齐到 12288（扇区 24），
    // 再追加分区数据（extent target_data 指向该起点）
    img.resize(24 * 512);
    img.append(QByteArray(8 * 512, '\xAA')); // 分区数据 4096B
    return img;
}

void TestSuper::detect()
{
    QByteArray img = buildMinimalSuper();
    QVERIFY(imgsuper::isSuper(img.mid(4096, 4)));
    QVERIFY(!imgsuper::isSuper(QByteArray("CrAU")));
}

void TestSuper::parseMinimal()
{
    QByteArray img = buildMinimalSuper();
    imgsuper::SuperInfo info;
    QVERIFY(imgsuper::parseSuper(img, info));
    QCOMPARE(info.partitions.size(), 1);
    QCOMPARE(info.partitions[0].name, "system");
    QCOMPARE(info.partitions[0].extents.size(), 1);
    QCOMPARE(info.partitions[0].extents[0].numSectors, 8ull);
}

void TestSuper::extractLinear()
{
    QByteArray img = buildMinimalSuper();
    imgsuper::SuperInfo info;
    QVERIFY(imgsuper::parseSuper(img, info));
    QString err;
    QList<QByteArray> parts = imgsuper::extractPartitions(img, info, &err);
    QCOMPARE(parts.size(), 1);
    QCOMPARE(parts[0].size(), 4096);
    QVERIFY(parts[0] == QByteArray(4096, '\xAA'));
}

void TestSuper::zeroExtent()
{
    // ZERO extent（target_type=1）：提取结果为等长零填充，且长度可与镜像文件大小无关
    QByteArray img = buildMinimalSuper(/*ZERO*/ 1);
    imgsuper::SuperInfo info;
    QVERIFY(imgsuper::parseSuper(img, info));
    QString err;
    QList<QByteArray> parts = imgsuper::extractPartitions(img, info, &err);
    QCOMPARE(parts.size(), 1);
    QCOMPARE(parts[0].size(), 4096);
    QVERIFY(parts[0] == QByteArray(4096, 0));
}

void TestSuper::rejectSectorOverflow()
{
    // numSectors = 2^55：×512 将回绕 quint64，必须拒绝（元数据本身合法，parse 应成功）
    QByteArray img = buildMinimalSuper(/*LINEAR*/ 0, Q_UINT64_C(1) << 55, 24);
    imgsuper::SuperInfo info;
    QVERIFY(imgsuper::parseSuper(img, info));
    QString err;
    QList<QByteArray> parts = imgsuper::extractPartitions(img, info, &err);
    QVERIFY(parts.isEmpty());
    QVERIFY(!err.isEmpty());
}

void TestSuper::rejectHugeLinear()
{
    // LINEAR extent 声明 2^31+512 字节：旧实现 static_cast<int> 会回绕为负（崩溃/错内容），
    // 新实现先按 qint64 上界校验，应干净拒绝并报错
    QByteArray img = buildMinimalSuper(/*LINEAR*/ 0, (Q_UINT64_C(1) << 22) + 1, 24);
    imgsuper::SuperInfo info;
    QVERIFY(imgsuper::parseSuper(img, info));
    QString err;
    QList<QByteArray> parts = imgsuper::extractPartitions(img, info, &err);
    QVERIFY(parts.isEmpty());
    QVERIFY(!err.isEmpty());
}

void TestSuper::rejectBadEntrySize()
{
    // partitions 条目大小 53 ≠ 52：必须拒绝
    QByteArray img = buildMinimalSuper(0, 8, 24, /*partEntrySize*/ 53);
    imgsuper::SuperInfo info;
    QVERIFY(!imgsuper::parseSuper(img, info));
    // extents 条目大小 25 ≠ 24：必须拒绝
    QByteArray img2 = buildMinimalSuper(0, 8, 24, 52, /*extEntrySize*/ 25);
    imgsuper::SuperInfo info2;
    QVERIFY(!imgsuper::parseSuper(img2, info2));
}

QTEST_APPLESS_MAIN(TestSuper)
#include "test_super.moc"
