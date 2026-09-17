// tests/test_eub_payload.cpp
//
// 载荷来源三条路径（spec §D8）：裸镜像 / LZ4 frame / BL tar 内查找。
// 本线无真机（真样本核对见文件末尾的 gated 槽，样本在 reference/eub-samples/，facts §H）：
// 前半段用自造合成 sboot（确定性非周期图案）+ imgtar::buildTar / appendMd5Footer
// 造包，不碰设备；后半段的"真样本硬断言"读 reference/eub-samples/ 下的官方 BL 包（**gitignored**，
// 样本本身绝不进仓库）—— 目录缺失时按 gating 策略 QSKIP/FAIL，见文件头的 EUB_SKIP_OR_FAIL。
// sboot.bin 是三星签名二进制，本仓不**分发**它（facts §F8）；这些槽是"手上有官方包时"的回归。
//
// 参照事实：BL_*.tar.md5 内是 sboot.bin（现代包为 .lz4 压缩，需 lz4.frame.decompress）
// —— facts §C1（reference/hubble/hubble.py:152-183）；本仓 lz4 能力为 LZ4 **frame** 格式
// （src/image_engine/compression/lz4_wrapper.cpp，用 LZ4F_* API）。
#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <utility>

#include "core/eub/eub_loadout.h"   // eub::sha1Hex（真样本槽与 §H2 记录对拍用）
#include "core/eub/eub_payload.h"
#include "eub_test_helpers.h"   // 真样本 gating（共享）：EUB_SAMPLES_DIR / EUB_SKIP_OR_FAIL / eubtest::*
#include "image_engine/compression/lz4_wrapper.h"
#include "image_engine/tar_image.h"


class TestEubPayload : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString writeFile(const QString &name, const QByteArray &bytes)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly)) qFatal("无法写临时文件");
        f.write(bytes);
        f.close();
        return path;
    }
    static QByteArray syntheticSboot()
    {
        // 非周期填充（xorshift32）：`(i * k) & 0xFF` 仍是 256 周期图案，会让"偏移错 256 的整数倍"漏检
        //（T4 实现者的变异证据：brief 原夹具下 offset 错 0x100 时 split 槽仍全绿）
        QByteArray b(0x8000, '\0');
        quint32 x = 0x12345678u;
        for (int i = 0; i < b.size(); ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b[i] = char(x & 0xFF); }
        return b;
    }
    // 同长度、异图案的第二份载荷（用于"取错条目/取错来源"能被 QCOMPARE 立刻判红）
    static QByteArray otherSyntheticSboot()
    {
        QByteArray b = syntheticSboot();
        for (int i = 0; i < b.size(); ++i) b[i] = char(b[i] ^ 0x5A);
        return b;
    }
    // 单条目 tar 的快捷封装
    static QByteArray tarOf(const QString &entryName, const QByteArray &data, bool md5Footer = false)
    {
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry e; e.name = entryName; e.data = data;
        entries << e;
        const QByteArray tar = imgtar::buildTar(entries);
        return md5Footer ? imgtar::appendMd5Footer(tar) : tar;
    }

