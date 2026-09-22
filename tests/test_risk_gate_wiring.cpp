// tests/test_risk_gate_wiring.cpp
//
// W2 Task 5：门控接线的**结构钉** —— 读生产源文件做文本断言（不构造任何 UI、不开设备、不碰配置）。
//
// 为什么需要它：门控的失效形态是"某个入口漏了那一行"。漏接线的代码**编译得过、跑起来也不报错**，
// 只有用户真点下去才会发现没有任何提醒 —— 而仓内没有 FlashPanel / SystemToolPanel / VulnPanel 的
// 行为用例（依赖闭包太大：Widgets + FlashTool + libusb，见 CMakeLists 里 test_flash_button_labels 的
// 注释），这条链只能靠读源文件钉住。
//
// 本文件两段：
//  ① 接线钉：五个生产文件里 `riskui::ensureAccepted(` 的出现次数与**位置**（逐入口、函数体头部），
//     以及每个闸门取 tier 的表达式（模式映射 / 功能映射，不是硬编的档）。
//  ② 结构对照钉（tripwire）：
//     * 按钮接线集合：FlashPanel 每个 `QPushButton` 的槽名集合 == "14 个**按钮类**的已过闸门入口
//       + 7 个按设计不过闸门入口"（= 21；**闸门总数是 15** —— 差的那个是 `confirmPreloaderPatch`，
//       它是提问不是按钮，见该槽的函数头注释）。**新增按钮 → 这条红 → 强迫作者回来核对新入口该不该过闸门。**
//     * 断线复位：**本文件里凡被 `setEnabled(...)` 碰过的按钮，`clearDeviceInfo` 必须禁用**
//       （两轮复审各撞见同一类缺口：Low-1a 的解锁/回锁/FRP、复审 ③ 的 EDL/MTK 连接类 ——
//       拔线后仍可点，而闸门按模式取档在 MODE_UNKNOWN 上给 None = 不弹窗）。
//     * 界线：偏好开关族（DPI/动画/状态栏/导航栏/字体/温控/调频/IO/内存/冻结）**有意不纳门控**，
//       那 11 个 handler 体内必须无闸门（按**行为**归类，不按 `settings put` 机制 —— 复审 Low-2）。
//
// 取材边界（结论必须连同这一段一起读）：
//   * 本用例只数字面出现次数与出现位置 —— 把 `if (!riskui::ensureAccepted(...)) return;` 包进
//     `if (false) { ... }`、改成注释、或把返回值丢掉，字面都还在，**本用例仍然绿**。
//     它是 **tripwire（防无声漏接线），不是"门控已生效"的证明**；闸门语义与分级映射的行为证明
//     在 tests/test_risk_confirm_dialog.cpp 与 tests/test_risk_tiers.cpp。
//   * 它钉得住的方向：**删/漏/挪走**那一行（次数少了、位置跑出函数体头部、tier 变成硬编值）。
//     它钉不住的方向：那一行还在但不生效（如上）。
//   * 读不到生产源文件 → **硬 FAIL**（不是 QSKIP）：钉在缺文件时静默失效正是本仓反复吃亏的形态。
//     ⚠️ 契约：源码树必须原地存在（PT_SRC_ROOT 是绝对路径，由 CMake 编进来）。
#include <QtTest>

#include <QFile>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace {

// 仓库根由 CMake 编进来（PT_SRC_ROOT），相对路径一律以 "/" 开头。
QString readSource(const QString &relPath)
{
    QFile f(QStringLiteral(PT_SRC_ROOT) + relPath);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

// 唯一闸门的字面 —— 接线一律用全限定名（见 risk_confirm_dialog.h 文件头）。
const QString kGate = QStringLiteral("riskui::ensureAccepted(");

// 出现次数；**打不开返回 -1**（调用方 QCOMPARE 到 0 以外的值即红）—— 不是静默 0。
int countIn(const QString &src, const QString &needle)
{
    if (src.isNull())
        return -1;
    return int(src.count(needle));
}

// 取 `signature` 那个函数定义的**函数体**（从左花括号到列 0 的右花括号）。
// 找不到（改名/删函数）返回空串 —— 调用方 QVERIFY2 硬 FAIL，不会退化成"没找到就跳过"。
QString functionBody(const QString &src, const QString &signature)
{
    const qsizetype begin = src.indexOf(signature);
    if (begin < 0)
        return QString();
    const qsizetype bodyStart = src.indexOf(QLatin1Char('{'), begin);
    if (bodyStart < 0)
        return QString();
    const qsizetype end = src.indexOf(QStringLiteral("\n}"), bodyStart);
    if (end < 0)
        return QString();
    return src.mid(bodyStart, end - bodyStart);
}

// 闸门在函数体里的**行号**（0 = 左花括号那一行）；没找到返回 -1。
int gateLineIn(const QString &body)
{
    const QStringList lines = body.split(QLatin1Char('\n'));
    for (int i = 0; i < lines.size(); ++i) {
        if (lines.at(i).contains(kGate))
            return i;
    }
    return -1;
}

// 全仓约定：闸门必须是函数体**第一件事**（漏在后面的闸门挡不住前面已经发起的动作）。
// 允许闸门上方有一行注释，故上限取 5。
const int kGateMaxLine = 5;

// 取设备模式文案的**分支字面量**：`case <caseLabel>:` 之后、同一分支内的第一个
// `assignStart "..."` 的引号内容（取不到返回空串）。
QString branchStringLiteral(const QString &src, const QString &caseLabel, const QString &assignStart)
{
    const qsizetype c = src.indexOf(caseLabel);
    if (c < 0)
        return QString();
    const qsizetype a = src.indexOf(assignStart, c);
    if (a < 0)
        return QString();
    const qsizetype q1 = src.indexOf(QLatin1Char('"'), a + assignStart.size() - 1);
    if (q1 < 0)
        return QString();
    const qsizetype q2 = src.indexOf(QLatin1Char('"'), q1 + 1);
    if (q2 < 0)
        return QString();
    return src.mid(q1 + 1, q2 - q1 - 1);
}

} // namespace

