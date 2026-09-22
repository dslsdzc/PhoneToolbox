// tests/test_risk_tiers.cpp
//
// riskui 风险分级映射的逐枚举/逐项钉子 —— W2 Task 1（用户协议与风险分级门控）。
//
// 为什么这些断言值钱：分级是门控的**唯一判据** —— `riskTierForMode/riskTierForFeature` 返回
// `None` 就不弹二次确认，对应入口直接执行写入/绕过动作。于是两类缺陷必须在**本文件**变红：
//   ① 某个模式/功能登记错档（该弹的不弹、不该弹的弹）；
//   ② DeviceMode 追加取值而映射没跟着登记 —— 这条由**编译期**机制兜住，不在运行期依赖用例
//      跑到（见下方 static_assert 与实现里 switch 无 default + `-Wswitch` 升级为 error，
//      两条机制各有变异留档：.superpowers/sdd/w2-task-1-mutation-logs/）。
//
// 做法是**独立第二副本**：期望表与文案逐字手写（不从实现反推）。实现改映射/改文案而这里没跟着改
// → 红。文案里含中文标点，逐字钉死是有意的：`tierWarnings` 的文本会**直接展示**在二次确认
// 对话框里，是用户看到的最后一道说明。
#include <QtTest>

#include "risk_ui.h"

