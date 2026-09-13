#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>

#include "image_engine/oppo_extract.h"
#include "image_engine/oppo_ofp.h"
#include "oppo_test_helpers.h"

// OFP 解包测试。合成包由 tests/oppo_test_helpers.h 按格式事实独立拼装（加密方向与被测
// 解密方向互为逆运算），期望值来自 docs/superpowers/specs/oppo-format-notes.md、设计 spec
// §4/§5 与参照实现 reference/oppo_decrypt/ofp_qc_decrypt.py（copy()/decryptfile()/checkhashfile()）。
// 断言一律落在真实产物的字节上 —— 不调用被测实现给自己生成期望值。
class TestOppoExtract : public QObject
{
    Q_OBJECT

private slots:
    void extractQcSynthetic();
    void extractQcPartialDecrypt();
    void extractCrossChunkDecrypt();
    void extractMtkPackage();
    void extractSparseAnnotation();
    void extractRejectsBadChecksum();
    void extractRejectsUnsafeNames();
    void extractKeepsSkipNotesWhenFatal();
    void extractRejectsSamePathAndMissing();
};

// 确定性伪随机（不用 qrand，避免未播种告警噪音）
static QByteArray pseudoRandom(int size, quint32 seed = 0x12345678u)
{
    QByteArray out(size, '\0');
    quint32 s = seed;
    for (int i = 0; i < size; ++i) {
        s = s * 1664525u + 1013904223u;
        out[i] = char((s >> 24) & 0xFF);
    }
    return out;
}

static QString writePkg(const QString &dirPath, const QString &name, const QByteArray &data)
{
    const QString path = QDir(dirPath).filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
        return QString();
    return path;
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

// 独立算摘要（QCryptographicHash 直算，不经被测代码）
static QString hexOf(const QByteArray &data, QCryptographicHash::Algorithm algo)
{
    QCryptographicHash h(algo);
    h.addData(data);
    return QString::fromLatin1(h.result().toHex());
}

// 进度回调观察器：记录 (文件名, 百分比) 序列
struct ProgressLog
{
    QStringList names;
    QList<int> percents;

    imgopp::ExtractProgress callback()
    {
        return [this](const QString &name, int percent) {
            names << name;
            percents << percent;
        };
    }

    bool monotonic() const
    {
        for (int i = 1; i < percents.size(); ++i)
            if (percents.at(i) < percents.at(i - 1))
                return false;
        return true;
    }
};

// ---- 三种提取策略 ----

// brief Step 1 断言集：Firmware 明文拷贝 + Sahara 整段解密的产物字节
void TestOppoExtract::extractQcSynthetic()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray elfPlain = pseudoRandom(0x800, 0xE1Fu);   // Sahara：整段解密
    const QByteArray bootPlain(4096, '\xAA');                  // Firmware：明文拷贝
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Sahara"), QStringLiteral("prog_ufs_firehose_test.elf"), elfPlain, 0},
         {QStringLiteral("Firmware"), QStringLiteral("boot.img"), bootPlain, 0}});
    QVERIFY(pkg.isValid());
    const QString pkgPath = writePkg(dir.path(), QStringLiteral("test.ofp"), pkg.blob);
    QVERIFY(!pkgPath.isEmpty());

    const QString outDir = dir.filePath(QStringLiteral("out"));
    QVERIFY(QDir().mkpath(outDir));

    ProgressLog log;
    QString err;
    const bool ok = imgopp::extractOFP(pkgPath, outDir, log.callback(), &err);
    QVERIFY2(ok, qPrintable(err));
    QVERIFY(err.isEmpty());

    QVERIFY(QFile::exists(outDir + "/prog_ufs_firehose_test.elf"));
    QVERIFY(QFile::exists(outDir + "/boot.img"));
    QCOMPARE(readFile(outDir + "/boot.img"), bootPlain);
    // 包内该段是密文 → 产物逐字节等于原始明文（不解密或整段拷贝都到不了这里）
    QCOMPARE(readFile(outDir + "/prog_ufs_firehose_test.elf"), elfPlain);
    QCOMPARE(QFileInfo(outDir + "/prog_ufs_firehose_test.elf").size(), qint64(elfPlain.size()));

    QCOMPARE(log.names.size(), 2);                              // 每文件完成回调一次
    QCOMPARE(log.names.last(), QStringLiteral("boot.img"));     // 非 sparse → 无标注
    QCOMPARE(log.percents.last(), 100);
    QVERIFY(log.monotonic());

    // error 允许为 nullptr（A9 契约）；输出目录不存在时自动创建（嵌套亦建）
    const QString outDir2 = dir.filePath(QStringLiteral("out2/nested"));
    QVERIFY(!QDir(outDir2).exists());
    QVERIFY(imgopp::extractOFP(pkgPath, outDir2, {}, nullptr));
    QCOMPARE(readFile(outDir2 + "/boot.img"), bootPlain);
}