class TestRiskGateWiring : public QObject
{
    Q_OBJECT

private slots:
    // ① 接线钉：FlashPanel 的 15 个 handler（W8-T5 把 preloader 安全修补纳入后 14→15）
    void flashPanelGatesAllFifteenHandlers();
    // ① 接线钉：其余三处（10 + 4 + 1）
    void systemToolPanelGatesTenEntries();
    void vulnPanelGatesFourEntries();
    void eubRecoveryDialogGatesOnStart();
    // ① 接线钉：首屏（MainWindow 构造之前）
    void mainAsksAgreementBeforeTheMainWindow();
    // ② tripwire：按钮接线集合 == 已过闸门 14 + 按设计不过闸门 6
    void flashPanelButtonConnectsMatchTheGatedSet();
    // ② 界线钉：偏好开关族**有意不纳门控**（W2-T5 复审 ④；命名按复审 Low-2 订正）
    void preferenceSwitchesStayUngatedByDesign();
    // ② 复位钉：全文件凡被 setEnabled 碰过的按钮，clearDeviceInfo 必须禁用（Low-1a + 复审 ③）
    void flashPanelClearsEveryButtonItCanEnable();
    // ② 回位钉：凡在复位表里 **且** 在 setDeviceInfo 里被 show() 的控件，必须也能在那里被 setEnabled(
    // （W2-T5 复审 High：只做复位不做回位 → 三个连接类键被治成死键）
    void flashPanelShownButtonsAreReenabledInTheirModeBranch();
    // ② 一对文案钉：设备模式显示名两侧一致（W6-T1 移交）
    void mtkBromDisplayNamePairIsConsistent();
};

// FlashPanel 的 15 个入口：每个入口的第一件事都是闸门，tier 逐个钉住取法。
// 取档来源分两类（W2-T5 复审 L-1(b) + W8-T5）：
//   * 11 个入口从**面板已有的设备模式**取（`m_deviceInfo.mode` —— 本面板没有独立的 m_currentMode，
//     见 W2-T5 报告；不为接线新增并行状态）；
//   * 4 个入口（解锁/回锁/FRP + W8-T5 的 preloader 安全修补）改用**功能映射**：
//     - 前三个**不由模式分支启用**（ADB+已 Root / MTK 已连接 都算），拔线后闸门若按模式取会落到
//       MODE_UNKNOWN → tier None → 不弹窗直接放行。功能映射不随模式漂，天然 fail-closed。
//     - preloader 修补是**跨模式的危险选项**（任何带 preloader 的 MTK 刷写都可能开），
//       它属"削弱安全机制"族 ⇒ 必须按**功能**取 Tier 3，不能跟着刷写那次 Tier 2 走。
void TestRiskGateWiring::flashPanelGatesAllFifteenHandlers()
{
    const QString src = readSource(QStringLiteral("/src/ui/flash_panel.cpp"));
    QVERIFY2(!src.isNull(),
             qPrintable(QStringLiteral("读不到生产源文件：%1/src/ui/flash_panel.cpp")
                            .arg(QStringLiteral(PT_SRC_ROOT))));

    // 计数：15 = 上表 15 个 handler 各一次（多一处 = 有人重复挂闸门 / 少一处 = 有人漏接线）。
    // ⚠️ 这正是 W8-T5 把 preloader 修补**单独提成一个函数**（而不是内联进 onFlashClicked）的原因：
    // 内联会让本函数出现两个闸门、把它变成 16，而"多一处"恰恰是本条要抓的信号。
    QCOMPARE(countIn(src, kGate), 15);

    const QString kByMode = QStringLiteral("riskui::riskTierForMode(");
    const QString kModeSource = QStringLiteral("m_deviceInfo.mode");

    struct GateRow {
        const char *signature;
        const char *tierExpr;    // 闸门取档必须出现的字面（focused 到具体 feature）
        bool        byMode;      // true → 额外要求出现 m_deviceInfo.mode（模式映射）
    };
    const GateRow rows[] = {
        { "void FlashPanel::onFlashClicked()",        "riskui::riskTierForMode(", true },
        { "void FlashPanel::onEraseClicked()",        "riskui::riskTierForMode(", true },
        { "void FlashPanel::onUpdateClicked()",       "riskui::riskTierForMode(", true },
        { "void FlashPanel::onSideloadClicked()",     "riskui::riskTierForMode(", true },
        { "void FlashPanel::onRunScriptClicked()",    "riskui::riskTierForMode(", true },
        { "void FlashPanel::onDumpClicked()",         "riskui::riskTierForMode(", true },
        { "void FlashPanel::onBootImageClicked()",    "riskui::riskTierForMode(", true },
        { "void FlashPanel::onBrickRepairClicked()",  "riskui::riskTierForMode(", true },
        // 后三个是"底层引导"入口：连接即把 programmer/DA 上传到设备 RAM —— 同属 Tier2 写路径
        { "void FlashPanel::onEdlConnect()",          "riskui::riskTierForMode(", true },
        { "void FlashPanel::onEdlPlanFlash()",        "riskui::riskTierForMode(", true },
        { "void FlashPanel::onMtkConnect()",          "riskui::riskTierForMode(", true },
        // L-1(b)：这三个改功能映射（理由见函数头注释）
        { "void FlashPanel::onUnlockBootloader()",
          "riskui::riskTierForFeature(riskui::RiskFeature::Unlock)", false },
        { "void FlashPanel::onLockBootloader()",
          "riskui::riskTierForFeature(riskui::RiskFeature::Unlock)", false },
        { "void FlashPanel::onFrpErase()",
          "riskui::riskTierForFeature(riskui::RiskFeature::FrpClear)", false },
        // W8-T5：preloader 安全修补的二次确认（**它不是按钮，是 MTK 刷写流程里的一个提问**）——
        // 提问在 onFlashClicked 里，**闸门单独提成一个函数**：内联会让 onFlashClicked 出现两个闸门，
        // 把上面那条"多一处 = 有人重复挂闸门"的判据变成不可判。
        { "bool FlashPanel::confirmPreloaderPatch()",
          "riskui::riskTierForFeature(riskui::RiskFeature::PreloaderPatch)", false },
    };
    QCOMPARE(int(sizeof(rows) / sizeof(rows[0])), 15);   // 表本身受检：少一行就少钉一个入口

    // 两组的**条数**也钉住（复审要求的两组拆表：11 模式取档 + 4 功能取档；W8-T5 前是 11 + 3）——
    // 只按行内 flag 判会被"整行挪组"骗过：把 unlock 那行写成 byMode=true 时，下面 region 里
    // 既没有 riskTierForMode( 也没有 m_deviceInfo.mode，仍会红；但反过来把某个模式取档的行
    // 误标 byMode=false 就少钉一处。条数是这张表的形状，必须显式。
    int byModeCount = 0, byFeatureCount = 0;
    for (const GateRow &row : rows)
        (row.byMode ? byModeCount : byFeatureCount) += 1;
    QCOMPARE(byModeCount, 11);
    QCOMPARE(byFeatureCount, 4);   // W8-T5 起 +1（preloader 安全修补）

    for (const GateRow &row : rows) {
        const QString signature = QString::fromUtf8(row.signature);
        const QString tierExpr = QString::fromUtf8(row.tierExpr);
        const QString body = functionBody(src, signature);
        QVERIFY2(!body.isEmpty(),
                 qPrintable(QStringLiteral("找不到函数定义（改名/删除？）：%1").arg(signature)));

        // 每个入口**恰好一次**：0 次 = 漏接线（本条正是本用例存在的理由）。
        QVERIFY2(body.count(kGate) == 1,
                 qPrintable(QStringLiteral("%1 的闸门出现 %2 次（应为 1 次）")
                                .arg(signature).arg(body.count(kGate))));

        // 位置：函数体第一件事（顶部允许一行注释）。
        const int line = gateLineIn(body);
        QVERIFY2(line >= 0 && line <= kGateMaxLine,
                 qPrintable(QStringLiteral("%1 的闸门在函数体第 %2 行（应 ≤ %3：闸门必须是第一件事）")
                                .arg(signature).arg(line).arg(kGateMaxLine)));

        // tier 表达式：硬编 `RiskTier::None`（= 永不弹窗）或硬编某一档都会被这条钉住。
        const QString region = body.mid(body.indexOf(kGate), 220);
        QVERIFY2(region.contains(tierExpr),
                 qPrintable(QStringLiteral("%1 的闸门没有取 %2").arg(signature, tierExpr)));
        if (row.byMode) {
            QVERIFY2(region.contains(kByMode) && region.contains(kModeSource),
                     qPrintable(QStringLiteral("%1 的闸门没有从面板已有的 m_deviceInfo.mode 取模式")
                                    .arg(signature)));
        }
    }
}

