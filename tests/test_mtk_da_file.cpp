// tests/test_mtk_da_file.cpp
//
// AllInOne DA 解析：合成夹具（覆盖 P1-P12 的边界）+ 真样本对拍
// （6 个真实文件 / 161 条目，与 reference/mtk-samples/da_parse_report.json 逐字段一致）。
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <utility>   // std::as_const（遍历 Qt 容器，不得用 qAsConst）

#include "core/modes/mtk_chip_table.h"   // T9 复审：查找键 = chip->dacode 的真样本用例
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
    // 切分另存（writeExtractedDAs）—— 写 da1.bin / da2.bin
    void writeExtractedDasRoundTrip();
    void writeExtractedDasKeepsDa2TailSignature();
    void writeExtractedDasOverwritesExistingFiles();
    void writeExtractedDasMissingDirFails();
    void writeExtractedDasRejectsEmptyData();
    void writeExtractedDasFirstWriteFailureLeavesNoFile();
    void writeExtractedDasSecondWriteFailureLeavesNoFile();
    void writeExtractedDasPartialWriteCleansSecondPath();
    // 真样本（reference/mtk-samples/，缺失时 SKIP；验证跑带 -DMTK_SAMPLES_REQUIRED=ON）
    void realSamplesMatchIndependentReport();
    void realSamplesSelectExpectedEntries();
    void realSamplesCoverCollisionAndVersionFilter();
    // T9 复审修复的真样本方向：查找键 = chip->dacode（不是设备报的 hw_code）
    void realSamplesLookUpByDacodeNotHwCode();
    void realSamplesWriteDaFilesRoundTrip();
};

using namespace mtktest;

// ---- 切分另存用例用的私有件 ----

// 非周期填充（xorshift32，同 test_spd_pac / test_eub_session 的做法）：`(i * k) & 0xFF` 一类
// 256 周期图案会让"偏移错 256 的整数倍"漏检；逐字节对拍要能分辨错位/截断。
static QByteArray patternedBytes(int n, quint32 seed)
{
    QByteArray b(n, '\0');
    quint32 x = seed;
    for (int i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b[i] = char(x & 0xFF);
    }
    return b;
}

// 把 raw 里第 0 条条目的 region[2]（DA2）末尾 sigLen 字节改成 pattern（长度须 == sigLen）。
// 为什么非改不可：builder 的每个 region 是**单一填充字节**，则"签名保留 / 被裁掉 / 被清零"只有
// **长度**可分；换成图案后内容也可分 —— 本项的核心验收是"不许裁 DA2 尾部签名"，对拍要能看见
// 签名那一段本身（否则一个"长度对、内容被裁后补零"的实现能蒙混过关）。
static bool patchDa2Tail(QByteArray &raw, const QByteArray &pattern)
{
    mtkbrom::DaFile f;
    QString err;
    if (!mtkbrom::parseDaFile(raw, f, &err) || f.entries.isEmpty())
        return false;
    const mtkbrom::DaEntry &e = f.entries.first();
    if (e.regions.size() < 3)
        return false;
    const mtkbrom::DaRegion &r2 = e.regions.at(2);
    if (r2.sigLen != quint32(pattern.size()) || r2.len < r2.sigLen)
        return false;
    // 用**解析器给的** fileOffset 定位（不是自己按 builder 布局算的常数）：夹具与解析器对同一
    // 偏移的两种算法互相印证，改 builder 时这里不会静默改到别的字节上。
    raw.replace(int(r2.fileOffset + r2.len - r2.sigLen), pattern.size(), pattern);
    return true;
}

// 真样本 / 落盘用例共用的两段可辨识载荷：region[0] 用作 EMI（选择层不用），
// region[1] = DA1、region[2] = DA2（带 sigLen 尾部签名，落在 pattern 上）。
// 返回 false = 夹具自身不合法（用例应当 QVERIFY 它，别让失败伪装成"实现有问题"）。
static bool makeWritableSelection(mtkbrom::DaSelection &out, const QByteArray &tailPattern,
                                  quint32 da1Len = 64, quint32 da2Len = 96, quint32 sigLen = 32)
{
    if (tailPattern.size() != int(sigLen) || da2Len < sigLen || da1Len == 0)
        return false;
    EntrySpec e;
    e.hwCode = 0x6765;
    RegionSpec r0; r0.len = 16;     r0.startAddr = 0x200000;
    RegionSpec r1; r1.len = da1Len; r1.startAddr = 0x2007000;
    RegionSpec r2; r2.len = da2Len; r2.startAddr = 0x80000000; r2.sigLen = sigLen;
    e.regions << r0 << r1 << r2;
    QByteArray raw = buildDa({e});
    if (!patchDa2Tail(raw, tailPattern))
        return false;
    mtkbrom::DaFile f;
    QString err;
    if (!mtkbrom::parseDaFile(raw, f, &err))
        return false;
    QStringList warn;
    return mtkbrom::selectDaEntry(f, 0x6765, 0, 0, &warn, out, &err);
}

