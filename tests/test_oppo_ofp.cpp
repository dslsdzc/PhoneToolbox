#include <QtTest>
#include <QFile>
#include <QTemporaryDir>

#include "image_engine/oppo_keys.h"
#include "image_engine/oppo_ofp.h"
#include "oppo_test_helpers.h"

// OFP 识别与解析测试。期望值全部来自格式速查
// docs/superpowers/specs/oppo-format-notes.md 与参照实现
// （reference/oppo_decrypt/ofp_qc_decrypt.py、ofp_mtk_decrypt.py），
// 合成包由 tests/oppo_test_helpers.h 按独立方向拼装 —— 非被测代码自证。
class TestOppoOfp : public QObject
{
    Q_OBJECT

private slots:
    void detectQcFromTail();
    void detectMtkFromTail();
    void detectRejectsRandomTail();

    void parseQcSynthetic();
    void parseQcGroupPolicies();
    void parseQcA57XmlLengthHack();
    void parseMtkSynthetic();

    void parseRejectsUnknownKey();
    void parseRejectsOutOfBounds();
    void parseRejectsPkZip();
    void parseRejectsGarbageOrMissing();
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

static QString writePkg(QTemporaryDir &dir, const QString &name, const QByteArray &data)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
        return QString();
    return path;
}

// ---- 识别 ----

// QC 判据：尾页 +0x10 LE32 == 0x7CEF（extract_xml() L119-123）
void TestOppoOfp::detectQcFromTail()
{
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Sahara"), QStringLiteral("prog_ufs_firehose_test.elf"),
          QByteArray(0x100, 'P'), 2}});
    QVERIFY(pkg.isValid());

    imgopp::OfpVariant v = imgopp::OfpVariant::Unknown;
    QVERIFY(imgopp::detectOFP(pkg.head, pkg.tail, quint64(pkg.blob.size()), v));
    QCOMPARE(v, imgopp::OfpVariant::Qc);
    // 二次调用稳定（结果不依赖内部状态）
    v = imgopp::OfpVariant::Unknown;
    QVERIFY(imgopp::detectOFP(pkg.head, pkg.tail, quint64(pkg.blob.size()), v));
    QCOMPARE(v, imgopp::OfpVariant::Qc);

    // 0x1000 页尺寸亦识别（extract_xml() 的 [0x200, 0x1000] 双候选）
    ofptest::QcBuildOptions big;
    big.pageSize = 0x1000;
    big.xmlOffsetPages = 1;
    big.firstDataPage = 4;
    const ofptest::QcPackage pkg4k = ofptest::buildQcPackage(
        {{QStringLiteral("Firmware"), QStringLiteral("boot.img"), QByteArray(0x200, 'B'), 0}}, big);
    QVERIFY(pkg4k.isValid());
    v = imgopp::OfpVariant::Unknown;
    QVERIFY(imgopp::detectOFP(pkg4k.head, pkg4k.tail, quint64(pkg4k.blob.size()), v));
    QCOMPARE(v, imgopp::OfpVariant::Qc);
}

// MTK 判据：首 16B 逐候选试解，明文前缀 == "MMM"（brutekey() L101-110）
void TestOppoOfp::detectMtkFromTail()
{
    // 用候选表末位 MTK8 → 覆盖完整遍历顺序（不是"第一个就中"）
    ofptest::MtkBuildOptions opts;
    opts.keyIndex = 8;
    const ofptest::MtkPackage pkg = ofptest::buildMtkPackage(
        {{QStringLiteral("system"), QStringLiteral("system.img"), QByteArray(0x300, 'S'), 0, -1}}, opts);
    QVERIFY(pkg.isValid());

    imgopp::OfpVariant v = imgopp::OfpVariant::Unknown;
    QVERIFY(imgopp::detectOFP(pkg.head, pkg.tail, quint64(pkg.blob.size()), v));
    QCOMPARE(v, imgopp::OfpVariant::Mtk);
    v = imgopp::OfpVariant::Unknown;
    QVERIFY(imgopp::detectOFP(pkg.head, pkg.tail, quint64(pkg.blob.size()), v));
    QCOMPARE(v, imgopp::OfpVariant::Mtk);
}