// SystemToolPanel 的 10 个入口 → 功能映射（逐个钉住"哪个入口对应哪个 RiskFeature"：
// 同一档内的彼此互换不会改变行为，所以只有这条钉得住计划表里的对应关系）。
// 10 = 4 个检测/诊断类（W2-T5 首版）+ 6 个**真改设备状态**的入口（W2-T5 复审裁决纳入：
// SELinux 切换 / remount rw / umount / 改 build.prop / 换开机动画 / 卸载应用）。
void TestRiskGateWiring::systemToolPanelGatesTenEntries()
{
    const QString src = readSource(QStringLiteral("/src/ui/system_tool_panel.cpp"));
    QVERIFY2(!src.isNull(),
             qPrintable(QStringLiteral("读不到生产源文件：%1/src/ui/system_tool_panel.cpp")
                            .arg(QStringLiteral(PT_SRC_ROOT))));

    QCOMPARE(countIn(src, kGate), 10);

    struct Entry {
        const char *signature;
        const char *feature;
    };
    const Entry entries[] = {
        // 功能名用**全限定**写法：RiskFeature 在 riskui 命名空间内，这些面板类不在其中
        // （首次全量构建实测：漏了 riskui:: 前缀 → "‘RiskFeature’未声明"，7 处一起炸）。
        // —— 检测/诊断类（首版 4 个）——
        { "void SystemToolPanel::onSpoofDevice()",      "riskui::RiskFeature::DeviceSpoof" },
        { "void SystemToolPanel::onSafetynetBypass()",  "riskui::RiskFeature::SafetyNet" },
        { "void SystemToolPanel::onSignatureSpoof()",   "riskui::RiskFeature::SignatureSpoof" },
        { "void SystemToolPanel::onOpenEngineerMode()", "riskui::RiskFeature::EngineerMode" },
        // —— 真改设备状态（复审裁决 6 个）——
        // 反例对照：SafetyNet/签名伪造那两个只做**检测**却已门控，SELinux 切换是真改状态，此前漏了。
        { "void SystemToolPanel::onSelinuxMode()",      "riskui::RiskFeature::SelinuxMode" },
        { "void SystemToolPanel::onMountSystem()",      "riskui::RiskFeature::SystemPartitionWrite" },
        { "void SystemToolPanel::onUnmountSystem()",    "riskui::RiskFeature::SystemPartitionWrite" },
        { "void SystemToolPanel::onEditBuildProp()",    "riskui::RiskFeature::SystemPartitionWrite" },
        { "void SystemToolPanel::onBootAnimation()",    "riskui::RiskFeature::SystemPartitionWrite" },
        { "void SystemToolPanel::onAppUninstall()",     "riskui::RiskFeature::AppUninstall" },
    };
    QCOMPARE(int(sizeof(entries) / sizeof(entries[0])), 10);

    for (const Entry &e : entries) {
        const QString signature = QString::fromUtf8(e.signature);
        const QString feature = QString::fromUtf8(e.feature);
        const QString body = functionBody(src, signature);
        QVERIFY2(!body.isEmpty(),
                 qPrintable(QStringLiteral("找不到函数定义（改名/删除？）：%1").arg(signature)));
        QVERIFY2(body.count(kGate) == 1,
                 qPrintable(QStringLiteral("%1 的闸门出现 %2 次（应为 1 次）")
                                .arg(signature).arg(body.count(kGate))));
        const int line = gateLineIn(body);
        QVERIFY2(line >= 0 && line <= kGateMaxLine,
                 qPrintable(QStringLiteral("%1 的闸门在函数体第 %2 行（应 ≤ %3）")
                                .arg(signature).arg(line).arg(kGateMaxLine)));
        const QString region = body.mid(body.indexOf(kGate), 200);
        QVERIFY2(region.contains(QStringLiteral("riskui::riskTierForFeature(")),
                 qPrintable(QStringLiteral("%1 的闸门没有用 riskui::riskTierForFeature(...)")
                                .arg(signature)));
        QVERIFY2(region.contains(feature),
                 qPrintable(QStringLiteral("%1 的闸门没有取 %2").arg(signature, feature)));
    }
}

