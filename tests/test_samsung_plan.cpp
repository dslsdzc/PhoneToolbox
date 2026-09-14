// tests/test_samsung_plan.cpp
//
// 计划层（PIT 文件名优先匹配 + .pit 唯一性回退 + 双向告警）：合成夹具（始终跑）+
// 真包硬断言（reference/samsung-samples/sm-j110h/ 存在时才跑，否则 QSKIP）。
// 真包事实依据：docs/superpowers/specs/samsung-odin-facts.md §3。
#ifndef ODIN_SAMPLES_DIR
#define ODIN_SAMPLES_DIR ""
#endif
// 1 = 真样本缺失时 FAIL 而非 SKIP（CMake 侧 ODIN_SAMPLES_REQUIRED=ON 时传入；默认 0 保持离线友好）。
#ifndef ODIN_SAMPLES_REQUIRED
#define ODIN_SAMPLES_REQUIRED 0
#endif

#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QTemporaryDir>
#include <utility>

#include "core/odin/samsung_plan.h"
#include "image_engine/tar_image.h"
#include "odin_test_helpers.h"

class TestSamsungPlan : public QObject
{
    Q_OBJECT
private slots:
    void matchesByPitFilename();
    void skipsEntriesWithoutImageName();
    void fallsBackToUniquePitEntry();
    void warnsBothMismatchDirections();
    void warnsWhenImageLargerThanPartition();
    void summarisesSmallImages();
    void reportsMd5FooterState();
    void loadsPitFromPackage();
    void refusesEmptyResult();
    // 真包（reference/samsung-samples/sm-j110h/，缺失时 QSKIP）
    void realPackagesBuildPlan();
    void rejectsWhenNoPitInPackage();
};

using namespace odintest;

// 夹具 bootSbootNv()（BOOT/spl.img、SBOOT/sboot.bin、wfixnv2/nvitem.bin）由 odin_test_helpers.h 提供。
// 造一个含若干条目的 .tar.md5（磁盘上），返回路径与条目名→(偏移,大小)
static QString writeTarMd5(const QString &dir, const QString &name,
                           const QList<imgtar::TarEntry> &entries, bool withFooter = true)
{
    const QByteArray tar = imgtar::buildTar(entries);
    const QByteArray out = withFooter ? imgtar::appendMd5Footer(tar) : tar;
    const QString path = dir + QLatin1Char('/') + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(out) != out.size())
        return QString();
    f.close();
    return path;
}

static imgtar::TarEntry tarEntry(const QString &name, const QByteArray &data)
{
    imgtar::TarEntry e;
    e.name = name;
    e.data = data;
    return e;
}

void TestSamsungPlan::matchesByPitFilename()
{
    QTemporaryDir dir;
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("BL.tar.md5"),
                                    {tarEntry(QStringLiteral("spl.img"), QByteArray(3000, 'A')),
                                     tarEntry(QStringLiteral("sboot.bin"), QByteArray(5000, 'B'))});
    QVERIFY(!tar.isEmpty());

    QList<PitSpec> es = bootSbootNv();                       // BOOT/spl.img、SBOOT/sboot.bin、wfixnv2/nvitem.bin
    odin::PitTable pit;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));

    odin::SamsungPlan plan;
    QVERIFY2(odin::buildSamsungPlan({tar}, pit, plan, &err, QStringLiteral("包内 BL.tar.md5 的 PIT")), qPrintable(err));
    QCOMPARE(plan.pitSource, QStringLiteral("包内 BL.tar.md5 的 PIT"));
    QCOMPARE(plan.entries.size(), 2);                        // wfixnv2 的 nvitem.bin 不在包内
    QCOMPARE(plan.entries[0].partition, QStringLiteral("BOOT"));   // 顺序 = PIT 条目顺序
    QCOMPARE(plan.entries[0].imageFile, QStringLiteral("spl.img"));
    QCOMPARE(plan.entries[0].sizeBytes, quint64(3000));
    QCOMPARE(plan.entries[0].pit.identifier, quint32(80));
    QCOMPARE(plan.entries[0].matchRule, QStringLiteral("文件名精确匹配"));
    QCOMPARE(plan.entries[1].partition, QStringLiteral("SBOOT"));
    QCOMPARE(plan.totalBytes, quint64(8000));
    QCOMPARE(plan.files.size(), 1);
    QVERIFY(plan.files[0].verifyOk);
    QVERIFY(plan.files[0].md5HasFooter);
    // 偏移自洽：按 sourceOffset 从文件读回的字节 = 镜像内容
    QFile f(tar);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QVERIFY(f.seek(qint64(plan.entries[0].sourceOffset)));
    QCOMPARE(f.read(3000), QByteArray(3000, 'A'));
    // 缺镜像 → 有告警且指明了条目与文件名
    bool warned = false;
    for (const QString &w : std::as_const(plan.warnings))
        warned = warned || (w.contains(QStringLiteral("wfixnv2")) && w.contains(QStringLiteral("nvitem.bin")));
    QVERIFY(warned);
}