// /dev/full 构造"打开成功、写入失败"（同 test_spd_pac::linkToFullDevice）：QFile 带缓冲 ——
// open 成功、write() 返回满额，失败在 flush() 现形，正是"可能已写出部分内容"要清理的那条分支。
// 平台无 /dev/full 或建不出符号链接 → false（调用方 QSKIP）。
static bool linkToFullDevice(const QString &linkPath)
{
    if (!QFileInfo::exists(QStringLiteral("/dev/full")))
        return false;
    if (!QFile::link(QStringLiteral("/dev/full"), linkPath))
        return false;
    return QFileInfo(linkPath).isSymLink();   // Windows 上 link() 造的是 .lnk，不是符号链接
}

// 目录下的常规文件（断言"没留半份产物"用；符号链接不计入 QDir::Files，失败腿另按 isSymLink 判）
static QStringList regularFiles(const QString &dir)
{
    return QDir(dir).entryList(QDir::Files, QDir::Name);
}

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
    // 取不到时是 0 —— 若放行就会与占位条目相撞。上游在装载阶段就剔除它们（daconfig.py:189/200
    // `if da.hw_code != 0:`）→ 本层对 dacode == 0 明确拒绝。
    if (!mtktest::sampleFileAvailable(QStringLiteral("MTK_AllInOne_DA_iot.bin"))
        || !mtktest::sampleFileAvailable(QStringLiteral("MTK_DA_V6.bin"))) {
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

    // F1：fresh 的 `DaSelection{}` 默认就是 entryIndex == -1 —— 直接断言 -1 是**恒真断言**，判别力为零。
    // 先用一次**成功**选择把 sel 填满（且预置值与默认值可区分），失败路径才有东西可残留/可污染。
    QStringList warn;
    mtkbrom::DaSelection sel;
    QVERIFY2(mtkbrom::selectDaEntry(iot, 0x6226, 0, 0, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.entryIndex, 2);
    QVERIFY2(sel.da1.startAddr != 0, "预置的成功结果必须与默认值（全 0）可区分");
    warn.clear();
    err.clear();
    QVERIFY(!mtkbrom::selectDaEntry(iot, 0, 0, 0, &warn, sel, &err));
    QVERIFY2(err.contains(QStringLiteral("dacode == 0")), qPrintable(err));
    // 失败即重置为默认（fail-closed）：不残留上一次的成功结果，也不留"entry 有值、字节为空"的半份选择
    QCOMPARE(sel.entryIndex, -1);
    QCOMPARE(sel.da1.startAddr, quint32(0));
    QVERIFY(sel.da1Bytes.isEmpty());

    // F2：**V6 文件** + dacode == 0 —— 失败路径不得写 out。isXmlForced 曾在本层早退**之前**赋值，
    // 于是 V6 文件下返回 false 却留下 isXmlForced == true（与 .h 的"失败时不返回半份选择"矛盾）。
    // 现有用例用的是 V5 形态的 iot（isV6 == false），测不出这个泄漏。
    QFile f6(dir + QStringLiteral("/MTK_DA_V6.bin"));
    QVERIFY(f6.open(QIODevice::ReadOnly));
    mtkbrom::DaFile v6;
    err.clear();
    QVERIFY2(mtkbrom::parseDaFile(f6.readAll(), v6, &err), qPrintable(err));
    QVERIFY(v6.isV6);                   // 前提：确实拿到 V6 文件，否则本段测不到该泄漏
    warn.clear();
    err.clear();
    QVERIFY(!mtkbrom::selectDaEntry(v6, 0, 0, 0, &warn, sel, &err));
    QVERIFY2(err.contains(QStringLiteral("dacode == 0")), qPrintable(err));
    QVERIFY2(!sel.isXmlForced, "失败路径不得留下 isXmlForced（V6 文件的早退泄漏）");
    QCOMPARE(sel.entryIndex, -1);
}

void TestMtkDaFile::rejectsOversizedRegionSlice()
{
    // >2GiB 的 region 在 int 截断下会静默给出空切片（"上传 0 字节后静默成功"家族）。
    // 真文件造不出来（真实样本 1–4 MiB）→ 手工构造 DaFile 直接喂选择层是正当手段。
    auto makeEntry = [](quint32 da1Len) {
        mtkbrom::DaEntry e;
        e.hwCode = 0x6765;
        e.regionCount = 3;
        mtkbrom::DaRegion r0, r1, r2;
        r0.len = 16;
        r1.len = da1Len;               // region[1] = DA1
        r2.len = 16;                   // region[2] 非空（否则先被"空 region"规则跳过，测不到 >2GiB 守卫）
        e.regions << r0 << r1 << r2;
        return e;
    };
    mtkbrom::DaFile ok;
    ok.raw = QByteArray(64, '\0');
    ok.entries << makeEntry(16);

    // F1：fresh 的 `DaSelection{}` 默认 entryIndex == -1 —— 直接断言 -1 是恒真断言。
    // 先用一次成功选择把 sel 填满，失败路径才有东西可残留/可污染。
    QStringList warn;
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtkbrom::selectDaEntry(ok, 0x6765, 0, 0, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.entryIndex, 0);
    QCOMPARE(sel.da1Bytes.size(), 16);
    warn.clear();
    err.clear();

    mtkbrom::DaFile big;
    big.raw = QByteArray(64, '\0');
    big.entries << makeEntry(0xFFFFFFFF);   // 第一条 > INT_MAX（first-match 会先命中的就是它）
    big.entries << makeEntry(16);           // 第二条：顺带造出"多条满足"的告警条件（F4①）

    QVERIFY(!mtkbrom::selectDaEntry(big, 0x6765, 0, 0, &warn, sel, &err));
    QVERIFY2(err.contains(QStringLiteral("int 范围")), qPrintable(err));
    // 失败即重置为默认：不残留上一次成功的结果，也不留半份选择
    QCOMPARE(sel.entryIndex, -1);
    QVERIFY(sel.da1Bytes.isEmpty());
    // F4①：选择**失败**时不得留下"已选中条目[x]"（多条满足）这类告警 —— 无实害但会误导排障
    bool misleading = false;
    for (const QString &w : std::as_const(warn))
        misleading = misleading || w.contains(QStringLiteral("满足版本过滤"));
    QVERIFY2(!misleading, qPrintable(warn.join(QStringLiteral(" | "))));
}

// ---- 切分另存（writeExtractedDAs）：写 da1.bin / da2.bin ----

void TestMtkDaFile::writeExtractedDasRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const QByteArray tail = patternedBytes(32, 0x51);
    mtkbrom::DaSelection sel;
    QVERIFY2(makeWritableSelection(sel, tail), "夹具 DA 自身不合法");
    QCOMPARE(sel.da1.len, quint32(64));
    QCOMPARE(sel.da2.len, quint32(96));
    QVERIFY2(sel.da1Bytes != sel.da2Bytes, "两段内容必须可区分（否则「取反/串位」类错误测不出）");

    QString p1, p2, err;
    QVERIFY2(mtkbrom::writeExtractedDAs(sel, dir.path(), p1, p2, &err), qPrintable(err));
    QCOMPARE(p1, dir.filePath(QStringLiteral("da1.bin")));
    QCOMPARE(p2, dir.filePath(QStringLiteral("da2.bin")));
    QVERIFY2(err.isEmpty(), qPrintable(err));   // 成功路径不留错误文案（不残留上一次的）

    QFile f1(p1);
    QFile f2(p2);
    QVERIFY(f1.open(QIODevice::ReadOnly));
    QVERIFY(f2.open(QIODevice::ReadOnly));
    const QByteArray got1 = f1.readAll();
    const QByteArray got2 = f2.readAll();
    QCOMPARE(got1, sel.da1Bytes);
    QCOMPARE(got2, sel.da2Bytes);
    QCOMPARE(got1.size(), int(sel.da1.len));
    QCOMPARE(got2.size(), int(sel.da2.len));
    // 两段没串位：da2.bin 的尾部签名段是夹具图案（da1.bin 里没有这段图案）
    QCOMPARE(got2.right(tail.size()), tail);
    QVERIFY2(!got1.contains(tail), "da1.bin 里不该出现 DA2 的签名段（两段串位/取反）");
    // 目录里只有这两个文件：不留临时件（本实现是直写，不留 .part 一类中间产物）
    QCOMPARE(regularFiles(dir.path()),
             QStringList({QStringLiteral("da1.bin"), QStringLiteral("da2.bin")}));
}

