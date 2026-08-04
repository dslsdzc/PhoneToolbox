#include <QtTest>
#include "image_engine/disk_image.h"

class TestDisk : public QObject
{
    Q_OBJECT
private slots:
    void detectGpt();
    void parseOnePartition();
    void extractPartitionData();
};

static QByteArray buildGptDisk()
{
    constexpr quint64 kSectors = 64;
    QByteArray disk(static_cast<int>(kSectors * 512), 0);
    // LBA0 保护 MBR: 分区类型 0xEE, 尾部 55AA
    disk[446 + 4] = char(0xEE);
    disk[511] = char(0x55); disk[510] = char(0xAA);
    // LBA1 GPT 头
    auto put32 = [&](qint64 off, quint32 v) { for (int i = 0; i < 4; ++i) disk[off + i] = char((v >> (i * 8)) & 0xFF); };
    auto put64 = [&](qint64 off, quint64 v) { for (int i = 0; i < 8; ++i) disk[off + i] = char((v >> (i * 8)) & 0xFF); };
    disk.replace(512, 8, "EFI PART");
    put32(512 + 12, 92);    // header size
    put64(512 + 24, 1);     // current LBA
    put64(512 + 32, kSectors - 1); // backup LBA
    put64(512 + 72, 2);     // 分区表 LBA
    put32(512 + 80, 4);     // 分区项数
    put32(512 + 84, 128);   // 项大小
    // LBA2 分区表: 1 个分区, first=8 last=23, name "system"
    qint64 e = 2 * 512;
    for (int i = 0; i < 16; ++i) disk[e + i] = char(0x00); // type GUID 全零(测试用)
    put64(e + 32, 8);
    put64(e + 40, 23);
    for (int i = 0; i < 6; ++i) disk[e + 56 + i * 2] = "system"[i];
    // 分区数据 16 扇区
    disk.replace(8 * 512, 16 * 512, QByteArray(16 * 512, '\x55'));
    return disk;
}

void TestDisk::detectGpt()
{
    QByteArray disk = buildGptDisk();
    QVERIFY(imgdisk::isGpt(disk.mid(512, 8)));
    QVERIFY(!imgdisk::isGpt(QByteArray("CrAU")));
}

void TestDisk::parseOnePartition()
{
    QByteArray disk = buildGptDisk();
    imgdisk::DiskInfo info;
    QVERIFY(imgdisk::parseGpt(disk, info));
    QCOMPARE(info.partitions.size(), 1);
    QCOMPARE(info.partitions[0].name, "system");
    QCOMPARE(info.partitions[0].startSector, 8ull);
}

void TestDisk::extractPartitionData()
{
    QByteArray disk = buildGptDisk();
    imgdisk::DiskInfo info;
    QVERIFY(imgdisk::parseGpt(disk, info));
    QByteArray out;
    QVERIFY(imgdisk::extractPartition(disk, info.partitions[0], out));
    QCOMPARE(out.size(), 16 * 512);
    QVERIFY(out == QByteArray(16 * 512, '\x55'));
}

QTEST_APPLESS_MAIN(TestDisk)
#include "test_disk.moc"
