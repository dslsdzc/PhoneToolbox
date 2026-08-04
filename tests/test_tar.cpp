#include <QtTest>
#include "image_engine/tar_image.h"

class TestTar : public QObject
{
    Q_OBJECT
private slots:
    void extractSimple();
    void emptyArchive();
    void md5Footer();
    void buildRoundTrip();              // Task 11: 打包往返
    void md5FooterWithTrailingNewline(); // 真实三星 .tar.md5 带尾 \n
    void extractDirSymlink();           // dir/symlink 分支 + linkTarget
    void badSizeRejected();             // 坏 size → false
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

// 构造 ustar: "subdir/" 目录项 + "link" 符号链接(linkTarget = "boot.img")
static QByteArray buildTarDirSymlink()
{
    QByteArray hdr(512, 0);
    hdr.replace(0, 7, "subdir/");
    hdr.replace(100, 8, octalField(0644, 8));
    hdr.replace(124, 12, octalField(0, 12));
    hdr[156] = '5';                          // 目录
    hdr.replace(257, 6, QByteArray("ustar\0", 6));
    hdr.replace(263, 2, "00");

    QByteArray h2(512, 0);
    h2.replace(0, 4, "link");
    h2.replace(100, 8, octalField(0777, 8));
    h2.replace(124, 12, octalField(0, 12));
    h2[156] = '2';                           // 符号链接
    h2.replace(157, 8, "boot.img");          // linkTarget
    h2.replace(257, 6, QByteArray("ustar\0", 6));
    h2.replace(263, 2, "00");
    return hdr + h2 + QByteArray(1024, 0);
}

// 构造 ustar: size 字段为非八进制文本 → 解析必须失败
static QByteArray buildTarBadSize()
{
    QByteArray hdr(512, 0);
    hdr.replace(0, 7, "bad.bin");
    hdr.replace(100, 8, octalField(0644, 8));
    hdr.replace(124, 12, QByteArray("notanumber!!")); // 12 字符非八进制
    hdr[156] = '0';
    hdr.replace(257, 6, QByteArray("ustar\0", 6));
    hdr.replace(263, 2, "00");
    return hdr + QByteArray(1024, 0);
}

void TestTar::buildRoundTrip()
{
    QList<imgtar::TarEntry> in;
    imgtar::TarEntry f; f.name = "boot.img"; f.data = QByteArray(10000, 'B');
    in.append(f);
    imgtar::TarEntry d; d.name = "subdir/"; d.isDir = true;
    in.append(d);
    QByteArray tar = imgtar::buildTar(in);
    QList<imgtar::TarEntry> out;
    QVERIFY(imgtar::extractTar(tar, out));
    QCOMPARE(out.size(), 2);
    QCOMPARE(out[0].name, "boot.img");
    QCOMPARE(out[0].data, QByteArray(10000, 'B'));
    QVERIFY(out[1].isDir);
}

void TestTar::md5FooterWithTrailingNewline()
{
    // 真实三星 .tar.md5 格式: [tar][32hex]  name\n (校验行后带尾 \n)
    QByteArray tar = buildTar();
    QByteArray withFooter = imgtar::appendMd5Footer(tar);
    QVERIFY(imgtar::verifyMd5Footer(withFooter + '\n'));
}

void TestTar::extractDirSymlink()
{
    QList<imgtar::TarEntry> entries;
    QVERIFY(imgtar::extractTar(buildTarDirSymlink(), entries));
    QCOMPARE(entries.size(), 2);
    QVERIFY(entries[0].isDir);
    QCOMPARE(entries[0].name, "subdir/");
    QVERIFY(entries[1].isSymlink);
    QCOMPARE(entries[1].linkTarget, "boot.img");
}

void TestTar::badSizeRejected()
{
    QList<imgtar::TarEntry> entries;
    QVERIFY(!imgtar::extractTar(buildTarBadSize(), entries));
}

QTEST_APPLESS_MAIN(TestTar)
#include "test_tar.moc"
