#include <QtTest>
#include "image_engine/tar_image.h"

class TestTar : public QObject
{
    Q_OBJECT
private slots:
    void extractSimple();
    void emptyArchive();
    void md5Footer();
};

static QByteArray octalField(int size, int fieldLen)
{
    QByteArray s = QByteArray::number(size, 8).rightJustified(fieldLen - 1, '0');
    return s + ' ';
}

// 构造 ustar: "test.txt" 内容 "hello"
static QByteArray buildTar()
{
    QByteArray hdr(512, 0);
    hdr.replace(0, 8, "test.txt");
    hdr.replace(100, 8, octalField(0644, 8));
    hdr.replace(108, 8, octalField(0, 8));   // uid
    hdr.replace(116, 8, octalField(0, 8));   // gid
    hdr.replace(124, 12, octalField(5, 12)); // size
    hdr.replace(136, 12, octalField(0, 12)); // mtime
    hdr.replace(148, 8, "        ");         // chksum 占位（0）
    hdr[156] = '0';                          // typeflag
    hdr.replace(257, 6, QByteArray("ustar\0", 6)); // 显式长度：const char* 重载会按 strlen 截断并缩短 QByteArray
    hdr.replace(263, 2, "00");
    QByteArray tar = hdr + QByteArray("hello") + QByteArray(507, 0);
    tar.append(QByteArray(1024, 0)); // 两个空块结尾
    return tar;
}

void TestTar::extractSimple()
{
    QList<imgtar::TarEntry> entries;
    QVERIFY(imgtar::extractTar(buildTar(), entries));
    QCOMPARE(entries.size(), 1);
    QCOMPARE(entries[0].name, "test.txt");
    QCOMPARE(entries[0].data, QByteArray("hello"));
}

void TestTar::emptyArchive()
{
    QList<imgtar::TarEntry> entries;
    QVERIFY(imgtar::extractTar(QByteArray(1024, 0), entries));
    QVERIFY(entries.isEmpty());
}

void TestTar::md5Footer()
{
    QByteArray tar = buildTar();
    QByteArray withFooter = imgtar::appendMd5Footer(tar);
    QVERIFY(withFooter.size() > tar.size());
    QVERIFY(imgtar::verifyMd5Footer(withFooter));
    QVERIFY(!imgtar::verifyMd5Footer(tar));
}

QTEST_APPLESS_MAIN(TestTar)
#include "test_tar.moc"
