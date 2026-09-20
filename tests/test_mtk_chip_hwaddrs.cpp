// tests/test_mtk_chip_hwaddrs.cpp
//
// W8-T2：逐芯片硬件地址表（ChipHwAddrs）。表体由 tools/gen_mtk_chip_table.py 从
// mtkclient（GPL-3.0 子模块，只读参照）的 mtkclient/config/brom_config.py 的 `hwconfig`
// 字典转写；本用例钉的是**转写的忠实性**（"有没有"与"值是多少"都对得上上游静态文本），
// **不是**"这些地址在真机上好用" —— 本仓无 MTK 真机，本表全部证据只有转写证据。
//
// 期望值的来源（可逐条复核，不是从生成器产物回抄的）：
//   上游 commit 71b01752d39461dfbf9c21bc29d7b0e121c40483（v2.1.4.1-20）的 brom_config.py，
//   `hwconfig` 字典里 89 个 `Chipconfig(...)` 调用。量法：ast 解析后逐 kwarg 判断
//   —— kwarg 缺失 **或** 值为 None 字面量都算"该字段缺失"（上游 `Chipconfig.__init__` 的
//   形参都有 None 默认值，把 None 当 0 就是砖机）。本文件的期望值由两条独立路径得出且一致：
//   控制方 2026-09-20 的预检、本任务实现者的独立 ast 脚本；两者都是 89/71/66/68/74/66/38/68。
//
// 上游字段的覆盖极不齐整 —— 这是本任务的核心事实，也是"全齐才收录"会砍掉功能的原因：
//   watchdog 71 / uart 66 / brom_payload_addr 68 / da_payload_addr 74 /
//   pl_payload_addr 66 / cqdma_base 38 / sej_base 68（共 89 条）。
//
// ⚠️ **W8-T3 起本表多了一列**：`var1`（控制传输的 wIndex）作为**第 8 个字段**由控制方
// 2026-09-20 裁决补入（上游 89 条实测 **56 条有值 / 33 条未写 / 0 条显式 None**，众数 10 = 41 条）。
// 因此本文件里"全齐"的口径由**七**字段改为**八**字段，T2 时代钉的 `nAllSeven = 37` 相应变为
// `nAllEight = 35`（不是表错，是"多了一列、能全齐的芯片少了 2 条"）。
// kamakiri 三要素口径（watchdog + uart + 任一 payload）**未变**，仍是 66；新增的是
// "三要素齐全 **且** var1 有值"= 56（可开箱即用）与"需调用方覆盖 var1"= 10 两个数字。
#include <QtTest>

#include <QStringList>
#include <QVector>

#include "core/modes/mtk_chip_table.h"

namespace {

// 与生成器/结构体**同序**的 8 个字段名（失败信息里用；顺序 = ChipHwAddrs 的声明顺序）。
// var1 在**末尾**（生成器 HW_FIELDS 的追加方式决定的：末尾追加不动前 7 列的位置）。
const char *const kFieldNames[8] = {
    "watchdog", "uart", "bromPayloadAddr", "daPayloadAddr",
    "plPayloadAddr", "cqdmaBase", "sejBase", "var1",
};

// kamakiri（漏洞利用）真正需要的三要素：watchdog + uart + 任一 payload 地址。
// 上游 kamakiri.py 只往载荷尾部写这三个值（见 W8-T3 的 fixExploitPayload 验收）。
bool kamakiriUsable(const mtkbrom::ChipHwAddrs &a)
{
    return a.hasWatchdog && a.hasUart
           && (a.hasBromPayloadAddr || a.hasDaPayloadAddr || a.hasPlPayloadAddr);
}

// 全扫 0x0000..0xFFFF 的 hw_code，收集命中的行（按 hw_code 升序）。
//
// 为什么全扫而不是列一张 hw_code 表：这样"表里有条目但查不到"（重复键 / 查找写错 / 表里
// 有条目被吞）也会露出来 —— 命中数必须等于表条数，且每个 code 必须命中它自己那一行。
// 兄弟用例 test_mtk_chip_table.cpp 的 knownChipsHaveExpectedGeneration 同款（0x0001 起）。
QVector<const mtkbrom::ChipHwAddrs *> scanAllHwAddrChips()
{
    QVector<const mtkbrom::ChipHwAddrs *> hits;
    for (quint32 hw = 0x0000; hw <= 0xFFFF; ++hw) {
        if (const mtkbrom::ChipHwAddrs *a = mtkbrom::lookupChipHwAddrs(quint16(hw)))
            hits.append(a);
    }
    return hits;
}

QString chipTag(quint32 hwCode)
{
    return QStringLiteral("0x%1").arg(hwCode, 4, 16, QLatin1Char('0'));
}

} // namespace