// 本项的核心验收：**DA2 保留尾部签名，不许裁剪**。裁剪是上游/本仓曾经踩过的方向
//（XFlash 的 boot_to 才剥签名，见 mtk_xflash_payload.h —— 剥签名是**协议层**的事，
//  落盘的是"文件里那一段"，与 LEGACY 上传的字节逐字节相同）。
void TestMtkDaFile::writeExtractedDasKeepsDa2TailSignature()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const QByteArray tail = patternedBytes(32, 0x77);
    mtkbrom::DaSelection sel;
    QVERIFY2(makeWritableSelection(sel, tail), "夹具 DA 自身不合法");
    // 前提：签名长度非 0 且小于 m_len —— 否则"裁了"与"没裁"在长度上不可分，本用例会变成空转
    QVERIFY(sel.da2.sigLen > 0);
    QVERIFY(sel.da2.len > sel.da2.sigLen);

    QString p1, p2, err;
    QVERIFY2(mtkbrom::writeExtractedDAs(sel, dir.path(), p1, p2, &err), qPrintable(err));
    QFile f2(p2);
    QVERIFY(f2.open(QIODevice::ReadOnly));
    const QByteArray written = f2.readAll();

    // 长度 = m_len（**不是** m_len - m_sig_len）：裁剪实现会短掉 sigLen 字节
    QCOMPARE(written.size(), int(sel.da2.len));
    // 尾部那 sigLen 字节**逐字节**还在（内容可分，不只看长度）
    QCOMPARE(written.right(int(sel.da2.sigLen)), tail);
    // 整段与选择层切出的字节逐字节一致：落盘不做任何二次加工（不裁、不补、不移位）
    QCOMPARE(written, sel.da2Bytes);
}