// 随机/过短/自相矛盾入参一律 false，且不写 variant、不崩
void TestOppoOfp::detectRejectsRandomTail()
{
    const QByteArray rnd = pseudoRandom(0x1400);
    imgopp::OfpVariant v = imgopp::OfpVariant::Unknown;
    QVERIFY(!imgopp::detectOFP(rnd.left(16), ofptest::tailOf(rnd, 0x1000), 0x1400, v));
    QCOMPARE(v, imgopp::OfpVariant::Unknown);

    // 尾不足一页（0x100 < 0x200）
    QVERIFY(!imgopp::detectOFP(rnd.left(16), rnd.left(0x100), 0x100, v));
    // 头不足 16B（MTK 试解无法进行，QC 也不中）
    QVERIFY(!imgopp::detectOFP(rnd.left(4), ofptest::tailOf(rnd, 0x1000), 0x1400, v));
    // tail 比整个文件还大（调用方参数矛盾）
    QVERIFY(!imgopp::detectOFP(rnd.left(16), ofptest::tailOf(rnd, 0x1000), 0x800, v));
    // 空入参
    QVERIFY(!imgopp::detectOFP(QByteArray(), QByteArray(), 0, v));
    QCOMPARE(v, imgopp::OfpVariant::Unknown);
}

// ---- QC 解析 ----

// brief Step 1 断言集：变体 / 命中 keyId / 条目数与分组 / 偏移 / Sahara 全解密
void TestOppoOfp::parseQcSynthetic()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Sahara"), QStringLiteral("prog_ufs_firehose_test.elf"),
          QByteArray(0x100, 'P'), 2},
         {QStringLiteral("Firmware"), QStringLiteral("boot.img"), QByteArray(0x180, 'B'), 10}});
    QVERIFY(pkg.isValid());
    const QString path = writePkg(dir, QStringLiteral("qc.ofp"), pkg.blob);
    QVERIFY(!path.isEmpty());

    imgopp::OfpInfo info;
    QString error;
    QVERIFY2(imgopp::parseOFP(path, info, &error), qPrintable(error));
    QVERIFY(error.isEmpty());

    QCOMPARE(info.variant, imgopp::OfpVariant::Qc);
    QCOMPARE(info.pageSize, quint32(0x200));
    QCOMPARE(info.keyId, QStringLiteral("V1.5.13"));
    // key/iv 与 Task 2 派生断言同源（16B ASCII 十六进制字符，Task 4 直接喂 aes128CfbDecrypt）
    QCOMPARE(info.key, QByteArray("94d62e831cf1a1a0"));
    QCOMPARE(info.iv, QByteArray("7ab5e33bd50d81ca"));

    QCOMPARE(info.files.size(), 2);
    QCOMPARE(info.files[0].group, QStringLiteral("Sahara"));
    QCOMPARE(info.files[0].name, QStringLiteral("prog_ufs_firehose_test.elf"));
    QVERIFY(info.files[0].fullDecrypt);
    QCOMPARE(info.files[0].offset, quint64(2) * 0x200);
    QCOMPARE(info.files[0].encryptedSize, info.files[0].size);  // Sahara：整段需解密

    QCOMPARE(info.files[1].group, QStringLiteral("Firmware"));
    QVERIFY(!info.files[1].fullDecrypt);
    QCOMPARE(info.files[1].offset, quint64(10) * 0x200);
    QCOMPARE(info.files[1].size, quint64(0x180));   // 落盘长度取 SizeInByteInSrc
    QCOMPARE(info.files[1].encryptedSize, quint64(0));  // 明文组
}