private slots:
    void rawImageIsLoadedVerbatim()
    {
        const QByteArray img = syntheticSboot();
        // 夹具自检：裸路径槽必须真的走裸路径（带 LZ4 魔数的"裸镜像"是另一条分支）
        QVERIFY(!eub::looksLikeLz4Frame(img));
        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("sboot.bin", img), out, &src, &err), qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(!src.wasCompressed);
        QVERIFY(src.description.contains(QStringLiteral("sboot.bin")));
        // "裸镜像"三字不在路径里 —— 本断言真正约束描述文案（仅 contains(文件名) 会被"原样回显路径"骗过）
        QVERIFY(src.description.contains(QStringLiteral("裸镜像")));
    }

    void lz4FrameImageIsDecompressed()
    {
        const QByteArray img = syntheticSboot();
        const QByteArray packed = imgcomp::lz4Compress(img);
        QVERIFY(!packed.isEmpty());
        QVERIFY(eub::looksLikeLz4Frame(packed));

        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("sboot.bin.lz4", packed), out, &src, &err), qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(src.wasCompressed);
        QVERIFY(src.description.contains(QStringLiteral("sboot.bin.lz4")));
        QVERIFY(src.description.contains(QStringLiteral("LZ4")));   // 同理：路径含 ".lz4" 但不含大写 "LZ4"
    }

    void tarMd5WithPlainSbootIsExtracted()
    {
        const QByteArray img = syntheticSboot();
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry e; e.name = QStringLiteral("sboot.bin"); e.data = img;
        imgtar::TarEntry other; other.name = QStringLiteral("param.bin"); other.data = QByteArray(16, '\x11');
        entries << e << other;
        const QByteArray tar = imgtar::appendMd5Footer(imgtar::buildTar(entries));

        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("BL_TEST.tar.md5", tar), out, &src, &err), qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(!src.wasCompressed);
        // 两条互补判据（原 `contains("tar")` 恒真已替换，T5 审查 Minor 3；改"删除"为"加固"是复审意见）：
        //   ① 条目名出现在描述里 —— 实现若退化成"裸镜像"描述（只回显文件名）即红
        //   ② 模板结构（"<包名> 内的 <条目名>"）在 —— 只回显条目名、不回显包名的实现即红
        // 变异证据（控制方实跑）：把 eub_payload.cpp:187 的 describe(fileName, entryInTar, ...)
        // 改成 describe(fileName, QString(), ...) → 夹在本 slot 的 ① 上变红（13 passed / 1 failed），
        // 其余 13 条（含 raw/lz4 两个同型槽：它们的文件名本就含条目名）全绿，与推演吻合。
        QVERIFY(src.description.contains(QStringLiteral("sboot.bin")));
        QVERIFY(src.description.contains(QStringLiteral("内的")));
    }

    void tarWithLz4SbootIsExtractedThenDecompressed()
    {
        const QByteArray img = syntheticSboot();
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry e; e.name = QStringLiteral("sboot.bin.lz4"); e.data = imgcomp::lz4Compress(img);
        entries << e;
        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("BL_LZ4.tar", imgtar::buildTar(entries)), out, &src, &err),
                 qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(src.wasCompressed);
    }

    void tarWithoutSbootFailsAndListsWhatItHas()
    {
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry a; a.name = QStringLiteral("boot.img");   a.data = QByteArray(8, '\x22');
        imgtar::TarEntry b; b.name = QStringLiteral("tzsw.img");   b.data = QByteArray(8, '\x33');
        entries << a << b;

        eub::SbootSource src;
        QByteArray out = QByteArrayLiteral("stale");   // 失败路径必须清空 out（仓内约定），预置陈旧字节才有断言力
        QString err;
        QVERIFY(!eub::loadSbootBytes(writeFile("BL_NO.tar", imgtar::buildTar(entries)), out, &src, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(out.isEmpty());
        QVERIFY(err.contains(QStringLiteral("boot.img")));   // 文案列出包内有什么（可行动）
    }

    void tarEntryIsMatchedByBasenameCaseInsensitively()
    {
        // 要点②：按 basename（最后一段 '/' 之后）大小写不敏感匹配。带目录前缀 + 全大写的条目
        // 仍须命中 —— 大小写敏感或整串比较的实现会在此判红（brief 原用例无覆盖）。
        const QByteArray img = otherSyntheticSboot();
        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("BL_SUB.tar.md5",
                                               tarOf(QStringLiteral("firmware/SBOOT.BIN"), img, true)),
                                     out, &src, &err), qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(!src.wasCompressed);
    }

    void tarPrefersPlainSbootOverLz4()
    {
        // 要点②：**先找 sboot.bin，没有才找 sboot.bin.lz4**。两条目并存时取错就会解出
        // otherSyntheticSboot() 的图案 → QCOMPARE 判红。.lz4 条目**放在前面**：条目顺序
        // 不能成为实现的依据。
        const QByteArray plain = syntheticSboot();
        const QByteArray viaLz4 = otherSyntheticSboot();
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry lz4e;   lz4e.name = QStringLiteral("sboot.bin.lz4"); lz4e.data = imgcomp::lz4Compress(viaLz4);
        imgtar::TarEntry plainE; plainE.name = QStringLiteral("sboot.bin");   plainE.data = plain;
        entries << lz4e << plainE;

        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("BL_BOTH.tar", imgtar::buildTar(entries)), out, &src, &err),
                 qPrintable(err));
        QCOMPARE(out, plain);
        QVERIFY(!src.wasCompressed);
    }

    void tarEntryNamedLz4WithPlainContentStaysVerbatim()
    {
        // 要点③：解压判据是**内容**（LZ4 frame 魔数），不是条目扩展名 —— 名字骗人时按裸字节收下。
        // 若实现按扩展名解压，lz4Decompress 对非 frame 数据返回空 → 本槽会以"解压失败"判红。
        const QByteArray img = syntheticSboot();
        QVERIFY(!eub::looksLikeLz4Frame(img));
        eub::SbootSource src;
        QByteArray out;
        QString err;
        QVERIFY2(eub::loadSbootBytes(writeFile("BL_MISNAME.tar", tarOf(QStringLiteral("sboot.bin.lz4"), img)),
                                     out, &src, &err), qPrintable(err));
        QCOMPARE(out, img);
        QVERIFY(!src.wasCompressed);
    }

    void tarIndexFailureIsNotFallenBackToRaw()
    {
        // 要点①：`indexTarStream` 失败 → **直接报错，不回退裸镜像路径**。把首条目的 size 字段
        // （偏移 124，base-8）破坏成非八进制 → 索引报"size 字段非法"。若实现回退成"整文件读作
        // 裸镜像"，本槽会拿到整个 tar 字节（非空）且返回 true → 两条断言同时判红。
        const QByteArray img = syntheticSboot();
        QByteArray tar = tarOf(QStringLiteral("sboot.bin"), img);
        tar.replace(124, 12, QByteArrayLiteral("zzzzzzzzzzzz"));

        eub::SbootSource src;
        QByteArray out = QByteArrayLiteral("stale");
        QString err;
        QVERIFY(!eub::loadSbootBytes(writeFile("BL_BROKEN.tar", tar), out, &src, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(out.isEmpty());
    }

    void corruptLz4Fails()
    {
        QByteArray junk = QByteArray::fromHex("04224d18") + QByteArray(64, '\x77');  // 魔数对、内容坏
        eub::SbootSource src;
        QByteArray out = QByteArrayLiteral("stale");
        QString err;
        QVERIFY(!eub::loadSbootBytes(writeFile("bad.lz4", junk), out, &src, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(out.isEmpty());
    }

    void emptyInputFails()
    {
        eub::SbootSource src;
        QByteArray out = QByteArrayLiteral("stale");
        QString err;
        QVERIFY(!eub::loadSbootBytes(writeFile("empty.bin", QByteArray()), out, &src, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(out.isEmpty());

        out = QByteArrayLiteral("stale");
        err.clear();
        QVERIFY(!eub::loadSbootBytes(m_dir.filePath("does-not-exist.bin"), out, &src, &err));
        QVERIFY(!err.isEmpty());   // 打不开的文件也要给中文原因（含路径），不能只返回 false
        QVERIFY(out.isEmpty());
    }

    void lz4MagicDetection()
    {
        QVERIFY(!eub::looksLikeLz4Frame(QByteArray::fromHex("04224d")));    // 太短
        QVERIFY(!eub::looksLikeLz4Frame(QByteArray::fromHex("00000000")));
        QVERIFY(eub::looksLikeLz4Frame(QByteArray::fromHex("04224d18" "00")));
    }

    void looksLikeTarMatchesMagicOrSuffix()
    {
        // "是不是 tar"的判据收口（Task 2 复审）：三份同义表达式（载荷层 / 对话框层 / 索引层）
        // 合并为这一处纯函数。两条判据各自独立成立：
        //   ① 魔数：257..261 为 "ustar"（与 image_engine/tar_image.cpp:631 同源）；
        //   ② 后缀：.tar / .tar.md5，**大小写不敏感**（包名在各来源里大小写混写）。
        QByteArray head(512, '\0');
        head.replace(257, 5, QByteArrayLiteral("ustar"));
        QVERIFY2(eub::looksLikeTar(head, QStringLiteral("no-suffix")),
                 "只有魔数（无后缀）也必须命中 —— 后缀是补充判据，不是前提");
        // 魔数错位 / 头不足 262 字节 → 不命中（短头不许越界，也不许按"像 tar"收下）
        QByteArray shifted(512, '\0');
        shifted.replace(258, 5, QByteArrayLiteral("ustar"));
        QVERIFY(!eub::looksLikeTar(shifted, QStringLiteral("no-suffix")));
        QVERIFY(!eub::looksLikeTar(QByteArrayLiteral("ustar"), QStringLiteral("no-suffix")));

        // 后缀判据（内容给空：命中只能来自后缀）
        QVERIFY(eub::looksLikeTar(QByteArray(), QStringLiteral("BL_X.tar")));
        QVERIFY(eub::looksLikeTar(QByteArray(), QStringLiteral("BL_X.tar.md5")));
        QVERIFY(eub::looksLikeTar(QByteArray(), QStringLiteral("BL_X.TAR")));        // 大小写不敏感
        QVERIFY(eub::looksLikeTar(QByteArray(), QStringLiteral("BL_X.Tar.MD5")));

        // 都不像 → 不命中（裸镜像路径：sboot.bin / sboot.bin.lz4 是**正常**来源，不是 tar）
        QVERIFY(!eub::looksLikeTar(QByteArray(512, '\0'), QStringLiteral("sboot.bin")));
        QVERIFY(!eub::looksLikeTar(QByteArray(512, '\0'), QStringLiteral("sboot.bin.lz4")));
        QVERIFY2(!eub::looksLikeTar(QByteArray(), QStringLiteral("sboot.tar.bin")),
                 "后缀必须落在**末尾**：名字中间出现 .tar 不算 tar");
    }

    // ---- loadNamedEntriesFromTar：按名取多条（backlog Task 1，9830 的 extraFiles 用） ----
    // 事实出处：hubble.py:152-185（BL tar 全条目解出后逐个尝试 lz4 解压）、Exynos9830.json:3
    // （files_to_send = ldfw.img / tzsw.img）。本槽的包与载荷都是**合成**的（不依赖真样本）；
    // 真样本上的同名核对在同文件末尾的 gated 槽。

    void namedEntriesAreExtractedInRequestedOrder()
    {
        // 两条目：一条裸文件、一条 .lz4（**解压后**比对 —— 不解压就会拿 .lz4 字节比，必红）。
        const QByteArray ldfw = syntheticSboot();
        const QByteArray tzsw = otherSyntheticSboot();
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry lz4e; lz4e.name = QStringLiteral("tzsw.img.lz4");
        lz4e.data = imgcomp::lz4Compress(tzsw);
        imgtar::TarEntry plainE; plainE.name = QStringLiteral("ldfw.img"); plainE.data = ldfw;
        imgtar::TarEntry decoy; decoy.name = QStringLiteral("sboot.bin"); decoy.data = QByteArray(32, '\x5A');
        entries << lz4e << plainE << decoy;   // 包内顺序与请求顺序**不同**：实现不能拿条目顺序顶替
        const QString path = writeFile("BL_NAMED.tar.md5", imgtar::buildTar(entries));

        QList<QByteArray> out;
        QString err;
        QVERIFY2(eub::loadNamedEntriesFromTar(path,
                                              {QStringLiteral("ldfw.img"), QStringLiteral("tzsw.img")},
                                              out, &err), qPrintable(err));
        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0], ldfw);
        QCOMPARE(out[1], tzsw);
        QVERIFY2(err.isEmpty(), qPrintable(err));   // 成功路径不留上一次的错误文本（与 loadSbootBytes 同约定）

        // 顺序 = **baseNames 顺序**（把请求反过来，结果也要反过来）：按包内条目顺序返回的实现二红
        QList<QByteArray> rev;
        QVERIFY2(eub::loadNamedEntriesFromTar(path,
                                              {QStringLiteral("tzsw.img"), QStringLiteral("ldfw.img")},
                                              rev, &err), qPrintable(err));
        QCOMPARE(rev.size(), 2);
        QCOMPARE(rev[0], tzsw);
        QCOMPARE(rev[1], ldfw);
    }

    void namedEntryMissingFailsAndListsMissingNamePlusPackageContents()
    {
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry a; a.name = QStringLiteral("ldfw.img");  a.data = syntheticSboot();
        imgtar::TarEntry b; b.name = QStringLiteral("param.bin"); b.data = QByteArray(8, '\x11');
        entries << a << b;

        QList<QByteArray> out;
        out << QByteArrayLiteral("stale");        // 失败路径必须清空（仓内约定），预置陈旧字节才有断言力
        QString err;
        QVERIFY(!eub::loadNamedEntriesFromTar(writeFile("BL_PART.tar", imgtar::buildTar(entries)),
                                              {QStringLiteral("ldfw.img"), QStringLiteral("tzsw.img")},
                                              out, &err));
        QVERIFY2(err.contains(QStringLiteral("tzsw.img")), qPrintable(err));   // 缺哪个要说清
        QVERIFY2(err.contains(QStringLiteral("param.bin")), qPrintable(err));  // 包内有什么（可行动）
        QVERIFY(out.isEmpty());
    }

    void namedEntryPrefersPlainOverLz4()
    {
        const QByteArray plain = syntheticSboot();
        const QByteArray viaLz4 = otherSyntheticSboot();
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry lz4e;   lz4e.name = QStringLiteral("ldfw.img.lz4"); lz4e.data = imgcomp::lz4Compress(viaLz4);
        imgtar::TarEntry plainE; plainE.name = QStringLiteral("ldfw.img");   plainE.data = plain;
        entries << lz4e << plainE;               // .lz4 在前：条目顺序不能成为实现的依据

        QList<QByteArray> out;
        QString err;
        QVERIFY2(eub::loadNamedEntriesFromTar(writeFile("BL_BOTH_NAMED.tar", imgtar::buildTar(entries)),
                                              {QStringLiteral("ldfw.img")}, out, &err), qPrintable(err));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0], plain);
    }

    void namedEntryIsMatchedByBasenameCaseInsensitively()
    {
        const QByteArray img = otherSyntheticSboot();
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry e; e.name = QStringLiteral("firmware/LDFW.IMG"); e.data = img;
        entries << e;

        QList<QByteArray> out;
        QString err;
        QVERIFY2(eub::loadNamedEntriesFromTar(writeFile("BL_SUB_NAMED.tar", imgtar::buildTar(entries)),
                                              {QStringLiteral("ldfw.img")}, out, &err), qPrintable(err));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0], img);
    }

    void namedEntryWithLz4ExtensionButPlainContentStaysVerbatim()
    {
        // 解压判据是**内容**（LZ4 frame 魔数），不是条目名后缀 —— 与 loadSbootBytes 同一规则。
        // 这里走的是"先找 tzsw.img、没有才找 tzsw.img.lz4"的**后半条**名字：命中 .lz4 条目但内容是裸字节。
        const QByteArray img = syntheticSboot();
        QVERIFY(!eub::looksLikeLz4Frame(img));
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry e; e.name = QStringLiteral("tzsw.img.lz4"); e.data = img;
        entries << e;

        QList<QByteArray> out;
        QString err;
        QVERIFY2(eub::loadNamedEntriesFromTar(writeFile("BL_MISNAME_NAMED.tar", imgtar::buildTar(entries)),
                                              {QStringLiteral("tzsw.img")}, out, &err), qPrintable(err));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0], img);
    }

    void namedEntriesEmptyRequestSucceedsWithoutTouchingTheFile()
    {
        // 空请求 = 没有要求任何条目 → 成功、出参清空，**且不碰文件**：传一个不存在的路径，
        // 若实现仍去 open/索引它，这里就会以"无法打开"判红。
        QList<QByteArray> out;
        out << QByteArrayLiteral("stale");
        QString err;
        QVERIFY2(eub::loadNamedEntriesFromTar(m_dir.filePath("does-not-exist.tar"), {}, out, &err),
                 qPrintable(err));
        QVERIFY(out.isEmpty());
    }

    // ---- 真样本硬断言（reference/eub-samples/，gitignored；缺失时 QSKIP/FAIL）----
    // 事实出处：docs/superpowers/specs/exynos-eub-facts.md §H（5 个官方 BL 包的核对结果）。
    // 只读样本、绝不写回；样本内容不进仓库（CMake 只把**目录路径**编进本目标）。

    void real9830BlPackageYieldsExtraFilesAndSboot()
    {
        // 9830 的 extraFiles 在真包里叫 ldfw.img.lz4 / tzsw.img.lz4（facts §H2）—— 裸名**不存在**，
        // 故本槽跑通即证明实现确实走了"先找同名、没有才同名 +.lz4"的**回退**分支（而不是碰巧命中）。
        const QString pkg = QStringLiteral("BL_SM-G980F_G980FXXSNHYB1.tar.md5");   // SM-G980F = Exynos9830
        if (!eubtest::sampleAvailable(pkg)) {
            EUB_SKIP_OR_FAIL(pkg);
        }
        const QString tar = eubtest::samplePath(pkg);

        // 夹具自检：先钉死真包**就是** lz4 包裹的形态 —— 否则下面的"回退"断言是空转。
        // ⚠️ 诚实边界（1b 审查 M4）：本自检用的是**被测同一原语**（imgtar::indexTarStream，见 eub_payload.cpp 的同名调用），
        //    因此它只证明"该包的形态符合预期"，**不独立于被测实现** —— 真包条目名的独立核对在
        //    .superpowers/sdd/eub-real-samples-verification.md（用 tar(1)/lz4(1) 自写工具，未用本仓代码）。
        QList<imgtar::TarIndexEntry> idx;
        QString idxErr;
        QVERIFY2(imgtar::indexTarStream(tar, idx, nullptr, &idxErr), qPrintable(idxErr));
        QStringList names;
        for (const imgtar::TarIndexEntry &e : std::as_const(idx))
            names << e.name;
        const QString listing = names.join(QStringLiteral("、"));
        QVERIFY2(names.contains(QStringLiteral("ldfw.img.lz4")), qPrintable(listing));
        QVERIFY2(names.contains(QStringLiteral("tzsw.img.lz4")), qPrintable(listing));
        QVERIFY2(names.contains(QStringLiteral("sboot.bin.lz4")), qPrintable(listing));
        QVERIFY2(!names.contains(QStringLiteral("ldfw.img")), qPrintable(listing));
        QVERIFY2(!names.contains(QStringLiteral("tzsw.img")), qPrintable(listing));
        QVERIFY2(!names.contains(QStringLiteral("sboot.bin")), qPrintable(listing));

        // ① extraFiles：按 Exynos9830.json 的 files_to_send 请求两个**裸名** → 必须经 .lz4 回退取到。
        // 尺寸是解压后的（.lz4 字节数 337909/634293 都比它小得多，拿 .lz4 原字节比必红）——
        // 这一条同时证明"取出后按内容解压"。
        QList<QByteArray> extras;
        // 预置 stale：空串上的 isEmpty() 近乎恒真（成功路径本就不写 err），预置后才有甄别力（1b 审查 M1）
        QString err = QStringLiteral("stale");
        QVERIFY2(eub::loadNamedEntriesFromTar(
                     tar, {QStringLiteral("ldfw.img"), QStringLiteral("tzsw.img")}, extras, &err),
                 qPrintable(err));
        QCOMPARE(extras.size(), 2);
        QCOMPARE(extras[0].size(), qsizetype(0x600000));   // facts §H2：ldfw 解压后 6,291,456
        QCOMPARE(extras[1].size(), qsizetype(0x180000));   // facts §H2：tzsw 解压后 1,572,864
        QVERIFY2(err.isEmpty(), qPrintable(err));          // 成功路径不留错误文本

        // ② 同一真包里的 sboot：sha1 必须与 §H2 记录**逐字符一致**（用本仓 eub::sha1Hex 现算，
        // 不是拿记录值回显）。
        eub::SbootSource src;
        QByteArray sboot;
        QVERIFY2(eub::loadSbootBytes(tar, sboot, &src, &err), qPrintable(err));
        QCOMPARE(sboot.size(), qsizetype(4194304));        // 0x400000
        QCOMPARE(eub::sha1Hex(sboot), QStringLiteral("59ea267f01320dfea781668ecf229585e010a7d5"));
        QVERIFY(src.wasCompressed);                        // 真包内是 sboot.bin.lz4
        QVERIFY(src.description.contains(QStringLiteral("sboot.bin.lz4")));
    }

    void real9610BlPackageHasNoExtraFilesSoRequestFails()
    {
        // 9610 的 BL 包里**没有** ldfw/tzsw（facts §H2）：同一函数请求这两个名字必须失败。
        // 同一包先跑一次 loadSbootBytes 成功 —— 失败可归因到"缺条目"，而不是"包本身坏掉"
        // （否则本槽会被一个与被测逻辑无关的原因骗绿）。
        const QString pkg = QStringLiteral("BL_SM-A505FN.tar.md5");   // SM-A505FN = Exynos9610
        if (!eubtest::sampleAvailable(pkg)) {
            EUB_SKIP_OR_FAIL(pkg);
        }
        const QString tar = eubtest::samplePath(pkg);

        eub::SbootSource src;
        QByteArray sboot;
        QString err;
        QVERIFY2(eub::loadSbootBytes(tar, sboot, &src, &err), qPrintable(err));   // 包本身可用
        QCOMPARE(sboot.size(), qsizetype(4194304));                              // facts §H2

        QList<QByteArray> extras;
        extras << QByteArrayLiteral("stale");   // 失败路径必须清空（仓内约定），预置陈旧字节才有断言力
        QVERIFY(!eub::loadNamedEntriesFromTar(
            tar, {QStringLiteral("ldfw.img"), QStringLiteral("tzsw.img")}, extras, &err));
        QVERIFY2(err.contains(QStringLiteral("ldfw.img")), qPrintable(err));
        QVERIFY2(err.contains(QStringLiteral("tzsw.img")), qPrintable(err));   // 两个都缺 → 一次报全
        QVERIFY(extras.isEmpty());
    }
};

QTEST_APPLESS_MAIN(TestEubPayload)
#include "test_eub_payload.moc"
