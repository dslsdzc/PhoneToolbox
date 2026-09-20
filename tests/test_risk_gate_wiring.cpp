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
//     * 按钮接线集合：FlashPanel 每个 `QPushButton` 的槽名集合 == "14 个已过闸门入口
//       + 6 个按设计不过闸门入口"。**新增按钮 → 这条红 → 强迫作者回来核对新入口该不该过闸门。**
//     * 断线复位：`setDeviceInfo` 能启用的每个按钮，`clearDeviceInfo` 必须禁用（W2-T5 复审 Low-1a：
//       漏了三个按钮 → 拔线后仍可点，而闸门按模式取档在 MODE_UNKNOWN 上给 None = 不弹窗）。
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
    // ① 接线钉：FlashPanel 的 14 个 handler
    void flashPanelGatesAllFourteenHandlers();
    // ① 接线钉：其余三处（10 + 3 + 1）
    void systemToolPanelGatesTenEntries();
    void vulnPanelGatesThreeEntries();
    void eubRecoveryDialogGatesOnStart();
    // ① 接线钉：首屏（MainWindow 构造之前）
    void mainAsksAgreementBeforeTheMainWindow();
    // ② tripwire：按钮接线集合 == 已过闸门 14 + 按设计不过闸门 6
    void flashPanelButtonConnectsMatchTheGatedSet();
    // ② 复位钉：setDeviceInfo 能启用的按钮，clearDeviceInfo 必须禁用（W2-T5 复审 Low-1a）
    void flashPanelClearsEveryButtonSetDeviceInfoCanEnable();
    // ② 一对文案钉：设备模式显示名两侧一致（W6-T1 移交）
    void mtkBromDisplayNamePairIsConsistent();
};

// FlashPanel 的 14 个入口：每个入口的第一件事都是闸门，tier 逐个钉住取法。
// 取档来源分两类（W2-T5 复审 L-1(b)）：
//   * 11 个入口从**面板已有的设备模式**取（`m_deviceInfo.mode` —— 本面板没有独立的 m_currentMode，
//     见 W2-T5 报告；不为接线新增并行状态）；
//   * 3 个入口（解锁/回锁/FRP）改用**功能映射** —— 这三个按钮**不由模式分支启用**（ADB+已 Root /
//     MTK 已连接 都算），拔线后闸门若按模式取会落到 MODE_UNKNOWN → tier None → 不弹窗直接放行。
//     功能映射不随模式漂，天然 fail-closed。
void TestRiskGateWiring::flashPanelGatesAllFourteenHandlers()
{
    const QString src = readSource(QStringLiteral("/src/ui/flash_panel.cpp"));
    QVERIFY2(!src.isNull(),
             qPrintable(QStringLiteral("读不到生产源文件：%1/src/ui/flash_panel.cpp")
                            .arg(QStringLiteral(PT_SRC_ROOT))));

    // 计数：14 = 上表 14 个 handler 各一次（多一处 = 有人重复挂闸门 / 少一处 = 有人漏接线）。
    QCOMPARE(countIn(src, kGate), 14);

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
    };
    QCOMPARE(int(sizeof(rows) / sizeof(rows[0])), 14);   // 表本身受检：少一行就少钉一个入口

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