namespace {

// 期望表（12 项，**顺序 = DeviceDetector::DeviceMode 的枚举值 0..11**，与 kModeNames 逐行对应）。
// 逐行手写、不从实现反推：实现把某档改掉 → 那一行红。
// 裁决来源：W2 计划「映射表」——除 MODE_UNKNOWN 外全部 Tier2（每个协议通道都是底层写路径；
// ADB/Recovery 模式下 FlashPanel 同样执行刷入/擦除，属写操作）。
const riskui::RiskTier kExpected[] = {
    riskui::RiskTier::None,       //  0 MODE_UNKNOWN          无设备 = 无写操作
    riskui::RiskTier::Tier2Boot,  //  1 MODE_ADB
    riskui::RiskTier::Tier2Boot,  //  2 MODE_FASTBOOT
    riskui::RiskTier::Tier2Boot,  //  3 MODE_FASTBOOTD
    riskui::RiskTier::Tier2Boot,  //  4 MODE_EDL_9008
    riskui::RiskTier::Tier2Boot,  //  5 MODE_MTK_DA
    riskui::RiskTier::Tier2Boot,  //  6 MODE_RECOVERY
    riskui::RiskTier::Tier2Boot,  //  7 MODE_MTK_BROM
    riskui::RiskTier::Tier2Boot,  //  8 MODE_HUAWEI_USB_UPDATE
    riskui::RiskTier::Tier2Boot,  //  9 MODE_SPD
    riskui::RiskTier::Tier2Boot,  // 10 MODE_SAMSUNG_ODIN
    riskui::RiskTier::Tier2Boot,  // 11 MODE_SAMSUNG_EUB
};

// 表长钉在**最后一个枚举项的值**上（不是"项数"）：kExpected 必须正好覆盖 0..MODE_SAMSUNG_EUB。
// ⚠️ 它**兜不住"追加取值"** —— `MODE_TMP = 12` 时本 static_assert 依然成立（12 == 12）：
//    变异⑧（.superpowers/sdd/w2-task-1-mutation-logs/）实测，追加取值时本 TU 照常编过（零报错）。
//    "追加取值"唯一的编译期防线是实现里的 -Wswitch，而那道防线**只在 GCC/Clang 生效**
//    （MSVC 上静默返回 None，见 risk_ui.cpp 的 ⚠️ 注释）。故本表与实现的登记靠人 + 评审，
//    这里**没有哨兵**；本用例的循环也只跑到 MODE_SAMSUNG_EUB，运行期同样发现不了新增取值。
static_assert(sizeof(kExpected) / sizeof(kExpected[0]) == DeviceDetector::MODE_SAMSUNG_EUB + 1,
              "DeviceMode 新增时同步更新本文件的 kExpected / kModeNames");

// 失败信息用的枚举名（诊断用，顺序同上；只为了红了能一眼看出是哪个模式）。
const char *const kModeNames[] = {
    "MODE_UNKNOWN", "MODE_ADB",          "MODE_FASTBOOT",        "MODE_FASTBOOTD",
    "MODE_EDL_9008", "MODE_MTK_DA",      "MODE_RECOVERY",        "MODE_MTK_BROM",
    "MODE_HUAWEI_USB_UPDATE", "MODE_SPD", "MODE_SAMSUNG_ODIN",   "MODE_SAMSUNG_EUB",
};
static_assert(sizeof(kModeNames) / sizeof(kModeNames[0]) == sizeof(kExpected) / sizeof(kExpected[0]),
              "kModeNames 与 kExpected 必须等长（否则失败信息会把模式名张冠李戴）");

// RiskFeature 全项（17 项，**顺序 = 枚举声明顺序**）：期望档位逐项手写。
// 分组依枚举注释：前 9 项写操作 → Tier2Boot；后 8 项诊断/绕过 → Tier3Diagnostic
// （W8-T5 起 16→17：新增 PreloaderPatch，属「削弱安全机制」族）。
// （W2-T5 复审裁决新增 4 项：SystemPartitionWrite / AppUninstall / SelinuxMode / DeviceSpoof ——
//   登记表漏项在这里红，这是本文件的核心机制。）
struct FeatureRow {
    riskui::RiskFeature feature;
    const char *name;
    riskui::RiskTier tier;
};

const FeatureRow kFeatures[] = {
    {riskui::RiskFeature::DieRepair,      "DieRepair",      riskui::RiskTier::Tier2Boot},
    {riskui::RiskFeature::FrpClear,       "FrpClear",       riskui::RiskTier::Tier2Boot},
    {riskui::RiskFeature::PartitionFlash, "PartitionFlash", riskui::RiskTier::Tier2Boot},
    {riskui::RiskFeature::Unlock,         "Unlock",         riskui::RiskTier::Tier2Boot},
    {riskui::RiskFeature::Sideload,       "Sideload",       riskui::RiskTier::Tier2Boot},
    {riskui::RiskFeature::AdbRoot,        "AdbRoot",        riskui::RiskTier::Tier2Boot},
    {riskui::RiskFeature::MtkUnlock,      "MtkUnlock",      riskui::RiskTier::Tier2Boot},
    {riskui::RiskFeature::SystemPartitionWrite, "SystemPartitionWrite", riskui::RiskTier::Tier2Boot},
    {riskui::RiskFeature::AppUninstall,   "AppUninstall",   riskui::RiskTier::Tier2Boot},
    {riskui::RiskFeature::VulnExploit,    "VulnExploit",    riskui::RiskTier::Tier3Diagnostic},
    {riskui::RiskFeature::EngineerMode,   "EngineerMode",   riskui::RiskTier::Tier3Diagnostic},
    {riskui::RiskFeature::SafetyNet,      "SafetyNet",      riskui::RiskTier::Tier3Diagnostic},
    {riskui::RiskFeature::SignatureSpoof, "SignatureSpoof", riskui::RiskTier::Tier3Diagnostic},
    {riskui::RiskFeature::ImeiTool,       "ImeiTool",       riskui::RiskTier::Tier3Diagnostic},
    {riskui::RiskFeature::SelinuxMode,    "SelinuxMode",    riskui::RiskTier::Tier3Diagnostic},
    {riskui::RiskFeature::DeviceSpoof,    "DeviceSpoof",    riskui::RiskTier::Tier3Diagnostic},
    // W8-T5：preloader 安全修补 —— 改的是「设备将来还信不信校验」，与 SelinuxMode 同族（Tier 3）。
    {riskui::RiskFeature::PreloaderPatch, "PreloaderPatch", riskui::RiskTier::Tier3Diagnostic},
};
constexpr int kFeatureCount = int(sizeof(kFeatures) / sizeof(kFeatures[0]));
static_assert(kFeatureCount == 17, "RiskFeature 全项表必须覆盖 17 项");

// RiskTier 的数值：None/Tier2Boot/Tier3Diagnostic = 0/2/3（1 留给了首屏「使用须知」）。
// 数值本身不参与运行时逻辑（ConsentStore 用显式 key），钉住是为了让这套编号语义不被悄悄改动
// ——名字里的 "2"/"3" 就是用户可见的级别序。
static_assert(static_cast<int>(riskui::RiskTier::None) == 0, "RiskTier::None 必须是 0");
static_assert(static_cast<int>(riskui::RiskTier::Tier2Boot) == 2, "RiskTier::Tier2Boot 必须是 2");
static_assert(static_cast<int>(riskui::RiskTier::Tier3Diagnostic) == 3,
              "RiskTier::Tier3Diagnostic 必须是 3");

// 诊断用：把档位印成名字（红了才知道期望/实际的档是哪一档）。
const char *tierName(riskui::RiskTier t)
{
    switch (t) {
    case riskui::RiskTier::None:           return "None";
    case riskui::RiskTier::Tier2Boot:      return "Tier2Boot";
    case riskui::RiskTier::Tier3Diagnostic: return "Tier3Diagnostic";
    }
    return "?";   // 三档已穷尽；新增档位时本 switch 会出 -Wswitch 警告（诊断用，不升级为 error）
}

} // namespace

