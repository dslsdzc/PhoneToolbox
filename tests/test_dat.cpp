#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "image_engine/dat_image.h"

class TestDat : public QObject
{
    Q_OBJECT
private slots:
    void applyNew();
    void datInsufficient();
    void badVersionRejected();
    void badRangeRejected();
    void hugeTotalBlocksRejected();
};

// 在临时目录写入 transfer.list + .dat
static bool writeFixture(QTemporaryDir &dir, const QByteArray &tlContent,
                         const QByteArray &datContent)
{
    QFile f(dir.filePath("transfer.list"));
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(tlContent);
    f.close();
    QFile d(dir.filePath("system.new.dat"));
    if (!d.open(QIODevice::WriteOnly))
        return false;
    d.write(datContent);
    d.close();
    return true;
}

void TestDat::applyNew()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFixture(dir, QByteArray("1\n2\nnew 2 0,2\n"), QByteArray(8192, '\x77')));
    QByteArray out;
    QString err;
    QVERIFY(imgdat::sdat2img(dir.filePath("transfer.list"), dir.filePath("system.new.dat"), out, &err));
    QCOMPARE(out.size(), 8192);
    QVERIFY(out == QByteArray(8192, '\x77'));
}

// .dat 数据不足 → false + error
void TestDat::datInsufficient()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFixture(dir, QByteArray("1\n2\nnew 2 0,2\n"), QByteArray(4096, '\x77')));
    QByteArray out;
    QString err;
    QVERIFY(!imgdat::sdat2img(dir.filePath("transfer.list"), dir.filePath("system.new.dat"), out, &err));
    QVERIFY(!err.isEmpty());
}

// 版本不在 1-4 → false + error
void TestDat::badVersionRejected()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFixture(dir, QByteArray("5\n2\nnew 2 0,2\n"), QByteArray(8192, '\x77')));
    QByteArray out;
    QString err;
    QVERIFY(!imgdat::sdat2img(dir.filePath("transfer.list"), dir.filePath("system.new.dat"), out, &err));
    QVERIFY(!err.isEmpty());
}

// 坏 range / 坏行 → false + error（不再静默跳过）
void TestDat::badRangeRejected()
{
    // 奇数个逗号 token（"0,2,5" 无法配对）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFixture(dir, QByteArray("1\n2\nnew 2 0,2,5\n"), QByteArray(8192, '\x77')));
    QByteArray out;
    QString err;
    QVERIFY(!imgdat::sdat2img(dir.filePath("transfer.list"), dir.filePath("system.new.dat"), out, &err));
    QVERIFY(!err.isEmpty());
    // 缺 range 段（"cmd count" 两 token）
    QTemporaryDir dir2;
    QVERIFY(dir2.isValid());
    QVERIFY(writeFixture(dir2, QByteArray("1\n2\nnew 2\n"), QByteArray(8192, '\x77')));
    err.clear();
    QVERIFY(!imgdat::sdat2img(dir2.filePath("transfer.list"), dir2.filePath("system.new.dat"), out, &err));
    QVERIFY(!err.isEmpty());
    // range 越界（end > totalBlocks）
    QTemporaryDir dir3;
    QVERIFY(dir3.isValid());
    QVERIFY(writeFixture(dir3, QByteArray("1\n2\nnew 2 0,3\n"), QByteArray(8192, '\x77')));
    err.clear();
    QVERIFY(!imgdat::sdat2img(dir3.filePath("transfer.list"), dir3.filePath("system.new.dat"), out, &err));
    QVERIFY(!err.isEmpty());
}

// 超大/非法 totalBlocks（分配前拦截，防 int 回绕与 bad_alloc）→ false + error
void TestDat::hugeTotalBlocksRejected()
{
    // 10^9 块 * 4096 远超 int 上限（若无校验会 4TB 分配 terminate）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(writeFixture(dir, QByteArray("1\n1000000000\nnew 2 0,2\n"), QByteArray(8192, '\x77')));
    QByteArray out;
    QString err;
    QVERIFY(!imgdat::sdat2img(dir.filePath("transfer.list"), dir.filePath("system.new.dat"), out, &err));
    QVERIFY(!err.isEmpty());
    // 非数字
    QTemporaryDir dir2;
    QVERIFY(dir2.isValid());
    QVERIFY(writeFixture(dir2, QByteArray("1\nabc\n"), QByteArray()));
    err.clear();
    QVERIFY(!imgdat::sdat2img(dir2.filePath("transfer.list"), dir2.filePath("system.new.dat"), out, &err));
    QVERIFY(!err.isEmpty());
    // <=0
    QTemporaryDir dir3;
    QVERIFY(dir3.isValid());
    QVERIFY(writeFixture(dir3, QByteArray("1\n0\n"), QByteArray()));
    err.clear();
    QVERIFY(!imgdat::sdat2img(dir3.filePath("transfer.list"), dir3.filePath("system.new.dat"), out, &err));
    QVERIFY(!err.isEmpty());
}

QTEST_APPLESS_MAIN(TestDat)
#include "test_dat.moc"
