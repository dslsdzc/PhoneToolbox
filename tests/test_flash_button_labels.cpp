// tests/test_flash_button_labels.cpp
//
// FlashPanel「刷入」按钮的 (文案, tooltip) 逐枚举钉子 —— EUB backlog Task 3。
//
// 缺陷出处（I3）：EUB 终审发现 FlashPanel 只复位按钮 text、没复位 tooltip。EUB 设备来过之后，
// 普通 fastboot 设备会顶着「EUB 救援…」文案 +「只发 RAM 镜像、不写存储」的救援提示执行**不可逆的
// 分区写入** —— 用户按 tooltip 的保证去点，正是救援工具最不该给的错误保证。
// 详见 .superpowers/sdd/eub-final-fix-report.md §I3（当时的修复**无自动化覆盖**，本文件就是那次的回归钉）。
//
// 为什么值得抽纯函数：flash_panel.cpp 的依赖闭包过大（FlashTool/AdbEmbedded/各协议通道/libusb），
// 仓内没有任何 FlashPanel 用例 —— 文案/tooltip 这类"读码才能发现"的缺陷一直只能靠人工核对。
// 抽出只依赖 DeviceDetector::DeviceMode 的纯函数后，本用例不构造任何窗口/设备即可把每个模式的
// 组合钉死（含"非协议模式必须文案与 tooltip 一并复位"这条 I3 回归钉）。
#include <QtTest>

#include "ui/flash_button_labels.h"

namespace {

struct ModeExpectation {
    DeviceDetector::DeviceMode mode;
    const char *name;     // 失败信息里点名的枚举值（不用 int，免得对数时来回换算）
    QString text;
    QString tooltip;      // 空 = 无提示（复位态）
};

// 全枚举表：**逐个枚举值一行**（不用 i 循环生成 —— 插入/重排/漏登记都要在这里显形）。
// 各协议模式的文案与 tooltip 逐字取自 src/ui/flash_panel.cpp（Task 3 收口前的现文），
// 与 src/ui/flash_button_labels.cpp 的实现互为独立副本：实现改字、这里没跟着改 → 红。
const ModeExpectation kModes[] = {
    {DeviceDetector::MODE_UNKNOWN, "MODE_UNKNOWN", QStringLiteral("刷入"), QString()},
    {DeviceDetector::MODE_ADB, "MODE_ADB", QStringLiteral("刷入"), QString()},
    {DeviceDetector::MODE_FASTBOOT, "MODE_FASTBOOT", QStringLiteral("刷入"), QString()},
    {DeviceDetector::MODE_FASTBOOTD, "MODE_FASTBOOTD", QStringLiteral("刷入"), QString()},
    {DeviceDetector::MODE_EDL_9008, "MODE_EDL_9008", QStringLiteral("刷入"), QString()},
    {DeviceDetector::MODE_MTK_DA, "MODE_MTK_DA", QStringLiteral("刷入"), QString()},
    {DeviceDetector::MODE_RECOVERY, "MODE_RECOVERY", QStringLiteral("刷入"), QString()},
    {DeviceDetector::MODE_MTK_BROM, "MODE_MTK_BROM", QStringLiteral("刷入"),
     QStringLiteral("协议通道按计划刷写：DA + 镜像 → 计划预览 → 按设备代际自动选择 "
                    "LEGACY / XFLASH / XML 链（三代均已实现，实际链路见日志「代际判定」）")},
    {DeviceDetector::MODE_HUAWEI_USB_UPDATE, "MODE_HUAWEI_USB_UPDATE", QStringLiteral("刷入"),
     QStringLiteral("协议通道整包/按计划刷写（按模式选择 update.app / pac+FDL / DA+镜像 / 三星 tar.md5）")},
    {DeviceDetector::MODE_SPD, "MODE_SPD", QStringLiteral("刷入"),
     QStringLiteral("协议通道整包/按计划刷写（按模式选择 update.app / pac+FDL / DA+镜像 / 三星 tar.md5）")},
    {DeviceDetector::MODE_SAMSUNG_ODIN, "MODE_SAMSUNG_ODIN", QStringLiteral("刷入"),
     QStringLiteral("协议通道整包/按计划刷写（按模式选择 update.app / pac+FDL / DA+镜像 / 三星 tar.md5）")},
    {DeviceDetector::MODE_SAMSUNG_EUB, "MODE_SAMSUNG_EUB", QStringLiteral("EUB 救援…"),
     QStringLiteral("EUB 救援：用你自备的原厂 BL（sboot.bin / BL_*.tar.md5）按 SoC 布局表分段注入设备 RAM，"
                    "把设备引导进 Download 模式；本流程只发 RAM 镜像、不写存储，完成后请继续用三星刷写")},
};

constexpr int kModeCount = int(sizeof(kModes) / sizeof(kModes[0]));

// 非协议模式（I3 的回归面）：这些模式下「刷入」执行的是 fastboot/ADB 分区写入或什么都不做，
// 都必须回到复位态 —— 文案「刷入」+ **空 tooltip**。
const DeviceDetector::DeviceMode kNonProtocolModes[] = {
    DeviceDetector::MODE_ADB,     DeviceDetector::MODE_FASTBOOT,
    DeviceDetector::MODE_FASTBOOTD, DeviceDetector::MODE_EDL_9008,
    DeviceDetector::MODE_MTK_DA,  DeviceDetector::MODE_RECOVERY,
    DeviceDetector::MODE_UNKNOWN,
};

QString describe(const char *modeName, const flashui::ButtonLabels &lb)
{
    return QStringLiteral("模式 %1: text=\"%2\", tooltip=\"%3\"")
        .arg(QString::fromUtf8(modeName), lb.text, lb.tooltip);
}

} // namespace