// 同名文件已存在时**覆盖**（文件名固定 da1.bin/da2.bin = 导出语义，不静默改名）。
// 判别力：旧内容比新内容**长** —— 若实现按追加/不截断写，文件尾部会留下旧字节，
// 后续按路径取用就是"静默用错数据"（比失败更糟）。
void TestMtkDaFile::writeExtractedDasOverwritesExistingFiles()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const QByteArray tail = patternedBytes(32, 0x1D);
    mtkbrom::DaSelection sel;
    QVERIFY2(makeWritableSelection(sel, tail), "夹具 DA 自身不合法");

    const QString p1 = dir.filePath(QStringLiteral("da1.bin"));
    const QString p2 = dir.filePath(QStringLiteral("da2.bin"));
    const QByteArray stale1(sel.da1Bytes.size() + 64, '\xEE');
    const QByteArray stale2(sel.da2Bytes.size() + 64, '\xEE');
    for (const auto &pair : {std::make_pair(p1, stale1), std::make_pair(p2, stale2)}) {
        QFile old(pair.first);
        QVERIFY(old.open(QIODevice::WriteOnly));
        QCOMPARE(old.write(pair.second), qint64(pair.second.size()));
        old.close();
    }

    QString w1, w2, err;
    QVERIFY2(mtkbrom::writeExtractedDAs(sel, dir.path(), w1, w2, &err), qPrintable(err));
    QCOMPARE(w1, p1);
    QCOMPARE(w2, p2);
    QFile f1(p1);
    QFile f2(p2);
    QVERIFY(f1.open(QIODevice::ReadOnly));
    QVERIFY(f2.open(QIODevice::ReadOnly));
    QCOMPARE(f1.readAll(), sel.da1Bytes);       // 长度也要一样：旧字节不许留在尾部
    QCOMPARE(f2.readAll(), sel.da2Bytes);
    QCOMPARE(f1.size(), qint64(sel.da1.len));
    QCOMPARE(f2.size(), qint64(sel.da2.len));
}

