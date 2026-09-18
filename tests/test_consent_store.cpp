// tests/test_consent_store.cpp
//
// legal::ConsentStore 的留痕语义 —— W2 Task 2（用户协议与风险分级门控）。
//
// 为什么这些断言值钱：ConsentStore 是"用户同意过什么"的**唯一记录**。判据放松一处，用户就会在
// **没确认过**的情况下进入写操作：
//   ① 版本比较变成"只要非零就算同意" —— `kAgreementVersion` 这道屏障被拆掉：旧版本留痕（升版后
//      本该重新确认）乃至任何手改出来的正整数，都会被当成"同意过当前版本"；
//   ② 两个 tier 落到同一把键 —— 接受 Tier2 顺带放行 Tier3，"两次独立确认"退化成一次。
// 两类缺陷都必须在**本文件**变红（逐条变异留档：.superpowers/sdd/w2-task-2-logs/）。
//
// 做法：**期望值独立于实现**。用例直接往临时 ini 里写键值（键名逐字手写，不从实现反推），再问
// store 的判据；以及 store 写完之后**另一把 QSettings 直读该 ini** 复核写入内容与落盘 —— 只测
// "同一对象读回自己刚写的东西"会漏掉"留痕根本没落盘 / 落在别的键上"这两类。
//
// 隔离：全程用 QTemporaryDir 里的临时 ini（显式 `QSettings(path, IniFormat)` 注入）。
// **不碰真实用户配置** —— 默认构造分支（`settings == nullptr` → UserScope 真实路径）本文件
// **刻意不构造**：那会写进本机用户配置目录；该分支的覆盖缺口见文件末「未覆盖」。
#include <QtTest>
#include <QSettings>
#include <QTemporaryDir>

#include "legal/consent_store.h"

namespace {

// 三把键名**逐字手写** = 对外契约（用例直写同名键、文档按此说明"留痕在本机"）。
// 实现若改键名，本文件里"直写这个键再问判据"的槽必红（读向）；"写完直读这个键"的槽同样必红
// （写向）。两侧都钉，是为了不让"实现和用例一起改键名"这种自洽式改动溜过去 —— 键名是给用户
// 和文档看的，不是内部实现细节。
const QString kAgreementKey = QStringLiteral("legal/agreementVersion");
const QString kTier2Key = QStringLiteral("legal/tier2Version");
const QString kTier3Key = QStringLiteral("legal/tier3Version");

// 版本边界（协议键与 tier 键**同款语义**，两处用例表共用这一张，防两处判据分叉）。
struct VersionRow {
    int stored;
    bool expected;
};

const VersionRow kVersionRows[] = {
    {legal::kAgreementVersion - 1, false},  // 旧版本 → 升版后必须重新确认
    {legal::kAgreementVersion, true},       // 正好当前版本
    {legal::kAgreementVersion + 1, true},   // 更高版本号不得回退（用户已接受更新版）
};

// 造一个"上一次运行留下的留痕"：ini 里只有指定键 = 指定值，返回其路径。
QString writeIni(const QTemporaryDir &dir, const QString &name, const QString &key, int value)
{
    const QString path = dir.filePath(name);
    QSettings s(path, QSettings::IniFormat);
    s.setValue(key, value);
    s.sync();
    return path;
}

// 失败信息里的"这个值相对当前版本是高还是低"（红了才看得懂是哪一行边界）。
QString versionRowNote(const VersionRow &row)
{
    const char *rel = row.stored == legal::kAgreementVersion
                          ? "正好当前版本"
                          : (row.stored < legal::kAgreementVersion ? "低于当前版本" : "高于当前版本");
    return QStringLiteral("存储值 %1（%2；kAgreementVersion = %3）")
        .arg(row.stored)
        .arg(QString::fromUtf8(rel))
        .arg(legal::kAgreementVersion);
}

} // namespace