class TestRiskTiers : public QObject
{
    Q_OBJECT

private slots:
    // 全枚举覆盖（本文件的**核心**）：DeviceMode 的每个取值都要有明确判定。
    // 变异①（留档）：把 MODE_EDL_9008 的 case 改成 None → 本槽 m=4 变红。
    void riskTierForModeCoversEveryMode()
    {
        for (int m = 0; m <= int(DeviceDetector::MODE_SAMSUNG_EUB); ++m) {
            const riskui::RiskTier actual =
                riskui::riskTierForMode(static_cast<DeviceDetector::DeviceMode>(m));
            QVERIFY2(actual == kExpected[m],
                     qPrintable(QStringLiteral("%1（枚举值 %2）判定为 %3，期望 %4")
                                    .arg(QString::fromUtf8(kModeNames[m]))
                                    .arg(m)
                                    .arg(QString::fromUtf8(tierName(actual)))
                                    .arg(QString::fromUtf8(tierName(kExpected[m])))));
        }
    }

    // 功能入口逐项：写操作 9 项 → Tier2Boot；诊断/绕过 8 项 → Tier3Diagnostic（W8-T5 起 +1）。
    // 变异⑤（留档）：把 EngineerMode 改成 Tier2Boot → 本槽对应项变红。
    void riskTierForFeatureMatchesEveryItem()
    {
        for (int i = 0; i < kFeatureCount; ++i) {
            const FeatureRow &row = kFeatures[i];
            const riskui::RiskTier actual = riskui::riskTierForFeature(row.feature);
            QVERIFY2(actual == row.tier,
                     qPrintable(QStringLiteral("RiskFeature::%1 判定为 %2，期望 %3")
                                    .arg(QString::fromUtf8(row.name))
                                    .arg(QString::fromUtf8(tierName(actual)))
                                    .arg(QString::fromUtf8(tierName(row.tier)))));
        }
    }

    // 标题逐字：对话框标题 = tierTitle()，改字这里必红（变异⑥留档）。
    void tierTitleIsExactForEveryTier()
    {
        QCOMPARE(riskui::tierTitle(riskui::RiskTier::Tier2Boot), QStringLiteral("底层引导与写操作"));
        QCOMPARE(riskui::tierTitle(riskui::RiskTier::Tier3Diagnostic), QStringLiteral("诊断与安全研究"));
        // None = 不需要确认的档：标题必须为空（非空会让调用方误以为该弹窗）。
        QCOMPARE(riskui::tierTitle(riskui::RiskTier::None), QString());
    }