// 目录不存在 / 路径是常规文件：不替调用方建目录（路径由调用方/用户给，猜错地方比失败更糟）——
// 对齐 spd::writeExtractedFdls 的姿态。失败必须清空两个出参（不残留上一次的成功路径）。
void TestMtkDaFile::writeExtractedDasMissingDirFails()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const QByteArray tail = patternedBytes(32, 0x2B);
    mtkbrom::DaSelection sel;
    QVERIFY2(makeWritableSelection(sel, tail), "夹具 DA 自身不合法");

    // 先来一次**成功**把出参填满：失败路径"清空出参"才有东西可清（fresh 就是空的话是恒真断言）
    QString p1, p2, err;
    QVERIFY2(mtkbrom::writeExtractedDAs(sel, dir.path(), p1, p2, &err), qPrintable(err));
    QVERIFY(!p1.isEmpty() && !p2.isEmpty());

    const QString missing = dir.filePath(QStringLiteral("not-there"));
    err.clear();
    QVERIFY(!mtkbrom::writeExtractedDAs(sel, missing, p1, p2, &err));
    QVERIFY2(err.contains(missing), qPrintable(err));   // 文案点明是哪个路径
    QVERIFY2(p1.isEmpty() && p2.isEmpty(), "失败路径必须清空出参（不残留上一次的成功路径）");
    QVERIFY2(!QFileInfo::exists(missing), "不得替调用方把目录建出来");

    // 路径存在但是**常规文件**（不是目录）：同样拒绝，且不把它当目录用
    const QString asFile = dir.filePath(QStringLiteral("afile"));
    QFile af(asFile);
    QVERIFY(af.open(QIODevice::WriteOnly));
    QCOMPARE(af.write("x", 1), qint64(1));
    af.close();
    err.clear();
    QVERIFY(!mtkbrom::writeExtractedDAs(sel, asFile, p1, p2, &err));
    QVERIFY2(err.contains(asFile), qPrintable(err));
    QVERIFY(p1.isEmpty() && p2.isEmpty());
    // 那个常规文件没被动过：既没被当目录用，也没被写坏（"路径是文件"这条分支不得回落到覆盖它）
    QFile check(asFile);
    QVERIFY(check.open(QIODevice::ReadOnly));
    QCOMPARE(check.readAll(), QByteArray("x"));
    // 目录里仍是"上一次成功写出的两个 + afile"：本次失败没多写、也没删别人的文件
    QCOMPARE(regularFiles(dir.path()),
             QStringList({QStringLiteral("afile"), QStringLiteral("da1.bin"), QStringLiteral("da2.bin")}));
}

// 空载荷（0 字节）拒写：空 DA 发出去起不了作用，失败点会被推到设备侧（同 spd 的空 FDL 判据）。
// 选择层已跳过 region[1]/[2] 长度为 0 的条目，故这里守的是**手工构造的 DaSelection**（调用方
// 可能自己拼一份，或将来由别的来源填）——守卫要在落盘口，而不是假设上游一定干净。
void TestMtkDaFile::writeExtractedDasRejectsEmptyData()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const QByteArray tail = patternedBytes(32, 0x3D);
    mtkbrom::DaSelection sel;
    QVERIFY2(makeWritableSelection(sel, tail), "夹具 DA 自身不合法");

    QString p1, p2, err;
    QVERIFY2(mtkbrom::writeExtractedDAs(sel, dir.path(), p1, p2, &err), qPrintable(err));

    for (int which = 1; which <= 2; ++which) {
        mtkbrom::DaSelection bad = sel;
        if (which == 1)
            bad.da1Bytes.clear();
        else
            bad.da2Bytes.clear();
        err.clear();
        QVERIFY(!mtkbrom::writeExtractedDAs(bad, dir.path(), p1, p2, &err));
        QVERIFY2(err.contains(QStringLiteral("空载荷")), qPrintable(err));
        // 指明**是**哪一段空的（文案格式："（DA1 N 字节 / DA2 M 字节）"）—— 只判"含空载荷"的话，
        // 把两段判据写反（查 da1 却报 da2）也照样通过
        QVERIFY2(err.contains(QStringLiteral("DA%1 0 字节").arg(which)), qPrintable(err));
        QVERIFY2(p1.isEmpty() && p2.isEmpty(), "失败路径必须清空出参");
        // 空载荷是**入口拒写**：目录里仍是上一次成功留下的两个文件，没有被清掉或被写空
        QCOMPARE(regularFiles(dir.path()),
                 QStringList({QStringLiteral("da1.bin"), QStringLiteral("da2.bin")}));
        QCOMPARE(QFileInfo(dir.filePath(QStringLiteral("da1.bin"))).size(), qint64(sel.da1.len));
        QCOMPARE(QFileInfo(dir.filePath(QStringLiteral("da2.bin"))).size(), qint64(sel.da2.len));
    }
}