// VulnPanel 的 4 个入口（扫描 + 批量利用 + 单条利用 + W8-T6 的 MTK BROM 漏洞利用）：同一档
// （spec §5：扫描也纳入；BROM 载荷同为"绕过安全机制"一族，故同档同函数）。
// ⚠️ **过闸门的是「执行利用」那一个入口**，不是本区的三个控件：
//    * 「解析载荷」不过闸门 —— 它只读本地文件/按用户显式开启的网络取字节，不碰设备（W2 的界线）；
//    * 「目标芯片」输入框是纯输入，更谈不上动作。
//    将来若有人给解析入口也挂上闸门，本槽会因计数变 5 而红 —— 那时先回来更新本条与 spec §5。
void TestRiskGateWiring::vulnPanelGatesFourEntries()
{
    const QString src = readSource(QStringLiteral("/src/ui/vuln_panel.cpp"));
    QVERIFY2(!src.isNull(),
             qPrintable(QStringLiteral("读不到生产源文件：%1/src/ui/vuln_panel.cpp")
                            .arg(QStringLiteral(PT_SRC_ROOT))));

    QCOMPARE(countIn(src, kGate), 4);

    const QStringList handlers = {
        QStringLiteral("void VulnPanel::onScan()"),
        QStringLiteral("void VulnPanel::onExploitAll()"),
        QStringLiteral("void VulnPanel::onExploitSingle(const QString &cveId)"),
        // W8-T6：BROM 漏洞利用的**执行**入口（解析载荷那条**有意不过闸门**，见函数头注释）
        QStringLiteral("void VulnPanel::onBromExploitRun()"),
    };
    QCOMPARE(handlers.size(), 4);

    for (const QString &signature : handlers) {
        const QString body = functionBody(src, signature);
        QVERIFY2(!body.isEmpty(),
                 qPrintable(QStringLiteral("找不到函数定义（改名/删除？）：%1").arg(signature)));
        QVERIFY2(body.count(kGate) == 1,
                 qPrintable(QStringLiteral("%1 的闸门出现 %2 次（应为 1 次）")
                                .arg(signature).arg(body.count(kGate))));
        const int line = gateLineIn(body);
        QVERIFY2(line >= 0 && line <= kGateMaxLine,
                 qPrintable(QStringLiteral("%1 的闸门在函数体第 %2 行（应 ≤ %3）")
                                .arg(signature).arg(line).arg(kGateMaxLine)));
        const QString region = body.mid(body.indexOf(kGate), 200);
        QVERIFY2(region.contains(QStringLiteral("riskui::riskTierForFeature(riskui::RiskFeature::VulnExploit)")),
                 qPrintable(QStringLiteral("%1 的闸门没有取 RiskFeature::VulnExploit").arg(signature)));
    }
}

// EUB 救援对话框的 onStart()：Tier 由 `riskTierForMode(MODE_SAMSUNG_EUB)` 给（spec §5）。
void TestRiskGateWiring::eubRecoveryDialogGatesOnStart()
{
    const QString src = readSource(QStringLiteral("/src/ui/eub_recovery_dialog.cpp"));
    QVERIFY2(!src.isNull(),
             qPrintable(QStringLiteral("读不到生产源文件：%1/src/ui/eub_recovery_dialog.cpp")
                            .arg(QStringLiteral(PT_SRC_ROOT))));

    QCOMPARE(countIn(src, kGate), 1);

    const QString body = functionBody(src, QStringLiteral("void EubRecoveryDialog::onStart()"));
    QVERIFY2(!body.isEmpty(), "找不到 EubRecoveryDialog::onStart 的定义");

    const QString region = body.mid(body.indexOf(kGate), 200);
    QVERIFY2(body.count(kGate) == 1, "onStart 的闸门出现次数不是 1");
    const int line = gateLineIn(body);
    QVERIFY2(line >= 0 && line <= kGateMaxLine,
             qPrintable(QStringLiteral("onStart 的闸门在函数体第 %1 行（应 ≤ %2）")
                            .arg(line).arg(kGateMaxLine)));
    QVERIFY2(region.contains(QStringLiteral("riskui::riskTierForMode(")),
             "onStart 的闸门没有用 riskui::riskTierForMode(...) 取档");
    QVERIFY2(region.contains(QStringLiteral("DeviceDetector::MODE_SAMSUNG_EUB")),
             "onStart 的闸门没有取 DeviceDetector::MODE_SAMSUNG_EUB");
}

