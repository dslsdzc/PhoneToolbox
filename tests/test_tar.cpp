#include <QtTest>
#include <QDir>
#include <QFile>
#include <QCryptographicHash>
#include <QTemporaryDir>
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
    void md5FooterBinaryVariant();       // ␣* 分隔符（真 MODEM 包形态）
    void md5FooterBinaryVariantRejects(); // ␣* 形态下篡改必须被拒
    // ---- Task G3: 流式接口 ----
    void streamBuildMatchesOld();       // buildTarStream 与 buildTar 逐字节一致
    void streamExtractMatchesOld();     // extractTarStream 与 extractTar 结果一致 + 穿越/符号链接防护
    void streamMd5Footer();             // appendMd5FooterStream/verifyMd5FooterStream + 解包自动校验
    void streamLegacyFooterRecognized(); // 遗留格式（>4KB 归档 + 无尾 \n 校验行）正确识别
    void streamBadInputs();             // 坏 size/截断/重名/空归档 → 不崩溃且按契约报错
    void indexTarStreamNamesOffsetsSizes();  // 名字/偏移/大小 + tarEnd
    void indexTarStreamRejectsBadInput();    // 坏 size / 截断 / 不存在
};

static bool writeFileBytes(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(data) == data.size();
}

static QByteArray readFileBytes(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

// 伪随机（确定性）数据
static QByteArray pattern(int size, int seed)
{
    QByteArray d(size, Qt::Uninitialized);
    quint32 x = quint32(seed) * 2654435761u;
    for (int i = 0; i < size; ++i) {
        x = x * 1664525u + 1013904223u;
        d[i] = char(x >> 24);
    }
    return d;
}

// 进度回调收集器：校验单调 + 首 0 + 末 total
struct ProgressProbe {
    QList<quint64> values;
    bool monotonic = true;
    void cb(quint64 v)
    {
        if (!values.isEmpty() && v < values.last())
            monotonic = false;
        values.append(v);
    }
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

// 真包实证（reference/samsung-samples/sm-j110h/MODEM_*.tar.md5 尾部逐字节）：
// 校验行分隔符是 `␣*`（md5sum 二进制模式），BL/CSC 是 `␣␣`（文本模式）。
// 只认 `␣␣` 会让 MODEM 包落到"无校验行 → 跳过校验"分支（校验静默失效）。
void TestTar::md5FooterBinaryVariant()
{
    const QByteArray tar = buildTar();
    const QByteArray hex = QCryptographicHash::hash(tar, QCryptographicHash::Md5).toHex();
    const QByteArray withFooter = tar + hex + " *" + QByteArray("MODEM_J110HDDU0AQF1.tar") + '\n';

    // 整读接口：识别 + 校验通过
    QVERIFY(imgtar::verifyMd5Footer(withFooter));

    // 流式接口：hasFooter=true（**不是**"无校验行"）+ 校验通过
    QTemporaryDir dir;
    const QString p = dir.path() + QStringLiteral("/modem.tar.md5");
    QVERIFY(writeFileBytes(p, withFooter));
    bool hasFooter = false;
    QString err;
    QVERIFY2(imgtar::verifyMd5FooterStream(p, &hasFooter, &err), qPrintable(err));
    QVERIFY(hasFooter);

    // 解包自动校验：␣* 形态同样走"校验通过 → 正常解包"（不是跳过校验）
    const QString outDir = dir.path() + QStringLiteral("/out");
    QVERIFY(QDir().mkpath(outDir));
    QVERIFY2(imgtar::extractTarStream(p, outDir, {}, &err), qPrintable(err));
    QCOMPARE(readFileBytes(outDir + QStringLiteral("/test.txt")), QByteArray("hello"));

    // 判别力：同一形态下改名/改数据 → 必须被拒（证明真的在校验，不是"识别了但没算"）
    QByteArray renamed = withFooter;
    renamed[renamed.size() - 20] = char(renamed[renamed.size() - 20] ^ 0x01); // 只改校验行里的文件名
    const QString p2 = dir.path() + QStringLiteral("/modem2.tar.md5");
    QVERIFY(writeFileBytes(p2, renamed));
    QVERIFY(imgtar::verifyMd5FooterStream(p2, &hasFooter, &err)); // 文件名不参与 MD5 → 仍通过
}

void TestTar::md5FooterBinaryVariantRejects()
{
    const QByteArray tar = buildTar();
    const QByteArray hex = QCryptographicHash::hash(tar, QCryptographicHash::Md5).toHex();
    QByteArray tampered = tar;
    tampered[100] = char(tampered[100] ^ 0x01);              // 改归档数据 → 校验行不再匹配
    const QByteArray withFooter = tampered + hex + " *" + QByteArray("MODEM.tar") + '\n';

    QVERIFY(!imgtar::verifyMd5Footer(withFooter));           // 整读接口拒绝

    QTemporaryDir dir;
    const QString p = dir.path() + QStringLiteral("/bad.tar.md5");
    QVERIFY(writeFileBytes(p, withFooter));
    bool hasFooter = false;
    QString err;
    QVERIFY(!imgtar::verifyMd5FooterStream(p, &hasFooter, &err));   // 流式接口拒绝
    QVERIFY(hasFooter);                                            // 且确实识别成了校验行
    QVERIFY(err.contains(QStringLiteral("MD5")));

    const QString outDir = dir.path() + QStringLiteral("/out");
    QVERIFY(QDir().mkpath(outDir));
    err.clear();
    QVERIFY(!imgtar::extractTarStream(p, outDir, {}, &err));        // 解包自动校验拒绝
    QVERIFY(err.contains(QStringLiteral("MD5")));
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

// ---- Task G3: 流式接口 ----

// buildTarStream 产物与旧接口 buildTar 逐字节一致（文件/目录/符号链接输入，含进度契约）
void TestTar::streamBuildMatchesOld()
{
    QTemporaryDir dir;
    const QString aPath = dir.path() + QStringLiteral("/a.bin");
    const QString bPath = dir.path() + QStringLiteral("/b.bin");
    const QString subPath = dir.path() + QStringLiteral("/sub");
    const QString lnPath = dir.path() + QStringLiteral("/ln");
    QVERIFY(writeFileBytes(aPath, pattern(3000, 1))); // 非 512 倍数
    QVERIFY(writeFileBytes(bPath, pattern(4096, 2))); // 512 倍数
    QVERIFY(QDir().mkpath(subPath));
    QVERIFY(QFile::link(aPath, lnPath));

    const QStringList files = {aPath, bPath, subPath, lnPath};
    // 旧接口对照条目（同顺序、同名、同数据）
    QList<imgtar::TarEntry> entries;
    imgtar::TarEntry a; a.name = QStringLiteral("a.bin"); a.data = pattern(3000, 1);
    entries.append(a);
    imgtar::TarEntry b; b.name = QStringLiteral("b.bin"); b.data = pattern(4096, 2);
    entries.append(b);
    imgtar::TarEntry d; d.name = QStringLiteral("sub"); d.isDir = true;
    entries.append(d);
    imgtar::TarEntry l; l.name = QStringLiteral("ln"); l.isSymlink = true;
    l.linkTarget = QFileInfo(lnPath).symLinkTarget();
    entries.append(l);
    const QByteArray oldTar = imgtar::buildTar(entries);

    ProgressProbe p;
    QString err;
    const QString outPath = dir.path() + QStringLiteral("/out.tar");
    QVERIFY2(imgtar::buildTarStream(files, outPath,
                                    [&p](quint64 v) { p.cb(v); }, &err),
             qPrintable(err));
    QCOMPARE(readFileBytes(outPath), oldTar); // 逐字节一致

    // 进度：首 0、末 3000+4096、单调
    QVERIFY(!p.values.isEmpty());
    QCOMPARE(p.values.first(), quint64(0));
    QCOMPARE(p.values.last(), quint64(7096));
    QVERIFY(p.monotonic);
}

// extractTarStream 与旧接口 extractTar 结果一致；穿越条目（.. 成分/前导 /）与符号链接不落盘
void TestTar::streamExtractMatchesOld()
{
    // 构造含各类条目的归档：正常文件（嵌套目录）、目录、符号链接、穿越条目（应跳过）
    QList<imgtar::TarEntry> entries;
    imgtar::TarEntry d; d.name = QStringLiteral("sub/dir/"); d.isDir = true;
    entries.append(d);
    imgtar::TarEntry f; f.name = QStringLiteral("sub/file.bin"); f.data = pattern(1234, 3);
    entries.append(f);
    imgtar::TarEntry l; l.name = QStringLiteral("ln"); l.isSymlink = true;
    l.linkTarget = QStringLiteral("target");
    entries.append(l);
    imgtar::TarEntry evil1; evil1.name = QStringLiteral("../evil.bin"); evil1.data = QByteArray("x");
    entries.append(evil1);
    imgtar::TarEntry evil2; evil2.name = QStringLiteral("/abs.bin"); evil2.data = QByteArray("y");
    entries.append(evil2);
    imgtar::TarEntry evil3; evil3.name = QStringLiteral("a/../../escape"); evil3.data = QByteArray("z");
    entries.append(evil3);
    imgtar::TarEntry evil4; evil4.name = QStringLiteral("a\\..\\evil.bin"); evil4.data = QByteArray("w");
    entries.append(evil4);
    imgtar::TarEntry evil5; evil5.name = QStringLiteral("C:/evil.bin"); evil5.data = QByteArray("v");
    entries.append(evil5);
    const QByteArray tar = imgtar::buildTar(entries);

    QTemporaryDir dir;
    const QString tarPath = dir.path() + QStringLiteral("/arch.tar");
    const QString outDir = dir.path() + QStringLiteral("/out");
    QVERIFY(writeFileBytes(tarPath, tar));
    QVERIFY(QDir().mkpath(outDir));

    // 旧接口对照
    QList<imgtar::TarEntry> oldOut;
    QVERIFY(imgtar::extractTar(tar, oldOut));

    ProgressProbe p;
    QString err;
    QVERIFY2(imgtar::extractTarStream(tarPath, outDir, [&p](quint64 v) { p.cb(v); }, &err),
             qPrintable(err));
    // 正常条目落盘
    QCOMPARE(readFileBytes(outDir + QStringLiteral("/sub/file.bin")), pattern(1234, 3));
    QVERIFY(QFileInfo::exists(outDir + QStringLiteral("/sub/dir")));
    // 符号链接不落盘
    QVERIFY(!QFileInfo::exists(outDir + QStringLiteral("/ln")));
    // 穿越条目不落盘（.. 解析到 outDir 外部 / outDir 内均不得出现）
    QVERIFY(!QFileInfo::exists(dir.path() + QStringLiteral("/evil.bin")));
    QVERIFY(!QFileInfo::exists(QStringLiteral("/abs.bin")));
    QVERIFY(!QFileInfo::exists(outDir + QStringLiteral("/escape")));
    QVERIFY(!QFileInfo::exists(dir.path() + QStringLiteral("/escape")));
    // 反斜杠（Windows 分隔符，a\..\ 向量）与驱动器前缀（C:/，Windows 绝对路径）条目不落盘
    QVERIFY(!QFileInfo::exists(outDir + QStringLiteral("/a\\..\\evil.bin")));
    QVERIFY(!QFileInfo::exists(outDir + QStringLiteral("/C:")));
    // 与旧接口结果一致：旧接口返回的条目名/数据与落盘文件对应
    QCOMPARE(oldOut.size(), 8);
    QCOMPARE(oldOut[1].name, QStringLiteral("sub/file.bin"));
    QCOMPARE(oldOut[1].data, pattern(1234, 3));
    // 进度：首 0、末 = 归档总字节（无校验行 → 全文件）、单调
    QVERIFY(!p.values.isEmpty());
    QCOMPARE(p.values.first(), quint64(0));
    QCOMPARE(p.values.last(), quint64(tar.size()));
    QVERIFY(p.monotonic);
}

// 三星 .tar.md5：追加/校验流式化（与整读接口逐字节一致）；解包自动校验；坏校验拒绝
void TestTar::streamMd5Footer()
{
    QList<imgtar::TarEntry> entries;
    imgtar::TarEntry f; f.name = QStringLiteral("boot.img"); f.data = pattern(5000, 4);
    entries.append(f);
    const QByteArray tar = imgtar::buildTar(entries);

    QTemporaryDir dir;
    const QString tarPath = dir.path() + QStringLiteral("/arch.tar");
    QVERIFY(writeFileBytes(tarPath, tar));

    // 无校验行：verify 返回 hasFooter=false + true
    bool hasFooter = true;
    QString err;
    QVERIFY(imgtar::verifyMd5FooterStream(tarPath, &hasFooter, &err));
    QVERIFY(!hasFooter);

    // 追加：产物与整读接口逐字节一致
    const QByteArray oldWithFooter = imgtar::appendMd5Footer(tar);
    QVERIFY(imgtar::appendMd5FooterStream(tarPath, &err));
    QCOMPARE(readFileBytes(tarPath), oldWithFooter);
    QVERIFY(imgtar::verifyMd5FooterStream(tarPath, &hasFooter, &err));
    QVERIFY(hasFooter);
    QVERIFY(imgtar::verifyMd5Footer(oldWithFooter));

    // 解包自动校验（校验行存在且匹配 → 正常解包）
    const QString outDir = dir.path() + QStringLiteral("/out");
    QVERIFY(QDir().mkpath(outDir));
    ProgressProbe p;
    QVERIFY2(imgtar::extractTarStream(tarPath, outDir, [&p](quint64 v) { p.cb(v); }, &err),
             qPrintable(err));
    QCOMPARE(readFileBytes(outDir + QStringLiteral("/boot.img")), pattern(5000, 4));
    // 进度范围 [0, 归档字节数]（不含校验行）
    QCOMPARE(p.values.last(), quint64(tar.size()));
    QVERIFY(p.monotonic);

    // 篡改归档数据 → 校验失败（解包与独立校验均拒绝）
    QByteArray tampered = readFileBytes(tarPath);
    tampered[100] = char(tampered[100] ^ 0x01);
    const QString badPath = dir.path() + QStringLiteral("/bad.tar.md5");
    QVERIFY(writeFileBytes(badPath, tampered));
    QVERIFY(!imgtar::verifyMd5FooterStream(badPath, &hasFooter, &err));
    QVERIFY(hasFooter);
    QVERIFY(!imgtar::verifyMd5Footer(tampered)); // 与整读接口一致拒绝
    const QString badOut = dir.path() + QStringLiteral("/badout");
    QVERIFY(QDir().mkpath(badOut));
    QVERIFY(!imgtar::extractTarStream(badPath, badOut, {}, &err));
    QVERIFY(err.contains(QStringLiteral("MD5")));
}

// 遗留 appendMd5Footer 产物形态（>4KB 归档 + 无尾 '\n'）: [tar]\n[32hex]  name@EOF
// —— 校验行名称延续到窗口外且无 '\n'，必须靠有界行尾确认识别，不得误判为无校验行
void TestTar::streamLegacyFooterRecognized()
{
    QList<imgtar::TarEntry> entries;
    imgtar::TarEntry f; f.name = QStringLiteral("legacy.bin"); f.data = pattern(6000, 7);
    entries.append(f);
    const QByteArray tar = imgtar::buildTar(entries);
    QVERIFY(tar.size() > 4096); // 尾部扫描窗口不覆盖全文件
    const QByteArray legacy = tar + '\n'
        + QCryptographicHash::hash(tar, QCryptographicHash::Md5).toHex()
        + QByteArray("  firmware.tar.md5"); // 无尾 '\n'
    QTemporaryDir dir;
    const QString p = dir.path() + QStringLiteral("/legacy.tar.md5");
    QVERIFY(writeFileBytes(p, legacy));

    bool hasFooter = false;
    QString err;
    QVERIFY2(imgtar::verifyMd5FooterStream(p, &hasFooter, &err), qPrintable(err));
    QVERIFY(hasFooter);
    QVERIFY(imgtar::verifyMd5Footer(legacy)); // 与整读接口识别一致
    // 解包自动识别校验行并校验通过（校验行不被当 tar 条目解析）
    const QString outDir = dir.path() + QStringLiteral("/out");
    QVERIFY(QDir().mkpath(outDir));
    QVERIFY2(imgtar::extractTarStream(p, outDir, {}, &err), qPrintable(err));
    QCOMPARE(readFileBytes(outDir + QStringLiteral("/legacy.bin")), pattern(6000, 7));
}

// 不可信输入：坏 size / 数据截断（越界拒绝，无部分产物）/ 空归档 / 重名与不存在输入 → 均 false + error，不崩溃
void TestTar::streamBadInputs()
{
    QTemporaryDir dir;

    // 坏 size 字段
    const QString badSize = dir.path() + QStringLiteral("/bad.tar");
    QVERIFY(writeFileBytes(badSize, buildTarBadSize()));
    QString e;
    QVERIFY(!imgtar::extractTarStream(badSize, dir.path(), {}, &e));
    QVERIFY(!e.isEmpty());

    // 数据截断（条目数据中部）：越界检查在打开输出文件前即拒绝 → 不产生部分产物文件。
    // （"部分产物删除"防御针对写盘中途 IO 失败：磁盘满/文件被并发修改，稳定文件下不可构造）
    QList<imgtar::TarEntry> entries;
    imgtar::TarEntry f; f.name = QStringLiteral("big.bin"); f.data = pattern(100000, 5);
    entries.append(f);
    QByteArray tar = imgtar::buildTar(entries);
    const QString truncated = dir.path() + QStringLiteral("/trunc.tar");
    QVERIFY(writeFileBytes(truncated, tar.left(tar.size() - 60000))); // 截掉大半数据
    e.clear();
    QVERIFY(!imgtar::extractTarStream(truncated, dir.path(), {}, &e));
    QVERIFY(e.contains(QStringLiteral("截断")));
    QVERIFY(!QFileInfo::exists(dir.path() + QStringLiteral("/big.bin"))); // 拒绝先于写盘，无部分产物

    // 空归档（1024 零块）：成功解包、无产物；进度末 = 1024
    const QString emptyTar = dir.path() + QStringLiteral("/empty.tar");
    QVERIFY(writeFileBytes(emptyTar, QByteArray(1024, 0)));
    ProgressProbe p;
    e.clear();
    QVERIFY2(imgtar::extractTarStream(emptyTar, dir.path(), [&p](quint64 v) { p.cb(v); }, &e),
             qPrintable(e));
    QCOMPARE(p.values.last(), quint64(1024));

    // buildTarStream 重名条目拒绝
    QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/x")));
    QVERIFY(QDir().mkpath(dir.path() + QStringLiteral("/y")));
    QVERIFY(writeFileBytes(dir.path() + QStringLiteral("/x/dup.bin"), QByteArray("a")));
    QVERIFY(writeFileBytes(dir.path() + QStringLiteral("/y/dup.bin"), QByteArray("b")));
    e.clear();
    QVERIFY(!imgtar::buildTarStream({dir.path() + QStringLiteral("/x/dup.bin"),
                                     dir.path() + QStringLiteral("/y/dup.bin")},
                                    dir.path() + QStringLiteral("/dup.tar"), {}, &e));
    QVERIFY(!e.isEmpty());
    QVERIFY(!QFileInfo::exists(dir.path() + QStringLiteral("/dup.tar")));

    // 输入不存在
    e.clear();
    QVERIFY(!imgtar::buildTarStream({dir.path() + QStringLiteral("/nope.bin")},
                                    dir.path() + QStringLiteral("/nope.tar"), {}, &e));
    QVERIFY(!e.isEmpty());
}

// 流式索引：名字 → (数据区绝对偏移, 字节数)；tarEnd = 归档区结束（不含 .tar.md5 校验行）
void TestTar::indexTarStreamNamesOffsetsSizes()
{
    QList<imgtar::TarEntry> in;
    imgtar::TarEntry a; a.name = QStringLiteral("spl.img");   a.data = pattern(3000, 11);
    imgtar::TarEntry d; d.name = QStringLiteral("sub/");      d.isDir = true;
    imgtar::TarEntry b; b.name = QStringLiteral("sboot.bin"); b.data = pattern(512, 12); // 512 整数倍
    in << a << d << b;
    const QByteArray tar = imgtar::buildTar(in);
    const QByteArray withFooter = imgtar::appendMd5Footer(tar);

    QTemporaryDir dir;
    const QString p = dir.path() + QStringLiteral("/idx.tar.md5");
    QVERIFY(writeFileBytes(p, withFooter));

    QList<imgtar::TarIndexEntry> idx;
    quint64 tarEnd = 0;
    QString err;
    QVERIFY2(imgtar::indexTarStream(p, idx, &tarEnd, &err), qPrintable(err));
    QCOMPARE(idx.size(), 3);
    QCOMPARE(idx[0].name, QStringLiteral("spl.img"));
    QCOMPARE(idx[0].size, quint64(3000));
    QCOMPARE(idx[0].isDir, false);
    QCOMPARE(idx[1].name, QStringLiteral("sub"));
    QVERIFY(idx[1].isDir);
    QCOMPARE(idx[2].name, QStringLiteral("sboot.bin"));
    QCOMPARE(idx[2].size, quint64(512));
    // 偏移自洽：按偏移读回文件，内容与构造一致（证明偏移是"数据区起点"而不是"头块起点"）
    QCOMPARE(readFileBytes(p).mid(int(idx[0].offset), 3000), pattern(3000, 11));
    QCOMPARE(readFileBytes(p).mid(int(idx[2].offset), 512), pattern(512, 12));
    // tarEnd = 归档区结束（不含校验行）→ 其前 1024 字节是两个空块
    QCOMPARE(tarEnd, quint64(tar.size()));
    QCOMPARE(readFileBytes(p).mid(int(tarEnd) - 1024, 1024), QByteArray(1024, 0));

    // 无校验行的裸 tar：tarEnd = 文件大小
    const QString p2 = dir.path() + QStringLiteral("/idx.tar");
    QVERIFY(writeFileBytes(p2, tar));
    QList<imgtar::TarIndexEntry> idx2;
    quint64 tarEnd2 = 0;
    QVERIFY2(imgtar::indexTarStream(p2, idx2, &tarEnd2, &err), qPrintable(err));
    QCOMPARE(idx2.size(), 3);
    QCOMPARE(tarEnd2, quint64(tar.size()));

    // 校验行**不符**的包：索引照建（位置已定），完整性判定归 verifyMd5FooterStream ——
    // 这条分离是"用户刷改包"不被索引层拦死的前提（计划层报 verifyOk=false，不拒刷）
    QByteArray tampered = tar;
    tampered[600] = char(tampered[600] ^ 0x01);
    const QByteArray bad = tampered + QCryptographicHash::hash(tar, QCryptographicHash::Md5).toHex()
                           + QByteArray("  bad.tar\n");     // 校验行按**未篡改**数据算 → 必然不符
    const QString p3 = dir.path() + QStringLiteral("/bad.tar.md5");
    QVERIFY(writeFileBytes(p3, bad));
    QList<imgtar::TarIndexEntry> idx3;
    quint64 tarEnd3 = 0;
    QVERIFY2(imgtar::indexTarStream(p3, idx3, &tarEnd3, &err), qPrintable(err));
    QVERIFY2(err.isEmpty(), "索引成功但 error 残留（契约：error 仅在返回 false 时有意义）");
    QCOMPARE(idx3.size(), 3);
    QCOMPARE(tarEnd3, quint64(tar.size()));
    // 而完整性检查必须报"不符"
    bool hasFooter = false;
    QVERIFY(!imgtar::verifyMd5FooterStream(p3, &hasFooter, &err));
    QVERIFY(hasFooter);
}

void TestTar::indexTarStreamRejectsBadInput()
{
    QTemporaryDir dir;
    QList<imgtar::TarIndexEntry> idx;
    quint64 tarEnd = 0;
    QString err;

    // 不存在
    QVERIFY(!imgtar::indexTarStream(dir.path() + QStringLiteral("/nope.tar"), idx, &tarEnd, &err));
    QVERIFY(!err.isEmpty());

    // size 非八进制
    const QString bad = dir.path() + QStringLiteral("/bad.tar");
    QVERIFY(writeFileBytes(bad, buildTarBadSize()));
    err.clear();
    QVERIFY(!imgtar::indexTarStream(bad, idx, &tarEnd, &err));
    QVERIFY(!err.isEmpty());

    // 数据区越界（截断）
    // 注: 声明数据区必须大于被截掉的 88 字节，否则 600 字节截断仍完整包含数据区（只丢尾部补零/结束块，
    //     与 extractTar/extractTarStream 一致地容忍）—— 用声明 3000 字节的条目复现"数据区中部截断"。
    QList<imgtar::TarEntry> big;
    imgtar::TarEntry f; f.name = QStringLiteral("big.bin"); f.data = pattern(3000, 13);
    big << f;
    const QByteArray whole = imgtar::buildTar(big);
    const QString trunc = dir.path() + QStringLiteral("/trunc.tar");
    QVERIFY(writeFileBytes(trunc, whole.left(600)));         // 头块 512 + 88 字节数据
    err.clear();
    QVERIFY(!imgtar::indexTarStream(trunc, idx, &tarEnd, &err));
    QVERIFY(!err.isEmpty());
}

QTEST_APPLESS_MAIN(TestTar)
#include "test_tar.moc"