void TestSamsungPlan::skipsEntriesWithoutImageName()
{
    QTemporaryDir dir;
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("BL.tar.md5"),
                                    {tarEntry(QStringLiteral("spl.img"), QByteArray(3000, 'A'))});
    QList<PitSpec> es = bootSbootNv();
    es[1].flashFilename = QByteArray();                      // 未声明
    es[2].flashFilename = QByteArray("-");                   // 字面占位符
    odin::PitTable pit;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));

    odin::SamsungPlan plan;
    QVERIFY2(odin::buildSamsungPlan({tar}, pit, plan, &err), qPrintable(err));
    QCOMPARE(plan.entries.size(), 1);
    // 未声明的条目**不得**产生"缺镜像"告警（只有一条汇总）
    for (const QString &w : std::as_const(plan.warnings))
        QVERIFY2(!w.contains(QStringLiteral("SBOOT")), qPrintable(w));
    bool summary = false;
    for (const QString &w : std::as_const(plan.warnings))
        summary = summary || w.contains(QStringLiteral("未声明镜像文件名"));
    QVERIFY(summary);
}

void TestSamsungPlan::fallsBackToUniquePitEntry()
{
    QTemporaryDir dir;
    // 包内 .pit 名与 PIT 条目声明的不一致（真反例：J1POP3G.pit vs J1POP3G_LTN_OPEN.pit）
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("CSC.tar.md5"),
                                    {tarEntry(QStringLiteral("J1POP3G.pit"), QByteArray(5012, 'P'))});
    QList<PitSpec> es;
    PitSpec p; p.name = QByteArray("PIT"); p.identifier = 70; p.flashFilename = QByteArray("J1POP3G_LTN_OPEN.pit");
    es << p;
    odin::PitTable pit;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));

    odin::SamsungPlan plan;
    QVERIFY2(odin::buildSamsungPlan({tar}, pit, plan, &err), qPrintable(err));
    QCOMPARE(plan.entries.size(), 1);
    QCOMPARE(plan.entries[0].imageFile, QStringLiteral("J1POP3G.pit"));
    QCOMPARE(plan.entries[0].matchRule, QStringLiteral(".pit 唯一性回退"));
    bool warned = false;
    for (const QString &w : std::as_const(plan.warnings))
        warned = warned || w.contains(QStringLiteral("唯一"));
    QVERIFY(warned);

    // 多个 .pit 候选 → 不再回退（不擅自选一个）
    const QString tar2 = writeTarMd5(dir.path(), QStringLiteral("CSC2.tar.md5"),
                                     {tarEntry(QStringLiteral("A.pit"), QByteArray(100, 'x')),
                                      tarEntry(QStringLiteral("B.pit"), QByteArray(100, 'y'))});
    odin::SamsungPlan plan2;
    QString err2;
    QVERIFY(!odin::buildSamsungPlan({tar2}, pit, plan2, &err2));   // 一条都匹配不上 → 失败
    QVERIFY(!err2.isEmpty());
}

