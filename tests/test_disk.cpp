#include <QtTest>
#include "image_engine/disk_image.h"

class TestDisk : public QObject
{
    Q_OBJECT
private slots:
    void detectGpt();
    void parseOnePartition();
    void extractPartitionData();
    void detectLayoutBoth();
    void parseGpt4096Lba();
    void extractPartition4096Lba();
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

// 4096 字节 LBA 的最小 GPT（手写字节合成 —— **不读** edl/ 子模块里的真实样本 gpt_sm8180x.bin：
// 未初始化的克隆里没有它，测试会挂）。布局依据（仅注释引用）：
//   * edl/edlclient/Library/TestFiles/gpt_sm8180x.bin：24576 字节、`EFI PART`@0x1000、
//     part_entry_lba=2、32 项 ×128B；
//   * reference/qdl/tests/data/rawprogram1.xml:12：`gpt_main1.bin num_partition_sectors="6"`
//     ⇒ 6 × 4096 = 24576 字节，与上者吻合。
// 与 512 版的唯一区别是 LBA 单位：头在 LBA1 = 字节 0x1000，表项数组在 LBA2 = 字节 0x2000。
static QByteArray buildGptDisk4096()
{
    constexpr qint64 kLba = 4096;
    constexpr quint64 kSectors = 8;                       // 8 × 4096 = 32768 字节
    QByteArray disk(static_cast<int>(kSectors * quint64(kLba)), 0);
    // LBA0 保护 MBR：MBR 结构本身恒为 512 字节（UEFI 规范），尾签名 55AA
    disk[446 + 4] = char(0xEE);
    disk[511] = char(0x55); disk[510] = char(0xAA);
    auto put32 = [&](qint64 off, quint32 v) { for (int i = 0; i < 4; ++i) disk[off + i] = char((v >> (i * 8)) & 0xFF); };
    auto put64 = [&](qint64 off, quint64 v) { for (int i = 0; i < 8; ++i) disk[off + i] = char((v >> (i * 8)) & 0xFF); };
    const qint64 hdr = kLba;                              // LBA1 → 字节 0x1000
    disk.replace(hdr, 8, "EFI PART");
    put32(hdr + 12, 92);             // header size
    put64(hdr + 24, 1);              // current LBA
    put64(hdr + 32, kSectors - 1);   // backup LBA
    put64(hdr + 72, 2);              // 分区表 LBA：**4096 单位** → 字节 0x2000
    put32(hdr + 80, 4);              // 分区项数
    put32(hdr + 84, 128);            // 项大小
    // LBA2 表项数组：1 个分区；first=3 last=6（同为 4096 单位），name "vendor"
    const qint64 e = 2 * kLba;                            // 字节 0x2000
    put64(e + 32, 3);
    put64(e + 40, 6);
    for (int i = 0; i < 6; ++i) disk[e + 56 + i * 2] = "vendor"[i];
    // 分区数据 = LBA3..6（4 个 4096 字节扇区）
    disk.replace(3 * kLba, 4 * kLba, QByteArray(4 * 4096, '\x77'));
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
    // 512 布局的守护：识别结果必须仍是 512（不能因为新增 4096 识别而漂移）
    QCOMPARE(info.sectorSize, quint64(512));
    QCOMPARE(info.totalSectors, quint64(64));
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

// 布局探测：两种布局各一条 + 失败/截断边界。flash_plan 的失败文案分层依赖这个函数
// （"文件被截断"与"表坏了"必须分得开），所以它自己也要有守护。
void TestDisk::detectLayoutBoth()
{
    quint64 lba = 0;
    QVERIFY(imgdisk::detectGptLayout(buildGptDisk(), lba));
    QCOMPARE(lba, quint64(512));
    lba = 0;
    QVERIFY(imgdisk::detectGptLayout(buildGptDisk4096(), lba));
    QCOMPARE(lba, quint64(4096));
    // 头都不在两处（0x200 / 0x1000）→ 探测失败，不猜布局
    QVERIFY(!imgdisk::detectGptLayout(QByteArray(8192, '\0'), lba));
    // 截断：签名在 0x200 但只有 600 字节 —— 头读得到（探测成功），由 parseGpt 的
    // "≥ 2 个 LBA"门槛拒绝；调用方据此报"文件过短"而不是"表坏了"
    QByteArray small = buildGptDisk();
    small.truncate(600);
    lba = 0;
    QVERIFY(imgdisk::detectGptLayout(small, lba));
    QCOMPARE(lba, quint64(512));
    imgdisk::DiskInfo info;
    QVERIFY(!imgdisk::parseGpt(small, info));
    QByteArray justUnder = buildGptDisk();
    justUnder.truncate(1023);                       // 差 1 字节到 2×512
    QVERIFY(!imgdisk::parseGpt(justUnder, info));
}

void TestDisk::parseGpt4096Lba()
{
    QByteArray disk = buildGptDisk4096();
    imgdisk::DiskInfo info;
    QVERIFY(imgdisk::parseGpt(disk, info));
    QCOMPARE(info.sectorSize, quint64(4096));              // 识别出的 LBA 尺寸
    QCOMPARE(info.totalSectors, quint64(8));               // size/4096 = 8（不是 size/512 = 64）
    QCOMPARE(info.partitions.size(), 1);
    QCOMPARE(info.partitions[0].name, "vendor");
    QCOMPARE(info.partitions[0].startSector, quint64(3));  // LBA 单位 = 4096 字节
    QCOMPARE(info.partitions[0].numSectors, quint64(4));   // last−first+1 = 6−3+1
}

void TestDisk::extractPartition4096Lba()
{
    QByteArray disk = buildGptDisk4096();
    imgdisk::DiskInfo info;
    QVERIFY(imgdisk::parseGpt(disk, info));
    QByteArray out;
    QVERIFY(imgdisk::extractPartition(disk, info.partitions[0], out, info.sectorSize));
    QCOMPARE(out.size(), 4 * 4096);
    QVERIFY(out == QByteArray(4 * 4096, '\x77'));
    // 陷阱钉子：lbaSize 必须随盘走。默认的 512 会把 startLBA=3 当成字节偏移 1536
    // （表项数组之前），**不报错但取错区段** —— 调用方必须传 DiskInfo::sectorSize
    // （src/ui/image_worker.cpp 的 DiskGpt 分支）。
    QByteArray wrong;
    QVERIFY(imgdisk::extractPartition(disk, info.partitions[0], wrong));
    QCOMPARE(wrong.size(), 4 * 512);
    QVERIFY(wrong != out);
}

QTEST_APPLESS_MAIN(TestDisk)
#include "test_disk.moc"