    // 风险要点：逐条逐字 + 非空 + 关键短语。
    // 变异③（留档）：把 Tier2Boot 的要点表改成空表 → 本槽 red。
    // 红在**第一处**断言 —— QCOMPARE 是致命断言，失败即从本槽 return，后面的逐字/关键短语断言
    // 都不会执行：留档日志里只有 `t2.size()` 那一条（不是"三处都会红"，那是想当然）。
    void tierWarningsCarryTheExactRiskPoints()
    {
        const QStringList t2 = riskui::tierWarnings(riskui::RiskTier::Tier2Boot);
        QCOMPARE(t2.size(), 3);
        QCOMPARE(t2.at(0), QStringLiteral("这些操作直接写设备存储或底层引导，失败可能导致设备变砖、"
                                          "数据不可恢复。"));
        QCOMPARE(t2.at(1), QStringLiteral("请先确认机型与固件匹配，并确认设备属于你或已获授权。"));
        QCOMPARE(t2.at(2), QStringLiteral("部分通道未经真机验证（见项目文档「诚实边界」），实际行为"
                                          "可能与预期不同。"));
        // 下面两条关键短语钉是**冗余保险，不是第二道独立防线**（W8 终审 round2 N3 实测）：
        // 上面的 `QCOMPARE` 是**致命断言**，任何文案改动都先在它那里红并 return，短语钉不会执行
        // （实测把"未经真机验证"改成"未经真机确认" → 红在逐字钉）。留着是为了"有人把逐字钉放宽成
        // `contains`"时仍能兜住"把这条改成不痛不痒的文案"。
        QVERIFY2(t2.join(QStringLiteral("\n")).contains(QStringLiteral("变砖")),
                 qPrintable(QStringLiteral("Tier2 要点必须写明「变砖」这一最坏后果 —— 实得 %1")
                                .arg(t2.join(QStringLiteral(" / ")))));
        QVERIFY2(!t2.isEmpty(), "Tier2 的要点表不得为空（空表 = 对话框没有风险说明）");

        const QStringList t3 = riskui::tierWarnings(riskui::RiskTier::Tier3Diagnostic);
        QCOMPARE(t3.size(), 4);          // W8 整分支终审 I1 起 3 → 4（补"未验证/变砖"一条）
        // W2-T5 复审 ② 改文案（2026-09-20 控制方采纳）：本档现在同时覆盖"漏洞利用"与
        // "削弱安全机制 / 改写设备标识"（SELinux 切换、设备信息伪造），第 1 条不再只写 exploit 场景。
        QCOMPARE(t3.at(0), QStringLiteral("这些功能会绕过或削弱设备的安全机制（SELinux / 签名校验 / "
                                          "账号与安全校验），或改写设备标识。"));
        QCOMPARE(t3.at(1), QStringLiteral("未经设备所有者同意对他人设备使用，可能违法。"));
        // 第 3 条同批改（原句"脚本来自第三方数据库…"只对 exploit 成立；本档还含 SELinux 切换与
        // 设备信息伪造，两类都不涉及第三方脚本）。
        QCOMPARE(t3.at(2), QStringLiteral("操作效果与后果由你自行判断并承担；本工具不对其效果负责。"));
        // 第 4 条 = W8 整分支终审 I1（2026-09-23）：本档正是 VulnExploit / PreloaderPatch 的门
        // （risk_ui.cpp 的 riskTierForFeature），而"未验证 / 可能变砖"此前只在 Tier2 说 ——
        // 更危险的档警告反而更少（力度倒挂）。exploit spec:172/:205 明文要求 UI 如实标注。
        QCOMPARE(t3.at(3), QStringLiteral("本档能力的真机行为未经验证（见项目文档「诚实边界」）：注入或"
                                          "改写后设备可能无响应、需要重新插拔，最坏情况会变砖；"
                                          "请先确认已备份、设备可恢复。"));
        // 与 Tier2 同款的两条关键短语钉 —— **同样只是冗余保险**（见上一条的说明：逐字钉是致命断言，
        // 先于它们红；真正的甄别力在 `QCOMPARE(t3.at(3), …)` 那条）。
        QVERIFY2(t3.join(QStringLiteral("\n")).contains(QStringLiteral("变砖")),
                 qPrintable(QStringLiteral("Tier3 要点必须写明「变砖」这一最坏后果（I1 的力度倒挂）"
                                           "—— 实得 %1").arg(t3.join(QStringLiteral(" / ")))));
        QVERIFY2(t3.join(QStringLiteral("\n")).contains(QStringLiteral("未经验证")),
                 qPrintable(QStringLiteral("Tier3 要点必须写明「未经验证」"
                                           "—— 实得 %1").arg(t3.join(QStringLiteral(" / ")))));
        QVERIFY2(t3.join(QStringLiteral("\n")).contains(QStringLiteral("绕过")),
                 qPrintable(QStringLiteral("Tier3 要点必须点明「绕过」安全机制 —— 实得 %1")
                                .arg(t3.join(QStringLiteral(" / ")))));
        QVERIFY2(!t3.isEmpty(), "Tier3 的要点表不得为空（空表 = 对话框没有风险说明）");

        // None = 不弹窗的档：要点必须为空（非空会让调用方误以为该弹窗）。
        QVERIFY2(riskui::tierWarnings(riskui::RiskTier::None).isEmpty(),
                 "None 档的要点表必须为空");
    }
};

QTEST_APPLESS_MAIN(TestRiskTiers)
#include "test_risk_tiers.moc"