// 第一轮**写不完整**（打开成功、刷盘失败，构造见 linkToFullDevice）：失败即清理该路径上的残留，
// 且"刷盘失败"要如实说（QFile 带缓冲 ⇒ 这条分支上 write() 的返回值仍是满额，照"实写 N 字节"写
// 会自相矛盾 —— spd_pac 的复审 M6 同款）。
void TestMtkDaFile::writeExtractedDasFirstWriteFailureLeavesNoFile()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const QString p1 = dir.filePath(QStringLiteral("da1.bin"));
    if (!linkToFullDevice(p1))
        QSKIP("本机无 /dev/full（或建不出符号链接）—— 离线构造不出「打开成功、写入失败」");
    const QByteArray tail = patternedBytes(32, 0x5F);
    mtkbrom::DaSelection sel;
    QVERIFY2(makeWritableSelection(sel, tail), "夹具 DA 自身不合法");

    QString w1, w2, err;
    QVERIFY(!mtkbrom::writeExtractedDAs(sel, dir.path(), w1, w2, &err));
    QVERIFY2(err.contains(QStringLiteral("da1.bin")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("刷盘失败")), qPrintable(err));
    QVERIFY(w1.isEmpty() && w2.isEmpty());
    QVERIFY2(!QFileInfo(p1).isSymLink(), "第一轮写失败后 da1.bin 仍留着条目（清理未执行）");
    QVERIFY2(regularFiles(dir.path()).isEmpty(), "第一轮失败留下了常规文件");
    QVERIFY2(!QFile::exists(dir.filePath(QStringLiteral("da2.bin"))), "第二轮不该被写到");
}

// 第二个文件写不进去（同名位置被一个**目录**占住）：整次调用失败，**第一个文件也要删掉** ——
// 只落成 da1.bin 会让调用方以为拿到了可用的一对（同 spd::writeExtractedFdls 的姿态）。
void TestMtkDaFile::writeExtractedDasSecondWriteFailureLeavesNoFile()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    // 占位目录：写同名常规文件必失败（QFile 打不开一个目录）
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("da2.bin")));
    const QByteArray tail = patternedBytes(32, 0x6B);
    mtkbrom::DaSelection sel;
    QVERIFY2(makeWritableSelection(sel, tail), "夹具 DA 自身不合法");

    QString w1, w2, err;
    QVERIFY(!mtkbrom::writeExtractedDAs(sel, dir.path(), w1, w2, &err));
    // 失败确实发生在**第二次**写：文案点名 da2.bin、不点 da1.bin —— 否则"直接返回 false、什么都没写"
    // 的桩实现也能让下面"第一个文件不存在"恒真通过；反过来第一轮就失败会在这里显出 da1.bin。
    QVERIFY2(!err.contains(QStringLiteral("da1.bin")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("da2.bin")), qPrintable(err));
    QVERIFY(w1.isEmpty() && w2.isEmpty());
    QVERIFY2(!QFile::exists(dir.filePath(QStringLiteral("da1.bin"))),
             "第一个文件没删：失败留下了半份产物");
    QVERIFY2(QFileInfo(dir.filePath(QStringLiteral("da2.bin"))).isDir(), "占位目录不该被动过");
    QVERIFY2(regularFiles(dir.path()).isEmpty(), "失败留下了常规文件");
}