class TestConsentStore : public QObject
{
    Q_OBJECT

private:
    // 每个槽一份（QTest 为**每个槽**新建本类实例）→ 槽之间不串味；只放小 ini。
    QTemporaryDir m_dir;

private slots:
    // 全新 ini：没留痕 = 未接受；且**只读判据不得写入任何东西**（同意必须来自用户动作，
    // 不能由"读过一次"产生）。
    void freshIniCountsAsNotAccepted()
    {
        QVERIFY2(m_dir.isValid(), "QTemporaryDir 建不出来 —— 本用例全程依赖临时 ini");
        QSettings s(m_dir.filePath(QStringLiteral("fresh.ini")), QSettings::IniFormat);
        legal::ConsentStore store(&s);

        QVERIFY2(!store.hasAcceptedAgreement(),
                 "全新的 ini（无 legal/agreementVersion 键）必须判为『未接受』");
        // 负控制的另一半：同一状态下的 tier 判据（None 除外）同样必须为 false。
        QVERIFY2(!store.hasAcceptedTier(riskui::RiskTier::Tier2Boot), "全新 ini 的 Tier2 必须判为未接受");
        QVERIFY2(!store.hasAcceptedTier(riskui::RiskTier::Tier3Diagnostic),
                 "全新 ini 的 Tier3 必须判为未接受");

        s.sync();
        QVERIFY2(!s.contains(kAgreementKey), "构造/读取不得写 legal/agreementVersion（发现即说明判据有副作用）");
        QVERIFY2(!s.contains(kTier2Key), "构造/读取不得写 legal/tier2Version");
        QVERIFY2(!s.contains(kTier3Key), "构造/读取不得写 legal/tier3Version");
    }

    // 接受后：同一 store 判 true；**落盘**（另一把 QSettings 直读 = 重开进程也认）；写入的是
    // kAgreementVersion 这个常量而不是手抄的字面量（常量升版而实现写死 1 → 这里红）。
    void acceptAgreementPersistsTheCurrentVersion()
    {
        QVERIFY2(m_dir.isValid(), "QTemporaryDir 建不出来 —— 本用例全程依赖临时 ini");
        const QString path = m_dir.filePath(QStringLiteral("accept.ini"));
        {
            QSettings s(path, QSettings::IniFormat);
            legal::ConsentStore store(&s);
            // 前置条件（"接受前必须是未接受"）：把"接受产生的效果"与"本来就判已接受"区分开 ——
            // 少了它，一个恒返回 true 的实现也能让本槽全绿。
            // ⚠️ 这条断言**必须**留在槽内：QVERIFY2 失败时只 `return` 当前函数，写进 void 辅助函数
            // 会变成"辅助函数静默返回、槽继续跑"的假绿（本仓已知坑）。
            QVERIFY2(!store.hasAcceptedAgreement(), "前置条件被破坏：新 ini 在 accept 之前就判为已接受");
            store.acceptAgreement();
            QVERIFY2(store.hasAcceptedAgreement(), "acceptAgreement() 之后同一 store 必须判为已接受");
        }
        {
            QSettings s(path, QSettings::IniFormat);
            QCOMPARE(s.value(kAgreementKey).toInt(), legal::kAgreementVersion);
            legal::ConsentStore reopened(&s);
            QVERIFY2(reopened.hasAcceptedAgreement(),
                     "新建 store 读同一 ini 必须仍判为已接受（留痕要落盘，不是内存态）");
        }
    }

    // **本文件的核心**：判据是 `存储值 >= kAgreementVersion`，不是"非零即同意"、也不是"必须相等"。
    // 变异留档打这里：`>=` 改成 `==` → 第三行（更高版本不回退）红。
    void versionBoundaryDecidesAcceptance()
    {
        QVERIFY2(m_dir.isValid(), "QTemporaryDir 建不出来 —— 本用例全程依赖临时 ini");
        for (const VersionRow &row : kVersionRows) {
            const QString path = writeIni(m_dir, QStringLiteral("v%1.ini").arg(row.stored),
                                          kAgreementKey, row.stored);
            QSettings s(path, QSettings::IniFormat);
            legal::ConsentStore store(&s);
            const bool actual = store.hasAcceptedAgreement();
            QVERIFY2(actual == row.expected,
                     qPrintable(QStringLiteral("ini 里 legal/agreementVersion %1，hasAcceptedAgreement() "
                                               "判为 %2，期望 %3")
                                    .arg(versionRowNote(row))
                                    .arg(actual ? QStringLiteral("true") : QStringLiteral("false"))
                                    .arg(row.expected ? QStringLiteral("true") : QStringLiteral("false"))));
        }
    }