// 首屏：`MainWindow window;` 之前问协议，用户拒绝 → **退出程序**（return 0，不进主界面）。
// 顺序是这里的要害：主窗口一构造就开始轮询设备，闸门必须在此之前。
void TestRiskGateWiring::mainAsksAgreementBeforeTheMainWindow()
{
    const QString src = readSource(QStringLiteral("/src/main.cpp"));
    QVERIFY2(!src.isNull(),
             qPrintable(QStringLiteral("读不到生产源文件：%1/src/main.cpp")
                            .arg(QStringLiteral(PT_SRC_ROOT))));

    QVERIFY2(src.contains(QStringLiteral("hasAcceptedAgreement()")), "main.cpp 没有查首屏留痕");
    QVERIFY2(src.contains(QStringLiteral("agreementui::ask(")), "main.cpp 没有弹首屏使用须知");

    const qsizetype ask = src.indexOf(QStringLiteral("agreementui::ask("));
    const qsizetype accept = src.indexOf(QStringLiteral("acceptAgreement()"));
    const qsizetype window = src.indexOf(QStringLiteral("MainWindow window;"));

    QVERIFY2(accept > ask, "acceptAgreement() 不在 ask() 之后（同意是先问了再记）");
    QVERIFY2(window > ask, "首屏门控不在 MainWindow 构造之前");
    QVERIFY2(window > accept, "首屏门控（问 + 记）不在 MainWindow 构造之前");

    // 用户拒绝 → 退出：ask 与 acceptAgreement 之间必须有 return（少这条 = 拒绝也照进主界面）。
    const QString between = src.mid(ask, accept - ask);
    QVERIFY2(between.contains(QStringLiteral("return")),
             "ask() 与 acceptAgreement() 之间没有 return —— 用户拒绝也会进入主界面");
}

// ② tripwire：FlashPanel 每个 QPushButton 的 clicked 槽名集合 == **21 个按钮槽**：
//    **14** 个按钮类的已过闸门入口 + **7** 个按设计不过闸门的入口。
//    ⚠️ 数与名单的对应关系（这处注释曾写错，2026-08/09 审查 Minor）——**闸门总数是 15，但按钮只有 14 个**：
//    第 15 个闸门是 `confirmPreloaderPatch`，它是 MTK 刷写流程里的一个**提问**、不由按钮触发，
//    故不在这份"按钮接线"名单里（它由 `flashPanelGatesAllFifteenHandlers` 的 rows 表钉住）。
//    下面三组各自标了数，**不要**把组的数当成"闸门总数"。
//    7 个不过闸门的是（不写设备/不需要风险确认）：文件选择、刷新分区列表、选 programmer 文件、
//    EDL 断开、MTK 断开、返回（本机 UI / 断开连接），以及 W8-T6 的 **DA 切分另存**（**本地只读**：
//    读一个 DA 文件、写两个文件，不碰设备、不削弱任何安全机制 ⇒ 按 W2 的界线（改设备状态才弹）
//    **有意不纳**；它连设备都不需要，故也常驻可用、不进复位表）。
//    新增按钮 → 槽名不在两侧任何一边 → 本用例红 → 作者必须回来回答"这个入口要不要过闸门"。
//    取材边界：这是**文本**钉 —— 换一种写法接线（如不用 connect(m_xxxBtn, ...)）它就数不到，
//    那时先在这里红（集合变小）而不是静默放过。
void TestRiskGateWiring::flashPanelButtonConnectsMatchTheGatedSet()
{
    const QString src = readSource(QStringLiteral("/src/ui/flash_panel.cpp"));
    QVERIFY2(!src.isNull(),
             qPrintable(QStringLiteral("读不到生产源文件：%1/src/ui/flash_panel.cpp")
                            .arg(QStringLiteral(PT_SRC_ROOT))));

    const QRegularExpression re(QStringLiteral(
        "connect\\(\\s*m_\\w+\\s*,\\s*&QPushButton::clicked\\s*,\\s*this\\s*,\\s*&FlashPanel::(\\w+)\\s*\\)"));
    // ⚠️ 变量名不能叫 `slots` —— Qt 的 `slots` 是宏（qobjectdefs.h），`QStringList slots;` 会被
    // 展开成 `QStringList ;`（GCC 报 "does not declare anything"）。
    QStringList connectedSlots;
    QRegularExpressionMatchIterator it = re.globalMatch(src);
    while (it.hasNext())
        connectedSlots << it.next().captured(1);

    // 解析器自身受检：写法变了先在这里红（否则集合为空、下面的比较会给出误导性的差异）。
    QCOMPARE(int(connectedSlots.size()), 21);

    const QStringList gated = {
        // —— 第一组：14 个**按钮类**的已过闸门入口（第 15 个闸门 `confirmPreloaderPatch` 是提问、
        //    不是按钮 ⇒ 不在这份按钮名单里；见本函数的函数头注释）——
        QStringLiteral("onFlashClicked"), QStringLiteral("onEraseClicked"),
        QStringLiteral("onUpdateClicked"), QStringLiteral("onSideloadClicked"),
        QStringLiteral("onRunScriptClicked"), QStringLiteral("onDumpClicked"),
        QStringLiteral("onBootImageClicked"), QStringLiteral("onUnlockBootloader"),
        QStringLiteral("onLockBootloader"), QStringLiteral("onFrpErase"),
        QStringLiteral("onBrickRepairClicked"), QStringLiteral("onEdlConnect"),
        QStringLiteral("onEdlPlanFlash"), QStringLiteral("onMtkConnect"),
        // —— 第二组：6 个按设计不过闸门（本机 UI / 文件选择 / 断开连接，不发起任何设备写入）——
        QStringLiteral("onSelectFile"), QStringLiteral("onRefreshPartitions"),
        QStringLiteral("onEdlSelectProgrammer"), QStringLiteral("onEdlDisconnect"),
        QStringLiteral("onMtkDisconnect"), QStringLiteral("switchToDeviceInfo"),
        // —— 第三组（W8-T6，1 个）：DA 切分另存（读 DA 文件 → 写 da1.bin/da2.bin；**不碰设备** ⇒ 不纳门控）——
        QStringLiteral("onExportDaSegments"),
    };
    // 14 + 6 + 1 = 21（**不是**"15 + 7" —— 15 是闸门数，其中一个是提问不是按钮）
    QCOMPARE(gated.size(), 21);

    QStringList got = connectedSlots, want = gated;
    got.sort();
    want.sort();
    QCOMPARE(got, want);
}

