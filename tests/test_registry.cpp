#include <QtTest>
#include "image_engine/registry.h"
#include "image_engine/fs/fs_image.h"

static void put16(QByteArray &d, int off, quint32 v)
{
    d[off] = char(v & 0xFF);
    d[off + 1] = char((v >> 8) & 0xFF);
}
static void put32(QByteArray &d, int off, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        d[off + i] = char((v >> (i * 8)) & 0xFF);
}

class TestRegistry : public QObject
{
    Q_OBJECT
private slots:
    void detectByMagic();
    void detectByExtension();
    void detectVendorMagic();
    void openFsImageDispatch();
};

void TestRegistry::detectByMagic()
{
    QCOMPARE(imgreg::detect(QByteArray("CrAU"), "ota.bin").format, imgreg::Format::Payload);
    QCOMPARE(imgreg::detect(QByteArray("ANDROID!"), "boot.img").format, imgreg::Format::Boot);
    QByteArray sparse(4, 0); sparse[0] = '\x3A'; sparse[1] = '\xFF'; sparse[2] = '\x26'; sparse[3] = '\xED';
    QCOMPARE(imgreg::detect(sparse, "system.img").format, imgreg::Format::Sparse);
    QCOMPARE(imgreg::detect(QByteArray("VNDRBOOT"), "vendor_boot.img").format, imgreg::Format::VendorBoot);
    QCOMPARE(imgreg::detect(QByteArray("\x1f\x8b", 2), "f.gz").format, imgreg::Format::Gzip);
    QCOMPARE(imgreg::detect(QByteArray("\x28\xb5\x2f\xfd", 4), "f.zst").format, imgreg::Format::Zstd);
    // EROFS: magic 0xE0F5E1E2 小端落盘（E2 E1 F5 E0）位于偏移 1024（EROFS_SUPER_OFFSET）
    QByteArray erofs(2048, 0);
    erofs[1024] = '\xE2'; erofs[1025] = '\xE1'; erofs[1026] = '\xF5'; erofs[1027] = '\xE0';
    QCOMPARE(imgreg::detect(erofs, "erofs.img").format, imgreg::Format::Erofs);
    // 偏移 0 出现 E2 E1 F5 E0 → 不再是 EROFS（旧实现误判: 魔数末字节 0x00 且位置错误）
    QByteArray fakeErofs(1028, 0);
    fakeErofs[0] = '\xE2'; fakeErofs[1] = '\xE1'; fakeErofs[2] = '\xF5'; fakeErofs[3] = '\xE0';
    QCOMPARE(imgreg::detect(fakeErofs, "fake.bin").format, imgreg::Format::Unknown);
    // super: geometry magic "gDla" 位于偏移 4096（LP_METADATA_GEOMETRY_OFFSET）
    QByteArray super(8192, 0);
    super[4096] = 'g'; super[4097] = 'D'; super[4098] = 'l'; super[4099] = 'a';
    QCOMPARE(imgreg::detect(super, "super.img").format, imgreg::Format::Super);
    // 偏移 0 出现 "0PLA" → 不再是 super（旧实现误判: metadata 头在偏移 8192，0 是保留区）
    QByteArray fakeSuper(8192, 0);
    fakeSuper.replace(0, 4, "0PLA");
    QCOMPARE(imgreg::detect(fakeSuper, "fake.bin").format, imgreg::Format::Unknown);
}

void TestRegistry::detectByExtension()
{
    QCOMPARE(imgreg::detect(QByteArray(512, 0), "firmware.tar.md5").format, imgreg::Format::TarMd5);
    QCOMPARE(imgreg::detect(QByteArray(512, 0), "boot.br").format, imgreg::Format::Br);
    QCOMPARE(imgreg::detect(QByteArray("garbage"), "weird.xyz").format, imgreg::Format::Unknown);
    // pac 旧格式无魔数（divinebird C 版, 头 1220B）→ 扩展名兜底
    QCOMPARE(imgreg::detect(QByteArray(1220, 0), "fw.pac").format, imgreg::Format::Pac);
}