void TestSamsungPlan::warnsBothMismatchDirections()
{
    QTemporaryDir dir;
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("BL.tar.md5"),
                                    {tarEntry(QStringLiteral("spl.img"), QByteArray(3000, 'A')),
                                     tarEntry(QStringLiteral("extra.bin"), QByteArray(10, 'E'))});
    QList<PitSpec> es = bootSbootNv();                        // SBOOT/sboot.bin、wfixnv2/nvitem.bin 都不在包内
    odin::PitTable pit;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));
    odin::SamsungPlan plan;
    QVERIFY2(odin::buildSamsungPlan({tar}, pit, plan, &err), qPrintable(err));
    bool missingInPkg = false, missingInPit = false;
    for (const QString &w : std::as_const(plan.warnings)) {
        if (w.contains(QStringLiteral("不在所选包内"))) missingInPkg = true;
        if (w.contains(QStringLiteral("extra.bin"))) missingInPit = true;
    }
    QVERIFY(missingInPkg);
    QVERIFY(missingInPit);
}

void TestSamsungPlan::warnsWhenImageLargerThanPartition()
{
    QTemporaryDir dir;
    // 分区 1024 扇区 × 512 = 512 KiB；镜像 600000 字节 > 512 KiB
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("BL.tar.md5"),
                                    {tarEntry(QStringLiteral("spl.img"), QByteArray(600000, 'A'))});
    QList<PitSpec> es = bootSbootNv();
    es[0].blockCount = 1024;
    odin::PitTable pit;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));
    odin::SamsungPlan plan;
    QVERIFY2(odin::buildSamsungPlan({tar}, pit, plan, &err), qPrintable(err));
    bool warned = false;
    for (const QString &w : std::as_const(plan.warnings))
        warned = warned || w.contains(QStringLiteral("放不下"));
    QVERIFY(warned);
}

void TestSamsungPlan::summarisesSmallImages()
{
    QTemporaryDir dir;
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("BL.tar.md5"),
                                    {tarEntry(QStringLiteral("spl.img"), QByteArray(3000, 'A'))});
    QList<PitSpec> es = bootSbootNv();
    es[0].blockCount = 4096;                                  // 2 MiB 分区 vs 3000 字节镜像 → 小于
    odin::PitTable pit;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));
    odin::SamsungPlan plan;
    QVERIFY2(odin::buildSamsungPlan({tar}, pit, plan, &err), qPrintable(err));
    int perEntry = 0, summary = 0;
    for (const QString &w : std::as_const(plan.warnings)) {
        if (w.contains(QStringLiteral("放不下"))) ++perEntry;
        if (w.contains(QStringLiteral("小于分区"))) ++summary;
    }
    QCOMPARE(perEntry, 0);                                    // 小于 ≠ 逐条告警
    QCOMPARE(summary, 1);                                     // 只出一条汇总
}

void TestSamsungPlan::reportsMd5FooterState()
{
    QTemporaryDir dir;
    // 无校验行的包：verifyOk=false + 明确告警（不静默）
    const QString noFooter = writeTarMd5(dir.path(), QStringLiteral("nofooter.tar"),
                                         {tarEntry(QStringLiteral("spl.img"), QByteArray(3000, 'A'))}, false);
    QList<PitSpec> es = bootSbootNv();
    odin::PitTable pit;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));
    odin::SamsungPlan plan;
    QVERIFY2(odin::buildSamsungPlan({noFooter}, pit, plan, &err), qPrintable(err));
    QCOMPARE(plan.files.size(), 1);
    QVERIFY(!plan.files[0].md5HasFooter);
    QVERIFY(!plan.files[0].verifyOk);
    bool warned = false;
    for (const QString &w : std::as_const(plan.warnings))
        warned = warned || w.contains(QStringLiteral("校验"));
    QVERIFY(warned);

    // 校验不符的包：verifyOk=false + 告警（**不拒刷** —— 用户可能故意刷改包；门控是预览里的勾选框）
    QByteArray tar = imgtar::buildTar({tarEntry(QStringLiteral("spl.img"), QByteArray(3000, 'A'))});
    tar[600] = char(tar[600] ^ 0x01);
    const QByteArray bad = tar + QCryptographicHash::hash(QByteArray(tar.left(0)), QCryptographicHash::Md5).toHex()
                           + "  x.tar\n";
    const QString badPath = dir.path() + QStringLiteral("/bad.tar.md5");
    {
        QFile f(badPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        QCOMPARE(f.write(bad), qint64(bad.size()));
    }
    odin::SamsungPlan plan2;
    QString err2;
    QVERIFY2(odin::buildSamsungPlan({badPath}, pit, plan2, &err2), qPrintable(err2));
    QVERIFY(plan2.files[0].md5HasFooter);
    QVERIFY(!plan2.files[0].verifyOk);
    bool warned2 = false;
    for (const QString &w : std::as_const(plan2.warnings))
        warned2 = warned2 || w.contains(QStringLiteral("MD5"));
    QVERIFY(warned2);
}