// W2-T5 复审 ④：**有意不纳门控**的"偏好开关"入口 —— DPI / 动画速度 / 状态栏 / 导航栏 / 字体 /
// 温控 / CPU·GPU 调频 / IO 调度 / 内存优化 / 应用冻结。判据是**行为**：它们改的是可随时改回的
// 用户偏好，**不改系统状态、也不削弱安全机制** —— 在"两档门控 + 不为每次操作弹窗"的设计
// （spec §1 非目标）下有意不纳（`onAppFreezeToggle` 的冻结/解冻同属这类可控开关，同样不纳）。
// 界线由控制方 2026-09-20 裁定：**改"设备状态"（写存储 / 削安全机制）才弹，改"偏好开关"不弹**。
// ⚠️ 命名与理由句按**复审 Low-2/Low-3** 订正过：本槽原叫 `settingsPutFamilyStaysUngatedByDesign`，
//    但磁盘实况是 —— 全文件 `settings put` 只有 11 处、且只落在 3 个 handler
//    （`onAnimationScale` 3 / `onStatusBar` 4 / `onNavBar` 4）；其余 8 个走别的机制
//    （`onSetDpi` = `wm density`、三个调频 = sysfs、`onMemoryOptimize` = `/proc/sys/vm`、
//    `onAppFreezeToggle` = `pm hide/disable`、`onThermalControl` = 读 + 备份、`onSetFont` = 纯提示框）。
//    故按**行为族**命名，不按机制命名；也删掉了"既不写设备存储"的说法（`onThermalControl` 落盘一份
//    配置备份到 /sdcard —— 是备份产物，不改设备行为，但原文那句话与磁盘不符）。
// 本槽把这条界线**钉成机器可检**（否则下一轮复核会把同一处当缺口重报）：
// 这 11 个 handler 的函数体里必须**一个闸门都没有**。若将来决定纳入，先在这里红 ——
// 作者必须回来同时改本槽、spec §3 的界线说明与接线计数（30 —— W8-T6 起：FlashPanel 15 + 系统工具 10
// + 漏洞面板 4 + EUB 1）。
void TestRiskGateWiring::preferenceSwitchesStayUngatedByDesign()
{
    const QString src = readSource(QStringLiteral("/src/ui/system_tool_panel.cpp"));
    QVERIFY2(!src.isNull(),
             qPrintable(QStringLiteral("读不到生产源文件：%1/src/ui/system_tool_panel.cpp")
                            .arg(QStringLiteral(PT_SRC_ROOT))));

    const QStringList ungated = {
        QStringLiteral("void SystemToolPanel::onSetDpi()"),
        QStringLiteral("void SystemToolPanel::onAnimationScale()"),
        QStringLiteral("void SystemToolPanel::onStatusBar()"),
        QStringLiteral("void SystemToolPanel::onNavBar()"),
        QStringLiteral("void SystemToolPanel::onSetFont()"),
        QStringLiteral("void SystemToolPanel::onThermalControl()"),
        QStringLiteral("void SystemToolPanel::onCpuGovernor()"),
        QStringLiteral("void SystemToolPanel::onGpuGovernor()"),
        QStringLiteral("void SystemToolPanel::onIOScheduler()"),
        QStringLiteral("void SystemToolPanel::onMemoryOptimize()"),
        QStringLiteral("void SystemToolPanel::onAppFreezeToggle()"),
    };
    QCOMPARE(ungated.size(), 11);   // 表本身受检

    for (const QString &signature : ungated) {
        const QString body = functionBody(src, signature);
        QVERIFY2(!body.isEmpty(),
                 qPrintable(QStringLiteral("找不到函数定义（改名/删除？）：%1").arg(signature)));
        QVERIFY2(body.count(kGate) == 0,
                 qPrintable(QStringLiteral("%1 里出现了闸门 —— 若是有意纳入门控，请同时更新 "
                                           "spec §3 的界线说明、接线计数与用例里的本张表")
                                .arg(signature)));
    }
}