// 第三种策略：非明文组仅前 min(0x40000, size) 解密，余下原样拷贝（decryptfile() L188-198）
void TestOppoExtract::extractQcPartialDecrypt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray plain = pseudoRandom(int(ofptest::kQcPartialDecryptSize) + 0x300, 0xC0FFEEu);
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Config"), QStringLiteral("config.bin"), plain, 0}});
    QVERIFY(pkg.isValid());
    const QString pkgPath = writePkg(dir.path(), QStringLiteral("partial.ofp"), pkg.blob);
    QVERIFY(!pkgPath.isEmpty());
    const QString outDir = dir.filePath(QStringLiteral("out"));

    // 反自证：包内该段确为密文（前 16B 与明文不同）——否则"产物 == 明文"恒真
    imgopp::OfpInfo info;
    QString err;
    QVERIFY2(imgopp::parseOFP(pkgPath, info, &err), qPrintable(err));
    QCOMPARE(info.files.size(), 1);
    QVERIFY(pkg.blob.mid(qsizetype(info.files[0].offset), 16) != plain.left(16));

    QVERIFY2(imgopp::extractOFP(pkgPath, outDir, {}, &err), qPrintable(err));
    // 前缀解密 + 尾部原样拷贝 → 整文件等于原始明文（少解/多解/全拷贝都会在此暴露）
    QCOMPARE(readFile(outDir + "/config.bin"), plain);
    QCOMPARE(QFileInfo(outDir + "/config.bin").size(), qint64(plain.size()));
}