// 组语义映射三条分支 + size 取 SizeInByteInSrc（非扇区对齐值）+ 校验属性透传
void TestOppoOfp::parseQcGroupPolicies()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray configPlain = pseudoRandom(int(ofptest::kQcPartialDecryptSize) + 0x100, 0xC0FFEEu);
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Sahara"), QStringLiteral("prog.elf"), QByteArray(0x300, 'A'), 0},
         {QStringLiteral("Firmware"), QStringLiteral("boot.img"), QByteArray(0x200, 'F'), 0},
         {QStringLiteral("DigestsToSign"), QStringLiteral("digests.bin"), QByteArray(0x100, 'D'), 0},
         {QStringLiteral("ChainedTableOfDigests"), QStringLiteral("chain.bin"), QByteArray(0x100, 'C'), 0},
         {QStringLiteral("Config"), QStringLiteral("config.bin"), configPlain, 0},
         {QStringLiteral("Provision"), QStringLiteral("prov.bin"), QByteArray(0x280, 'V'), 0,
          QStringLiteral("sha256-deadbeef"), QStringLiteral("md5-cafebabe"), true},
         // 无 SizeInByteInSrc 属性 → 长度回退 SizeInSectorInSrc × 页尺寸（0x180 → 1 扇区）
         {QStringLiteral("Sahara"), QStringLiteral("old-prog.elf"), QByteArray(0x180, 'O'), 0,
          QString(), QString(), false, true}});
    QVERIFY(pkg.isValid());
    const QString path = writePkg(dir, QStringLiteral("qc-groups.ofp"), pkg.blob);
    QVERIFY(!path.isEmpty());

    imgopp::OfpInfo info;
    QString error;
    QVERIFY2(imgopp::parseOFP(path, info, &error), qPrintable(error));
    QCOMPARE(info.files.size(), 7);

    QCOMPARE(info.files[0].group, QStringLiteral("Sahara"));
    QVERIFY(info.files[0].fullDecrypt);
    QCOMPARE(info.files[0].encryptedSize, quint64(0x300));

    QCOMPARE(info.files[1].group, QStringLiteral("Firmware"));
    QCOMPARE(info.files[1].encryptedSize, quint64(0));
    QVERIFY(!info.files[1].fullDecrypt);
    QCOMPARE(info.files[2].group, QStringLiteral("DigestsToSign"));
    QCOMPARE(info.files[2].encryptedSize, quint64(0));
    QCOMPARE(info.files[3].group, QStringLiteral("ChainedTableOfDigests"));
    QCOMPARE(info.files[3].encryptedSize, quint64(0));

    // 其余组：前 min(0x40000, size) 解密（decryptfile() L189-190 size = min(decryptsize, rlength)）
    QCOMPARE(info.files[4].group, QStringLiteral("Config"));
    // 0x40100 非页对齐 → 取的是 SizeInByteInSrc，不是 SizeInSectorInSrc × pagesize（0x40200）
    QCOMPARE(info.files[4].size, ofptest::kQcPartialDecryptSize + 0x100);
    QCOMPARE(info.files[4].encryptedSize, ofptest::kQcPartialDecryptSize);
    QVERIFY(!info.files[4].fullDecrypt);

    QCOMPARE(info.files[5].group, QStringLiteral("Provision"));
    QCOMPARE(info.files[5].size, quint64(0x280));
    QCOMPARE(info.files[5].encryptedSize, quint64(0x280));
    QCOMPARE(info.files[5].sha256Hex, QStringLiteral("sha256-deadbeef"));
    QCOMPARE(info.files[5].md5Hex, QStringLiteral("md5-cafebabe"));
    QVERIFY(info.files[5].sparse);
    QVERIFY(!info.files[0].sparse);

    // SizeInByteInSrc 缺失 → 回退扇区长度（1 × 0x200），而非载荷实际字节数 0x180
    QCOMPARE(info.files[6].name, QStringLiteral("old-prog.elf"));
    QCOMPARE(info.files[6].size, quint64(0x200));
    QCOMPARE(info.files[6].encryptedSize, quint64(0x200));
}

// A57 老包：尾页 +0x18 长度字段 < 200 → 按 (fileSize-page)-xmlOffset-0x57 重算
// （extract_xml() L132-133）。夹具按重算公式的逆推布局，字段写 0。
void TestOppoOfp::parseQcA57XmlLengthHack()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ofptest::QcBuildOptions opts;
    opts.a57XmlLengthHack = true;
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Sahara"), QStringLiteral("prog.elf"), QByteArray(0x180, 'P'), 0}}, opts);
    QVERIFY(pkg.isValid());
    QVERIFY(pkg.xmlLength >= 200);   // 清单本身不短；短的是被写坏的 +0x18 字段
    const QString path = writePkg(dir, QStringLiteral("qc-a57.ofp"), pkg.blob);
    QVERIFY(!path.isEmpty());

    imgopp::OfpInfo info;
    QString error;
    QVERIFY2(imgopp::parseOFP(path, info, &error), qPrintable(error));
    QCOMPARE(info.variant, imgopp::OfpVariant::Qc);
    QCOMPARE(info.keyId, QStringLiteral("V1.5.13"));
    QCOMPARE(info.files.size(), 1);
    QCOMPARE(info.files[0].name, QStringLiteral("prog.elf"));
    QCOMPARE(info.files[0].size, quint64(0x180));
    QVERIFY(info.files[0].fullDecrypt);
}

// ---- MTK 解析 ----

