#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "image_engine/dat_image.h"

class TestDat : public QObject
{
    Q_OBJECT
private slots:
    void applyNew();
};

void TestDat::applyNew()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString tl = dir.filePath("transfer.list");
    const QString dat = dir.filePath("system.new.dat");
    QFile f(tl);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("1\n");        // 版本
    f.write("2\n");        // 总块数
    f.write("new 2 0,2\n"); // 2 块（块 0-1），range 数 = count
    f.close();
    QFile d(dat);
    QVERIFY(d.open(QIODevice::WriteOnly));
    d.write(QByteArray(8192, '\x77'));
    d.close();
    QByteArray out;
    QString err;
    QVERIFY(imgdat::sdat2img(tl, dat, out, &err));
    QCOMPARE(out.size(), 8192);
    QVERIFY(out == QByteArray(8192, '\x77'));
}

QTEST_APPLESS_MAIN(TestDat)
#include "test_dat.moc"
