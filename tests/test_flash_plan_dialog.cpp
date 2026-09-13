// tests/test_flash_plan_dialog.cpp
//
// Phase B Task 8：刷写计划预览对话框的**确认门控**（真机未验证告知 → 未勾选不得开刷）。
// 无 DISPLAY 环境（CI/headless）下靠 QT_QPA_PLATFORM=offscreen 跑（见 CMakeLists 的
// set_tests_properties），因此这里只断言**控件状态与确认语义**，不做像素级渲染断言。
#include <QtTest>
#include <QCheckBox>
#include <QListWidget>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>

#include "core/edl/flash_plan.h"
#include "ui/flash_plan_dialog.h"

class TestFlashPlanDialog : public QObject
{
    Q_OBJECT
private slots:
    void startButtonGatedByCheckbox();
    void rejectPathLeavesConfirmedFalse();
    void previewsPlanEntriesAndWarnings();
    void buildAndShowFailsBeforeShowingDialog();
};

// 勾选框门控：未勾选 → startButton 禁用；勾选后可用；accept 之前 confirmed() 恒为 false。
// 末尾的 dlg.accept() 断言"确认路径置位"——对话框里唯一会走 accept 的控件就是 startButton
// （取消/关闭走 reject），故这条断言等价于"只有点『开始刷写』才置位"。
void TestFlashPlanDialog::startButtonGatedByCheckbox()
{
    edl::FlashPlan plan;
    plan.source = QStringLiteral("test");
    plan.storageType = QStringLiteral("ufs");
    FlashPlanDialog dlg(plan);

    auto *start = dlg.findChild<QPushButton *>(QStringLiteral("startButton"));
    auto *ack = dlg.findChild<QCheckBox *>(QStringLiteral("ackCheck"));
    QVERIFY(start && ack);
    QVERIFY(!ack->isChecked());        // 默认未勾选
    QVERIFY(!start->isEnabled());      // 未勾选 → 禁用
    ack->setChecked(true);
    QVERIFY(start->isEnabled());       // 勾选后可刷
    QVERIFY(!dlg.confirmed());
    dlg.accept();
    QVERIFY(dlg.confirmed());          // 仅"开始刷写"路径置位

    // 反向门控（防"恒 true"的假实现）：勾回去也必须立刻恢复禁用
    ack->setChecked(false);
    QVERIFY(!start->isEnabled());
}

// 取消路径（reject）不得置位 confirmed() —— buildAndShow 据此拒绝回填 outDir
// （否则用户点「取消」也会开刷）。
void TestFlashPlanDialog::rejectPathLeavesConfirmedFalse()
{
    edl::FlashPlan plan;
    FlashPlanDialog dlg(plan);
    dlg.reject();
    QVERIFY(!dlg.confirmed());
}

// 预览内容不是空壳：条目逐个进表（列数钉住 7：分区/LUN/起始扇区/扇区数/大小/文件/校验），
// 计划告警进告警列表 —— 防"界面出来了但什么都没显示"。
void TestFlashPlanDialog::previewsPlanEntriesAndWarnings()
{
    edl::PlanEntry program;
    program.action = edl::PlanEntry::Action::Program;
    program.partitionName = QStringLiteral("boot");
    program.imageFile = QStringLiteral("/tmp/none/boot.img");
    program.lun = 2;
    program.startSector = 100;
    program.numSectors = 2;
    program.sectorSize = 4096;
    program.rawBytes = 8192;

    edl::PlanEntry erase;
    erase.action = edl::PlanEntry::Action::Erase;
    erase.lun = 0;
    erase.sectorSize = 4096;              // startSector/numSectors 全 0 = 整 LUN 擦

    edl::FlashPlan plan;
    plan.source = QStringLiteral("unit-test");
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {erase, program};
    plan.totalBytes = 8192;
    plan.warnings << QStringLiteral("测试告警一条");

    FlashPlanDialog dlg(plan);
    auto *table = dlg.findChild<QTableView *>(QStringLiteral("planTable"));
    QVERIFY(table && table->model());
    QCOMPARE(table->model()->rowCount(), 2);
    QCOMPARE(table->model()->columnCount(), 7);
    QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("（擦除）整 LUN"));
    QCOMPARE(table->model()->index(0, 3).data().toString(), QStringLiteral("整 LUN"));
    QCOMPARE(table->model()->index(1, 0).data().toString(), QStringLiteral("boot"));
    QCOMPARE(table->model()->index(1, 1).data().toString(), QStringLiteral("2"));       // LUN
    QCOMPARE(table->model()->index(1, 4).data().toString(), QStringLiteral("8 KiB"));   // 大小
    QCOMPARE(table->model()->index(1, 5).data().toString(), QStringLiteral("boot.img"));// 文件

    auto *warn = dlg.findChild<QListWidget *>(QStringLiteral("warningsList"));
    QVERIFY(warn);
    QCOMPARE(warn->count(), 1);
    QVERIFY(warn->isVisibleTo(&dlg));      // 有告警 → 列表可见（对话框未 show，故用 isVisibleTo）
}

// 计划构建失败必须**在弹窗前**返回（否则模态 exec() 在无人点击的环境里挂死；本用例就是那条红线）
// 且错误文案非空、outDir 不被回填。
void TestFlashPlanDialog::buildAndShowFailsBeforeShowingDialog()
{
    QTemporaryDir empty;                   // 空目录：无 rawprogram*.xml / settings.xml → 必然失败
    QVERIFY(empty.isValid());
    QString outDir = QStringLiteral("未回填");
    QString error;
    QVERIFY(!FlashPlanDialog::buildAndShow(empty.path(), nullptr, &outDir, &error));
    QVERIFY(!error.isEmpty());             // 构建失败 ≠ 用户取消（取消时 error 为空）
    QCOMPARE(outDir, QStringLiteral("未回填"));
}

QTEST_MAIN(TestFlashPlanDialog)
#include "test_flash_plan_dialog.moc"
