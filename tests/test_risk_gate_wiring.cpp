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
//  ② 按钮接线对照钉（tripwire）：FlashPanel 每个 `QPushButton` 的槽名集合 == "14 个已过闸门入口
//     + 6 个按设计不过闸门入口"。**新增按钮 → 这条红 → 强迫作者回来核对新入口该不该过闸门。**
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
    // ① 接线钉：其余三处（4 + 3 + 1）
    void systemToolPanelGatesFourEntries();
    void vulnPanelGatesThreeEntries();
    void eubRecoveryDialogGatesOnStart();
    // ① 接线钉：首屏（MainWindow 构造之前）
    void mainAsksAgreementBeforeTheMainWindow();
    // ② tripwire：按钮接线集合 == 已过闸门 14 + 按设计不过闸门 6
    void flashPanelButtonConnectsMatchTheGatedSet();
    // ② 一对文案钉：设备模式显示名两侧一致（W6-T1 移交）
    void mtkBromDisplayNamePairIsConsistent();
};

// FlashPanel 的 14 个入口：每个入口的第一件事都是闸门，且 tier 从**面板已有的设备模式**取
// （`m_deviceInfo.mode` —— 本面板没有独立的 m_currentMode，见 W2-T5 报告；不为接线新增并行状态）。
void TestRiskGateWiring::flashPanelGatesAllFourteenHandlers()
{
    const QString src = readSource(QStringLiteral("/src/ui/flash_panel.cpp"));
    QVERIFY2(!src.isNull(),
             qPrintable(QStringLiteral("读不到生产源文件：%1/src/ui/flash_panel.cpp")
                            .arg(QStringLiteral(PT_SRC_ROOT))));

    // 计数：14 = 上表 14 个 handler 各一次（多一处 = 有人重复挂闸门 / 少一处 = 有人漏接线）。
    QCOMPARE(countIn(src, kGate), 14);

    const QStringList handlers = {
        QStringLiteral("void FlashPanel::onFlashClicked()"),
        QStringLiteral("void FlashPanel::onEraseClicked()"),
        QStringLiteral("void FlashPanel::onUpdateClicked()"),
        QStringLiteral("void FlashPanel::onSideloadClicked()"),
        QStringLiteral("void FlashPanel::onRunScriptClicked()"),
        QStringLiteral("void FlashPanel::onDumpClicked()"),
        QStringLiteral("void FlashPanel::onBootImageClicked()"),
        QStringLiteral("void FlashPanel::onUnlockBootloader()"),
        QStringLiteral("void FlashPanel::onLockBootloader()"),
        QStringLiteral("void FlashPanel::onFrpErase()"),
        QStringLiteral("void FlashPanel::onBrickRepairClicked()"),
        // 后三个是"底层引导"入口：连接即把 programmer/DA 上传到设备 RAM —— 同属 Tier2 写路径
        QStringLiteral("void FlashPanel::onEdlConnect()"),
        QStringLiteral("void FlashPanel::onEdlPlanFlash()"),
        QStringLiteral("void FlashPanel::onMtkConnect()"),
    };
    QCOMPARE(handlers.size(), 14);   // 表本身受检：这张表少一行，下面的循环就少钉一个入口

    for (const QString &signature : handlers) {
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

        // tier 表达式：必须是**模式映射**且取自面板已有的 m_deviceInfo.mode ——
        // 硬编 `RiskTier::None`（= 永不弹窗）或硬编某一档都会被这条钉住。
        const QString region = body.mid(body.indexOf(kGate), 200);
        QVERIFY2(region.contains(QStringLiteral("riskui::riskTierForMode(")),
                 qPrintable(QStringLiteral("%1 的闸门没有用 riskui::riskTierForMode(...) 取档")
                                .arg(signature)));
        QVERIFY2(region.contains(QStringLiteral("m_deviceInfo.mode")),
                 qPrintable(QStringLiteral("%1 的闸门没有从面板已有的 m_deviceInfo.mode 取模式")
                                .arg(signature)));
    }
}

// SystemToolPanel 的 4 个入口 → 功能映射（逐个钉住"哪个入口对应哪个 RiskFeature"：
// 四个都是 Tier3，互换不会改变行为，所以只有这条钉得住计划表里的对应关系）。
void TestRiskGateWiring::systemToolPanelGatesFourEntries()
{
    const QString src = readSource(QStringLiteral("/src/ui/system_tool_panel.cpp"));
    QVERIFY2(!src.isNull(),
             qPrintable(QStringLiteral("读不到生产源文件：%1/src/ui/system_tool_panel.cpp")
                            .arg(QStringLiteral(PT_SRC_ROOT))));

    QCOMPARE(countIn(src, kGate), 4);

    struct Entry {
        const char *signature;
        const char *feature;
    };
    const Entry entries[] = {
        // 功能名用**全限定**写法：RiskFeature 在 riskui 命名空间内，这些面板类不在其中
        // （首次全量构建实测：漏了 riskui:: 前缀 → "‘RiskFeature’未声明"，7 处一起炸）。
        { "void SystemToolPanel::onSpoofDevice()",      "riskui::RiskFeature::ImeiTool" },
        { "void SystemToolPanel::onSafetynetBypass()",  "riskui::RiskFeature::SafetyNet" },
        { "void SystemToolPanel::onSignatureSpoof()",   "riskui::RiskFeature::SignatureSpoof" },
        { "void SystemToolPanel::onOpenEngineerMode()", "riskui::RiskFeature::EngineerMode" },
    };
    QCOMPARE(int(sizeof(entries) / sizeof(entries[0])), 4);

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