// 第二轮**写不完整**（打开成功、刷盘失败）：先写成的 da1.bin 是"半份产物"（调用方以为拿到了可用的
// 一对），后写的 da2.bin 路径上的残留也要清掉。
void TestMtkDaFile::writeExtractedDasPartialWriteCleansSecondPath()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const QString p2 = dir.filePath(QStringLiteral("da2.bin"));
    if (!linkToFullDevice(p2))
        QSKIP("本机无 /dev/full（或建不出符号链接）—— 离线构造不出「打开成功、写入失败」");
    const QByteArray tail = patternedBytes(32, 0x83);
    mtkbrom::DaSelection sel;
    QVERIFY2(makeWritableSelection(sel, tail), "夹具 DA 自身不合法");

    QString w1, w2, err;
    QVERIFY(!mtkbrom::writeExtractedDAs(sel, dir.path(), w1, w2, &err));
    QVERIFY2(err.contains(QStringLiteral("da2.bin")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("刷盘失败")), qPrintable(err));
    QVERIFY(w1.isEmpty() && w2.isEmpty());
    QVERIFY2(!QFile::exists(dir.filePath(QStringLiteral("da1.bin"))),
             "第一个文件没删：失败留下了半份产物");
    QVERIFY2(!QFileInfo(p2).isSymLink(), "第二个路径仍留着条目（清理未执行）");
    QVERIFY2(regularFiles(dir.path()).isEmpty(), "失败留下了常规文件");
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