    // tier 独立：接受 Tier2 不得顺带放行 Tier3（两把键、两次确认）。变异（两个 tier 返回同一键）
    // 打这里的第一条 Tier3 断言。
    void tiersAreRecordedIndependently()
    {
        QVERIFY2(m_dir.isValid(), "QTemporaryDir 建不出来 —— 本用例全程依赖临时 ini");
        const QString path = m_dir.filePath(QStringLiteral("tiers.ini"));
        QSettings s(path, QSettings::IniFormat);
        legal::ConsentStore store(&s);

        store.acceptTier(riskui::RiskTier::Tier2Boot);
        QVERIFY2(store.hasAcceptedTier(riskui::RiskTier::Tier2Boot), "接受过的 Tier2 必须判为已接受");
        QVERIFY2(!store.hasAcceptedTier(riskui::RiskTier::Tier3Diagnostic),
                 "接受 Tier2 不得顺带放行 Tier3 —— 两个 tier 是两次独立确认");

        store.acceptTier(riskui::RiskTier::Tier3Diagnostic);
        QVERIFY2(store.hasAcceptedTier(riskui::RiskTier::Tier3Diagnostic), "接受后的 Tier3 必须判为已接受");
        QVERIFY2(store.hasAcceptedTier(riskui::RiskTier::Tier2Boot), "接受 Tier3 不得把 Tier2 的留痕弄丢");

        // 落盘复核：两把键各写各的，写入的都是**当前协议版本**（不是 0/1 之类的手抄值）。
        QSettings check(path, QSettings::IniFormat);
        QCOMPARE(check.value(kTier2Key).toInt(), legal::kAgreementVersion);
        QCOMPARE(check.value(kTier3Key).toInt(), legal::kAgreementVersion);

        // 重开（新 store 读同一 ini）：两个 tier 的留痕都还在。
        legal::ConsentStore reopened(&check);
        QVERIFY2(reopened.hasAcceptedTier(riskui::RiskTier::Tier2Boot), "重开后 Tier2 留痕必须还在");
        QVERIFY2(reopened.hasAcceptedTier(riskui::RiskTier::Tier3Diagnostic), "重开后 Tier3 留痕必须还在");
    }

    // 键名契约的**读向**：直接往 ini 写这两把键，判据必须跟着变 —— 单靠"实现写、实现读"的自洽
    // 回路分辨不出键名被改（写读都跟着改就一致了），本条从外部把键名钉死。
    void tierKeysAreExactAndSeparate()
    {
        QVERIFY2(m_dir.isValid(), "QTemporaryDir 建不出来 —— 本用例全程依赖临时 ini");
        const QString path = writeIni(m_dir, QStringLiteral("keys.ini"), kTier2Key,
                                      legal::kAgreementVersion);
        QSettings s(path, QSettings::IniFormat);
        legal::ConsentStore store(&s);

        QVERIFY2(store.hasAcceptedTier(riskui::RiskTier::Tier2Boot),
                 "legal/tier2Version 必须是 Tier2 的判据键");
        QVERIFY2(!store.hasAcceptedTier(riskui::RiskTier::Tier3Diagnostic),
                 "legal/tier2Version 不得成为 Tier3 的判据键（两档各有一把键）");

        s.setValue(kTier3Key, legal::kAgreementVersion);
        s.sync();
        QVERIFY2(store.hasAcceptedTier(riskui::RiskTier::Tier3Diagnostic),
                 "legal/tier3Version 必须是 Tier3 的判据键");
        QVERIFY2(!store.hasAcceptedAgreement(),
                 "tier 的留痕不得被当作用户协议（首屏）的留痕 —— 两者是两件事");
    }