// 尾 0x6C 头字段 + 0x60 文件表（name/start/length/enclength/filename）+ 空行跳过
void TestOppoOfp::parseMtkSynthetic()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ofptest::MtkBuildOptions opts;
    opts.keyIndex = 8;                                  // MTK8（直接 ASCII 常量）
    opts.prjname = QStringLiteral("CPH_TEST_PRJ");
    opts.flashtype = QStringLiteral("UFS");
    opts.prjinfo = QStringLiteral("PRJINFO-X");
    const QList<ofptest::MtkFileSpec> files{
        {QStringLiteral("system"), QStringLiteral("system.img"), QByteArray(0x300, 'S'), 0, 0x200},
        {QStringLiteral("vendor"), QStringLiteral("vendor.img"), QByteArray(0x280, 'V'), 0, -1},
        {QStringLiteral("boot"), QStringLiteral("boot.img"), QByteArray(0x40, 'B'), 0, 0}};
    opts.entryCount = quint32(files.size()) + 2;        // 表尾 2 条空行 → 必须跳过
    const ofptest::MtkPackage pkg = ofptest::buildMtkPackage(files, opts);
    QVERIFY(pkg.isValid());
    const QString path = writePkg(dir, QStringLiteral("mtk.ofp"), pkg.blob);
    QVERIFY(!path.isEmpty());

    imgopp::OfpInfo info;
    QString error;
    QVERIFY2(imgopp::parseOFP(path, info, &error), qPrintable(error));
    QVERIFY(error.isEmpty());

    QCOMPARE(info.variant, imgopp::OfpVariant::Mtk);
    QCOMPARE(info.keyId, QStringLiteral("MTK8"));
    QCOMPARE(info.key, QByteArray("ab3f76d7989207f2"));
    QCOMPARE(info.iv, QByteArray("2bf515b3a9737835"));
    QCOMPARE(info.projectName, QStringLiteral("CPH_TEST_PRJ"));
    QCOMPARE(info.version, QStringLiteral("UFS"));      // MTK flashtype
    QCOMPARE(info.pageSize, quint32(0));

    QCOMPARE(info.files.size(), 3);                     // 空 filename 行不产出条目
    const quint64 sizes[3] = {0x300, 0x280, 0x40};
    const quint64 encSizes[3] = {0x200, 0x280, 0};      // 前 0x200 加密 / 全加密 / 全明文
    for (int i = 0; i < 3; ++i) {
        QCOMPARE(info.files[i].name, files[i].filename);
        QCOMPARE(info.files[i].offset, pkg.starts[i]);
        QCOMPARE(info.files[i].size, sizes[i]);
        QCOMPARE(info.files[i].encryptedSize, encSizes[i]);
        QVERIFY(!info.files[i].fullDecrypt);
        QVERIFY(info.files[i].group.isEmpty());
    }
    // 自动布局从包头区 0x200 起（夹具契约）
    QCOMPARE(pkg.starts[0], quint64(0x200));
}

// ---- 拒绝路径 ----

// 候选表外的 key 加密 → "密钥未知"；正确 key 但密文被破坏 → 同一错误分支（A9 收口）
void TestOppoOfp::parseRejectsUnknownKey()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ofptest::QcBuildOptions opts;
    opts.keyOverride = QByteArrayLiteral("0123456789abcdef");  // 16B，不在 qcKeyCandidates() 内
    opts.ivOverride = QByteArrayLiteral("fedcba9876543210");
    const ofptest::QcPackage unknown = ofptest::buildQcPackage(
        {{QStringLiteral("Sahara"), QStringLiteral("prog.elf"), QByteArray(0x100, 'P'), 0}}, opts);
    QVERIFY(unknown.isValid());
    const QString unknownPath = writePkg(dir, QStringLiteral("unknown.ofp"), unknown.blob);
    QVERIFY(!unknownPath.isEmpty());

    // 失败时 info 保持调用方原值（无半截状态）
    imgopp::OfpInfo info;
    info.variant = imgopp::OfpVariant::Mtk;
    info.keyId = QStringLiteral("sentinel");
    info.files.append(imgopp::OfpFile());
    QString error;
    QVERIFY(!imgopp::parseOFP(unknownPath, info, &error));
    QVERIFY2(error.contains(QStringLiteral("密钥未知")), qPrintable(error));
    QCOMPARE(info.variant, imgopp::OfpVariant::Mtk);
    QCOMPARE(info.keyId, QStringLiteral("sentinel"));
    QCOMPARE(info.files.size(), 1);

    // 清单密文首字节翻转（密钥正确）→ 试解全部落空，同样归入密钥/损坏分支
    const ofptest::QcPackage good = ofptest::buildQcPackage(
        {{QStringLiteral("Sahara"), QStringLiteral("prog.elf"), QByteArray(0x100, 'P'), 0}});
    QVERIFY(good.isValid());
    QByteArray damaged = good.blob;
    damaged[qsizetype(good.xmlOffset)] = char(damaged.at(qsizetype(good.xmlOffset)) ^ 0xFF);
    const QString damagedPath = writePkg(dir, QStringLiteral("damaged.ofp"), damaged);
    QVERIFY(!damagedPath.isEmpty());

    error.clear();
    QVERIFY(!imgopp::parseOFP(damagedPath, info, &error));
    QVERIFY2(error.contains(QStringLiteral("密钥未知")), qPrintable(error));

    // error 允许为 nullptr（A9 契约）——不得崩
    QVERIFY(!imgopp::parseOFP(damagedPath, info, nullptr));
}