void TestSamsungPlan::loadsPitFromPackage()
{
    QTemporaryDir dir;
    const QByteArray pitBytes = buildPit(bootSbootNv());
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("CSC.tar.md5"),
                                    {tarEntry(QStringLiteral("J1POP3G.pit"), pitBytes),
                                     tarEntry(QStringLiteral("cache.img"), QByteArray(4096, 'C'))});
    odin::PitTable pit;
    QString pitPath, err;
    QVERIFY2(odin::loadPitFromPackage({tar}, pit, &pitPath, &err), qPrintable(err));
    QCOMPARE(pit.entries.size(), 3);
    QCOMPARE(pit.entries[0].partitionName, QStringLiteral("BOOT"));
    QVERIFY(pitPath.contains(QStringLiteral("J1POP3G.pit")));
}

void TestSamsungPlan::refusesEmptyResult()
{
    QTemporaryDir dir;
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("X.tar.md5"),
                                    {tarEntry(QStringLiteral("nothing.bin"), QByteArray(10, 'x'))});
    QList<PitSpec> es = bootSbootNv();
    odin::PitTable pit;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(es), pit, &err), qPrintable(err));
    odin::SamsungPlan plan;
    err.clear();
    QVERIFY(!odin::buildSamsungPlan({tar}, pit, plan, &err));      // 一条都匹配不上 → 拒
    QVERIFY(!err.isEmpty());
    // 空输入
    err.clear();
    QVERIFY(!odin::buildSamsungPlan({}, pit, plan, &err));
    QVERIFY(!err.isEmpty());
    // 空 PIT
    err.clear();
    odin::PitTable empty;
    QVERIFY(!odin::buildSamsungPlan({tar}, empty, plan, &err));
    QVERIFY(!err.isEmpty());
}

void TestSamsungPlan::rejectsWhenNoPitInPackage()
{
    QTemporaryDir dir;
    const QString tar = writeTarMd5(dir.path(), QStringLiteral("BL.tar.md5"),
                                    {tarEntry(QStringLiteral("spl.img"), QByteArray(3000, 'A'))});
    odin::PitTable pit;
    QString pitPath, err;
    QVERIFY(!odin::loadPitFromPackage({tar}, pit, &pitPath, &err));
    QVERIFY(!err.isEmpty());
}

