// tests/test_mtk_plan_dialog.cpp
//
// MTK 计划预览对话框：确认门控 + 预览内容（条目进表、告警进列表）。
// offscreen 跑（CMakeLists 的 ENVIRONMENT 与 test_flash_plan_dialog 同款），只断言控件状态。
#include <QtTest>
#include <QCheckBox>
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
