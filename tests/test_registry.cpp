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
}

void TestRegistry::detectByExtension()
{
    QCOMPARE(imgreg::detect(QByteArray(512, 0), "firmware.tar.md5").format, imgreg::Format::TarMd5);
    QCOMPARE(imgreg::detect(QByteArray(512, 0), "boot.br").format, imgreg::Format::Br);
    QCOMPARE(imgreg::detect(QByteArray("garbage"), "weird.xyz").format, imgreg::Format::Unknown);
}

QTEST_APPLESS_MAIN(TestRegistry)
#include "test_registry.moc"