    // tier 的留痕同样受**协议版本**约束：协议升版 → 两个 tier 都要重新确认（与协议键同款边界，
    // 语义必须是同一套；这里若与 versionBoundaryDecidesAcceptance 分叉 → 两处表打架）。
    void tierAcceptanceIsBoundByAgreementVersion()
    {
        QVERIFY2(m_dir.isValid(), "QTemporaryDir 建不出来 —— 本用例全程依赖临时 ini");
        for (const VersionRow &row : kVersionRows) {
            const QString path = writeIni(m_dir, QStringLiteral("tv%1.ini").arg(row.stored),
                                          kTier2Key, row.stored);
            QSettings s(path, QSettings::IniFormat);
            legal::ConsentStore store(&s);
            const bool actual = store.hasAcceptedTier(riskui::RiskTier::Tier2Boot);
            QVERIFY2(actual == row.expected,
                     qPrintable(QStringLiteral("ini 里 legal/tier2Version %1，hasAcceptedTier(Tier2Boot) "
                                               "判为 %2，期望 %3")
                                    .arg(versionRowNote(row))
                                    .arg(actual ? QStringLiteral("true") : QStringLiteral("false"))
                                    .arg(row.expected ? QStringLiteral("true") : QStringLiteral("false"))));
        }
    }

    // None 档：本来就无需确认 → 恒 true；且**不留痕**（None 不是一次"确认"，
    // 不该产生记录 —— Task 4 的闸门对 None 的口径是"不弹窗且不写入"）。
    // 第一条断言的恒真风险由紧随其后的负控制挡住：同一状态（全新 ini）下 Tier2/Tier3 必须为 false，
    // 说明这个 true 来自"None 无需确认"这条规则，而不是"判据恒真"。
    void noneTierNeedsNoConsentAndWritesNothing()
    {
        QVERIFY2(m_dir.isValid(), "QTemporaryDir 建不出来 —— 本用例全程依赖临时 ini");
        QSettings s(m_dir.filePath(QStringLiteral("none.ini")), QSettings::IniFormat);
        legal::ConsentStore store(&s);

        QVERIFY2(store.hasAcceptedTier(riskui::RiskTier::None), "None 档不需要确认，必须判为已接受");
        QVERIFY2(!store.hasAcceptedTier(riskui::RiskTier::Tier2Boot),
                 "负控制：同一状态下 Tier2 必须为 false（否则上面的 true 说明判据恒真）");
        QVERIFY2(!store.hasAcceptedTier(riskui::RiskTier::Tier3Diagnostic),
                 "负控制：同一状态下 Tier3 必须为 false");

        store.acceptTier(riskui::RiskTier::None);
        s.sync();
        QVERIFY2(!s.contains(kTier2Key), "acceptTier(None) 不得写 legal/tier2Version");
        QVERIFY2(!s.contains(kTier3Key), "acceptTier(None) 不得写 legal/tier3Version");
        QVERIFY2(!s.contains(kAgreementKey), "acceptTier(None) 不得写 legal/agreementVersion");
    }

};

// ── 未覆盖（如实的缺口，别把这份绿灯读成"全都验过"）──────────────────────────────
// * **默认构造分支**（`ConsentStore store;` / `ConsentStore(nullptr)`）本文件刻意不构造：它走
//   QSettings::UserScope，会落到本机真实用户配置目录 —— 用例**不许**碰那里（本仓约定）。该分支
//   的覆盖由生产接线（Task 5：src/main.cpp 用默认构造）与人工核对承担，本文件不claim它。
// * **键名/版本常量与协议正文的一致性**不在本文件：那是 Task 3 的三路比对用例
//   （spec 正文 ↔ resources/legal/user-agreement.md ↔ legal::kAgreementVersion）。
// * 本文件只钉"判据与留痕"，**不钉 UI**：二次确认对话框与唯一闸门 riskui::ensureAccepted 在
//   tests/test_risk_confirm_dialog.cpp（Task 4）。
QTEST_APPLESS_MAIN(TestConsentStore)
#include "test_consent_store.moc"
