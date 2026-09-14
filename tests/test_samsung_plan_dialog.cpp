// tests/test_samsung_plan_dialog.cpp
//
// 三星计划预览对话框：确认门控 + 预览内容（条目进表、两类不匹配进告警）。
// offscreen 跑（CMakeLists 的 ENVIRONMENT 与 test_flash_plan_dialog 同款），只断言控件状态。
#include <QtTest>
#include <QCheckBox>
#include <QListWidget>
#include <QPushButton>
#include <QTableView>

#include "core/odin/samsung_plan.h"
#include "ui/samsung_plan_dialog.h"

class TestSamsungPlanDialog : public QObject
{
    Q_OBJECT
private slots:
    void startButtonGatedByCheckbox();
    void rejectLeavesConfirmedFalse();
    void previewsEntriesAndWarnings();
};

static odin::SamsungPlan oneEntryPlan()
{
    odin::SamsungPlan plan;
    plan.pitSource = QStringLiteral("包内 CSC.tar.md5 的 J1POP3G.pit");
    odin::SamsungPlanFile f;
    f.path = QStringLiteral("/tmp/pack/CSC_ODD.tar.md5");
    f.sizeBytes = 12345;
    f.md5HasFooter = true;
    f.verifyOk = true;
    plan.files << f;
    odin::SamsungPlanEntry e;
    e.partition = QStringLiteral("BOOT");
    e.imageFile = QStringLiteral("spl.img");
    e.sizeBytes = 32768;
    e.sourceOffset = 1024;
    e.fileIndex = 0;
    e.matchRule = QStringLiteral("文件名精确匹配");
    e.pit.partitionName = QStringLiteral("BOOT");
    e.pit.identifier = 80;
    plan.entries << e;
    plan.totalBytes = 32768;
    return plan;
}

void TestSamsungPlanDialog::startButtonGatedByCheckbox()
{
    SamsungPlanDialog dlg(oneEntryPlan());
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

void TestSamsungPlanDialog::rejectLeavesConfirmedFalse()
{
    SamsungPlanDialog dlg(oneEntryPlan());
    dlg.reject();
    QVERIFY(!dlg.confirmed());
}

void TestSamsungPlanDialog::previewsEntriesAndWarnings()
{
    odin::SamsungPlan plan = oneEntryPlan();
    plan.warnings << QStringLiteral("PIT 条目 BOOT2 声明的镜像 spl2.img 不在所选包内（跳过）")
                  << QStringLiteral("包内镜像 extra.bin 未出现在 PIT 中（跳过）");
    SamsungPlanDialog dlg(plan);
    auto *table = dlg.findChild<QTableView *>(QStringLiteral("planTable"));
    QVERIFY(table && table->model());
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->columnCount(), 5);
    QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("BOOT"));
    QCOMPARE(table->model()->index(0, 1).data().toString(), QStringLiteral("spl.img"));
    QCOMPARE(table->model()->index(0, 2).data().toString(), QStringLiteral("32 KiB"));
    QCOMPARE(table->model()->index(0, 3).data().toString(), QStringLiteral("CSC_ODD.tar.md5"));
    QCOMPARE(table->model()->index(0, 4).data().toString(), QStringLiteral("文件名精确匹配"));
    auto *warn = dlg.findChild<QListWidget *>(QStringLiteral("warningsList"));
    QVERIFY(warn);
    QCOMPARE(warn->count(), 2);              // 两类不匹配都在预览里（不静默）
    QVERIFY(warn->isVisibleTo(&dlg));
}

QTEST_MAIN(TestSamsungPlanDialog)
#include "test_samsung_plan_dialog.moc"