// 文件表越界（spec §5 恶意输入防护）：QC 偏移越界 / MTK 数据段越界
void TestOppoOfp::parseRejectsOutOfBounds()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // QC：FileOffsetInSrc 指向包外（0x8000 页 × 0x200 = 16MiB）
    const ofptest::QcPackage qc = ofptest::buildQcPackage(
        {{QStringLiteral("Config"), QStringLiteral("super.img"), QByteArray(0x100, 'X'), 0x8000}});
    QVERIFY(qc.isValid());
    const QString qcPath = writePkg(dir, QStringLiteral("oob.ofp"), qc.blob);
    QVERIFY(!qcPath.isEmpty());

    imgopp::OfpInfo info;
    QString error;
    QVERIFY(!imgopp::parseOFP(qcPath, info, &error));
    QVERIFY2(error.contains(QStringLiteral("越界")), qPrintable(error));

    // MTK：条目 start 指向包外
    const ofptest::MtkPackage mtk = ofptest::buildMtkPackage(
        {{QStringLiteral("system"), QStringLiteral("system.img"), QByteArray(0x100, 'S'), 0x100000, -1}});
    QVERIFY(mtk.isValid());
    const QString mtkPath = writePkg(dir, QStringLiteral("oob-mtk.ofp"), mtk.blob);
    QVERIFY(!mtkPath.isEmpty());

    error.clear();
    QVERIFY(!imgopp::parseOFP(mtkPath, info, &error));
    QVERIFY2(error.contains(QStringLiteral("越界")), qPrintable(error));
}

// 老式密码 ZIP 包（spec §7；ofp_qc_decrypt.py main() L286-290 的 PK 判定）
void TestOppoOfp::parseRejectsPkZip()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writePkg(dir, QStringLiteral("legacy.ofp"),
                                  QByteArrayLiteral("PK\x03\x04") + pseudoRandom(0x800));
    QVERIFY(!path.isEmpty());

    imgopp::OfpInfo info;
    QString error;
    QVERIFY(!imgopp::parseOFP(path, info, &error));
    QVERIFY2(error.contains(QStringLiteral("ZIP")), qPrintable(error));
}

// 随机数据 / 小于一页 / 空文件 / 路径不存在：明确中文错误，不崩
void TestOppoOfp::parseRejectsGarbageOrMissing()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    imgopp::OfpInfo info;
    QString error;

    const QByteArray garbage = pseudoRandom(0x1400);
    const QString garbagePath = writePkg(dir, QStringLiteral("garbage.ofp"), garbage);
    QVERIFY(!garbagePath.isEmpty());
    QVERIFY(!imgopp::parseOFP(garbagePath, info, &error));
    QVERIFY2(error.contains(QStringLiteral("不是有效的 OFP 包")), qPrintable(error));

    // 越过一页的最小尺寸门槛（< 0x200）
    error.clear();
    const QString tinyPath = writePkg(dir, QStringLiteral("tiny.ofp"), garbage.left(0x100));
    QVERIFY(!tinyPath.isEmpty());
    QVERIFY(!imgopp::parseOFP(tinyPath, info, &error));
    QVERIFY2(error.contains(QStringLiteral("过小")), qPrintable(error));

    // 空文件
    error.clear();
    const QString emptyPath = writePkg(dir, QStringLiteral("empty.ofp"), QByteArray());
    QVERIFY(!emptyPath.isEmpty());
    QVERIFY(!imgopp::parseOFP(emptyPath, info, &error));
    QVERIFY(!error.isEmpty());

    // 路径不存在
    error.clear();
    QVERIFY(!imgopp::parseOFP(dir.filePath(QStringLiteral("nope.ofp")), info, &error));
    QVERIFY2(error.contains(QStringLiteral("无法打开")), qPrintable(error));
}

QTEST_APPLESS_MAIN(TestOppoOfp)
#include "test_oppo_ofp.moc"
