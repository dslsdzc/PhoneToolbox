// tests/test_mtk_chip_table.cpp
//
// 芯片表（由 tools/gen_mtk_chip_table.py 从 mtkclient 的 brom_config.py 转写）。
// 这里只钉"表可用 + 未收录明确返回 nullptr + 已知机型代际正确"，不钉具体条目数（表随上游变化）。
#include <QtTest>

#include "core/modes/mtk_chip_table.h"

class TestMtkChipTable : public QObject
{
    Q_OBJECT
private slots:
    void tableIsPopulated();
    void knownChipsHaveExpectedGeneration();
    void unknownChipReturnsNull();
    void damodeNames();
};

void TestMtkChipTable::tableIsPopulated()
{
    // "更大的表"（用户决策）的落地口径是**整表转写**（不是挑几十个常用芯片），不是条目数绝对值。
    //   下限 80 而非 100：上游 71b0175 = **89 条**（v2.1.4.1-20），master 同样 ~89 ——
    //   100 是不可达的圆整数；下限 80 = 留约 10% 余量。
    //   本用例只钉"表没被换成 stub / 没被截断"；解析回归由生成脚本的结构不变量
    //   （解析条数 == dict int key 数 + 逐 key 校验）在生成期就拦住，不靠这个数字。
    QVERIFY2(mtkbrom::chipTableSize() >= 80,
             qPrintable(QStringLiteral("表项数 %1（上游 71b0175 = 89）").arg(mtkbrom::chipTableSize())));
}

void TestMtkChipTable::knownChipsHaveExpectedGeneration()
{
    // 已知机型（与真样本 da_parse_report.json 的实测一致）：
    //   0x907/0x992/0x1066/0x1129 = V6 文件里的条目 → XML 代
    //   0x6752 = 现代老平台 → 非 XML（LEGACY 或 XFLASH，两者都由 damode 给出）
    //   （不列 0x6765：上游**表里没有**这个 hw_code —— 它只出现在 DA 真样本里，
    //     拿它断言表内容会假红。）
    // 更正（实测上游表，非推测）："出现在 V6 文件里" **不等于**代际是 XML —— 文件格式（isV6，
    // 见 mtk_da_file.h 的 P10）与 DA 协议代是两条轴。上游表里 0x907/0x1129 = XML，
    // 而 0x992/0x1066 = **XFLASH**（真样本 MTK_DA_V6.bin 里两者都有条目）。
    // 故本用例只钉 0x907 是 XML，不对 0x992/0x1066 的代际下断言。
    const mtkbrom::ChipInfo *xml = mtkbrom::lookupChip(0x0907);
    QVERIFY(xml != nullptr);
    QCOMPARE(xml->damode, mtkbrom::DaMode::Xml);
    const mtkbrom::ChipInfo *legacy = mtkbrom::lookupChip(0x6752);
    QVERIFY(legacy != nullptr);
    QVERIFY(legacy->damode != mtkbrom::DaMode::Xml);
    // 生成脚本保证 dacode 非 0（缺省回填 hw_code）—— 0 会让 DA 条目匹配必然落空
    QVERIFY(legacy->dacode != 0);
    // iot 位必须转写进来（Task 2 审查 ⚠️：IoT 芯片在 LEGACY 里走另一套 region 映射）
    // 上游 brom_config.py 的 0x6226（MT6226）是 iot=True；真样本 iot DA 文件里就有 0x6226 条目
    const mtkbrom::ChipInfo *iot = mtkbrom::lookupChip(0x6226);
    QVERIFY(iot != nullptr);
    QVERIFY2(iot->iot, "0x6226 必须是 iot=true（上游 hwconfig 如此）—— 上层据此拒绝 IoT 芯片");
    QVERIFY2(!legacy->iot, "0x6752 不是 IoT 芯片（防止把 iot 位写成恒 true）");
    // 表中所有条目都必须落在三个已知枚举内（生成时的映射写错会在这里露出来）
    for (quint16 hw = 0x0001; hw < 0xFFFF; ++hw) {
        const mtkbrom::ChipInfo *c = mtkbrom::lookupChip(hw);
        if (!c)
            continue;
        QVERIFY(c->damode == mtkbrom::DaMode::Legacy
                || c->damode == mtkbrom::DaMode::XFlash
                || c->damode == mtkbrom::DaMode::Xml);
        QCOMPARE(c->hwCode, hw);
    }
}

void TestMtkChipTable::unknownChipReturnsNull()
{
    // 表外芯片 → nullptr；**上层必须明确报错不猜**（spec §8）
    QVERIFY(mtkbrom::lookupChip(0x0001) == nullptr);
    QVERIFY(mtkbrom::lookupChip(0xFFFF) == nullptr);
}

void TestMtkChipTable::damodeNames()
{
    QCOMPARE(mtkbrom::damodeName(mtkbrom::DaMode::Legacy), QStringLiteral("LEGACY"));
    QCOMPARE(mtkbrom::damodeName(mtkbrom::DaMode::XFlash), QStringLiteral("XFLASH"));
    QCOMPARE(mtkbrom::damodeName(mtkbrom::DaMode::Xml), QStringLiteral("XML"));
}

QTEST_APPLESS_MAIN(TestMtkChipTable)
#include "test_mtk_chip_table.moc"