// W2-T5 复审 High 的判据钉（"**显示即启用**"）：凡在 `clearDeviceInfo()` 里被 `setEnabled(false)`
// 的控件，只要在 `setDeviceInfo` 里被 `show()`，就必须**也在 `setDeviceInfo` 里**出现 `setEnabled(`。
// 为什么：这类控件的启用路径若只留在 handler 里，而 handler 只有点到按钮才能进入，则
// `clearDeviceInfo()` 一跑（拔线 / 取消选中 / 空列表刷新）就把它们**禁死** —— 设备回来时 `show()`
// 出来的是灰键，点击路径整条断掉（`e179bb5` 的实测后果：连接 EDL / EDL 刷写计划… / 连接 MTK）。
// 复位钉只问"禁有没有"、不问"启有没有"，所以那条回归对它全绿 —— 本槽补的就是这个方向。
// 取材边界（**tripwire，不是运行期不变量**）：只数字面文本 —— 把启用写成**注释**
// （`// m_edlConnectBtn->setEnabled(...)`）、放进 `if (false)` 之类的不可达分支、或换成
// `setDisabled(false)` 等**等价写法**，本钉都会**绿**（复审实测：真行改成内容相同的注释后，
// 复位钉不受影响，于是"禁死"回来而**两条钉都不红**）。它防的是"改动时忘了回位"，不是"有人蓄意绕过"。
//   * 也只认 `m_xxxBtn->show()` 与 `m_xxxBtn->setEnabled(` 两种写法 —— `hide()`、父控件禁用、
//     别名/局部指针写法同样逃逸（与复位钉同款口径）；
//   * 它**不会**误伤 `m_edlSelectProgBtn`（被 `show()` 但不在复位表里 → 前件不成立）。
// 覆盖计数：复位表 16 个里被 show() 的是 **5** 个（EDL/MTK 两支的连接 / 断开 / 计划键），
// 其余 11 个（刷入/擦除/读取/更新/Sideload/脚本/启动镜像/解锁/回锁/FRP/死砖）常驻可见、不走 show()。
void TestRiskGateWiring::flashPanelShownButtonsAreReenabledInTheirModeBranch()
{
    const QString src = readSource(QStringLiteral("/src/ui/flash_panel.cpp"));
    QVERIFY2(!src.isNull(),
             qPrintable(QStringLiteral("读不到生产源文件：%1/src/ui/flash_panel.cpp")
                            .arg(QStringLiteral(PT_SRC_ROOT))));

    const QString setBody = functionBody(
        src, QStringLiteral("void FlashPanel::setDeviceInfo(const DeviceInfo &info)"));
    const QString clearBody = functionBody(src, QStringLiteral("void FlashPanel::clearDeviceInfo()"));
    QVERIFY2(!setBody.isEmpty(), "找不到 FlashPanel::setDeviceInfo 的定义（改名/删除？）");
    QVERIFY2(!clearBody.isEmpty(), "找不到 FlashPanel::clearDeviceInfo 的定义（改名/删除？）");

    // 复位表成员（clearDeviceInfo 体内 setEnabled(false) 的控件）
    QStringList cleared;
    const QRegularExpression re(QStringLiteral("(m_\\w+)->setEnabled\\(false\\)"));
    QRegularExpressionMatchIterator it = re.globalMatch(clearBody);
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        if (!cleared.contains(name))
            cleared << name;
    }
    cleared.sort();
    QCOMPARE(cleared.size(), 16);   // 复位表形状（与复位钉同一张表）

    QStringList shownAndCleared;
    for (const QString &name : cleared) {
        if (!setBody.contains(name + QStringLiteral("->show()")))
            continue;   // 前件不成立：不靠 show() 出现 → 不在本钉范围内
        shownAndCleared << name;
        const QString needle = name + QStringLiteral("->setEnabled(");
        QVERIFY2(setBody.count(needle) >= 1,
                 qPrintable(QStringLiteral("%1 在 clearDeviceInfo 被禁用、在 setDeviceInfo 被 show()，"
                                           "但那里没有把它重新启用 —— 复位会变成**禁死**："
                                           "它的启用路径只剩 handler，而 handler 只有点到按钮才进得去")
                                .arg(name)));
    }
    shownAndCleared.sort();
    // 5 = EDL/MTK 两支的连接 / 断开 / 计划键（修前这里恰好红三个：连接 EDL / EDL 刷写计划… / 连接 MTK）
    QCOMPARE(shownAndCleared.join(QStringLiteral(",")),
             QStringLiteral("m_edlConnectBtn,m_edlDisconnectBtn,m_edlPlanBtn,"
                            "m_mtkConnectBtn,m_mtkDisconnectBtn"));
}

