#include <QtTest>
#include "image_engine/boot_image.h"

class TestBoot : public QObject
{
    Q_OBJECT
private slots:
    void detect();
    void parseV0();
    void parseV2();
    void parseV4();
};

// 构造 v0: header 1632B 占第一页（mkbootimg 将 header 补零到 page_size），
// kernel 1 页 + ramdisk 1 页，page_size=4096
static QByteArray buildBootV0()
{
    const QByteArray kernel(4096, 'K');
    const QByteArray ramdisk(4096, 'R');
    QByteArray hdr(1632, 0);
    hdr.replace(0, 8, "ANDROID!");
    auto put32 = [&](int off, quint32 v) {
        hdr[off] = char(v); hdr[off + 1] = char(v >> 8);
        hdr[off + 2] = char(v >> 16); hdr[off + 3] = char(v >> 24);
    };
    put32(8, 4096);   // kernel_size
    put32(16, 4096);  // ramdisk_size
    put32(36, 4096);  // page_size
    put32(40, 0);     // header_version
    put32(44, 0x000A0B0C); // os_version
    hdr.replace(64, 13, QByteArray("console=ttyS0", 13)); // cmdline@64 (512B)
    return hdr + QByteArray(4096 - 1632, '\0') + kernel + ramdisk;
}

void TestBoot::detect()
{
    QVERIFY(imgboot::isBootImage(QByteArray("ANDROID!")));
    QVERIFY(!imgboot::isBootImage(QByteArray("VNDRBOOT")));
}

void TestBoot::parseV0()
{
    QByteArray raw = buildBootV0();
    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(raw, info));
    QCOMPARE(info.headerVersion, 0u);
    QCOMPARE(info.pageSize, 4096u);
    QCOMPARE(info.kernel.size(), 4096);
    QCOMPARE(info.ramdisk.size(), 4096);
    QVERIFY(info.kernel.startsWith("KKKK"));
    QCOMPARE(info.cmdline, QByteArray("console=ttyS0"));
}

void TestBoot::parseV2()
{
    // v2: header 1660B（AOSP packed 布局）：
    //   v1 = 1632 + recovery_dtbo_size(4)@1632 + recovery_dtbo_offset(8)@1636 + header_size(4)@1644
    //   v2 = v1 + dtb_size(4)@1648 + dtb_addr(8)@1652
    // 段序 kernel→ramdisk→second→recovery_dtbo→dtb，每段 4096 对齐
    QByteArray hdr(1660, 0);
    hdr.replace(0, 8, "ANDROID!");
    auto put32 = [&](int off, quint32 v) {
        hdr[off] = char(v); hdr[off + 1] = char(v >> 8);
        hdr[off + 2] = char(v >> 16); hdr[off + 3] = char(v >> 24);
    };
    auto put64 = [&](int off, quint64 v) {
        for (int i = 0; i < 8; ++i)
            hdr[off + i] = char(v >> (8 * i));
    };
    put32(8, 4096);    // kernel_size
    put32(16, 4096);   // ramdisk_size
    put32(24, 4096);   // second_size
    put32(36, 4096);   // page_size
    put32(40, 2);      // header_version
    put32(44, 0x000A0B0C); // os_version
    put32(1632, 4096);  // recovery_dtbo_size (uint32@1632)
    put64(1636, 16384); // recovery_dtbo_offset (uint64@1636，非零以捕获 8 字节跨字段读取 bug)
    put32(1648, 4096);  // dtb_size
    QByteArray raw = hdr + QByteArray(4096 - 1660, '\0')
                   + QByteArray(4096, 'K')  // kernel
                   + QByteArray(4096, 'R')  // ramdisk
                   + QByteArray(4096, 'S')  // second
                   + QByteArray(4096, 'O')  // recovery_dtbo
                   + QByteArray(4096, 'T'); // dtb
    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(raw, info));
    QCOMPARE(info.headerVersion, 2u);
    QCOMPARE(info.dtbSize, 4096u);
    QVERIFY(info.kernel.startsWith("KKKK"));
    QVERIFY(info.ramdisk.startsWith("RRRR"));
    QVERIFY(info.dtb.startsWith("TTTT"));
}

void TestBoot::parseV4()
{
    // v4: header 1580B 占第一页，kernel 4096 + ramdisk 4096（固定 4096 页）
    QByteArray hdr(1580, 0);
    hdr.replace(0, 8, "ANDROID!");
    auto put32 = [&](int off, quint32 v) {
        hdr[off] = char(v); hdr[off + 1] = char(v >> 8);
        hdr[off + 2] = char(v >> 16); hdr[off + 3] = char(v >> 24);
    };
    put32(8, 4096);   // kernel_size
    put32(12, 4096);  // ramdisk_size
    put32(20, 1580);  // header_size
    put32(40, 4);     // header_version
    hdr.replace(44, 13, QByteArray("console=ttyS0", 13)); // cmdline@44 (1536B)
    QByteArray raw = hdr + QByteArray(4096 - 1580, '\0')
                   + QByteArray(4096, 'K') + QByteArray(4096, 'R');
    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(raw, info));
    QCOMPARE(info.headerVersion, 4u);
    QCOMPARE(info.kernel.size(), 4096);
    QCOMPARE(info.ramdisk.size(), 4096);
    QVERIFY(info.kernel.startsWith("KKKK"));
    QVERIFY(info.ramdisk.startsWith("RRRR"));
    QCOMPARE(info.cmdline, QByteArray("console=ttyS0"));
}

QTEST_APPLESS_MAIN(TestBoot)
#include "test_boot.moc"
