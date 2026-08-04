#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "image_engine/twrp_image.h"

class TestTwrp : public QObject
{
    Q_OBJECT
private slots:
    void detect();
    void extractV1();
};

void TestTwrp::detect()
{
    QVERIFY(imgtwrp::isTwrpBackup(QByteArray("TWRP")));
    QVERIFY(!imgtwrp::isTwrpBackup(QByteArray("ANDROID!")));
}

void TestTwrp::extractV1()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString win = dir.filePath("boot.win");
    QByteArray hdr(64, 0);
    hdr.replace(0, 4, "TWRP");
    hdr[4] = 1; // version 1
    auto put64 = [&](int off, quint64 v) { for (int i = 0; i < 8; ++i) hdr[off + i] = char((v >> (i * 8)) & 0xFF); };
    put64(5, 4096);    // restore_size
    put64(13, 4096);   // packed_size
    put64(21, 4096);   // restore_used
    put64(29, 4096);   // backup_size
    put64(37, 4096);   // backup_used
    QFile f(win);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(hdr);
    f.write(QByteArray(4096, '\x33'));
    f.close();
    QByteArray out;
    QString err;
    QVERIFY(imgtwrp::extractWin(win, out, &err));
    QCOMPARE(out.size(), 4096);
    QVERIFY(out == QByteArray(4096, '\x33'));
}

QTEST_APPLESS_MAIN(TestTwrp)
#include "test_twrp.moc"