class TestMtkChipHwAddrs : public QObject
{
    Q_OBJECT
private slots:
    void everyUpstreamChipIsReachable();
    void fieldCoverageMatchesUpstream();
    void kamakiriUsabilityBoundary();
    void presenceFlagAndValueAgree();
    void knownChipsHaveExpectedAddresses();
    void unknownChipReturnsNull();
};

// 89 条全部可查、逐 code 命中自己那一行、且与兄弟表（lookupChip，DA 上传路径用）同 key 集。
void TestMtkChipHwAddrs::everyUpstreamChipIsReachable()
{
    QVector<const mtkbrom::ChipHwAddrs *> hits;
    for (quint32 hw = 0x0000; hw <= 0xFFFF; ++hw) {
        const quint16 code = quint16(hw);
        const mtkbrom::ChipHwAddrs *a = mtkbrom::lookupChipHwAddrs(code);
        const mtkbrom::ChipInfo *c = mtkbrom::lookupChip(code);
        // 两张表由**同一次生成**从同一个 hwconfig 转写 ⇒ 存在性必须逐 code 一致。
        // 若生成器在某一侧漏了一条，DA 上传路径与漏洞利用路径会对同一颗芯片给出互相矛盾的结论
        // （一边"认识"、一边"未收录"）—— 那是最难查的一类不一致。
        QVERIFY2((a != nullptr) == (c != nullptr),
                 qPrintable(QStringLiteral("%1：hwaddrs=%2 与 chip=%3 的存在性不一致")
                                .arg(chipTag(code))
                                .arg(a ? QStringLiteral("有") : QStringLiteral("null"))
                                .arg(c ? QStringLiteral("有") : QStringLiteral("null"))));
        if (!a)
            continue;
        // 命中邻居（表未按 hw_code 排序 / 查找写错）会在这里露出来
        QCOMPARE(a->hwCode, code);
        hits.append(a);
    }
    // 89 = 上游 hwconfig 的条目数。用 == 而不是 >=：既钉"没丢条目"，也钉"没多出条目"
    // （多出来意味着解析读到了别的 dict / 上游改了表却没重跑生成器）。
    QCOMPARE(hits.size(), 89);
}