void TestRegistry::detectVendorMagic()
{
    // KDZ v3: 8B 魔数（kdztools unkdz.py _dz_header = \x28\x05\x00\x00\x24\x38\x22\x25）
    QCOMPARE(imgreg::detect(QByteArrayLiteral("\x28\x05\x00\x00\x24\x38\x22\x25"), "f.kdz").format,
             imgreg::Format::Kdz);
    QCOMPARE(imgreg::detect(QByteArrayLiteral("\x28\x05\x00\x00\x24\x38\x22"), "f.kdz").format,
             imgreg::Format::Unknown); // 截断 7B → 不误判
    // SIN v3: [0]=0x03 + "SIN"
    QByteArray sin(8, 0);
    sin[0] = '\x03'; sin[1] = 'S'; sin[2] = 'I'; sin[3] = 'N';
    QCOMPARE(imgreg::detect(sin, "f.sin").format, imgreg::Format::Sin);
    QByteArray fakeSin(4, 0);
    fakeSin[0] = '\x03'; fakeSin[1] = 'S'; fakeSin[2] = 'I'; fakeSin[3] = 'X';
    QCOMPARE(imgreg::detect(fakeSin, "f.bin").format, imgreg::Format::Unknown);
    // 华为 update.app: 魔数 0x55 0xAA
    QCOMPARE(imgreg::detect(QByteArray("\x55\xAA", 2), "update.app").format,
             imgreg::Format::UpdateApp);
    QCOMPARE(imgreg::detect(QByteArray("\x55\x00", 2), "update.app").format,
             imgreg::Format::Unknown);
    // GPT: 主分区表头 "EFI PART" 位于 LBA1（偏移 512）
    QByteArray gpt(520, 0);
    gpt.replace(512, 8, "EFI PART");
    QCOMPARE(imgreg::detect(gpt, "disk.img").format, imgreg::Format::DiskGpt);
    QByteArray notGpt(520, 0);
    QCOMPARE(imgreg::detect(notGpt, "f.bin").format, imgreg::Format::Unknown);
    // TWRP 备份: "TWRP" @0
    QCOMPARE(imgreg::detect(QByteArray("TWRP"), "boot.win").format, imgreg::Format::TwrpWin);
    // pac 新格式: 版本串 "BP_R1.0.0"/"BP_R2.0.1" UTF-16LE @0（字符间嵌 \0）
    QByteArray pac(2124, 0);
    QByteArray verUtf16(18, 0);
    const char *ver = "BP_R1.0.0";
    for (int i = 0; ver[i]; ++i)
        verUtf16[i * 2] = ver[i];
    pac.replace(0, verUtf16.size(), verUtf16);
    QCOMPARE(imgreg::detect(pac, "f.pac").format, imgreg::Format::Pac);
    // pac 新格式辅助信号: 0xfffafffa 魔数 @2116
    QByteArray pac2(2124, 0);
    pac2[2116] = '\xFA'; pac2[2117] = '\xFF'; pac2[2118] = '\xFA'; pac2[2119] = '\xFF';
    QCOMPARE(imgreg::detect(pac2, "f.bin").format, imgreg::Format::Pac);
}

void TestRegistry::openFsImageDispatch()
{
    // 空镜像 → nullptr + error（不崩溃）
    QString err;
    QVERIFY(imgfs::openFsImage(QByteArray(), &err) == nullptr);
    QVERIFY(!err.isEmpty());
    // 未知格式 → nullptr + error
    err.clear();
    QVERIFY(imgfs::openFsImage(QByteArray("garbage bytes"), &err) == nullptr);
    QVERIFY(!err.isEmpty());
    // GPT 磁盘镜像不是文件系统镜像 → nullptr
    QByteArray gpt(520, 0);
    gpt.replace(512, 8, "EFI PART");
    QVERIFY(imgfs::openFsImage(gpt, nullptr) == nullptr);

    // EROFS: 仅有魔数的截断头 → detect 命中但 superblock 解析失败 → nullptr
    QByteArray erofsTrunc(1028, 0);
    erofsTrunc[1024] = '\xE2'; erofsTrunc[1025] = '\xE1'; erofsTrunc[1026] = '\xF5'; erofsTrunc[1027] = '\xE0';
    QVERIFY(imgfs::openFsImage(erofsTrunc, &err) == nullptr);

    // EROFS: 合法最小 superblock（blkszbits=12 → 4096B 块）→ 分派成功
    QByteArray erofsImg(2048, 0);
    erofsImg[1024] = '\xE2'; erofsImg[1025] = '\xE1'; erofsImg[1026] = '\xF5'; erofsImg[1027] = '\xE0';
    erofsImg[1036] = char(12); // blkszbits u8 @super+12
    imgfs::FsImage *fs = imgfs::openFsImage(erofsImg, &err);
    QVERIFY(fs != nullptr);
    QList<imgfs::FsEntry> entries;
    // 镜像内无真实 inode 树 → list 干净失败（不崩溃）
    QVERIFY(!fs->list("/", entries));
    // EROFS 只读: replace 不支持、repack 原样返回
    QVERIFY(!fs->replace("a.txt", QByteArray("x")));
    QCOMPARE(fs->repack(), erofsImg);
    delete fs;

    // ext4: 魔数命中但 superblock 字段非法（inodeCount=0）→ nullptr
    QByteArray ext4Bad(4096, 0);
    ext4Bad[1080] = '\x53'; ext4Bad[1081] = '\xEF';
    QVERIFY(imgfs::openFsImage(ext4Bad, &err) == nullptr);

    // ext4: 合法最小 superblock → 分派成功
    QByteArray ext4Img(4096, 0);
    ext4Img[1080] = '\x53'; ext4Img[1081] = '\xEF'; // s_magic
    put32(ext4Img, 1024, 1);   // s_inodes_count
    put32(ext4Img, 1028, 1);   // s_blocks_count_lo
    put32(ext4Img, 1048, 2);   // s_log_block_size → 4096B 块
    put32(ext4Img, 1056, 1);   // s_blocks_per_group
    put32(ext4Img, 1064, 1);   // s_inodes_per_group
    put16(ext4Img, 1112, 256); // s_inode_size
    fs = imgfs::openFsImage(ext4Img, &err);
    QVERIFY(fs != nullptr);
    // 镜像内无真实 inode → 替换干净失败; repack 校验 super 后原样返回
    QVERIFY(!fs->replace("missing.txt", QByteArray("x")));
    QCOMPARE(fs->repack(), ext4Img);
    delete fs;
}

QTEST_APPLESS_MAIN(TestRegistry)
#include "test_registry.moc"
