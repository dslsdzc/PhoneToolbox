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
    void extractV1Split();
    void extractV1Truncated();
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

void TestTwrp::extractV1Split()
{
    // 分段正例: packed_size(8192) 为跨分段总量，主文件仅含 4096B（合法: 其余在 .win001）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString win = dir.filePath("boot.win");
    QByteArray hdr(64, 0);
    hdr.replace(0, 4, "TWRP");
    hdr[4] = 1;
    auto put64 = [&](int off, quint64 v) { for (int i = 0; i < 8; ++i) hdr[off + i] = char((v >> (i * 8)) & 0xFF); };
    put64(5, 8192);    // restore_size
    put64(13, 8192);   // packed_size = 主文件 + 分段总量
    put64(21, 8192);   // restore_used
    QFile f(win);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(hdr);
    f.write(QByteArray(4096, '\x33'));
    f.close();
    QFile seg(dir.filePath("boot.win001"));
    QVERIFY(seg.open(QIODevice::WriteOnly));
    seg.write(QByteArray(4096, '\x44'));
    seg.close();
    QByteArray out;
    QString err;
    QVERIFY(imgtwrp::extractWin(win, out, &err));
    QCOMPARE(out.size(), 8192);
    QCOMPARE(out.left(4096), QByteArray(4096, '\x33'));
    QCOMPARE(out.mid(4096), QByteArray(4096, '\x44'));
}

void TestTwrp::extractV1Truncated()
{
    // 截断负例: 声明 8192B 实际只有 4096B 且无分段文件 → 必须失败并报"备份数据不完整"
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString win = dir.filePath("boot.win");
    QByteArray hdr(64, 0);
    hdr.replace(0, 4, "TWRP");
    hdr[4] = 1;
    auto put64 = [&](int off, quint64 v) { for (int i = 0; i < 8; ++i) hdr[off + i] = char((v >> (i * 8)) & 0xFF); };
    put64(5, 8192);    // restore_size
    put64(13, 8192);   // packed_size
    put64(21, 8192);   // restore_used
    QFile f(win);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(hdr);
    f.write(QByteArray(4096, '\x33'));
    f.close();
    QByteArray out;
    QString err;
    QVERIFY(!imgtwrp::extractWin(win, out, &err));
    QVERIFY(err.contains("不完整"));
    QVERIFY(out.isEmpty());
}

QTEST_APPLESS_MAIN(TestTwrp)
#include "test_twrp.moc"
