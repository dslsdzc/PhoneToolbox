#include <QtTest>
#include "image_engine/registry.h"

class TestRegistry : public QObject
{
    Q_OBJECT
private slots:
    void detectByMagic();
    void detectByExtension();
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
}

QTEST_APPLESS_MAIN(TestRegistry)
#include "test_registry.moc"
