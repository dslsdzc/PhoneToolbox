// tests/test_mtk_da_file.cpp
//
// AllInOne DA 解析：合成夹具（覆盖 P1-P12 的边界）+ 真样本对拍
// （6 个真实文件 / 161 条目，与 reference/mtk-samples/da_parse_report.json 逐字段一致）。
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <utility>   // std::as_const（遍历 Qt 容器，不得用 qAsConst）

#include "core/modes/mtk_da_file.h"
#include "mtk_test_helpers.h"

class TestMtkDaFile : public QObject
{
    Q_OBJECT
private slots:
    void parsesHeaderAndEntries();
    void detectsV6ByMarkerNotBanner();
    void probesOldFormatWithCrossValidation();
    void rejectsBadInputs();
    void rejectsZeroCountDa();
    void toleratesRealWorldShapes();
    // 条目选择（selectDaEntry）：规则 1-5
    void selectsByVersionFilterAndHardcodedRegions();
    void skipsCandidatesWithEmptyDaRegions();
    void reportsNoCandidateWithAvailableCodes();
    void breaksVersionTiesByTakingFirst();
    void rejectsZeroDacode();
    void rejectsOversizedRegionSlice();
    // 真样本（reference/mtk-samples/，缺失时 SKIP；验证跑带 -DMTK_SAMPLES_REQUIRED=ON）
    void realSamplesMatchIndependentReport();
    void realSamplesSelectExpectedEntries();
    void realSamplesCoverCollisionAndVersionFilter();
};

using namespace mtktest;

static EntrySpec entryWith3Regions(quint16 hwCode = 0x6765)
{
    EntrySpec e;
    e.hwCode = hwCode;
    RegionSpec r0; r0.startAddr = 0x200000; r0.len = 16;
    RegionSpec r1; r1.startAddr = 0x2007000; r1.len = 16;
    RegionSpec r2; r2.startAddr = 0x80000000; r2.len = 16; r2.sigLen = 0x100;
    e.regions << r0 << r1 << r2;
    return e;
}

void TestMtkDaFile::parsesHeaderAndEntries()
{
    const QByteArray data = buildDa({entryWith3Regions(), entryWith3Regions(0x6752)});
    mtkbrom::DaFile f;
    QString err;
    QVERIFY2(mtkbrom::parseDaFile(data, f, &err), qPrintable(err));
    QCOMPARE(f.count, quint32(2));
    QCOMPARE(f.entries.size(), 2);
    QVERIFY(!f.isV6);
    QVERIFY(!f.oldFormat);
    const mtkbrom::DaEntry &e = f.entries.at(0);
    QCOMPARE(e.magic, quint16(0xDADA));
    QCOMPARE(e.hwCode, quint16(0x6765));
    QCOMPARE(e.hwSubCode, quint16(0x8A00));
    QCOMPARE(e.hwVersion, quint16(0xCA00));
    QCOMPARE(e.pagesize, quint16(4096));
    QCOMPARE(e.regionCount, quint16(3));
    QCOMPARE(e.regions.size(), 3);
    QCOMPARE(e.regions.at(2).startAddr, quint32(0x80000000));
    QCOMPARE(e.regions.at(2).sigLen, quint32(0x100));
    // m_buf 指向真实载荷：按它切片能读回 builder 写入的填充字节
    const mtkbrom::DaRegion &r0 = e.regions.at(0);
    QCOMPARE(data.mid(int(r0.fileOffset), int(r0.len)), QByteArray(16, char(1)));
    QCOMPARE(f.entries.at(1).hwCode, quint16(0x6752));
}

void TestMtkDaFile::detectsV6ByMarkerNotBanner()
{
    // P10：判别式是**负向**的（含 "MTK_DA_v6" 才算 V6）；横幅版本号不得用于逻辑
    mtkbrom::DaFile f;
    QString err;
    QVERIFY2(mtkbrom::parseDaFile(buildDa({entryWith3Regions()}, /*v6=*/true), f, &err), qPrintable(err));
    QVERIFY(f.isV6);
    // 横幅写着 v5.1624 但**不含** "MTK_DA_v6" → 仍是老代（真实 iot.bin 就是这种）
    QVERIFY2(mtkbrom::parseDaFile(
                 buildDa({entryWith3Regions()}, false, false,
                         QByteArray("MTK_AllInOne_DA_v5.1624.00.00_viperbjk")), f, &err),
             qPrintable(err));
    QVERIFY(!f.isV6);
}

