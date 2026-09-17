// tests/test_eub_loadout.cpp
//
// 布局表的**数值硬断言**：8 个 SoC、共 41 个段，每段的 name/offset/length 逐值钉死。
// 数值的唯一来源是设计 spec §5.4（docs/superpowers/specs/2026-09-17-exynos-eub-design.md），
// spec 的表逐条来自 facts §C3–§C6；每条 sourceNote 必须能落到 reference/ 里的 file:line
// （reference/ 是 gitignored 的只读参照，故本文件对 sourceNote 只断言**字符串形态**，
// 不读那些文件 —— 这样用例在只有仓库内容的机器上也能跑）。
// **真机未验证**（sboot.bin 是三星签名二进制，本仓不**分发**它，facts §F1/§F8）：本文件只钉住
// "表内容 == spec §5.4"，不对任何设备行为做断言。
// 真样本已下载（reference/eub-samples/，facts §H）并由末尾四条"真样本硬断言"实跑核对 ——
// 读官方 BL 包解出的 sboot_Exynos*.bin（**gitignored**，样本本身绝不进仓库）：目录缺失时按
// gating 策略 QSKIP/FAIL（见 eub_test_helpers.h），只在"手上有官方包"的机器上实跑。
// 注意：这是**真样本核对**，不是真机验证（本线仍无任何 Exynos 设备）。
#include <QtTest>

#include <QFile>
#include <QRegularExpression>

#include "core/eub/eub_loadout.h"
#include "eub_test_helpers.h"   // 真样本 gating：EUB_SAMPLES_DIR / EUB_SKIP_OR_FAIL / eubtest::*

using Seg = QPair<QString, QPair<quint64, quint64>>;   // 名字, (offset, length)

// 表所需的最小 sboot 尺寸 = max(offset + length)——与 splitSboot 的越界判据同一口径。
// 真样本槽用它算"富余"：不硬编表数字，表被改宽/改窄时富余断言跟着变红。
static quint64 tableNeed(const eub::EubLoadout &lo)
{
    quint64 need = 0;
    for (const eub::EubSegment &s : lo.segments)
        need = qMax(need, s.offset + s.length);
    return need;
}

static eub::EubLoadout mustLoad(const QString &soc)
{
    eub::EubLoadout lo;
    QString err;
    if (!eub::eubLoadoutFor(soc, lo, &err))
        qFatal("查表失败: %s", qPrintable(err));
    return lo;
}

static void expectSegs(const eub::EubLoadout &lo, const QList<Seg> &want)
{
    QCOMPARE(lo.segments.size(), want.size());
    for (qsizetype i = 0; i < want.size(); ++i) {
        QCOMPARE(lo.segments[i].name, want[i].first);
        QCOMPARE(lo.segments[i].offset, want[i].second.first);
        QCOMPARE(lo.segments[i].length, want[i].second.second);
    }
}

// evidence 的全部合法取值（eub_loadout.h 顶部列的四档）
static QStringList legalEvidence()
{
    return {QStringLiteral("双源一致"),
            QStringLiteral("单源+实战报告"),
            QStringLiteral("双源分歧（采信 hubble 连续切法）"),
            QStringLiteral("单源")};
}

// sourceNote 的形态要求：至少一条 `reference/<仓库>/<文件>:<行>`。仓库段必须写全 ——
// reference/ 下有两个同名 exynos-usbdl.c（原版与 vdavid003 fork），只写文件名会指错仓库。
static const QRegularExpression kRefCitation(
    QStringLiteral("reference/[A-Za-z0-9._-]+/[^\\s:：;；]*:\\d+"));

static QString hex0x(quint64 v) { return QStringLiteral("0x") + QString::number(v, 16); }

// 合成夹具（本线无真机；真样本核对见文件末尾的 gated 槽，facts §H）。图案必须**非周期**：若用 `i & 0xFF` 这类 256 周期图案，
// "切到第 N 段" 的内容比对只能钉住 `offset mod 256` —— offset 错 0x100 也会全绿。
// 这里用 xorshift32 做 i 的确定性散列，任意错位都会在内容比对上现形。
static QByteArray patternedImage(qsizetype size)
{
    QByteArray b(size, '\0');
    for (qsizetype i = 0; i < size; ++i) {
        quint32 s = quint32(i);
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        b[i] = char(s & 0xFF);
    }
    return b;
}