// ---- 真包（3 个 SM-J110H 包 + CSC 内的 J1POP3G.pit）----
void TestSamsungPlan::realPackagesBuildPlan()
{
    const QString root = QString::fromLatin1(ODIN_SAMPLES_DIR);
    const QString dir = root + QStringLiteral("/sm-j110h");
    if (root.isEmpty() || !QDir(dir).exists()) {
#if ODIN_SAMPLES_REQUIRED
        QFAIL("真样本目录不存在，但本次构建要求真样本（ODIN_SAMPLES_REQUIRED=ON）");
#else
        QSKIP("真样本目录不存在（reference/ 为 gitignored）");
#endif
    }
    const QString bl = dir + QStringLiteral("/BL_J110HXXU0AQJ1_CL1240844_QB15258762_REV02_user_low_ship.tar.md5");
    const QString csc = dir + QStringLiteral("/CSC_ODD_J110HODD0AQF2_CL1214683_QB14017971_REV02_user_low_ship.tar.md5");
    const QString modem = dir + QStringLiteral("/MODEM_J110HDDU0AQF1_CL2068124_QB6737246_REV00.tar.md5");
    QVERIFY(QFileInfo::exists(bl) && QFileInfo::exists(csc) && QFileInfo::exists(modem));

    // ① PIT 从 CSC 包内取（真包内名 J1POP3G.pit；条目声明的是 J1POP3G_LTN_OPEN.pit → 走 .pit 唯一性回退）
    odin::PitTable pit;
    QString pitPath, err;
    QVERIFY2(odin::loadPitFromPackage({bl, csc, modem}, pit, &pitPath, &err), qPrintable(err));
    QCOMPARE(pit.entries.size(), 30);

    // ② 三个包一起构建计划：10 个镜像全部匹配（4 BL + 3 CSC + 3 MODEM）
    odin::SamsungPlan plan;
    QVERIFY2(odin::buildSamsungPlan({bl, csc, modem}, pit, plan, &err,
                                    QStringLiteral("包内 J1POP3G.pit")), qPrintable(err));
    QCOMPARE(plan.entries.size(), 10);
    QCOMPARE(plan.totalBytes, quint64(32768 + 1573888 + 1573888 + 634880
                                     + 5012 + 18903312 + 8057072
                                     + 8388608 + 2097152 + 181868));
    // 三个包都带校验行且校验通过（含 MODEM 的 "␣*" 变体 —— Task 1 的修复在此被真实包验证）
    QCOMPARE(plan.files.size(), 3);
    for (const odin::SamsungPlanFile &f : std::as_const(plan.files)) {
        QVERIFY2(f.md5HasFooter, qPrintable(QFileInfo(f.path).fileName()));
        QVERIFY2(f.verifyOk, qPrintable(QFileInfo(f.path).fileName()));
    }
    // 分区 ↔ 镜像配对逐个核对（真数据；10 条 = 4 BL + 3 CSC + 3 MODEM）
    QMap<QString, QString> expect;
    expect[QStringLiteral("BOOT")]     = QStringLiteral("spl.img");
    expect[QStringLiteral("SBOOT")]    = QStringLiteral("sboot.bin");
    expect[QStringLiteral("SBOOT2")]   = QStringLiteral("sboot2.bin");
    expect[QStringLiteral("PARAM")]    = QStringLiteral("param.lfs");
    expect[QStringLiteral("PIT")]      = QStringLiteral("J1POP3G.pit");   // 反例：条目声明 J1POP3G_LTN_OPEN.pit
    expect[QStringLiteral("CSC")]      = QStringLiteral("cache.img");
    expect[QStringLiteral("HIDDEN")]   = QStringLiteral("hidden.img");
    expect[QStringLiteral("MODEM")]    = QStringLiteral("SPRDCP.img");
    expect[QStringLiteral("WDSP")]     = QStringLiteral("SPRDDSP.img");
    expect[QStringLiteral("wfixnv2")]  = QStringLiteral("nvitem.bin");
    for (const odin::SamsungPlanEntry &e : std::as_const(plan.entries)) {
        QVERIFY2(expect.contains(e.partition), qPrintable(e.partition));
        QCOMPARE(e.imageFile, expect.value(e.partition));
        QVERIFY(e.sourceOffset > 0);
        QCOMPARE(e.pit.partitionName, e.partition);
    }
    // BOOT2/spl2.img 不在任何包内 → 必须有一条"不在所选包内"的告警
    bool boot2Warned = false;
    for (const QString &w : std::as_const(plan.warnings))
        boot2Warned = boot2Warned || w.contains(QStringLiteral("spl2.img"));
    QVERIFY(boot2Warned);
    // CSC 包内的 J1POP3G.pit 自身也被当作镜像（PIT 条目的 flashFilename 是 J1POP3G_LTN_OPEN.pit）
    bool pitMatched = false;
    for (const odin::SamsungPlanEntry &e : std::as_const(plan.entries))
        pitMatched = pitMatched || e.matchRule == QStringLiteral(".pit 唯一性回退");
    QVERIFY(pitMatched);
    // 分区大小核对：真数据里只有 MODEM(SPRDCP.img) 恰好等于分区大小，其余都小于 → 只应有一条汇总告警
    int summary = 0;
    for (const QString &w : std::as_const(plan.warnings))
        summary += w.contains(QStringLiteral("小于分区")) ? 1 : 0;
    QCOMPARE(summary, 1);
    // 镜像 > 分区：真数据里一条都不该有
    for (const QString &w : std::as_const(plan.warnings))
        QVERIFY2(!w.contains(QStringLiteral("放不下")), qPrintable(w));
}

QTEST_APPLESS_MAIN(TestSamsungPlan)
#include "test_samsung_plan.moc"