// 跨块解密（本模块最高风险不变量）: 解密区间必须 > 1 个分块（0x100000）才能让 decryptRange
// 走到第二次迭代，从而执行"把上一块末 16B 密文作为下一块 IV"的进位（oppo_extract.cpp
// decryptRange()）—— CFB 的反馈是密文块，丢了进位则第二块起 keystream 全错。
// 组选择: 只有 Sahara（fullDecrypt）的解密区间 == size；Config 等组的解密前缀被 parseOFP
// 截到 min(0x40000,size)（oppo_ofp.cpp L253），最多一块，进不了跨块路径。
// 夹具侧: buildQcPackage 对整段明文一次调用 aes128CfbEncrypt（单一 keystream 贯穿全部
// 0x120000 字节），与真实包"整段 CFB 密文"语义一致；若夹具改成逐块独立加密，本用例将失去意义。
void TestOppoExtract::extractCrossChunkDecrypt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    constexpr int kBigSize = 0x120000;   // 1.125 MiB = 块 1（0x100000）+ 块 2（0x20000）
    const QByteArray plain = pseudoRandom(kBigSize, 0x1CEBu);
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Sahara"), QStringLiteral("prog_big.elf"), plain, 0}});
    QVERIFY(pkg.isValid());
    const QString pkgPath = writePkg(dir.path(), QStringLiteral("big.ofp"), pkg.blob);
    QVERIFY(!pkgPath.isEmpty());

    // 反自证：包内该段确为密文（首 16B 与明文不同），且条目长度确实跨过 1 MiB 块界
    imgopp::OfpInfo info;
    QString err;
    QVERIFY2(imgopp::parseOFP(pkgPath, info, &err), qPrintable(err));
    QCOMPARE(info.files.size(), 1);
    QVERIFY(info.files[0].fullDecrypt);
    QCOMPARE(info.files[0].size, quint64(kBigSize));
    QVERIFY(info.files[0].size > 0x100000u);
    QVERIFY(pkg.blob.mid(qsizetype(info.files[0].offset), 16) != plain.left(16));

    const QString outDir = dir.filePath(QStringLiteral("out"));
    ProgressLog log;
    QVERIFY2(imgopp::extractOFP(pkgPath, outDir, log.callback(), &err), qPrintable(err));
    const QByteArray got = readFile(outDir + "/prog_big.elf");
    QCOMPARE(got.size(), qsizetype(kBigSize));
    // 块界起点即 IV 进位生效点：先单独钉一次 —— QCOMPARE 失败会立即返回，此断言排在最前
    // 才能在失败输出里直接看到第二块（0x100000 起）的字节差异，而不是被前 1 MiB 掩住
    QCOMPARE(got.mid(0x100000), plain.mid(0x100000));
    QCOMPARE(got, plain);   // 全量逐字节（0x120000 字节整体比较）
    QCOMPARE(log.percents.last(), 100);
}

// MTK 变体：文件表 encrypted_length 三形态（前 0x200 解密 / 整段解密 / 全明文）
void TestOppoExtract::extractMtkPackage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray systemPlain = pseudoRandom(0x500, 0x5A5Eu);
    const QByteArray vendorPlain = pseudoRandom(0x280, 0x7E57u);
    const QByteArray bootPlain = pseudoRandom(0x40, 0xB007u);
    ofptest::MtkBuildOptions opts;
    opts.keyIndex = 8;   // MTK8（key/iv 来自 parseOFP 的命中候选，本层不关心具体值）
    const ofptest::MtkPackage pkg = ofptest::buildMtkPackage(
        {{QStringLiteral("system"), QStringLiteral("system.img"), systemPlain, 0, 0x200},
         {QStringLiteral("vendor"), QStringLiteral("vendor.img"), vendorPlain, 0, -1},
         {QStringLiteral("boot"), QStringLiteral("boot.img"), bootPlain, 0, 0}},
        opts);
    QVERIFY(pkg.isValid());
    const QString pkgPath = writePkg(dir.path(), QStringLiteral("mtk.ofp"), pkg.blob);
    QVERIFY(!pkgPath.isEmpty());
    const QString outDir = dir.filePath(QStringLiteral("out"));

    ProgressLog log;
    QString err;
    QVERIFY2(imgopp::extractOFP(pkgPath, outDir, log.callback(), &err), qPrintable(err));
    QVERIFY(err.isEmpty());
    QCOMPARE(readFile(outDir + "/system.img"), systemPlain);
    QCOMPARE(readFile(outDir + "/vendor.img"), vendorPlain);
    QCOMPARE(readFile(outDir + "/boot.img"), bootPlain);
    QCOMPARE(log.names.size(), 3);
    QCOMPARE(log.percents.last(), 100);
}

