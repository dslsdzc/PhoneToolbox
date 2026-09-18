// tests/test_mtk_plan_dialog.cpp
//
// MTK 计划预览对话框：确认门控 + 预览内容（条目进表、告警进列表）。
// offscreen 跑（CMakeLists 的 ENVIRONMENT 与 test_flash_plan_dialog 同款），只断言控件状态。
#include <QtTest>
#include <QCheckBox>
#include <QFile>
#include <QListWidget>
#include <QPushButton>
#include <QTableView>

#include "core/mtk_flash_plan.h"
#include "ui/mtk_plan_dialog.h"

class TestMtkPlanDialog : public QObject
{
    Q_OBJECT
private slots:
    void startButtonGatedByCheckbox();
    void rejectLeavesConfirmedFalse();
    void previewsEntriesAndWarnings();
    void buildAndShowRejectsEmptyPlanBeforeShowing();
    void minimumSizeMatchesDialogFamily();
    void pinsInitialSizeMatchesMinimumInProductionSource();
};

static mtkplan::MtkFlashPlan oneEntryPlan()
{
    mtkplan::MtkFlashPlan plan;
    mtkplan::PlanEntry e;
    e.partition = QStringLiteral("boot");
    e.imagePath = QStringLiteral("/fw/boot.img");
    e.imageSize = 1024 * 1024;
    e.partitionSize = 32ull * 1024 * 1024;
    e.matchRule = QStringLiteral("exact");
    plan.entries << e;
    plan.totalBytes = e.imageSize;
    plan.warnings << QStringLiteral("其中 1 个镜像小于分区容量（正常：分区尾部保持原样）");
    return plan;
}

void TestMtkPlanDialog::startButtonGatedByCheckbox()
{
    MtkPlanDialog dlg(oneEntryPlan());
    auto *start = dlg.findChild<QPushButton *>(QStringLiteral("startButton"));
    auto *ack = dlg.findChild<QCheckBox *>(QStringLiteral("ackCheck"));
    QVERIFY(start && ack);
    QVERIFY(!ack->isChecked());
    QVERIFY(!start->isEnabled());            // 未勾选 → 不得开刷
    ack->setChecked(true);
    QVERIFY(start->isEnabled());
    QVERIFY(!dlg.confirmed());
    dlg.accept();
    QVERIFY(dlg.confirmed());
    ack->setChecked(false);                  // 反向门控（防"恒 true"的假实现）
    QVERIFY(!start->isEnabled());
}

void TestMtkPlanDialog::rejectLeavesConfirmedFalse()
{
    MtkPlanDialog dlg(oneEntryPlan());
    dlg.reject();
    QVERIFY(!dlg.confirmed());
}

void TestMtkPlanDialog::previewsEntriesAndWarnings()
{
    MtkPlanDialog dlg(oneEntryPlan());
    auto *table = dlg.findChild<QTableView *>(QStringLiteral("planTable"));
    QVERIFY(table && table->model());
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->columnCount(), 5);
    QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("boot.img"));   // 镜像文件
    QCOMPARE(table->model()->index(0, 1).data().toString(), QStringLiteral("boot"));       // 目标分区
    QCOMPARE(table->model()->index(0, 2).data().toString(), QStringLiteral("1.0 MiB"));    // 镜像大小
    QCOMPARE(table->model()->index(0, 3).data().toString(), QStringLiteral("32.0 MiB"));   // 分区大小
    QCOMPARE(table->model()->index(0, 4).data().toString(), QStringLiteral("精确"));       // 匹配规则
    auto *warn = dlg.findChild<QListWidget *>(QStringLiteral("warningsList"));
    QVERIFY(warn);
    QCOMPARE(warn->count(), 1);              // 告警不静默
}

// 同族三个计划对话框（EDL / Odin / MTK）装同一个 PlanPreviewWidget：最小尺寸必须一致。
// ⚠️ **不要**在这里补 `dlg.size() >= dlg.minimumSize()` 之类的断言：Qt 本就保证 size() ≥
// minimumSize()，只要下面那条 QCOMPARE 成立，那类断言对任何实现都**恒真**（实测：把实现里的
// `resize(880,520)` 改成 `resize(1200,800)`，它们照样全绿）。
// ⚠️ 也**不要**写严格相等 `QCOMPARE(dlg.size(), QSize(880,520))`：虽有甄别力，但有平台脆弱性
//（Qt 只把 size() 钳到 min 以上、**不保证等于**；任何环境下 sizeHint 把窗口撑过 880x520 就误红）。
// "初始尺寸 == 最小尺寸"这个事实由**源文件钉**负责 —— 见本文件
// `pinsInitialSizeMatchesMinimumInProductionSource`，它不受 DPI/sizeHint 影响。
void TestMtkPlanDialog::minimumSizeMatchesDialogFamily()
{
    MtkPlanDialog dlg(oneEntryPlan());
    QCOMPARE(dlg.minimumSize(), QSize(880, 520));     // 运行期判据（只钉最小尺寸）
}

// A-35 的结构钉：「初始尺寸 == 最小尺寸」= 生产源码里 `setMinimumSize(880, 520)` 与
// `resize(880, 520)` **两处都在**。这一事实在运行期测不出（上面那条注释写了两个失败方案），
// 故钉**代码事实**（先例：test_flash_button_labels 的 flashPanelSetsFlashButtonLabelsExactlyOnce；
// 以及 test_odin_libusb_transport / test_edl_libusb_transport 的源文件钉）。
// ⚠️ 取材边界：只数**字面出现次数** —— 把调用包进 `if (false)` 仍然绿（唯一逃逸形态，已实测：
//    m1-struct-mut-b-*）；反向地，注释里写出该字面量或把调用换行书写会误红。
// ⚠️ 契约：要求源码树原地存在（路径被编进二进制）—— 读不到即**硬 FAIL**、不 QSKIP
//    （"钉在缺文件时静默失效"正是本仓反复吃亏的形态）。
void TestMtkPlanDialog::pinsInitialSizeMatchesMinimumInProductionSource()
{
    QFile f(QStringLiteral(QT_TESTCASE_SOURCEDIR "/src/ui/mtk_plan_dialog.cpp"));
    QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(f.fileName()));
    const QString src = QString::fromUtf8(f.readAll());
    QCOMPARE(src.count(QStringLiteral("setMinimumSize(880, 520)")), 1);
    QCOMPARE(src.count(QStringLiteral("resize(880, 520)")), 1);
}

// 空计划 = 必须**弹窗前**失败且写出 *error（"err != 初值"证明结论是本次写的，不是赖调用方的老值；
// parent=nullptr 且不驱动对话框 —— 一旦实现走到 exec()，模态循环出不来，用例必红）
void TestMtkPlanDialog::buildAndShowRejectsEmptyPlanBeforeShowing()
{
    const QString sentinel = QStringLiteral("未回填的初值");
    QString err = sentinel;
    mtkplan::MtkFlashPlan empty;
    QVERIFY(!MtkPlanDialog::buildAndShow(empty, nullptr, &err));
    QVERIFY(err != sentinel);
    QVERIFY(!err.isEmpty());
}

QTEST_MAIN(TestMtkPlanDialog)
#include "test_mtk_plan_dialog.moc"