// **DA 条目查找键 = 芯片表的 dacode，不是设备报的 hw_code**（T9 复审修复的真样本方向）。
// 出处：上游 `mtkclient/Library/DA/daconfig.py:208-209` 用 `chipconfig.dacode` 查，
// 而 `dasetup` 是按**条目自己的** `da.hw_code` 建的（:190/:192/:201）；本仓
// `mtk_chip_table.h:29-30` 同口径（"dacode 才是查 DA 条目的键"）。
// 真样本实测（reference/mtk-samples/，与独立解析器报告 da_parse_report.json 对拍）：
//   MTK_DA_V5.bin 里有 `hw_code == 0x6735`（index 14）/`0x6758`（19）/`0x8173`（43）的条目，
//   而**没有** `0x0321` —— 芯片表把 0x0321/0x0335/0x0337 映射到 dacode 0x6735。
// 于是：① 用 dacode(0x6735) 查 → 命中；② 用设备 hw_code(0x0321) 查 → 无候选（正是修前的行为）。
void TestMtkDaFile::realSamplesLookUpByDacodeNotHwCode()
{
    if (!mtktest::sampleFileAvailable(QStringLiteral("MTK_DA_V5.bin"))) {
#if MTK_SAMPLES_REQUIRED
        QFAIL("真样本缺失，但本次构建要求真样本（MTK_SAMPLES_REQUIRED=ON）");
#else
        QSKIP("真样本缺失（reference/ 为 gitignored）");
#endif
    }
    QFile f(mtktest::samplesDir() + QStringLiteral("/MTK_DA_V5.bin"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    mtkbrom::DaFile v5;
    QString err;
    QVERIFY2(mtkbrom::parseDaFile(f.readAll(), v5, &err), qPrintable(err));

    // 本仓芯片表里**全部 5 个 dacode != hw_code 的 LEGACY 条目**（映射到 3 个 dacode）：
    // 设备报的 hw_code（左）≠ 真正要拿去找 DA 条目的 dacode（右）。
    struct Case { quint16 deviceHwCode; quint16 dacode; };
    const Case kCases[] = {{0x0321, 0x6735}, {0x0335, 0x6735}, {0x0337, 0x6735},
                           {0x0507, 0x6758}, {0x8172, 0x8173}};
    for (const Case &c : kCases) {
        const mtkbrom::ChipInfo *chip = mtkbrom::lookupChip(c.deviceHwCode);
        QVERIFY2(chip != nullptr, qPrintable(QStringLiteral("表内应有 0x%1")
                                                 .arg(c.deviceHwCode, 4, 16, QLatin1Char('0'))));
        QCOMPARE(chip->dacode, c.dacode);
        QCOMPARE(chip->damode, mtkbrom::DaMode::Legacy);

        QStringList warn;
        mtkbrom::DaSelection sel;
        // ① 用 **dacode** 查 → 命中（真实条目三条 region 齐全，DA1/DA2 切片非空）
        QVERIFY2(mtkbrom::selectDaEntry(v5, chip->dacode, 0, 0, &warn, sel, &err), qPrintable(err));
        QCOMPARE(sel.entry.hwCode, c.dacode);      // 条目的 hw_code 就等于 dacode
        QVERIFY(sel.da1.len > 0);
        QVERIFY(sel.da2.len > 0);
        QCOMPARE(sel.da1Bytes.size(), int(sel.da1.len));

        // ② 用**设备报的 hw_code** 当键 → 无候选；诊断里必须列出该 dacode
        //    （证明"文件里确实有这条，只是键用错了"—— 正是修前的行为）
        warn.clear();
        err.clear();
        QVERIFY2(!mtkbrom::selectDaEntry(v5, c.deviceHwCode, 0, 0, &warn, sel, &err),
                 "设备 hw_code 不是查找键，用它查必须失败");
        QVERIFY2(err.contains(QStringLiteral("0x%1").arg(c.dacode, 4, 16, QLatin1Char('0'))),
                 qPrintable(err));
    }
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

    // hwVersion 维的真样本判别点（F3）：设备 hw=0x8000 → 条目[17]（0x8000 ≤ 设备）；
    // 设备 hw=0x0100 → 三条都不满足（[17]/[18] 的 0x8000 与 [27] 的 0xCA00 都 > 0x0100）→ **无候选**。
    warn.clear();
    err.clear();
    QVERIFY2(mtkbrom::selectDaEntry(iot, 0x6270, 0x8000, 0, &warn, sel, &err), qPrintable(err));
    QCOMPARE(sel.entryIndex, 17);
    QCOMPARE(sel.entry.hwVersion, quint16(0x8000));
    warn.clear();
    err.clear();
    QVERIFY(!mtkbrom::selectDaEntry(iot, 0x6270, 0x0100, 0, &warn, sel, &err));
    QVERIFY2(err.contains(QStringLiteral("0x6270")), qPrintable(err));   // 无候选的 error 仍列出可用 hw_code

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

// 真样本的切分另存：合成夹具能覆盖边界，但 region 偏移/长度/**签名长度**都不由本仓决定 ——
// 用真文件落一次盘，把两段与源文件切片逐字节对拍（V5 的 0x6752：region[1] m_buf 4429000/m_len 87480，
// region[2] m_buf 4516488/m_len 226552/**m_sig_len 256**，数字取自独立解析器报告 da_parse_report.json）。
void TestMtkDaFile::realSamplesWriteDaFilesRoundTrip()
{
    if (!mtktest::sampleFileAvailable(QStringLiteral("MTK_DA_V5.bin"))) {
#if MTK_SAMPLES_REQUIRED
        QFAIL("真样本缺失，但本次构建要求真样本（MTK_SAMPLES_REQUIRED=ON）");
#else
        QSKIP("真样本缺失（reference/ 为 gitignored）");
#endif
    }
    QFile f(mtktest::samplesDir() + QStringLiteral("/MTK_DA_V5.bin"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray v5Data = f.readAll();
    mtkbrom::DaFile v5;
    QString err;
    QVERIFY2(mtkbrom::parseDaFile(v5Data, v5, &err), qPrintable(err));
    QStringList warn;
    mtkbrom::DaSelection sel;
    QVERIFY2(mtkbrom::selectDaEntry(v5, 0x6752, 0, 0, &warn, sel, &err), qPrintable(err));
    // 前提：真样本这一条的 DA2 确实带签名（sigLen 非 0）—— 否则本用例测不到"不裁剪"
    QCOMPARE(sel.da2.sigLen, quint32(256));
    QCOMPARE(sel.da2.len, quint32(226552));

    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    QString p1, p2;
    QVERIFY2(mtkbrom::writeExtractedDAs(sel, dir.path(), p1, p2, &err), qPrintable(err));
    QFile f1(p1);
    QFile f2(p2);
    QVERIFY(f1.open(QIODevice::ReadOnly));
    QVERIFY(f2.open(QIODevice::ReadOnly));
    // 盘上字节 == 源文件对应切片（含 DA2 尾部那 256 字节签名）：裁剪 → 长度不符；补零/移位 →
    // 逐字节不符；两种情况都在下面两条对拍上红（真文件的这段签名自带内容，不是整段同色）。
    QCOMPARE(f1.readAll(), v5Data.mid(int(sel.da1.fileOffset), int(sel.da1.len)));
    QCOMPARE(f2.readAll(), v5Data.mid(4516488, 226552));
    QCOMPARE(f1.size(), qint64(sel.da1.len));
    QCOMPARE(f2.size(), qint64(226552));
}

QTEST_APPLESS_MAIN(TestMtkDaFile)
#include "test_mtk_da_file.moc"