// sparse 仅标注（spec §4）：进度回调名带标注，产物字节原样输出（不自动转 raw）
void TestOppoExtract::extractSparseAnnotation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 真 sparse 镜像以魔数 0xED26FF3A（LE）起始；原样输出 → 产物必须与包内字节一致
    QByteArray sparsePlain = QByteArray::fromHex("3aff26ed");
    sparsePlain += pseudoRandom(0x300, 0x5A125u);
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Config"), QStringLiteral("super.img"), sparsePlain, 0,
          QString(), QString(), true}});
    QVERIFY(pkg.isValid());
    const QString pkgPath = writePkg(dir.path(), QStringLiteral("sparse.ofp"), pkg.blob);
    QVERIFY(!pkgPath.isEmpty());
    const QString outDir = dir.filePath(QStringLiteral("out"));

    ProgressLog log;
    QString err;
    QVERIFY2(imgopp::extractOFP(pkgPath, outDir, log.callback(), &err), qPrintable(err));
    QCOMPARE(log.names.size(), 1);
    QCOMPARE(log.names.first(), QStringLiteral("super.img（sparse 镜像，原样输出）"));
    QCOMPARE(readFile(outDir + "/super.img"), sparsePlain);
    QCOMPARE(QFileInfo(outDir + "/super.img").size(), qint64(sparsePlain.size()));
}

// ---- 校验（checkhashfile() L206-245）----

// sha256/md5 不符 → false + 中文 error；已写产物保留（不回滚），后续条目不再提取
void TestOppoExtract::extractRejectsBadChecksum()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // ---- 1) sha256 不符：清单值是"内容被翻转一字节"的摘要（长度合法、必然不匹配）----
    const QByteArray good = pseudoRandom(0x200, 0xBEEFu);
    const QByteArray bad = pseudoRandom(0x200, 0xDEADu);
    QByteArray flipped = bad;
    flipped[0] = char(flipped.at(0) ^ 0xFF);
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Firmware"), QStringLiteral("recovery.img"), good, 0},
         {QStringLiteral("Firmware"), QStringLiteral("boot.img"), bad, 0,
          hexOf(flipped, QCryptographicHash::Sha256)},
         {QStringLiteral("Firmware"), QStringLiteral("vendor.img"), good, 0}});
    QVERIFY(pkg.isValid());
    const QString pkgPath = writePkg(dir.path(), QStringLiteral("badsha.ofp"), pkg.blob);
    QVERIFY(!pkgPath.isEmpty());
    const QString outDir = dir.filePath(QStringLiteral("out"));

    QString err;
    QVERIFY(!imgopp::extractOFP(pkgPath, outDir, {}, &err));
    QVERIFY2(err.contains(QStringLiteral("校验失败")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("boot.img")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("sha256")), qPrintable(err));
    // 已写产物保留（spec §5：不回滚已写文件）
    QCOMPARE(readFile(outDir + "/recovery.img"), good);
    QVERIFY(QFile::exists(outDir + "/boot.img"));
    QCOMPARE(readFile(outDir + "/boot.img"), bad);        // 已落盘但校验不过
    QVERIFY(!QFile::exists(outDir + "/vendor.img"));      // 校验失败即止，后续条目不提取

    // ---- 2) md5 不符（sha256 属性为空 → 只校验 md5）----
    const QByteArray data2 = pseudoRandom(0x180, 0x1357u);
    QByteArray flipped2 = data2;
    flipped2[0] = char(flipped2.at(0) ^ 0xFF);
    const ofptest::QcPackage pkg2 = ofptest::buildQcPackage(
        {{QStringLiteral("Firmware"), QStringLiteral("md5me.img"), data2, 0,
          QString(), hexOf(flipped2, QCryptographicHash::Md5)}});
    QVERIFY(pkg2.isValid());
    const QString pkg2Path = writePkg(dir.path(), QStringLiteral("badmd5.ofp"), pkg2.blob);
    QVERIFY(!pkg2Path.isEmpty());
    const QString outDir2 = dir.filePath(QStringLiteral("out2"));

    err.clear();
    QVERIFY(!imgopp::extractOFP(pkg2Path, outDir2, {}, &err));
    QVERIFY2(err.contains(QStringLiteral("校验失败")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("md5me.img")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("md5")), qPrintable(err));

    // ---- 3) 大写 hex 命中 → 校验通过（大小写不敏感）----
    const ofptest::QcPackage pkg3 = ofptest::buildQcPackage(
        {{QStringLiteral("Firmware"), QStringLiteral("upper.img"), data2, 0,
          hexOf(data2, QCryptographicHash::Sha256).toUpper(),
          hexOf(data2, QCryptographicHash::Md5).toUpper()}});
    QVERIFY(pkg3.isValid());
    const QString pkg3Path = writePkg(dir.path(), QStringLiteral("upper.ofp"), pkg3.blob);
    QVERIFY(!pkg3Path.isEmpty());
    const QString outDir3 = dir.filePath(QStringLiteral("out3"));

    err.clear();
    QVERIFY2(imgopp::extractOFP(pkg3Path, outDir3, {}, &err), qPrintable(err));
    QVERIFY(err.isEmpty());
    QCOMPARE(readFile(outDir3 + "/upper.img"), data2);
}

