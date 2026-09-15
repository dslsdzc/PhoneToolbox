// tests/test_samsung_plan_dialog.cpp
//
// 三星计划预览对话框：确认门控 + 预览内容（条目进表、两类不匹配进告警）。
// offscreen 跑（CMakeLists 的 ENVIRONMENT 与 test_flash_plan_dialog 同款），只断言控件状态。
#include <QtTest>
#include <QCheckBox>
#include <QListWidget>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>

#include "core/odin/samsung_plan.h"
#include "odin_test_helpers.h"
#include "ui/samsung_plan_dialog.h"

class TestSamsungPlanDialog : public QObject
{
    Q_OBJECT
private slots:
    void startButtonGatedByCheckbox();
    void rejectLeavesConfirmedFalse();
    void previewsEntriesAndWarnings();
    void buildAndShowStopsBeforeShowingDialog();
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

// 失败路径必须在**弹窗前**返回，且给出非空 *error —— 调用方（FlashPanel）按
// "false + error 空 = 用户取消 / 非空 = 失败"分派，失败一旦静默成"取消"，刷写就不开始且没有报错。
// 红线由"无人交互"承担：本用例 parent = nullptr、不驱动任何对话框 —— 一旦实现走到
// SamsungPlanDialog::exec()，模态循环再也出不来（ctest 超时），用例必红（与 Phase B 的
// buildAndShowFailsBeforeShowingDialog 同款判别力）。
// 四条失败来源：包打不开 / 一个包都没选 / 包内无 .pit / 显式 PIT 打不开。
// "err != 初值"比"非空"更强：证明失败结论是**本次调用写进去**的，不是赖调用方的老值。
void TestSamsungPlanDialog::buildAndShowStopsBeforeShowingDialog()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString sentinel = QStringLiteral("未回填的初值");
    QString err;

    // (a) 包路径不存在：包索引（indexTarStream）打不开文件即失败
    err = sentinel;
    QVERIFY(!SamsungPlanDialog::buildAndShow({QStringLiteral("/不存在/x.tar.md5")}, QString(),
                                             nullptr, &err));
    QVERIFY(err != sentinel);
    QVERIFY(!err.isEmpty());

    // (b) 一个包都没选（包内 PIT 查找必然落空）
    err = sentinel;
    QVERIFY(!SamsungPlanDialog::buildAndShow({}, QString(), nullptr, &err));
    QVERIFY(err != sentinel);
    QVERIFY(!err.isEmpty());

    // (c) 包在、可索引，但内部没有 .pit（合成夹具：只有 boot.img —— 与 test_samsung_plan 共用
    //     odin_test_helpers.h 的 writeTarMd5/tarEntry，不各写一份）
    const QString noPit = odintest::writeTarMd5(
        dir.path(), QStringLiteral("BL.tar.md5"),
        {odintest::tarEntry(QStringLiteral("boot.img"), QByteArray(3000, 'A'))});
    QVERIFY(!noPit.isEmpty());
    err = sentinel;
    QVERIFY(!SamsungPlanDialog::buildAndShow({noPit}, QString(), nullptr, &err));
    QVERIFY(err != sentinel);
    QVERIFY(!err.isEmpty());

    // (d) 显式 PIT 指向不存在的文件（显式优先于包内 → 先炸在 PIT 上）
    err = sentinel;
    QVERIFY(!SamsungPlanDialog::buildAndShow({noPit}, dir.filePath(QStringLiteral("nope.pit")),
                                             nullptr, &err));
    QVERIFY(err != sentinel);
    QVERIFY(!err.isEmpty());
}

QTEST_MAIN(TestSamsungPlanDialog)
#include "test_samsung_plan_dialog.moc"