// 逐字段"有没有"的覆盖直方图 —— 对"生成器漏字段"最有甄别力的一组数字。
void TestMtkChipHwAddrs::fieldCoverageMatchesUpstream()
{
    const QVector<const mtkbrom::ChipHwAddrs *> hits = scanAllHwAddrChips();
    QCOMPARE(hits.size(), 89);

    int nWatchdog = 0, nUart = 0, nBrom = 0, nDa = 0, nPl = 0, nCqdma = 0, nSej = 0, nVar1 = 0;
    int nAllEight = 0, nUsable = 0, nUsableButIncomplete = 0, nOnlyCqdmaMissing = 0;
    int nExploitReady = 0, nNeedsVar1Override = 0;
    for (const mtkbrom::ChipHwAddrs *a : hits) {
        nWatchdog += a->hasWatchdog;
        nUart     += a->hasUart;
        nBrom     += a->hasBromPayloadAddr;
        nDa       += a->hasDaPayloadAddr;
        nPl       += a->hasPlPayloadAddr;
        nCqdma    += a->hasCqdmaBase;
        nSej      += a->hasSejBase;
        nVar1     += a->hasVar1;

        const bool allEight = a->hasWatchdog && a->hasUart && a->hasBromPayloadAddr
                              && a->hasDaPayloadAddr && a->hasPlPayloadAddr
                              && a->hasCqdmaBase && a->hasSejBase && a->hasVar1;
        nAllEight += allEight;
        const bool usable = kamakiriUsable(*a);
        nUsable += usable;
        nUsableButIncomplete += (usable && !allEight);
        // W8-T3 的两个新口径：三要素齐全之后，var1 决定"开箱即用"还是"要调用方覆盖"。
        nExploitReady += (usable && a->hasVar1);
        nNeedsVar1Override += (usable && !a->hasVar1);
        // "只缺 cqdma_base"（其余六个都有）：kamakiri **不用** cqdma，这些芯片照样可用。
        // 必须严格判（六个**逐个**都要有），不能用"可用 && 缺 cqdma"这种松弛写法 ——
        // 后者会把"还缺别的字段"的芯片也算进来，数字就失去意义了。
        const bool allButCqdma = a->hasWatchdog && a->hasUart && a->hasBromPayloadAddr
                                 && a->hasDaPayloadAddr && a->hasPlPayloadAddr && a->hasSejBase;
        nOnlyCqdmaMissing += (allButCqdma && !a->hasCqdmaBase);
    }

    // 8 个数字逐条对着上游 71b0175 数过（见文件头"期望值的来源"；var1=56 是 W8-T3 补的）
    QCOMPARE(nWatchdog, 71);
    QCOMPARE(nUart, 66);
    QCOMPARE(nBrom, 68);
    QCOMPARE(nDa, 74);
    QCOMPARE(nPl, 66);
    QCOMPARE(nCqdma, 38);
    QCOMPARE(nSej, 68);

    // W8-T3 补的第 8 列（var1）：56 条有值 —— 与上游 71b0175 的 ast 实测一致
    // （众数 10 = 41 条；取值集合 {10,37,40,115,127,172,177,180,204}）。
    QCOMPARE(nVar1, 56);
    // 本任务最大的坑的量化：按"全齐才收录"实现，可用芯片会从 66 掉到 35
    // —— 白扔 31 条（其中**只缺 cqdma_base** 的就有 17 条，而 kamakiri 根本不用 cqdma）。
    // 这几条断言是"不许按全齐口径过滤"的可执行证据：一旦有人把过滤条件收紧，数字立刻对不上。
    // ⚠️ 口径变更留痕：T2 时代是"七字段全齐 = 37"，W8-T3 加了 var1 列后八字段全齐 = **35**
    //    （37 里那两颗 0x0279/0x0335 之类仍有 var1，掉出来的 2 条是"其余七列齐全但上游没给 var1"）。
    QCOMPARE(nAllEight, 35);
    QCOMPARE(nUsable, 66);                // 三要素口径**没变**（var1 不参与这三要素）
    QCOMPARE(nUsableButIncomplete, 31);   // 66 - 35
    QCOMPARE(nOnlyCqdmaMissing, 17);
    // 三要素齐全的 66 条里：56 条表里有 var1（开箱即用）、10 条要调用方显式覆盖
    // （上游对那 10 条也没给 var1 —— 上游靠 bruteforce 扫 0..0xFF 找）。
    QCOMPARE(nExploitReady, 56);
    QCOMPARE(nNeedsVar1Override, 10);
    QCOMPARE(nExploitReady + nNeedsVar1Override, nUsable);
}