// ---- 恶意输入防护（spec §5）----

// 条目名穿越防护：跳过恶意条目（不落盘到 outDir 之外），其余条目继续提取（不中断整包）
void TestOppoExtract::extractRejectsUnsafeNames()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray legit = pseudoRandom(0x180, 0x5AFEu);
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Firmware"), QStringLiteral("../evil.img"), QByteArray(0x40, 'E'), 0},
         {QStringLiteral("Firmware"), QStringLiteral("/abs/evil.img"), QByteArray(0x40, 'A'), 0},
         {QStringLiteral("Firmware"), QStringLiteral("sub/evil.img"), QByteArray(0x40, 'S'), 0},
         {QStringLiteral("Firmware"), QStringLiteral("boot.img"), legit, 0}});
    QVERIFY(pkg.isValid());
    const QString pkgPath = writePkg(dir.path(), QStringLiteral("unsafe.ofp"), pkg.blob);
    QVERIFY(!pkgPath.isEmpty());
    const QString outDir = dir.filePath(QStringLiteral("out"));

    ProgressLog log;
    QString err;
    // 跳过恶意条目但整包继续（同 tar_image.cpp extractTarStream() 的条目名防护惯例）：
    // 有合法条目被提取 → true，*error 携带逐条跳过原因
    QVERIFY2(imgopp::extractOFP(pkgPath, outDir, log.callback(), &err), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("文件名")), qPrintable(err));
    QCOMPARE(readFile(outDir + "/boot.img"), legit);
    QCOMPARE(log.names.size(), 1);        // 被跳过的条目既无产物也不计进度
    QCOMPARE(log.percents.last(), 100);
    // 一个字节都没落到 outDir 之外
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("evil.img"))));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("abs"))));
    QVERIFY(!QDir(outDir).exists(QStringLiteral("sub")));

    // 全部条目名不安全 → 无任何产物 → false（error 保留逐条原因）
    const ofptest::QcPackage only = ofptest::buildQcPackage(
        {{QStringLiteral("Firmware"), QStringLiteral("../evil.img"), QByteArray(0x40, 'E'), 0}});
    QVERIFY(only.isValid());
    const QString onlyPath = writePkg(dir.path(), QStringLiteral("only-evil.ofp"), only.blob);
    QVERIFY(!onlyPath.isEmpty());
    QString err2;
    QVERIFY(!imgopp::extractOFP(onlyPath, dir.filePath(QStringLiteral("out2")), {}, &err2));
    QVERIFY2(err2.contains(QStringLiteral("文件名")), qPrintable(err2));

    // 文件表为空（0 条目）→ 无任何产物 → false 且不崩（不产出空包目录）
    ofptest::MtkBuildOptions emptyOpts;
    const ofptest::MtkPackage empty = ofptest::buildMtkPackage({}, emptyOpts);
    QVERIFY(empty.isValid());
    const QString emptyPath = writePkg(dir.path(), QStringLiteral("empty.ofp"), empty.blob);
    QVERIFY(!emptyPath.isEmpty());
    QString err3;
    QVERIFY(!imgopp::extractOFP(emptyPath, dir.filePath(QStringLiteral("out3")), {}, &err3));
    QVERIFY(!err3.isEmpty());
}