class TestFlashButtonLabels : public QObject
{
    Q_OBJECT

private slots:
    // 表本身的自检：表长必须等于「最后一个枚举值 + 1」，且逐行的枚举值等于行号。
    // 枚举值在表里是**逐行写清的常量**（不是 i 生成）—— 插入/重排枚举、或在中间漏登记，
    // 行号与枚举值立刻对不上 → 红。新增枚举值必须同时在本表与实现里登记，否则该模式的
    // 文案/tooltip 无人看守（C++17 无枚举反射，末尾追加的检测见报告"疑虑"）。
    void modeTableIsComplete()
    {
        QCOMPARE(kModeCount, int(DeviceDetector::MODE_SAMSUNG_EUB) + 1);
        for (int i = 0; i < kModeCount; ++i) {
            QVERIFY2(int(kModes[i].mode) == i,
                     qPrintable(QStringLiteral("表第 %1 行登记的是 %2（值 %3）—— 枚举与表已错位")
                                    .arg(i)
                                    .arg(QString::fromUtf8(kModes[i].name))
                                    .arg(int(kModes[i].mode))));
        }
    }

    // 逐枚举断言（全枚举覆盖）：每个模式的文案与 tooltip 都要与钉住的期望值逐字相同。
    void everyModeMatchesItsPinnedLabels()
    {
        for (int i = 0; i < kModeCount; ++i) {
            const ModeExpectation &e = kModes[i];
            const flashui::ButtonLabels lb = flashui::flashButtonLabelsFor(e.mode);
            QVERIFY2(lb.text == e.text,
                     qPrintable(QStringLiteral("text 不符 —— %1；期望 \"%2\"")
                                    .arg(describe(e.name, lb), e.text)));
            QVERIFY2(lb.tooltip == e.tooltip,
                     qPrintable(QStringLiteral("tooltip 不符 —— %1；期望 \"%2\"")
                                    .arg(describe(e.name, lb), e.tooltip)));
        }
    }

    // I3 回归钉（本文件的**核心**）：非协议模式下文案与 tooltip 必须**一起**复位。
    // 变异①：把 MODE_FASTBOOT 也映射成救援文案/tooltip → 本槽红。
    // 变异②：复位态的 tooltip 改成非空（如仍留着救援提示）→ 本槽红。
    void nonProtocolModesAreFullyReset()
    {
        for (const DeviceDetector::DeviceMode mode : kNonProtocolModes) {
            const flashui::ButtonLabels lb = flashui::flashButtonLabelsFor(mode);
            QVERIFY2(lb.text == QStringLiteral("刷入"),
                     qPrintable(QStringLiteral("非协议模式的文案必须是「刷入」—— %1")
                                    .arg(describe("non-protocol", lb))));
            // 空字符串 = 无提示。非空即意味着这条提示会漂到别的模式上（I3）。
            QVERIFY2(lb.tooltip.isEmpty(),
                     qPrintable(QStringLiteral("非协议模式的 tooltip 必须为空（I3：只复位 text 不复位 "
                                               "tooltip 会让按钮顶着救援承诺执行不可逆写入）—— %1")
                                    .arg(describe("non-protocol", lb))));
        }
    }

    // EUB 的 tooltip 里那句承诺（"只发 RAM 镜像、不写存储"）是 I3 的事故核心：文案复位了、这句
    // 还在，用户就会按它去点一个**会写分区**的按钮。钉子只钉"这句还在"，措辞调整请同步本文件。
    void eubTooltipKeepsItsRamOnlyPromise()
    {
        const flashui::ButtonLabels lb =
            flashui::flashButtonLabelsFor(DeviceDetector::MODE_SAMSUNG_EUB);
        QCOMPARE(lb.text, QStringLiteral("EUB 救援…"));
        QVERIFY2(lb.tooltip.contains(QStringLiteral("只发 RAM 镜像、不写存储")),
                 qPrintable(QStringLiteral("EUB 救援 tooltip 必须写明 RAM-only 承诺 —— %1")
                                .arg(describe("MODE_SAMSUNG_EUB", lb))));
    }

    // 协议模式的 tooltip 必须非空（空 = 复位态，会与 I3 的"文案/提示与动作不符"混在一起分不清）,
    // 且协议模式**不能**共用同一个空值 —— 这条与上面两条一起把"复位态"与"协议态"划开。
    void protocolModesCarryTheirOwnTooltip()
    {
        const DeviceDetector::DeviceMode protocolModes[] = {
            DeviceDetector::MODE_MTK_BROM, DeviceDetector::MODE_HUAWEI_USB_UPDATE,
            DeviceDetector::MODE_SPD,      DeviceDetector::MODE_SAMSUNG_ODIN,
            DeviceDetector::MODE_SAMSUNG_EUB,
        };
        for (const DeviceDetector::DeviceMode mode : protocolModes) {
            const flashui::ButtonLabels lb = flashui::flashButtonLabelsFor(mode);
            QVERIFY2(!lb.tooltip.isEmpty(),
                     qPrintable(QStringLiteral("协议模式的 tooltip 不应为空（那是复位态的样子）—— %1")
                                    .arg(describe("protocol", lb))));
        }
    }
};

QTEST_APPLESS_MAIN(TestFlashButtonLabels)
#include "test_flash_button_labels.moc"