// 诚实边界：上游数据本身就凑不齐 kamakiri 三要素的那些芯片（**不是本仓漏转写**）。
void TestMtkChipHwAddrs::kamakiriUsabilityBoundary()
{
    const QVector<const mtkbrom::ChipHwAddrs *> hits = scanAllHwAddrChips();

    QStringList unusable, allSevenMissing;
    int usable = 0;
    for (const mtkbrom::ChipHwAddrs *a : hits) {
        if (kamakiriUsable(*a)) {
            ++usable;
            continue;
        }
        unusable.append(chipTag(a->hwCode));
        if (!a->hasWatchdog && !a->hasUart && !a->hasBromPayloadAddr && !a->hasDaPayloadAddr
            && !a->hasPlPayloadAddr && !a->hasCqdmaBase && !a->hasSejBase)
            allSevenMissing.append(chipTag(a->hwCode));
    }

    QCOMPARE(usable, 66);
    // 23 条缺 watchdog 或 uart ⇒ 本特性对它们**不可用**。它们**仍然在表内**（lookup 非空、
    // 不是 nullptr）—— 上层（T3+）必须显式拒绝，不许拿 0 兜底当"没有地址"。
    // 这是**上游数据事实**（89 条里 23 条如此），不是本仓缺陷，也不许含糊成"未收录"。
    QCOMPARE(unusable.size(), 23);

    // 其中 10 条八个字段全缺 —— 上游原文里这些行是**整段注释掉的**（如 0x6236 的
    // `# sej_base=0x80140000,`），逐条列出以便人工对着上游复核：
    QCOMPARE(allSevenMissing,
             QStringList({QStringLiteral("0x6236"), QStringLiteral("0x6238"),
                          QStringLiteral("0x6253"), QStringLiteral("0x6256"),
                          QStringLiteral("0x625a"), QStringLiteral("0x6268"),
                          QStringLiteral("0x6270"), QStringLiteral("0x6276"),
                          QStringLiteral("0x6291"), QStringLiteral("0x8518")}));
}

// 值槽与存在位的对称不变量：值 == 0 **当且仅当** has == false。
//
// 这一条是"缺字段"这一类事故的全局杀手：
//   ① 缺的字段被填 0 冒充存在（往 watchdog 写 0 = 砖机）→ has 为真而值 == 0 → 红；
//   ② 存在位被抹掉但值还留着 → 值 != 0 而 has 为假 → 红。
// 生成器侧另有一条独立的对称校验（present 且值为 0 → 拒绝生成），两条腿互为证据。
void TestMtkChipHwAddrs::presenceFlagAndValueAgree()
{
    const QVector<const mtkbrom::ChipHwAddrs *> hits = scanAllHwAddrChips();
    QCOMPARE(hits.size(), 89);

    for (const mtkbrom::ChipHwAddrs *a : hits) {
        const quint32 values[8] = {a->watchdog, a->uart, a->bromPayloadAddr,
                                   a->daPayloadAddr, a->plPayloadAddr,
                                   a->cqdmaBase, a->sejBase, a->var1};
        const bool present[8] = {a->hasWatchdog, a->hasUart, a->hasBromPayloadAddr,
                                 a->hasDaPayloadAddr, a->hasPlPayloadAddr,
                                 a->hasCqdmaBase, a->hasSejBase, a->hasVar1};
        for (int i = 0; i < 8; ++i) {
            QVERIFY2((values[i] == 0) == !present[i],
                     qPrintable(QStringLiteral("%1 的 %2：值=0x%3 而存在位=%4 —— "
                                               "值槽为 0 必须当且仅当存在位为假")
                                    .arg(chipTag(a->hwCode), QString::fromLatin1(kFieldNames[i]))
                                    .arg(values[i], 8, 16, QLatin1Char('0'))
                                    .arg(present[i] ? QStringLiteral("真") : QStringLiteral("假"))));
        }
    }
}