void TestMtkDaFile::probesOldFormatWithCrossValidation()
{
    mtkbrom::DaFile f;
    QString err;
    // P8：0xD8 老格式**无真实样本** —— 只合成夹具，用例注释里如实标注。
    // **必须 2 条目**：探测点 0x6C+0xD8 要正好落在 entry[1] 的 magic 上（单条目时该处是尾部载荷，
    // 探不到 0xDADA —— brief 初版给的单条目夹具会让本用例必红）。P4 的探测语义不变。
    // （`entryWith3Regions(hwCode = 0x6765)` 是 brief 里已有的夹具；第二条换个 hwCode 便于区分）
    QVERIFY(!mtkbrom::parseDaFile(buildDa({entryWith3Regions(), entryWith3Regions(0x6752)}, false,
                                          /*oldFormat=*/true),
                                  f, &err));
    // 探测结果**照填**（可观测），解析**明确拒绝**（不按新格式偏移硬解老格式）
    QVERIFY(f.oldFormat);
    QCOMPARE(f.count, quint32(2));
    QVERIFY(f.entries.isEmpty());
    QVERIFY2(err.contains(QStringLiteral("0xD8")) || err.contains(QStringLiteral("老格式")), qPrintable(err));

    // 反向：新格式（0xDC）文件不许被判成老格式
    mtkbrom::DaFile ok;
    err.clear();
    QVERIFY2(mtkbrom::parseDaFile(buildDa({entryWith3Regions(), entryWith3Regions(0x6752)}), ok, &err),
             qPrintable(err));
    QVERIFY(!ok.oldFormat);
    QCOMPARE(ok.entries.size(), 2);
}

void TestMtkDaFile::rejectsBadInputs()
{
    mtkbrom::DaFile f;
    QString err;
    // 太短
    QVERIFY(!mtkbrom::parseDaFile(QByteArray(0x40, '\0'), f, &err));
    QVERIFY(!err.isEmpty());
    // 头部标记不符
    QByteArray noMagic = buildDa({entryWith3Regions()});
    noMagic.replace(0, 4, "XXXX");
    err.clear();
    QVERIFY(!mtkbrom::parseDaFile(noMagic, f, &err));
    QVERIFY(!err.isEmpty());
    // P9：count_da 与 EOF 不符（声称 1000 条，文件只够 1 条）
    QByteArray badCount = buildDa({entryWith3Regions()});
    putLe32(badCount, 0x68, 1000);
    err.clear();
    QVERIFY(!mtkbrom::parseDaFile(badCount, f, &err));
    QVERIFY2(err.contains(QStringLiteral("截断")), qPrintable(err));   // 钉住具体判据（别写 `|| !err.isEmpty()` 这种恒真兜底）
    // P11：m_buf + m_len 越界（把 region[0] 的 m_len 改到超大）
    QByteArray oob = buildDa({entryWith3Regions()});
    putLe32(oob, 0x6C + 0x14 + 4, 0x7FFFFFFF);
    err.clear();
    QVERIFY(!mtkbrom::parseDaFile(oob, f, &err));
    QVERIFY(!err.isEmpty());
    // 条目 magic 不是 0xDADA（条目边界唯一的锚点 → fail-closed）
    QByteArray badMagic = buildDa({entryWith3Regions()});
    putLe16(badMagic, 0x6C, 0x0000);
    err.clear();
    QVERIFY(!mtkbrom::parseDaFile(badMagic, f, &err));
    QVERIFY(!err.isEmpty());
}

void TestMtkDaFile::rejectsZeroCountDa()
{
    // count_da == 0：0 条目的 DA 文件没有任何可用条目 —— 明确拒绝
    // （该判据原比实现宽：独立基准 parse_da.py:110 同样把 count_da==0 记为 error）
    QByteArray zeroCount = buildDa({entryWith3Regions()});
    putLe32(zeroCount, 0x68, 0);
    mtkbrom::DaFile f;
    QString err;
    QVERIFY(!mtkbrom::parseDaFile(zeroCount, f, &err));
    QVERIFY2(err.contains(QStringLiteral("0 个条目")), qPrintable(err));
}