// 跳过清单在**致命失败**时不得被吞掉（清扫 PA6）：条目名净化的逐条提示累积在 *error（appendNote），
// 若失败收口（fail）直接赋值，用户只会看到最后一句（本例："无法创建输出目录"），完全不知道包里还有
// 条目被跳过 —— 而"包不完整/解不开"恰恰是最需要那份清单的场景。
// 构造：一个不安全条目（先累积跳过提示）+ outDir 指向一个**已存在的普通文件**（mkpath 必失败 →
// 触发后续致命错误），于是两条诊断必须同时在 *error 里。
void TestOppoExtract::extractKeepsSkipNotesWhenFatal()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Firmware"), QStringLiteral("../evil.img"), QByteArray(0x40, 'E'), 0},
         {QStringLiteral("Firmware"), QStringLiteral("boot.img"), QByteArray(0x80, 'B'), 0}});
    QVERIFY(pkg.isValid());
    const QString pkgPath = writePkg(dir.path(), QStringLiteral("notes.ofp"), pkg.blob);
    QVERIFY(!pkgPath.isEmpty());

    const QString blocked = dir.filePath(QStringLiteral("blocked"));
    QVERIFY(!writePkg(dir.path(), QStringLiteral("blocked"), QByteArray("x")).isEmpty());

    QString err;
    QVERIFY(!imgopp::extractOFP(pkgPath, blocked, {}, &err));
    QVERIFY2(err.contains(QStringLiteral("文件名不安全")), qPrintable(err));    // ← 跳过提示必须还在
    QVERIFY2(err.contains(QStringLiteral("无法创建输出目录")), qPrintable(err));  // 致命原因也要在
}

// 路径守卫（spec §5）：产物路径 == 包路径 → 拒绝，且必须"先判后开"（包不被截断）
void TestOppoExtract::extractRejectsSamePathAndMissing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray payload = pseudoRandom(0x100, 0x600Du);
    const QString pkgName = QStringLiteral("guard-pkg.ofp");
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Firmware"), pkgName, payload, 0}});
    QVERIFY(pkg.isValid());
    const QString pkgPath = writePkg(dir.path(), pkgName, pkg.blob);
    QVERIFY(!pkgPath.isEmpty());

    // outDir = 包所在目录 且 条目名 == 包文件名 → 产物路径与输入包同一
    QString err;
    QVERIFY(!imgopp::extractOFP(pkgPath, dir.path(), {}, &err));
    QVERIFY2(err.contains(QStringLiteral("相同")), qPrintable(err));
    // 先判后开：包必须完好（未被 Truncate，仍可解析）
    QCOMPARE(QFileInfo(pkgPath).size(), qint64(pkg.blob.size()));
    imgopp::OfpInfo info;
    QVERIFY2(imgopp::parseOFP(pkgPath, info, &err), qPrintable(err));

    // 包不存在 → 明确中文错误
    err.clear();
    QVERIFY(!imgopp::extractOFP(dir.filePath(QStringLiteral("nope.ofp")), dir.path(), {}, &err));
    QVERIFY2(err.contains(QStringLiteral("无法打开")), qPrintable(err));

    // 低于一页的包（解析层拒绝）→ false + 非空 error
    err.clear();
    const QString tinyPath = writePkg(dir.path(), QStringLiteral("tiny.ofp"), QByteArray(0x100, '\0'));
    QVERIFY(!tinyPath.isEmpty());
    QVERIFY(!imgopp::extractOFP(tinyPath, dir.filePath(QStringLiteral("tiny-out")), {}, &err));
    QVERIFY(!err.isEmpty());
}

QTEST_APPLESS_MAIN(TestOppoExtract)
#include "test_oppo_extract.moc"