// `clearDeviceInfo()`（"设备没了"的复位口）**必须把本文件里凡被 `setEnabled(...)` 碰过的按钮
// 全部禁用**。两轮复审各撞见同一类缺口：先是 m_unlockBtn/m_lockBtn/m_frpBtn（Low-1a），
// 再是 EDL/MTK 的连接 / 断开 / 刷写计划键（复审 ③）—— 都是"由 handler 自己 setEnabled、不由
// setDeviceInfo 的模式分支统一复位"的按钮。判据因此从"setDeviceInfo 启用过谁"扩成**全文件**，
// 免得下一轮再一处处补：
//   * 提取：`src/ui/flash_panel.cpp` 全文的 `m_xxxBtn->setEnabled(` （**不论 true/false、不论参数
//     是常量还是条件表达式** —— 条件式启用也能抓到，这正是上一版只扫 `setEnabled(true)` 的漏网处），
//     排除 clearDeviceInfo 自身（它的 setEnabled(false) 是要检查的对象，不是启用点）；
//   * 集合本身排序后逐字钉住 16 个成员（成员换了/新增一个都要红）；
//   * 逐个要求 clearDeviceInfo 里有**恰好一次** `m_xxxBtn->setEnabled(false);`。
// 为什么值得：拔线后仍可点的按钮 → 用户点下去 → 闸门按模式取档得到 MODE_UNKNOWN → tier None
// → **不弹窗直接放行**（今天只剩各 handler 自己的 serialNumber 判据兜着，那是另一道）。
// **取材边界（tripwire，不是运行期不变量）**：只认 `m_xxxBtn->setEnabled(` 这一种写法 ——
// 用局部指针/别名/别的成员名形式改启用状态就漏；也不管 show()/hide() 与父控件禁用。
// **未声明的逃逸形态（复审实测仍绿，只声明不修）**：`setDisabled(true/false)` —— 它与
// `setEnabled(false/true)` 语义等价，是最可能被写出来的变体，但本钉完全看不见它（既不在 `touched`
// 集合里、也不触发任何断言）。真要用它，请连同本注释与集合字面一起改。
// 另一条已知形态（响亮方向）：needle 是 `name + "->setEnabled(false);"`，**空白敏感** ——
// 写成 `setEnabled( false );` 或加空格会让 count 变 0 → 红；那是"防漏"的安全方向，
// 纯排版改动造成的误报属**预期**，改的人照红改回原写法即可。
// 覆盖范围说明（复审 ③(c)）：这些按钮成员都是 FlashPanel 的私有成员，全仓 grep 只在
// flash_panel.h/.cpp 出现，故**没有**别的文件里的启用点落在本钉之外。
void TestRiskGateWiring::flashPanelClearsEveryButtonItCanEnable()
{
    const QString src = readSource(QStringLiteral("/src/ui/flash_panel.cpp"));
    QVERIFY2(!src.isNull(),
             qPrintable(QStringLiteral("读不到生产源文件：%1/src/ui/flash_panel.cpp")
                            .arg(QStringLiteral(PT_SRC_ROOT))));

    const QString clearSig = QStringLiteral("void FlashPanel::clearDeviceInfo()");
    const QString clearBody = functionBody(src, clearSig);
    QVERIFY2(!clearBody.isEmpty(), "找不到 FlashPanel::clearDeviceInfo 的定义（改名/删除？）");
    const qsizetype clearBegin = src.indexOf(QLatin1Char('{'), src.indexOf(clearSig));
    const qsizetype clearEnd = clearBegin + clearBody.size();

    QStringList touched;
    const QRegularExpression re(QStringLiteral("(m_\\w+)->setEnabled\\("));
    QRegularExpressionMatchIterator it = re.globalMatch(src);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const qsizetype pos = m.capturedStart(1);
        if (pos >= clearBegin && pos < clearEnd)
            continue;   // clearDeviceInfo 自己的复位调用不算"启用点"
        const QString name = m.captured(1);
        if (!touched.contains(name))
            touched << name;
    }
    touched.sort();

    // 16 个被 setEnabled 碰过的按钮（逐个列出而非只比条数：条数相同而成员换了也要红）。
    // 其中 m_unlockBtn/m_lockBtn/m_frpBtn（Low-1a）与 m_edlConnectBtn/m_edlDisconnectBtn/
    // m_edlPlanBtn/m_mtkConnectBtn/m_mtkDisconnectBtn（复审 ③）是两轮实际补上的成员。
    QCOMPARE(touched.join(QStringLiteral(",")),
             QStringLiteral("m_bootImgBtn,m_brickRepairBtn,m_dumpBtn,m_edlConnectBtn,"
                            "m_edlDisconnectBtn,m_edlPlanBtn,m_eraseBtn,m_flashBtn,m_frpBtn,"
                            "m_lockBtn,m_mtkConnectBtn,m_mtkDisconnectBtn,m_runScriptBtn,"
                            "m_sideloadBtn,m_unlockBtn,m_updateBtn"));

    // 说明：这里**不再**单列"两轮补上的 8 个成员必在 touched 里"的循环 —— 复审实测它被上面那条
    // `join(",") == <16 成员字面>` 完全吸收（只要那条过，这 8 个必然都在），**没有独立甄别力**。
    // 成员的来历记在注释里就够了：m_unlockBtn/m_lockBtn/m_frpBtn（Low-1a）、
    // m_edlConnectBtn/m_edlDisconnectBtn/m_edlPlanBtn/m_mtkConnectBtn/m_mtkDisconnectBtn（复审 ③）。

    for (const QString &name : touched) {
        const QString needle = name + QStringLiteral("->setEnabled(false);");
        QVERIFY2(clearBody.count(needle) == 1,
                 qPrintable(QStringLiteral("clearDeviceInfo 没有（或重复）复位 %1 —— 拔线后它仍可点，"
                                           "而闸门按模式取档在 MODE_UNKNOWN 上给 None = 不弹窗")
                                .arg(name)));
    }
}

// ② 设备模式显示名两侧一致（W6-T1 移交的收口）：
//    `DeviceDetector::getModeDisplayName`（core，device_detector.cpp）与 FlashPanel 的**本地副本**
//    （flash_panel.cpp 的 setDeviceInfo 内 switch）都显示 MODE_MTK_BROM 的名字。
//    白名单同收 BROM 与 preloader，而**身份要握手后由 GET_BL_VER 判定** —— 只写 "MTK BROM" 是把
//    preloader 说成 BROM。两侧各一份字面，改了 core 不改 UI 是无声的漂移，故用这条钉住**两者相等**。
//    取值边界：只比对这两个分支的第一个字符串字面量（写法换了 → 取不到 → 这里红）。
void TestRiskGateWiring::mtkBromDisplayNamePairIsConsistent()
{
    const QString coreSrc = readSource(QStringLiteral("/src/core/device_detector.cpp"));
    const QString uiSrc = readSource(QStringLiteral("/src/ui/flash_panel.cpp"));
    QVERIFY2(!coreSrc.isNull() && !uiSrc.isNull(),
             qPrintable(QStringLiteral("读不到生产源文件（%1）").arg(QStringLiteral(PT_SRC_ROOT))));

    // core：getModeDisplayName 的 `case MODE_MTK_BROM: ... return QStringLiteral("...");`
    const QString coreName = branchStringLiteral(
        coreSrc, QStringLiteral("case MODE_MTK_BROM:"), QStringLiteral("return QStringLiteral(\""));
    // UI：FlashPanel 本地副本的 `case DeviceDetector::MODE_MTK_BROM: ... modeStr = "...";`
    const QString uiName = branchStringLiteral(
        uiSrc, QStringLiteral("case DeviceDetector::MODE_MTK_BROM:"), QStringLiteral("modeStr = \""));

    QVERIFY2(!coreName.isEmpty(), "取不到 device_detector.cpp 的 MODE_MTK_BROM 文案（写法变了？）");
    QVERIFY2(!uiName.isEmpty(), "取不到 flash_panel.cpp 的 MODE_MTK_BROM 文案（写法变了？）");

    QCOMPARE(uiName, coreName);                                     // 两侧一致
    QCOMPARE(uiName, QStringLiteral("MTK BROM / Preloader"));       // 且是 W6-T1 定下的那句话
}

QTEST_APPLESS_MAIN(TestRiskGateWiring)
#include "test_risk_gate_wiring.moc"