void TestMtkDaFile::toleratesRealWorldShapes()
{
    // P1/P7：pagesize 0/1、m_len==0、hw_code==0 占位条目都必须容忍（真实文件里都有）
    EntrySpec placeholder;
    placeholder.hwCode = 0x0000;
    placeholder.pagesize = 0;
    RegionSpec zeroLen; zeroLen.len = 0; zeroLen.startAddr = 0;
    placeholder.regions << zeroLen;
    EntrySpec normal = entryWith3Regions();
    normal.pagesize = 1;

    mtkbrom::DaFile f;
    QString err;
    QVERIFY2(mtkbrom::parseDaFile(buildDa({placeholder, normal}), f, &err), qPrintable(err));
    QCOMPARE(f.entries.size(), 2);
    QCOMPARE(f.entries.at(0).hwCode, quint16(0));
    QCOMPARE(f.entries.at(0).pagesize, quint16(0));
    QCOMPARE(f.entries.at(0).regions.at(0).len, quint32(0));
    QCOMPARE(f.entries.at(1).pagesize, quint16(1));
}

void TestMtkDaFile::selectsByVersionFilterAndHardcodedRegions()
{
    EntrySpec old1 = entryWith3Regions(0x6765);
    old1.hwVersion = 0xCA00; old1.swVersion = 0x0000;
    EntrySpec new1 = entryWith3Regions(0x6765);
    new1.hwVersion = 0xCB00; new1.swVersion = 0x0000;   // 更高的 hw_ver
    mtkbrom::DaFile f;
    QString err;
    QVERIFY2(mtkbrom::parseDaFile(buildDa({old1, new1}), f, &err), qPrintable(err));

    QStringList warn;
    mtkbrom::DaSelection sel;
    // deviceHwVer=0/0 → 版本维**全部旁路**（两条都满足）→ **上游 first-match：取文件顺序首个**（条目[0]，0xCA00）
    QVERIFY2(mtkbrom::selectDaEntry(f, 0x6765, 0, 0, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.entryIndex, 0);
    QCOMPARE(sel.entry.hwVersion, quint16(0xCA00));
    QVERIFY2(!warn.isEmpty(), "并列（2 条满足）必须留告警（取首个是上游语义，但不能静默）");
    // region 硬编码：da1=region[1]、da2=region[2]（**不是** region[0]）
    QCOMPARE(sel.da1.startAddr, quint32(0x2007000));
    QCOMPARE(sel.da2.startAddr, quint32(0x80000000));
    QCOMPARE(sel.da2.sigLen, quint32(0x100));
    QCOMPARE(sel.da1Bytes.size(), 16);
    QCOMPARE(sel.da2Bytes.size(), 16);
    // da2 切片 = 文件里 region[2] 的载荷（**保留签名**：本模块不裁剪，长度即 m_len）；
    // 填充字节按**条目下标**变（builder 写 char(1 + i*3 + r)）—— 命中条目[0] ⇒ 1 + 0*3 + 2
    QCOMPARE(sel.da2Bytes, QByteArray(16, char(1 + 0 * 3 + 2)));

    // 版本过滤生效：deviceHwVer=0xCA00 → 第二条 0xCB00 > 设备值被滤掉 → 首个**满足者**仍是条目[0]
    warn.clear();
    QVERIFY2(mtkbrom::selectDaEntry(f, 0x6765, 0xCA00, 0, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.entryIndex, 0);
    QCOMPARE(sel.entry.hwVersion, quint16(0xCA00));

    // 但上面那条在 first-match 下**不判别**"有没有做版本过滤"（不过滤也是条目[0]）——
    // 判别的构造是**把高版本条目放到文件顺序前面**：不过滤就会选到它。
    mtkbrom::DaFile fHi;
    err.clear();
    QVERIFY2(mtkbrom::parseDaFile(buildDa({new1, old1}), fHi, &err), qPrintable(err));
    warn.clear();
    QVERIFY2(mtkbrom::selectDaEntry(fHi, 0x6765, 0xCA00, 0, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.entryIndex, 1);                          // 条目[0] 的 0xCB00 > 设备 0xCA00 → 被滤掉
    QCOMPARE(sel.entry.hwVersion, quint16(0xCA00));

    // swVersion 维同理（deviceSwVer 过滤；hw 维给 0 旁路，避免两个维度互相掩盖）
    EntrySpec hiSw = entryWith3Regions(0x6765);
    hiSw.swVersion = 0xE201;
    EntrySpec loSw = entryWith3Regions(0x6765);
    loSw.swVersion = 0xE100;
    mtkbrom::DaFile fSw;
    err.clear();
    QVERIFY2(mtkbrom::parseDaFile(buildDa({hiSw, loSw}), fSw, &err), qPrintable(err));
    warn.clear();
    QVERIFY2(mtkbrom::selectDaEntry(fSw, 0x6765, 0, 0xE100, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.entryIndex, 1);                          // 条目[0] 的 sw 0xE201 > 设备 0xE100 → 被滤掉
    QCOMPARE(sel.entry.swVersion, quint16(0xE100));
}

void TestMtkDaFile::skipsCandidatesWithEmptyDaRegions()
{
    // 真实形态（da_parse_report.json 的 warnings）：大量条目 region[1].m_len == 0
    EntrySpec emptyDa1 = entryWith3Regions(0x6765);
    emptyDa1.regions[1].len = 0;
    EntrySpec good = entryWith3Regions(0x6765);
    good.hwVersion = 0xCA00;

    mtkbrom::DaFile f;
    QString err;
    QVERIFY2(mtkbrom::parseDaFile(buildDa({emptyDa1, good}), f, &err), qPrintable(err));
    QStringList warn;
    mtkbrom::DaSelection sel;
    QVERIFY2(mtkbrom::selectDaEntry(f, 0x6765, 0, 0, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.entryIndex, 1);                       // 跳过了 DA1 为空的候选
    bool noted = false;
    for (const QString &w : std::as_const(warn))
        noted = noted || w.contains(QStringLiteral("region[1]"));
    QVERIFY(noted);

    // 只有空 region 候选 → 失败（不得"上传 0 字节后静默成功"）
    mtkbrom::DaFile only;
    QVERIFY2(mtkbrom::parseDaFile(buildDa({emptyDa1}), only, &err), qPrintable(err));
    err.clear();
    QVERIFY(!mtkbrom::selectDaEntry(only, 0x6765, 0, 0, &warn, sel, &err));
    QVERIFY(!err.isEmpty());

    // 规则 3 的另两个子条件（自审补：上面只覆盖了 region[1].len == 0）：
    // region 数不足 3 与 region[2].len == 0 同样必须跳过并记明原因。
    // 前两条的版本**故意高于** good2：first-match 下若不跳过它们，选中的就是它们 → entryIndex 断言才有判别力。
    EntrySpec twoRegions;                       // 只有 region[0]/[1]：DA2 缺位
    twoRegions.hwVersion = 0xCB00;
    RegionSpec r0; r0.startAddr = 0x200000;
    RegionSpec r1; r1.startAddr = 0x2007000;
    twoRegions.regions << r0 << r1;
    EntrySpec emptyDa2 = entryWith3Regions(0x6765);
    emptyDa2.hwVersion = 0xCC00;
    emptyDa2.regions[2].len = 0;
    EntrySpec good2 = entryWith3Regions(0x6765);   // hwVersion 0xCA00（三条里最低）

    mtkbrom::DaFile f2;
    err.clear();
    QVERIFY2(mtkbrom::parseDaFile(buildDa({twoRegions, emptyDa2, good2}), f2, &err), qPrintable(err));
    warn.clear();
    QVERIFY2(mtkbrom::selectDaEntry(f2, 0x6765, 0, 0, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.entryIndex, 2);
    QCOMPARE(sel.entry.hwVersion, quint16(0xCA00));
    bool notedCount = false, notedR2 = false;
    for (const QString &w : std::as_const(warn)) {
        notedCount = notedCount || w.contains(QStringLiteral("region 数不足"));
        notedR2 = notedR2 || w.contains(QStringLiteral("region[2]"));
    }
    QVERIFY(notedCount);
    QVERIFY(notedR2);
}

void TestMtkDaFile::rejectsZeroDacode()
{
    // P7：hw_code == 0 是占位条目，**不得**被选中。dacode 由芯片表给出（默认 = 设备 hw_code），
    // 取不到时是 0 —— 若放行就会与占位条目相撞。上游在装载阶段就剔除它们（daconfig.py:200
    // `if da.hw_code != 0:`）→ 本层对 dacode == 0 明确拒绝。
    if (!mtktest::sampleFileAvailable(QStringLiteral("MTK_AllInOne_DA_iot.bin"))) {
#if MTK_SAMPLES_REQUIRED
        QFAIL("真样本缺失，但本次构建要求真样本（MTK_SAMPLES_REQUIRED=ON）");
#else
        QSKIP("真样本缺失（reference/ 为 gitignored）");
#endif
    }
    const QString dir = mtktest::samplesDir();
    QFile fi(dir + QStringLiteral("/MTK_AllInOne_DA_iot.bin"));
    QVERIFY(fi.open(QIODevice::ReadOnly));
    mtkbrom::DaFile iot;
    QString err;
    QVERIFY2(mtkbrom::parseDaFile(fi.readAll(), iot, &err), qPrintable(err));
    // 该文件里**确实有** hw_code == 0 的占位条目（否则本用例是空转）
    bool hasPlaceholder = false;
    for (const mtkbrom::DaEntry &e : std::as_const(iot.entries))
        hasPlaceholder = hasPlaceholder || e.hwCode == 0;
    QVERIFY2(hasPlaceholder, "iot 文件里应有 hw_code == 0 的占位条目");

    QStringList warn;
    mtkbrom::DaSelection sel;
    QVERIFY(!mtkbrom::selectDaEntry(iot, 0, 0, 0, &warn, sel, &err));
    QVERIFY2(err.contains(QStringLiteral("dacode == 0")), qPrintable(err));
    QCOMPARE(sel.entryIndex, -1);      // 失败时 out 保持默认（不返回半份选择）
}

void TestMtkDaFile::rejectsOversizedRegionSlice()
{
    // >2GiB 的 region 在 int 截断下会静默给出空切片（"上传 0 字节后静默成功"家族）。
    // 真文件造不出来（真实样本 1–4 MiB）→ 手工构造 DaFile 直接喂选择层是正当手段。
    mtkbrom::DaFile f;
    f.raw = QByteArray(64, '\0');
    mtkbrom::DaEntry e;
    e.hwCode = 0x6765;
    e.regionCount = 3;
    mtkbrom::DaRegion r0, r1, r2;
    r2.len = 16;                       // region[2] 非空（否则先被"空 region"规则跳过，测不到 >2GiB 守卫）
    r1.len = 0xFFFFFFFF;               // > INT_MAX（region[1] = DA1）
    e.regions << r0 << r1 << r2;
    f.entries << e;

    QStringList warn;
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY(!mtkbrom::selectDaEntry(f, 0x6765, 0, 0, &warn, sel, &err));
    QVERIFY2(err.contains(QStringLiteral("int 范围")), qPrintable(err));
    QCOMPARE(sel.entryIndex, -1);
}

void TestMtkDaFile::reportsNoCandidateWithAvailableCodes()
{
    mtkbrom::DaFile f;
    QString err;
    QVERIFY2(mtkbrom::parseDaFile(buildDa({entryWith3Regions(0x6765), entryWith3Regions(0x6752)}), f, &err),
             qPrintable(err));
    QStringList warn;
    mtkbrom::DaSelection sel;
    err.clear();
    QVERIFY(!mtkbrom::selectDaEntry(f, 0x9999, 0, 0, &warn, sel, &err));
    QVERIFY2(err.contains(QStringLiteral("0x6765")), qPrintable(err));   // 列出可用 hw_code
    QVERIFY2(err.contains(QStringLiteral("0x6752")), qPrintable(err));
}

void TestMtkDaFile::breaksVersionTiesByTakingFirst()
{
    // 规则 4：**上游 first-match**（daconfig.py:207-218 `if self.da_loader is None` 取文件顺序首个满足者，
    // 不是"取最大版本"——Important-1 已核上游）→ 多条满足时取**首个** + warnings 记明
    // （P2 的唯一键是 5 元组含 pagesize，本层不做 pagesize 匹配）。
    // 自审补：这条分支若无用例，就是没跑过的代码。
    EntrySpec lowPg = entryWith3Regions(0x6765);   // 前四字段与下一条完全相同，只有 pagesize 不同
    lowPg.pagesize = 0;
    EntrySpec highPg = entryWith3Regions(0x6765);
    highPg.pagesize = 1;

    mtkbrom::DaFile f;
    QString err;
    QVERIFY2(mtkbrom::parseDaFile(buildDa({lowPg, highPg}), f, &err), qPrintable(err));
    QStringList warn;
    mtkbrom::DaSelection sel;
    QVERIFY2(mtkbrom::selectDaEntry(f, 0x6765, 0, 0, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.entryIndex, 0);                     // 并列 → 第一条
    QCOMPARE(sel.entry.pagesize, quint16(0));
    bool noted = false;
    for (const QString &w : std::as_const(warn))
        noted = noted || (w.contains(QStringLiteral("5 元组")) && w.contains(QStringLiteral("pagesize")));
    QVERIFY(noted);
    // 两条目的载荷填充不同（builder 按条目序号）：切片来自**第一条**，不是碰巧相等的第二条
    QCOMPARE(sel.da1Bytes, QByteArray(16, char(1 + 0 * 3 + 1)));
}

void TestMtkDaFile::realSamplesMatchIndependentReport()
{
    if (!mtktest::sampleFileAvailable(QStringLiteral("da_parse_report.json"))) {
#if MTK_SAMPLES_REQUIRED
        QFAIL("真样本目录不存在，但本次构建要求真样本（MTK_SAMPLES_REQUIRED=ON）");
#else
        QSKIP("真样本目录不存在（reference/ 为 gitignored）");
#endif
    }
    const QString dir = mtktest::samplesDir();
    // 独立解析器产物（由 reference/mtk-samples/parse_da.py 生成，非本模块产出）
    QFile jf(dir + QStringLiteral("/da_parse_report.json"));
    QVERIFY2(jf.open(QIODevice::ReadOnly), "缺 da_parse_report.json（样本侦察产物）");
    const QJsonArray report = QJsonDocument::fromJson(jf.readAll()).array();
    QVERIFY2(report.size() >= 6, qPrintable(QStringLiteral("报告条目文件数 %1").arg(report.size())));

    int totalEntries = 0;
    for (const QJsonValue &fv : report) {
        const QJsonObject fileObj = fv.toObject();
        const QString path = dir + QLatin1Char('/') + fileObj.value("file").toString();
        QFile df(path);
        QVERIFY2(df.open(QIODevice::ReadOnly), qPrintable(path));
        const QByteArray data = df.readAll();
        // 与报告独立算出的 sha256 比对（证明读的是同一个文件）
        QCOMPARE(fileObj.value("sha256").toString(),
                 QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()));

        mtkbrom::DaFile f;
        QString err;
        QVERIFY2(mtkbrom::parseDaFile(data, f, &err), qPrintable(path + ": " + err));
        QCOMPARE(f.count, quint32(fileObj.value("count_da").toInt()));
        QVERIFY(f.isV6 == fileObj.value("v6_flag_mtkclient").toBool());
        QCOMPARE(f.oldFormat, fileObj.value("old_ldr").toBool());

        const QJsonArray entries = fileObj.value("entries").toArray();
        QCOMPARE(f.entries.size(), entries.size());
        for (int i = 0; i < entries.size(); ++i) {
            const QJsonObject eo = entries.at(i).toObject();
            const mtkbrom::DaEntry &e = f.entries.at(i);
            QCOMPARE(int(e.magic), eo.value("magic").toInt());
            QCOMPARE(int(e.hwCode), eo.value("hw_code").toInt());
            QCOMPARE(int(e.hwSubCode), eo.value("hw_sub_code").toInt());
            QCOMPARE(int(e.hwVersion), eo.value("hw_version").toInt());
            QCOMPARE(int(e.swVersion), eo.value("sw_version").toInt());
            QCOMPARE(int(e.pagesize), eo.value("pagesize").toInt());
            QCOMPARE(int(e.entryRegionIndex), eo.value("entry_region_index").toInt());
            QCOMPARE(int(e.regionCount), eo.value("entry_region_count").toInt());
            const QJsonArray regions = eo.value("regions").toArray();
            QCOMPARE(e.regions.size(), regions.size());
            for (int r = 0; r < regions.size(); ++r) {
                const QJsonObject ro = regions.at(r).toObject();
                QCOMPARE(int(e.regions.at(r).fileOffset), ro.value("m_buf").toInt());
                QCOMPARE(int(e.regions.at(r).len), ro.value("m_len").toInt());
                // 注：m_start_addr / m_start_offset 常用 0xf1000000 一类值（**超 INT_MAX**）——
                // QJsonValue::toInt() 对越界值静默返回 0（Qt6），必须走 toDouble 再转 quint32。
                QCOMPARE(e.regions.at(r).startAddr,
                         quint32(ro.value("m_start_addr").toDouble()));
                QCOMPARE(e.regions.at(r).startOffset,
                         quint32(ro.value("m_start_offset").toDouble()));
                QCOMPARE(int(e.regions.at(r).sigLen), ro.value("m_sig_len").toInt());
            }
            ++totalEntries;
        }
    }
    // 样本侦察实测：161 条；少于 150 说明样本或报告有问题（不静默放过）
    QVERIFY2(totalEntries >= 150, qPrintable(QStringLiteral("对拍条目数 %1").arg(totalEntries)));
}

void TestMtkDaFile::realSamplesSelectExpectedEntries()
{
    // 按文件判可用性（与 T1 一致）：目录在而样本缺失时 SKIP，不是误 FAIL
    if (!mtktest::sampleFileAvailable(QStringLiteral("MTK_DA_V5.bin"))
        || !mtktest::sampleFileAvailable(QStringLiteral("MTK_DA_V6.bin"))) {
#if MTK_SAMPLES_REQUIRED
        QFAIL("真样本缺失，但本次构建要求真样本（MTK_SAMPLES_REQUIRED=ON）");
#else
        QSKIP("真样本缺失（reference/ 为 gitignored）");
#endif
    }
    const QString dir = mtktest::samplesDir();
    QFile f5(dir + QStringLiteral("/MTK_DA_V5.bin"));
    QFile f6(dir + QStringLiteral("/MTK_DA_V6.bin"));
    QVERIFY(f5.open(QIODevice::ReadOnly));
    QVERIFY(f6.open(QIODevice::ReadOnly));
    const QByteArray v5Data = f5.readAll();
    const QByteArray v6Data = f6.readAll();
    mtkbrom::DaFile v5, v6;
    QString err;
    QVERIFY2(mtkbrom::parseDaFile(v5Data, v5, &err), qPrintable(err));
    QVERIFY2(mtkbrom::parseDaFile(v6Data, v6, &err), qPrintable(err));
    QVERIFY(!v5.isV6);
    QVERIFY(v6.isV6);                       // V6 文件含 "MTK_DA_v6"（实测偏移 0x20）

    QStringList warn;
    mtkbrom::DaSelection sel;
    // V5 的 0x6752：DA2 地址实测 0x40000000（现代老平台），且**不是** XML
    QVERIFY2(mtkbrom::selectDaEntry(v5, 0x6752, 0, 0, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.da2.startAddr, quint32(0x40000000));
    QVERIFY(!sel.isXmlForced);
    QVERIFY(sel.da1.len > 0);
    QVERIFY(sel.da2.len > 0);
    QCOMPARE(sel.da1Bytes.size(), int(sel.da1.len));
    // 切片内容 = 文件 [m_buf, +m_len)；数字取自**独立解析器报告**（entry[16]：region[0] =
    // m_buf 4428368/m_len 624，region[1] = m_buf 4429000/m_len 87480）——按 region[0] 取会拿到 624 字节，对不上
    QCOMPARE(sel.da1.fileOffset, quint32(4429000));
    QCOMPARE(sel.da1.len, quint32(87480));
    QCOMPARE(sel.da1Bytes, v5Data.mid(4429000, 87480));
    // LEGACY 语义：da2Bytes **保留尾部签名**（m_len 已含 sig_len，绝不裁）
    QCOMPARE(sel.da2Bytes.size(), int(sel.da2.len));

    // V6 的 0x907：强制 XML（本期 LEGACY 不实现 → 由上层报"需 XML 协议"）
    warn.clear();
    QVERIFY2(mtkbrom::selectDaEntry(v6, 0x907, 0, 0, &warn, sel, &err), qPrintable(err));
    QVERIFY(sel.isXmlForced);
    QCOMPARE(sel.da2.startAddr, quint32(0x40000000));
}

void TestMtkDaFile::realSamplesCoverCollisionAndVersionFilter()
{
    // 自审补：真样本上的 **5 元组碰撞**（P2）与**版本过滤** —— brief 给的两个 hw_code（V5 0x6752、
    // V6 0x907）在各自文件里都只有一条，覆盖不到这两条规则。
    if (!mtktest::sampleFileAvailable(QStringLiteral("MTK_AllInOne_DA_iot.bin"))
        || !mtktest::sampleFileAvailable(QStringLiteral("MTK_AllInOne_DA_mt6590.bin"))) {
#if MTK_SAMPLES_REQUIRED
        QFAIL("真样本缺失，但本次构建要求真样本（MTK_SAMPLES_REQUIRED=ON）");
#else
        QSKIP("真样本缺失（reference/ 为 gitignored）");
#endif
    }
    const QString dir = mtktest::samplesDir();
    QString err;
    QStringList warn;
    mtkbrom::DaSelection sel;

    // iot: hw_code=0x6226 的条目 [2]/[3] 前四字段完全相同（sub=0/hw_ver=0x8A00/sw_ver=0x8A00），
    // 只有 pagesize（0 / 1）不同 —— 用 4 元组建 map 会在这里静默丢条目。
    // P5 caveat：本层按 brief **恒用 region[1]/region[2]**（手机侧映射）；IoT 芯片走另一套 region 映射，
    // D1 在通道层（Task 9）显式拒绝 —— 本用例只验证"选中哪一条"，不代表 IoT 的 region 语义已被支持。
    QFile fi(dir + QStringLiteral("/MTK_AllInOne_DA_iot.bin"));
    QVERIFY(fi.open(QIODevice::ReadOnly));
    const QByteArray iotData = fi.readAll();
    mtkbrom::DaFile iot;
    QVERIFY2(mtkbrom::parseDaFile(iotData, iot, &err), qPrintable(err));
    QVERIFY2(mtkbrom::selectDaEntry(iot, 0x6226, 0, 0, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.entryIndex, 2);                     // 两条版本相同 → first-match 取文件顺序首个（条目[2]）
    QCOMPARE(sel.entry.pagesize, quint16(0));
    bool noted = false;
    for (const QString &w : std::as_const(warn))
        noted = noted || w.contains(QStringLiteral("5 元组"));
    QVERIFY(noted);
    // 两条目的载荷不同（报告：region[1].m_buf = 325852 / 434340）→ 切片必须来自被选中的条目[2]
    QCOMPARE(sel.da1.fileOffset, quint32(325852));
    QCOMPARE(sel.da1Bytes, iotData.mid(325852, int(sel.da1.len)));

    // iot: hw_code=0x6270 是**真样本上唯一能判别 swVersion 过滤**的点 —— 首个可用条目[17]（sw 0x8000）
    // 排在条目[27]（sw 0x0100）前面：设备 sw 给 0x0100 时，条目[17] 必须被滤掉才会选到条目[27]。
    // （不做过滤的实现两次都返回条目[17] → 下面的断言杀得掉它。）
    warn.clear();
    err.clear();
    QVERIFY2(mtkbrom::selectDaEntry(iot, 0x6270, 0, 0x8000, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.entryIndex, 17);
    QCOMPARE(sel.entry.swVersion, quint16(0x8000));
    warn.clear();
    QVERIFY2(mtkbrom::selectDaEntry(iot, 0x6270, 0, 0x0100, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.entryIndex, 27);                    // 条目[17] 的 sw 0x8000 > 设备 0x0100 → 被滤掉
    QCOMPARE(sel.entry.swVersion, quint16(0x0100));

    // mt6590: hw_code=0x6575 两条目（[1] hw 0xCA00/sw 0xE100、[2] hw 0xCB00/sw 0xE201）
    // → 设备版本旁路时 **first-match = 文件顺序首个**（条目[1]，不是版本更大的条目[2]）。
    QFile fm(dir + QStringLiteral("/MTK_AllInOne_DA_mt6590.bin"));
    QVERIFY(fm.open(QIODevice::ReadOnly));
    const QByteArray mtData = fm.readAll();
    mtkbrom::DaFile mt;
    err.clear();
    QVERIFY2(mtkbrom::parseDaFile(mtData, mt, &err), qPrintable(err));

    warn.clear();
    QVERIFY2(mtkbrom::selectDaEntry(mt, 0x6575, 0, 0, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.entryIndex, 1);                     // 上游 first-match：取文件顺序首个，非最大版本
    QCOMPARE(sel.entry.hwVersion, quint16(0xCA00));
    QCOMPARE(sel.da1.len, quint32(137884));
    QCOMPARE(sel.da1Bytes.size(), int(sel.da1.len));
}

QTEST_APPLESS_MAIN(TestMtkDaFile)
#include "test_mtk_da_file.moc"