// 抽查：8 颗芯片的 8 个值与 8 个存在位逐一对着上游行号复核。
// 选取标准：覆盖"八字段全齐 / 只缺 cqdma / 只缺 uart / 只有 sej / 八字段全缺"五种形态，
// W8-T3 追加的 var1 列同时覆盖"有值"（0x0279/0x0335/0x0907/0x6752/0x1066）与"上游没给"
// （0x0571/0x6226/0x6236 三颗上游原文就是注释掉的 `# var1`）两种形态。
// 值的选择：**字段齐全的那 4 颗**（0x0279/0x0335/0x0907/0x6752）的存在值两两不同 ——
// 这样字段互换、把某一行抄给别的行这类错误会露出来。（缺失字段的值槽一律写 0，于是
// 0x0571/0x6226/0x6236 等会有多个 0 槽 —— 那是"没有该字段"的写法、不是"值相同"，
// 本表对"有没有"的表达是 hasXxx，值槽的 0 在 hasXxx == false 时无意义。）
// 注意上游大量芯片的各字段值彼此相同 —— 如 0x0279 与 0x0326 完全一致（含存在位）——
// 只抽查其中一颗是不够的（0x0326 不在抽查表里，正说明这一点）。
void TestMtkChipHwAddrs::knownChipsHaveExpectedAddresses()
{
    struct ChipExpectation {
        quint16 hwCode;
        quint32 value[8];       // 顺序同 kFieldNames；has 为 false 的槽写 0（不参与断言）
        bool    has[8];         // 末位 = var1（W8-T3 补的第 8 列）
        int     upstreamLine;   // 上游 71b0175 里该 Chipconfig( 的起始行，可 sed 复核
    };
    const ChipExpectation expected[] = {
        // 0x0279（上游 :1613，最低 hw_code）：八字段全齐
        // var1=10（上游 :1614 `var1=0xA,`）
        {0x0279, {0x10007000, 0x11002000, 0x100a00, 0x201000, 0x40200000, 0x10212c00, 0x1000a000, 0xa},
                 {true, true, true, true, true, true, true, true}, 1613},
        // 0x0335（上游 :1133）：八字段全齐，watchdog/cqdma/sej 与 0x0279 不同值
        //   （var1 恰好与 0x0279 不同：40 vs 10 —— 抽查表里因此也覆盖了 var1 的差异）
        // var1=40（上游 :1134）
        {0x0335, {0x10212000, 0x11002000, 0x100a00, 0x201000, 0x40200000, 0x10217c00, 0x10008000, 0x28},
                 {true, true, true, true, true, true, true, true}, 1133},
        // 0x0907（上游 :1832，XML 代 —— 兄弟用例也用它）：watchdog/uart/cqdma 都与众不同
        // var1=10（上游 :1833）
        {0x0907, {0x1c007000, 0x11001000, 0x100a00, 0x201000, 0x40200000, 0x10212000, 0x1000a000, 0xa},
                 {true, true, true, true, true, true, true, true}, 1832},
        // 0x6752（上游 :1202）：八字段全齐，pl 与 0x0279 不同（0x40001000 vs 0x40200000）
        // var1=40（上游 :1203）
        {0x6752, {0x10007000, 0x11002000, 0x100a00, 0x201000, 0x40001000, 0x10212c00, 0x1000a000, 0x28},
                 {true, true, true, true, true, true, true, true}, 1202},
        // 0x1066（上游 :1534）：**只缺 cqdma_base** 的 17 条之一 —— 表内但字段不全的形态：
        // lookup 必须非空，且 cqdma 的存在位必须是假（不许填 0 冒充）
        // var1=0x73=115（上游 :1535 `var1=0x73,  # confirmed`）—— 它是三要素齐全但缺 cqdma 的那 17 条之一，
        // 且**有** var1：正好说明"字段全齐"与"可用"是两件事。
        {0x1066, {0x10007000, 0x11002000, 0x100a00, 0x201000, 0x40200000, 0x00000000, 0x1000a000, 0x73},
                 {true, true, true, true, true, false, true, true}, 1534},
        // 0x0571（上游 :480）：只有 watchdog（uart 在上游是注释行）—— 存在位必须逐字段独立，
        // 不许"一颗芯片要么全有要么全无"地简化
        // var1 未给（上游 :480 `0x571: Chipconfig(  # var1` —— 那行就是被注释掉的 var1 位）
        {0x0571, {0x10007000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
                 {true, false, false, false, false, false, false, false}, 480},
        // 0x6226（上游 :673，iot=True 的 MT6226）：只有 sej_base（其余皆注释）——
        // "表内有、但三要素一个都不够"的中间形态
        {0x6226, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80140000, 0x00000000},
                 {false, false, false, false, false, false, true, false}, 673},
        // 0x6236（上游 :688）：八个字段全缺（连 sej_base 那行都是注释掉的）
        {0x6236, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000},
                 {false, false, false, false, false, false, false, false}, 688},
    };

    const quint32 mtkbrom::ChipHwAddrs::*const valueMembers[8] = {
        &mtkbrom::ChipHwAddrs::watchdog, &mtkbrom::ChipHwAddrs::uart,
        &mtkbrom::ChipHwAddrs::bromPayloadAddr, &mtkbrom::ChipHwAddrs::daPayloadAddr,
        &mtkbrom::ChipHwAddrs::plPayloadAddr, &mtkbrom::ChipHwAddrs::cqdmaBase,
        &mtkbrom::ChipHwAddrs::sejBase, &mtkbrom::ChipHwAddrs::var1,
    };
    const bool mtkbrom::ChipHwAddrs::*const hasMembers[8] = {
        &mtkbrom::ChipHwAddrs::hasWatchdog, &mtkbrom::ChipHwAddrs::hasUart,
        &mtkbrom::ChipHwAddrs::hasBromPayloadAddr, &mtkbrom::ChipHwAddrs::hasDaPayloadAddr,
        &mtkbrom::ChipHwAddrs::hasPlPayloadAddr, &mtkbrom::ChipHwAddrs::hasCqdmaBase,
        &mtkbrom::ChipHwAddrs::hasSejBase, &mtkbrom::ChipHwAddrs::hasVar1,
    };

    for (const ChipExpectation &e : expected) {
        const mtkbrom::ChipHwAddrs *a = mtkbrom::lookupChipHwAddrs(e.hwCode);
        QVERIFY2(a != nullptr, qPrintable(QStringLiteral("%1 不在表内（上游 %2 有这一条）")
                                              .arg(chipTag(e.hwCode))
                                              .arg(e.upstreamLine)));
        QCOMPARE(a->hwCode, e.hwCode);
        for (int i = 0; i < 8; ++i) {
            const QString field = QString::fromLatin1(kFieldNames[i]);
            QVERIFY2((a->*valueMembers[i]) == e.value[i],
                     qPrintable(QStringLiteral("%1 的 %2：期望 0x%3 实得 0x%4（上游 :%5）")
                                    .arg(chipTag(e.hwCode), field)
                                    .arg(e.value[i], 8, 16, QLatin1Char('0'))
                                    .arg(a->*valueMembers[i], 8, 16, QLatin1Char('0'))
                                    .arg(e.upstreamLine)));
            QVERIFY2((a->*hasMembers[i]) == e.has[i],
                     qPrintable(QStringLiteral("%1 的 %2 存在位：期望 %3 实得 %4（上游 :%5）")
                                    .arg(chipTag(e.hwCode), field)
                                    .arg(e.has[i] ? QStringLiteral("真") : QStringLiteral("假"))
                                    .arg((a->*hasMembers[i]) ? QStringLiteral("真")
                                                             : QStringLiteral("假"))
                                    .arg(e.upstreamLine)));
        }
    }
}

// 表外 hw_code → nullptr（上层必须明确报错，不得猜 —— 与兄弟表 lookupChip 同口径）。
void TestMtkChipHwAddrs::unknownChipReturnsNull()
{
    QVERIFY(mtkbrom::lookupChipHwAddrs(0x0000) == nullptr);
    QVERIFY(mtkbrom::lookupChipHwAddrs(0x0001) == nullptr);
    QVERIFY(mtkbrom::lookupChipHwAddrs(0xFFFF) == nullptr);
    // 0x6765 是**上游 hwconfig 里没有**的 hw_code（它只出现在真 DA 样本里）——
    // 拿它当"未收录"的判据是安全的，且与兄弟用例的注释互相印证。
    QVERIFY(mtkbrom::lookupChipHwAddrs(0x6765) == nullptr);
}

QTEST_APPLESS_MAIN(TestMtkChipHwAddrs)
#include "test_mtk_chip_hwaddrs.moc"