// VulnPanel 的 3 个入口（扫描 + 批量利用 + 单条利用）：同一档（spec §5：扫描也纳入）。
void TestRiskGateWiring::vulnPanelGatesThreeEntries()
{
    const QString src = readSource(QStringLiteral("/src/ui/vuln_panel.cpp"));
    QVERIFY2(!src.isNull(),
             qPrintable(QStringLiteral("读不到生产源文件：%1/src/ui/vuln_panel.cpp")
                            .arg(QStringLiteral(PT_SRC_ROOT))));

    QCOMPARE(countIn(src, kGate), 3);

    const QStringList handlers = {
        QStringLiteral("void VulnPanel::onScan()"),
        QStringLiteral("void VulnPanel::onExploitAll()"),
        QStringLiteral("void VulnPanel::onExploitSingle(const QString &cveId)"),
    };
    QCOMPARE(handlers.size(), 3);

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

// ② tripwire：FlashPanel 每个 QPushButton 的 clicked 槽名集合 == 14 个已过闸门入口
//    + 6 个**按设计不过闸门**的入口（不写设备/不需要风险确认：文件选择、返回、刷新分区列表、
//    选 programmer 文件、EDL 断开、MTK 断开 —— 逐条都只做本机 UI 或断开连接）。
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
    QCOMPARE(int(connectedSlots.size()), 20);

    const QStringList gated = {
        // 14 个已过闸门的入口
        QStringLiteral("onFlashClicked"), QStringLiteral("onEraseClicked"),
        QStringLiteral("onUpdateClicked"), QStringLiteral("onSideloadClicked"),
        QStringLiteral("onRunScriptClicked"), QStringLiteral("onDumpClicked"),
        QStringLiteral("onBootImageClicked"), QStringLiteral("onUnlockBootloader"),
        QStringLiteral("onLockBootloader"), QStringLiteral("onFrpErase"),
        QStringLiteral("onBrickRepairClicked"), QStringLiteral("onEdlConnect"),
        QStringLiteral("onEdlPlanFlash"), QStringLiteral("onMtkConnect"),
        // 6 个按设计不过闸门（本机 UI / 文件选择 / 断开连接，不发起任何设备写入）
        QStringLiteral("onSelectFile"), QStringLiteral("onRefreshPartitions"),
        QStringLiteral("onEdlSelectProgrammer"), QStringLiteral("onEdlDisconnect"),
        QStringLiteral("onMtkDisconnect"), QStringLiteral("switchToDeviceInfo"),
    };
    QCOMPARE(gated.size(), 20);

    QStringList got = connectedSlots, want = gated;
    got.sort();
    want.sort();
    QCOMPARE(got, want);
}

// W2-T5 复审 Low-1(a)：`clearDeviceInfo()`（"设备没了"的复位口）漏掉了三个按钮 ——
// 拔线后它们仍可点，而闸门按模式取档会得到 MODE_UNKNOWN → tier None → **不弹窗直接放行**；
// 那时只剩各 handler 自己的 serialNumber 判据兜着（另一道，不是闸门这道）。
// 本槽钉"setDeviceInfo 能启用的每个按钮，clearDeviceInfo 必须禁用"这条不变量：
//   * 提取：从 setDeviceInfo 函数体抓 `m_xxxBtn->setEnabled(true)`（正则），逐个要求在
//     clearDeviceInfo 函数体里有 `m_xxxBtn->setEnabled(false);`；
//   * 集合本身也钉住（排序后逐字比对）——新增启用点 → 红 → 作者必须回答"拔线后怎么复位"。
// 取材边界：纯文本断言（只认 `setEnabled(true)` 这种写法）；且**不覆盖**由各处理函数自己启用的
// 按钮（EDL/MTK 的连接类按钮就是这批）—— 那批拔线后同样不复位，属**另一处已知缺口**
// （不由本槽背书，见 W2-T5 报告 §疑虑）。
void TestRiskGateWiring::flashPanelClearsEveryButtonSetDeviceInfoCanEnable()
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

    QStringList enabled;
    const QRegularExpression re(QStringLiteral("(m_\\w+)->setEnabled\\(true\\)"));
    QRegularExpressionMatchIterator it = re.globalMatch(setBody);
    while (it.hasNext()) {
        const QString name = it.next().captured(1);
        if (!enabled.contains(name))
            enabled << name;
    }
    enabled.sort();
    // 7 个"要有设备才能点"的按钮（unlock/lock/frp 就是 Low-1 漏掉的那三个）。
    // 逐个列出而非只比条数：条数相同而成员换了（例如拿 flashBtn 顶替 frpBtn）也要红。
    QCOMPARE(enabled.join(QStringLiteral(",")),
             QStringLiteral("m_brickRepairBtn,m_dumpBtn,m_flashBtn,m_frpBtn,"
                            "m_lockBtn,m_unlockBtn,m_updateBtn"));

    for (const QString &name : enabled) {
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