class TestEubLoadout : public QObject
{
    Q_OBJECT

private slots:
    void table8890()   // spec §5.4 行 1；数值出处 reference/exynos-usbdl/scripts/split-sboot-8890.sh:2-5
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos8890"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x2000}}, {"el3_mon", {0x2000, 0x24000}},
                        {"bl2", {0x26000, 0x26D10}}, {"bootloader", {0x61000, 0xD1000}}});
        QCOMPARE(lo.style.header, eub::zeroStyle().header);      // facts §B4：exynos-usbdl 路径
        QCOMPARE(lo.style.trailer, eub::zeroStyle().trailer);    // facts §B5
        // 精确串（不是 contains）："单源" 与 "单源+实战报告" 是两档，掉字必须变红
        QCOMPARE(lo.evidence, QStringLiteral("单源+实战报告"));
        QVERIFY(lo.sourceNote.contains(
            QStringLiteral("reference/exynos-usbdl/scripts/split-sboot-8890.sh:2-5")));
        QVERIFY(lo.sourceNote.contains(
            QStringLiteral("reference/exynos8890-exynos-usbdl-recovery/exynos-usbdl-recover.sh:87-93")));
        QVERIFY(kRefCitation.match(lo.sourceNote).hasMatch());
        // sha1 出处：脚本第 1 行注释（G930W8VLS6CSH1，facts §C9）
        QCOMPARE(lo.sbootSha1, QByteArray("9322ccb4e9b382b8cc67ff9ef989c459a763621f"));
    }

    void table8895()   // spec §5.4 行 2；出处 reference/exynos-usbdl/scripts/split-sboot-8895.sh:2-7
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos8895"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x2000}}, {"bl31", {0x2000, 0x28000}},
                        {"bl2", {0x2A000, 0x30000}}, {"fwbl1", {0x0, 0x2000}},
                        {"part5", {0x72000, 0xD1000}}, {"part6", {0x143000, 0x80000}}});
        QCOMPARE(lo.style.header, eub::zeroStyle().header);      // 同为 exynos-usbdl 路径
        QCOMPARE(lo.style.trailer, eub::zeroStyle().trailer);
        QCOMPARE(lo.evidence, QStringLiteral("单源"));            // 表内第 5/6 段脚本未命名（facts §C6）
        QVERIFY(lo.sourceNote.contains(
            QStringLiteral("reference/exynos-usbdl/scripts/split-sboot-8895.sh:2-7")));
        QCOMPARE(lo.sbootSha1, QByteArray("648a3e2c4de149250c575b4f14de096e147cc799"));
    }

    void table7580()   // spec §5.4 行 3：双源分歧（facts §C4）
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos7580"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x2000}}, {"bl31", {0x2000, 0x30000}},
                        {"bl2", {0x32000, 0x8000}}, {"u-boot", {0x3A000, 0xD1000}}});
        QCOMPARE(lo.style.header, eub::dnwStyle().header);       // hubble 路径（facts §B4）
        QCOMPARE(lo.style.trailer, eub::dnwStyle().trailer);
        QCOMPARE(lo.evidence, QStringLiteral("双源分歧（采信 hubble 连续切法）"));
        QVERIFY(lo.sourceNote.contains(QStringLiteral("reference/hubble/ExynosData/Exynos7580.json")));
        // 未采信那一源的值与出处必须留在表项里 —— 采信不等于隐去分歧
        QVERIFY(lo.sourceNote.contains(QStringLiteral("0x7D10")));
        QVERIFY(lo.sourceNote.contains(QStringLiteral(
            "reference/exynos8890-exynos-usbdl-recovery/A510F/split-sboot-7580.sh:4")));
        // sha1 **留空**（终审 I2）：唯一记了修订的正是**未被采信的那一源**（其 bl2=0x7D10 恰被本表的
        // 0x8000 否决）—— 拿它当"本表所用修订"会给出方向相反的保证（用户文件若与它相符，UI 会报
        // "sha1 一致：你的固件与该表所用修订相同"，而两源切法本身就不一致）。该修订只留在 sourceNote
        // 里供人工比对；UI 走"该表未记录固件修订"分支（对话框用例另有钉）。
        QVERIFY(lo.sbootSha1.isEmpty());
        QVERIFY(lo.sourceNote.contains(QStringLiteral("466852d1")));      // 另一源的修订号仍可查
        QVERIFY(lo.sourceNote.contains(QStringLiteral("A510FXXS8CTI7")));
    }

    void table7885()   // spec §5.4 行 4；出处 reference/hubble/ExynosData/Exynos7885.json:5-28
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos7885"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x2000}}, {"bl31", {0x2000, 0x25000}},
                        {"bl2", {0x27000, 0x2A000}}, {"fwbl1", {0x0, 0x2000}},
                        {"u-boot", {0x61800, 0xD1000}}});
        QCOMPARE(lo.style.header, eub::dnwStyle().header);
        QCOMPARE(lo.style.trailer, eub::dnwStyle().trailer);
        QCOMPARE(lo.evidence, QStringLiteral("单源"));
        QVERIFY(lo.sourceNote.contains(QStringLiteral("reference/hubble/ExynosData/Exynos7885.json:5-28")));
        QVERIFY(lo.sbootSha1.isEmpty());   // 该源未记录所用 sboot 修订（facts §C9 只给了三个脚本）
    }

    void table9610()   // spec §5.4 行 5：双源一致（facts §C5）
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos9610"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x2000}}, {"epbl", {0x2000, 0x13000}},
                        {"bl2", {0x15000, 0x2F000}}, {"fwbl1", {0x0, 0x2000}},
                        {"u-boot", {0x5A000, 0x180000}}, {"el3_mon", {0x1DA000, 0x40000}}});
        QCOMPARE(lo.style.header, eub::dnwStyle().header);
        QCOMPARE(lo.style.trailer, eub::dnwStyle().trailer);
        QCOMPARE(lo.evidence, QStringLiteral("双源一致"));
        QVERIFY(lo.sourceNote.contains(QStringLiteral("reference/hubble/ExynosData/Exynos9610.json:5-34")));
        QVERIFY(lo.sourceNote.contains(
            QStringLiteral("reference/exynos9610-usb-emergency-recovery/split_bootloader_a505.sh:1-6")));
        QVERIFY(lo.sourceNote.contains(
            QStringLiteral("reference/exynos9610-usb-emergency-recovery/dltool/dltool.c:311-319")));
        // 第二源另有 part6（0x21A000/0x101000），本表**不采纳** —— 该决定必须记录（facts §C5）
        QVERIFY(lo.sourceNote.contains(QStringLiteral("part6")));
        QVERIFY(!lo.responseSupport);
        QVERIFY(lo.extraFiles.isEmpty());
        QVERIFY(lo.sbootSha1.isEmpty());
    }

    void table9810()   // spec §5.4 行 6；出处 reference/hubble/ExynosData/Exynos9810.json:5-33
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos9810"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x2000}}, {"bl31", {0x2000, 0x13000}},
                        {"bl2", {0x15000, 0x4F000}}, {"fwbl1", {0x0, 0x2000}},
                        {"u-boot", {0x7D000, 0x180000}}, {"el3_mon", {0x1FD000, 0x40000}}});
        QCOMPARE(lo.style.header, eub::dnwStyle().header);
        QCOMPARE(lo.style.trailer, eub::dnwStyle().trailer);
        QCOMPARE(lo.evidence, QStringLiteral("单源"));
        QVERIFY(lo.sourceNote.contains(QStringLiteral("reference/hubble/ExynosData/Exynos9810.json:5-33")));
        QVERIFY(!lo.responseSupport);
        QVERIFY(lo.sbootSha1.isEmpty());
    }

    void table9820()   // spec §5.4 行 7；出处 reference/hubble/ExynosData/Exynos9820.json:5-28（response_support=true）
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos9820"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x3000}}, {"epbl", {0x3000, 0x13000}},
                        {"bl2", {0x16000, 0x52000}}, {"u-boot", {0xA4000, 0x180000}},
                        {"el3_mon", {0x224000, 0x40000}}});
        QCOMPARE(lo.style.header, eub::dnwStyle().header);
        QCOMPARE(lo.style.trailer, eub::dnwStyle().trailer);
        QCOMPARE(lo.evidence, QStringLiteral("单源"));
        QVERIFY(lo.sourceNote.contains(QStringLiteral("reference/hubble/ExynosData/Exynos9820.json:5-28")));
        QVERIFY(lo.responseSupport);          // facts §C7/§C8：该 SoC 会回显
        QVERIFY(lo.extraFiles.isEmpty());
        QVERIFY(lo.sbootSha1.isEmpty());
    }

    void table9830()   // spec §5.4 行 8；出处 reference/hubble/ExynosData/Exynos9830.json:2-28（files_to_send + response_support）
    {
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos9830"));
        expectSegs(lo, {{"fwbl1", {0x0, 0x3000}}, {"epbl", {0x3000, 0x13000}},
                        {"bl2", {0x16000, 0x6C000}}, {"lk", {0xDB000, 0x280000}},
                        {"el3_mon", {0x35B000, 0x40000}}});
        QCOMPARE(lo.style.header, eub::dnwStyle().header);
        QCOMPARE(lo.style.trailer, eub::dnwStyle().trailer);
        QCOMPARE(lo.evidence, QStringLiteral("单源"));
        QVERIFY(lo.sourceNote.contains(QStringLiteral("reference/hubble/ExynosData/Exynos9830.json:2-28")));
        QCOMPARE(lo.extraFiles, QStringList({"ldfw.img", "tzsw.img"}));   // facts §C7：段后另发的 BL 包内文件
        QVERIFY(lo.responseSupport);
        QVERIFY(lo.sbootSha1.isEmpty());
    }

    void allLoadoutsCoverTheEightSocsInSpecOrder()
    {
        const QList<eub::EubLoadout> all = eub::allLoadouts();
        QCOMPARE(all.size(), 8);   // spec §5.4 全表 = 8 个 SoC
        QStringList socs;
        QStringList evidence;
        qsizetype totalSegs = 0;
        for (const eub::EubLoadout &lo : all) {
            socs << lo.soc;
            evidence << lo.evidence;
            totalSegs += lo.segments.size();
            QVERIFY2(kRefCitation.match(lo.sourceNote).hasMatch(),
                     qPrintable(lo.soc + QStringLiteral(": ") + lo.sourceNote));
        }
        QCOMPARE(socs, QStringList({"Exynos8890", "Exynos8895", "Exynos7580", "Exynos7885",
                                    "Exynos9610", "Exynos9810", "Exynos9820", "Exynos9830"}));
        QCOMPARE(totalSegs, qsizetype(41));   // 8 张表共 41 段（4+6+4+5+6+6+5+5）
        for (const QString &e : evidence)
            QVERIFY2(legalEvidence().contains(e), qPrintable(e));   // 证据等级只有这四档，写错即红
        // 每张表都能被自己的 SoC 名（大小写不敏感）原样查回
        for (const eub::EubLoadout &lo : all) {
            eub::EubLoadout got;
            QString err;
            QVERIFY2(eub::eubLoadoutFor(lo.soc.toLower(), got, &err), qPrintable(err));
            QCOMPARE(got.soc, lo.soc);
        }
    }

    void lookupIsCaseInsensitiveAndRejectsUnknown()
    {
        eub::EubLoadout lo;
        QString err;
        QVERIFY(eub::eubLoadoutFor(QStringLiteral("exynos9610"), lo, &err));
        QCOMPARE(lo.soc, QStringLiteral("Exynos9610"));
        QVERIFY(!eub::eubLoadoutFor(QStringLiteral("Exynos9999"), lo, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(err.contains(QStringLiteral("Exynos9999")));
        QVERIFY(err.contains(QStringLiteral("该 SoC 无公开布局表")));   // spec §7 的文案
        for (const eub::EubLoadout &t : eub::allLoadouts())             // 文案必须列出支持的 8 个（可行动）
            QVERIFY2(err.contains(t.soc), qPrintable(err));
        QVERIFY(lo.soc.isEmpty());   // 查表失败不留陈旧表项（fail-closed）
    }

    void splitCutsExactRangesAndRepeatsAreIdentical()
    {
        // 合成样本：长度 ≥ 9610 表的最大末段（0x1DA000 + 0x40000 = 0x21A000），非周期图案
        const QByteArray sboot = patternedImage(0x220000);
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos9610"));
        QList<QPair<QString, QByteArray>> parts;
        QString err;
        QVERIFY2(eub::splitSboot(sboot, lo, parts, &err), qPrintable(err));
        QCOMPARE(parts.size(), lo.segments.size());
        // 逐段核对：名字 + 长度 + 内容与 `sboot.mid(offset, length)` 逐字节相同 ——
        // 任意一段的 offset/length 错位都会在这里现形（夹具非周期，见文件头说明）
        for (qsizetype i = 0; i < lo.segments.size(); ++i) {
            const eub::EubSegment &s = lo.segments[i];
            QCOMPARE(parts[i].first, s.name);
            QCOMPARE(parts[i].second.size(), qsizetype(s.length));
            QCOMPARE(parts[i].second, sboot.mid(qsizetype(s.offset), qsizetype(s.length)));
        }
        QCOMPARE(parts[0].second, sboot.mid(0x0, 0x2000));
        QCOMPARE(parts[2].second, sboot.mid(0x15000, 0x2F000));
        // 重发段（facts §C5）：第 4 段与第 1 段**逐字节相同**（都切自 offset 0）
        QCOMPARE(parts[3].second, parts[0].second);
    }

    void splitFailsClosedWhenImageTooShort()
    {
        const QByteArray shortImage(0x1000, '\xAB');   // 小于 8890 第一段之后的任何段
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos8890"));
        QList<QPair<QString, QByteArray>> parts;
        QString err;
        QVERIFY(!eub::splitSboot(shortImage, lo, parts, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(parts.isEmpty());                      // **绝不**产出半段
        // spec §7 的文案必须给出两个尺寸（"表需要 ≥ 0x…，你的文件是 0x…"）——否则用户无法行动
        // 表所需 = max(offset+length)：0x2000 / 0x26000 / 0x4CD10 / **0x132000**（bootloader 末段）
        QVERIFY(err.contains(hex0x(0x132000), Qt::CaseInsensitive));
        QVERIFY(err.contains(hex0x(0x1000), Qt::CaseInsensitive));    // 实际文件尺寸
    }

    void splitAcceptsExactlySizedImageAndRejectsOneByteShort()
    {
        // 边界：8890 表需要 max(offset+length) = 0x61000 + 0xD1000 = 0x132000 字节
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos8890"));
        QList<QPair<QString, QByteArray>> parts;
        QString err;
        QVERIFY2(eub::splitSboot(patternedImage(0x132000), lo, parts, &err), qPrintable(err));
        QCOMPARE(parts.size(), lo.segments.size());     // 恰好够 → 成功（不是 <=）
        QCOMPARE(parts[3].second.size(), qsizetype(0xD1000));

        // 差一字节 → 失败，且**上一轮成功留下的 parts 必须被清掉**（调用方忽略返回值也拿不到半段）
        QVERIFY(!eub::splitSboot(patternedImage(0x131FFF), lo, parts, &err));
        QVERIFY(err.contains(hex0x(0x132000), Qt::CaseInsensitive));
        QVERIFY(err.contains(hex0x(0x131FFF), Qt::CaseInsensitive));
        QVERIFY(parts.isEmpty());
    }

    void sha1HexMatchesKnownVector()
    {
        QCOMPARE(eub::sha1Hex(QByteArray("abc")),
                 QStringLiteral("a9993e364706816aba3e25717850c26c9cd0d89d"));
        QCOMPARE(eub::sha1Hex(QByteArray()),          // 空输入的已知向量
                 QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    }

    void detectSocFromImageFindsExynosToken()   // facts §A4：hubble.py:192-202
    {
        QByteArray img(0x8000, '\0');
        img.replace(0x1234, 12, QByteArray("EXYNOS9610xx"));
        QCOMPARE(eub::detectSocFromImage(img), QStringLiteral("Exynos9610"));
        QCOMPARE(eub::detectSocFromImage(QByteArray(1024, '\0')), QString());
        // 首个匹配优先（hubble 取 findall()[0]）：后面再出现别的 SoC token 也不改答案
        img.replace(0x2000, 12, QByteArray("EXYNOS9810xx"));
        QCOMPARE(eub::detectSocFromImage(img), QStringLiteral("Exynos9610"));
        // 只有 "EXYNOS" 没有数字 → 不是 SoC token（正则要求 [0-9]+）
        QByteArray noDigits(0x100, '\0');
        noDigits.replace(0x10, 6, QByteArray("EXYNOS"));
        QCOMPARE(eub::detectSocFromImage(noDigits), QString());
    }

    // ---- 真样本硬断言（reference/eub-samples/，gitignored；缺失时 QSKIP/FAIL）----
    // 事实出处：docs/superpowers/specs/exynos-eub-facts.md §H2（5 个官方 BL 包解出的 sboot.bin）。
    // 只读样本、绝不写回；样本内容不进仓库（CMake 只把**目录路径**编进本目标）。

    void realSampleSbootsSplitWithinTableBounds()
    {
        // 表能不能**切开真文件**：合成夹具按需造尺寸，永远测不到"表比真包大"这类错。
        // ⚠️ 这些期望值**钉死在具体修订**上（1b 审查 M5）：样本文件名不含修订，重下同类机的 BL 会**原地覆盖**
        //    同名文件 → 红的会是这些以**样本**命名的断言。届时**先核对修订**（下表）再怀疑表：
        //      Exynos9830 ← SM-G980F `G980FXXSNHYB1` ｜ Exynos9610 ← SM-A505FN（FUS 最新）
        //      Exynos7580 ← SM-A510F `A510FXXS8CTI7`（= 表内 sourceNote 记录的修订）
        //      Exynos8895 ← SM-G950F `G950FXXUCDZE9` ｜ Exynos8890 ← SM-G930F `G930FXXU8EVG3`
        //    （表内 8895/8890 两个 sha1 绑定的是 2017 年修订，**已从 FUS 下架**，故手上样本的 sboot 与之不同——见 facts §H3）
        struct Sample { const char *soc; const char *file; quint64 size; };
        const Sample samples[] = {
            {"Exynos9830", "sboot_Exynos9830.bin", 4194304},   // 0x400000
            {"Exynos9610", "sboot_Exynos9610.bin", 4194304},   // 0x400000
            {"Exynos7580", "sboot_Exynos7580.bin", 1618192},   // 0x18B110
            {"Exynos8895", "sboot_Exynos8895.bin", 1847568},   // 0x1C3110
            {"Exynos8890", "sboot_Exynos8890.bin", 1777936},   // 0x1B2110
        };
        for (const Sample &s : samples) {
            if (!eubtest::sampleAvailable(QLatin1String(s.file))) {
                EUB_SKIP_OR_FAIL(QLatin1String(s.file));   // 样本是一个整体：缺一个就整体跳过/判红
            }
        }

        QStringList withoutSample;
        int withSample = 0;
        for (const eub::EubLoadout &lo : eub::allLoadouts()) {
            const Sample *hit = nullptr;
            for (const Sample &s : samples) {
                if (lo.soc == QLatin1String(s.soc))
                    hit = &s;
            }
            if (!hit) {
                withoutSample << lo.soc;
                continue;
            }
            ++withSample;
            const QByteArray sboot = eubtest::readSample(QLatin1String(hit->file));
            QCOMPARE(quint64(sboot.size()), hit->size);   // facts §H2 的尺寸（逐 SoC 钉死）
            QList<QPair<QString, QByteArray>> parts;
            QString err;
            QVERIFY2(eub::splitSboot(sboot, lo, parts, &err), qPrintable(lo.soc + QStringLiteral(": ") + err));
            QCOMPARE(parts.size(), lo.segments.size());
            // 逐段核对：名字 + 长度 + 内容 == sboot.mid(offset, length)。
            // ⚠️ 期望值取自**被测的同一张表** → 本循环只证明"切片自洽 + **不越界**"（与 splitSboot 同源判据），
            //    **证明不了 offset 是否正确**：界内错误（挪一点但仍在文件内）会自洽通过
            //    （1b 审查 Important；实现者的 m2a 变异日志即反证：part6 改 0x80100 时本循环仍绿）。
            //    偏移的正确性由下一个槽 realSampleSbootsCarryTableIndependentLandmarks 的**与表无关地标**承担。
            for (qsizetype i = 0; i < lo.segments.size(); ++i) {
                const eub::EubSegment &seg = lo.segments[i];
                QCOMPARE(parts[i].first, seg.name);
                QCOMPARE(quint64(parts[i].second.size()), seg.length);
                QVERIFY2(parts[i].second == sboot.mid(qsizetype(seg.offset), qsizetype(seg.length)),
                         qPrintable(QStringLiteral("%1 第 %2 段（%3）内容与真 sboot 的切片不符")
                                        .arg(lo.soc).arg(i).arg(seg.name)));
            }
        }
        QCOMPARE(withSample, 5);
        // 无样本的 3 个 SoC 跳过并计数：7885/9810/9820 没有对应机型的官方 BL 包（facts §H 只覆盖 5 个）
        QCOMPARE(withoutSample, QStringList({"Exynos7885", "Exynos9810", "Exynos9820"}));
    }

    void realSampleSbootsCarryTableIndependentLandmarks()
    {
        // 与表无关的结构地标（1b 审查 Important 的落实）：期望值来自**真样本的字节结构**，
        // 不是被测的表 —— 表把这些 offset 写偏（哪怕只偏 0x100）这里就红，而上面那条自指的
        // "逐段内容相符"循环对界内错误无能为力。
        // 事实出处：docs/superpowers/specs/exynos-eub-facts.md §H2；
        // .superpowers/sdd/eub-real-samples-verification.md §6.1（A 5/5）、§6.2（B 9/9）、§6.3（C 反例）。
        struct Sample { const char *soc; const char *file; };
        const Sample samples[] = {
            {"Exynos9830", "sboot_Exynos9830.bin"},
            {"Exynos9610", "sboot_Exynos9610.bin"},
            {"Exynos7580", "sboot_Exynos7580.bin"},
            {"Exynos8895", "sboot_Exynos8895.bin"},
            {"Exynos8890", "sboot_Exynos8890.bin"},
        };
        for (const Sample &s : samples) {
            if (!eubtest::sampleAvailable(QLatin1String(s.file)))
                EUB_SKIP_OR_FAIL(QLatin1String(s.file));
        }

        // B 的判据：头 4 字节 01000014 + 字节 8..12 为 414238d5（§6.2 的 9/9 共同形态）
        const QByteArray kHead4 = QByteArray::fromHex("01000014");
        const QByteArray kTail = QByteArray::fromHex("414238d5");

        for (const Sample &s : samples) {
            eub::EubLoadout lo;
            QString err;
            QVERIFY2(eub::eubLoadoutFor(QLatin1String(s.soc), lo, &err), qPrintable(err));
            const QByteArray sboot = eubtest::readSample(QLatin1String(s.file));
            QVERIFY2(!sboot.isEmpty(), s.file);
            QVERIFY(lo.segments.size() >= 2);

            // —— 地标 A：第二段 offset + 8 处是 ASCII "daeh"（小端 "head"）——
            const qsizetype aOff = qsizetype(lo.segments[1].offset) + 8;
            QVERIFY2(sboot.mid(aOff, 4) == QByteArray("daeh"),
                     qPrintable(QStringLiteral("%1 第二段（%2）offset+8 不是 daeh，实际 %3")
                                    .arg(lo.soc, lo.segments[1].name)
                                    .arg(QString::fromLatin1(sboot.mid(aOff, 4).toHex()))));

            // —— 地标 B / 反例 C：逐段判"该有镜像头的必须有、该没有的必须没有" ——
            for (const eub::EubSegment &seg : lo.segments) {
                const bool expectHeader = (seg.name == QLatin1String("bl2")
                                           || seg.name == QLatin1String("u-boot")
                                           || seg.name == QLatin1String("part5")
                                           || seg.name == QLatin1String("bootloader"));
                const bool expectNoHeader = (seg.name == QLatin1String("lk")
                                             || seg.name == QLatin1String("part6"));
                if (!expectHeader && !expectNoHeader)
                    continue;   // 其余段（fwbl1 / bl31 / epbl / el3_mon / 重发段）真样本里无共同形态，不断言
                const QByteArray head = sboot.mid(qsizetype(seg.offset), 12);
                const bool hasHeader = head.startsWith(kHead4) && head.mid(8, 4) == kTail;
                if (expectHeader)
                    QVERIFY2(hasHeader,
                             qPrintable(QStringLiteral("%1 的 %2（0x%3）起点不是镜像头，实际 %4")
                                            .arg(lo.soc, seg.name).arg(seg.offset, 0, 16)
                                            .arg(QString::fromLatin1(head.toHex()))));
                else
                    QVERIFY2(!hasHeader,
                             qPrintable(QStringLiteral("%1 的 %2（0x%3）本应**无**标准镜像头，却命中 01000014…414238d5")
                                            .arg(lo.soc, seg.name).arg(seg.offset, 0, 16)));
            }
        }
    }

    void real7580SbootSha1MatchesTableSourceNote()
    {
        // facts §H2：真 A510FXXS8CTI7 的 sboot sha1 = 466852d1…，与 7580 表 sourceNote 里记的
        // **未被采信那一源**的修订号逐字符一致（表项 sbootSha1 刻意留空，见 table7580()）。
        const QString file = QStringLiteral("sboot_Exynos7580.bin");
        if (!eubtest::sampleAvailable(file)) {
            EUB_SKIP_OR_FAIL(file);
        }
        const QByteArray sboot = eubtest::readSample(file);
        QVERIFY2(!sboot.isEmpty(), qPrintable(file));
        const QString sha1 = eub::sha1Hex(sboot);
        QCOMPARE(sha1, QStringLiteral("466852d13fa02d51729d21633f47708308579f58"));
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos7580"));
        QVERIFY(lo.sbootSha1.isEmpty());                    // 表项不记该 sha1（采信的分歧源无修订记录）
        QVERIFY2(lo.sourceNote.contains(sha1), qPrintable(lo.sourceNote));   // 但出处里记着 → 两处一致
    }

    void real8895SbootHasOnly272BytesSurplus()
    {
        // facts §H2：真 sboot 1,847,568 字节 vs 表所需 max(offset + length) = 0x1C3000 = 1,847,296
        // → 富余**恰好 272**。这是"表能用、但几乎没余量"的边界：既证明表没写超，也把"表多要一字节
        // 才够"这类改动钉红 —— 合成夹具按需造尺寸，这条边界它永远碰不到。
        const QString file = QStringLiteral("sboot_Exynos8895.bin");
        if (!eubtest::sampleAvailable(file)) {
            EUB_SKIP_OR_FAIL(file);
        }
        const QByteArray sboot = eubtest::readSample(file);
        const eub::EubLoadout lo = mustLoad(QStringLiteral("Exynos8895"));
        QCOMPARE(quint64(sboot.size()), quint64(1847568));   // 真文件尺寸
        QCOMPARE(tableNeed(lo), quint64(1847296));           // 表所需 = 0x143000 + 0x80000
        QCOMPARE(quint64(sboot.size()) - tableNeed(lo), quint64(272));   // 富余：**恰好 272**
        // 272 字节也算"够"：切段必须成功且末段（part6，0x143000/0x80000）真的切得出来 ——
        // 只断言"没报错"不够，末段尾部必须落在真文件之内。
        QList<QPair<QString, QByteArray>> parts;
        QString err;
        QVERIFY2(eub::splitSboot(sboot, lo, parts, &err), qPrintable(err));
        QCOMPARE(parts.size(), lo.segments.size());
        QCOMPARE(quint64(parts.last().second.size()), quint64(0x80000));
    }
};

QTEST_APPLESS_MAIN(TestEubLoadout)
#include "test_eub_loadout.moc"
