// tests/test_flash_plan_dialog.cpp
//
// Phase B Task 8：刷写计划预览对话框的**确认门控**（真机未验证告知 → 未勾选不得开刷）。
// 无 DISPLAY 环境（CI/headless）下靠 QT_QPA_PLATFORM=offscreen 跑（见 CMakeLists 的
// set_tests_properties），因此这里只断言**控件状态与确认语义**，不做像素级渲染断言。
//
// PB-B6：整包入口的解包从 GUI 线程搬到 OppoExtractWorker（工作线程）+ 条目边界取消。
// 对话框本体（buildAndShowPackage）会弹模态进度条并进入 exec()，在无人点击的环境里
// **不能**直接驱动（旧用例 buildAndShowFailsBeforeShowingDialog 正是这条红线：失败必须在
// 弹窗前返回）。故这里直接驱动它背后的 OppoExtractWorker —— 对话框与用例走的是同一段
// 逻辑（同一个 worker + 同一个引擎取消钩子），覆盖边界见各用例注释与交接报告。
#include <QtTest>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QListWidget>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>

#include "core/edl/flash_plan.h"
#include "ui/flash_plan_dialog.h"
#include "ui/oppo_extract_worker.h"
#include "image_engine/oppo_keys.h"
#include "oppo_test_helpers.h"

class TestFlashPlanDialog : public QObject
{
    Q_OBJECT
private slots:
    void startButtonGatedByCheckbox();
    void rejectPathLeavesConfirmedFalse();
    void previewsPlanEntriesAndWarnings();
    void buildAndShowFailsBeforeShowingDialog();
    // PB-B6：解包线程化（进度来自工作线程 / 条目边界取消）
    void packageExtractRunsOnWorkerThread();
    void packageExtractCancelStopsAtFileBoundary();
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

// ==================== PB-B6：解包线程化 ====================

namespace {

// 合成 OPS 包（**两条目**版）：条目数据占扇区 0..N-1、settings.xml 密文紧随其后、
// 尾页 = 最后 0x200 字节。构造口径与 tests/test_image_worker.cpp 的 opsPackage() 同源
// （尾页字段偏移同 oppo_ops.cpp 常量：version/flags/0x7CEF/settings 扇区/明文长度），
// 只是把清单扩到多条 —— "取消在条目边界生效"至少需要两个条目才观察得到。
// 注: 与 image_worker 的夹具一样，settings.xml 密文由被测模块自己的密码 helper 产出
// （imgopp::opsEncrypt + opsKeyCandidates），密文本身不是本用例的断言对象。
bool writeOpsPackage(const QString &path, const QStringList &entryNames)
{
    const QByteArray mboxBlob = imgopp::opsKeyCandidates().at(0).mboxBlob;   // mbox5
    QString xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                 "<ProFile>\n  <BasicInfo Project=\"PB-B6\" Version=\"V1\"/>\n"
                                 "  <Program>\n");
    for (int i = 0; i < entryNames.size(); ++i)
        xml += QStringLiteral("    <program filename=\"%1\" FileOffsetInSrc=\"%2\""
                              " SizeInByteInSrc=\"512\"/>\n")
                   .arg(entryNames.at(i)).arg(i);
    xml += QStringLiteral("  </Program>\n</ProFile>\n");
    const QByteArray xmlBytes = xml.toUtf8();
    // 补齐式 (0x10 - len%0x10) 在已对齐时也补一整块（同 test_image_worker / test_oppo_ops）
    const QByteArray padded = xmlBytes + QByteArray(0x10 - (xmlBytes.size() % 0x10), '\0');
    const QByteArray cipher = imgopp::opsEncrypt(padded, mboxBlob);

    const qsizetype settingsSector = entryNames.size();   // 条目数据之后（0x200 对齐）
    QByteArray blob(0x1000, '\0');
    for (int i = 0; i < entryNames.size(); ++i)
        blob.replace(i * 512, 512, QByteArray(512, char('A' + i)));
    blob.replace(settingsSector * 512, cipher.size(), cipher);
    const qsizetype tailBase = blob.size() - 0x200;
    ofptest::putLE32(blob, tailBase + 0x00, 2);                              // version（A11 判据）
    ofptest::putLE32(blob, tailBase + 0x04, 1);                              // flags（A11 判据）
    ofptest::putLE32(blob, tailBase + 0x10, 0x7CEF);                         // 魔数
    ofptest::putLE32(blob, tailBase + 0x14, quint32(settingsSector));        // settings 扇区
    ofptest::putLE32(blob, tailBase + 0x18, quint32(xmlBytes.size()));       // 清单明文长度
    ofptest::putFixed(blob, tailBase + 0x1C, 16, QStringLiteral("PB-B6"));   // project id
    ofptest::putFixed(blob, tailBase + 0x2C, 32, QStringLiteral("V1"));      // firmware 名

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(blob) == blob.size();
}

// 等待 worker 结束（用与产品代码同款的"守卫循环"：queued 的 finished 可能早于 exec() 到达，
// 那时 quit() 是空操作 —— 裸 exec() 会一直等下去，故以 isFinished() 为准绳）。
// 超时仅约束"真挂死"（正常路径毫秒级）：60s 到点仍未结束 → 返回 false 由调用方 QVERIFY2。
bool awaitExtract(OppoExtractWorker &worker, int timeoutMs = 60000)
{
    QEventLoop loop;
    QObject::connect(&worker, &OppoExtractWorker::finished, &loop, &QEventLoop::quit);
    QElapsedTimer clock;
    clock.start();
    while (!worker.isFinished()) {
        if (clock.elapsed() > timeoutMs)
            return false;
        loop.exec();
    }
    worker.waitForFinished();
    return true;
}

} // namespace

// 线程化本身（PB-B6 的要点）：解包在**工作线程**执行、进度按文件发（不是"开始/结束"两档），
// 收尾必为 100，产物齐全。旧实现是 GUI 线程同步跑 + 手动 processEvents 泵 —— 该形态下
// `ranInWorker` 恒为 false（进度回调在 GUI 线程内直接调用），本用例即那道的判别力所在。
void TestFlashPlanDialog::packageExtractRunsOnWorkerThread()
{
    QTemporaryDir pkgDir, outDir;
    QVERIFY(pkgDir.isValid() && outDir.isValid());
    const QString pkg = pkgDir.filePath(QStringLiteral("fw.ops"));
    QVERIFY(writeOpsPackage(pkg, {QStringLiteral("a.img"), QStringLiteral("b.img")}));

    OppoExtractWorker worker;
    std::atomic<int> progressCount{0};
    std::atomic<int> lastPercent{0};
    std::atomic<bool> ranInWorker{false};
    // DirectConnection：本槽在**发射线程**执行（应为工作线程）—— 只碰原子量，不碰 GUI/Qt 控件。
    QObject::connect(&worker, &OppoExtractWorker::progress, &worker,
                     [&](const QString &, int percent) {
                         if (QThread::currentThread() != QCoreApplication::instance()->thread())
                             ranInWorker.store(true);
                         progressCount.fetch_add(1);
                         lastPercent.store(percent);
                     },
                     Qt::DirectConnection);

    worker.start(pkg, outDir.path());
    QVERIFY2(awaitExtract(worker), "解包未在 60s 内结束");

    QVERIFY(!worker.cancelRequested());
    QVERIFY2(worker.ok(), qPrintable(worker.error()));
    QVERIFY(worker.error().isEmpty());
    QCOMPARE(progressCount.load(), 2);        // 每完成一个文件一次
    QCOMPARE(lastPercent.load(), 100);        // 最后一个文件完成后必为 100
    QVERIFY(ranInWorker.load());              // 解包确实跑在 GUI 线程之外
    QVERIFY(QFile::exists(outDir.filePath(QStringLiteral("a.img"))));
    QVERIFY(QFile::exists(outDir.filePath(QStringLiteral("b.img"))));
}

// **取消在条目边界生效**（本轮唯一能停下来的点）：第一个文件完成时请求取消 —
// DirectConnection 让请求发生在工作线程内、且恰好在引擎"写完并校验完 a.img"之后，
// 于是"下一个条目不再开始"是确定性的，不依赖任何时序竞争（不靠 sleep、不靠 UI 点击）。
// 同时钉住取消后的三件事：ok=false 且 cancelRequested=true（调用方据此走"取消"而非"失败"）、
// 文案如实报"已完成 1/2 个文件"、产物目录**原样保留**（回收由调用方决定，worker 不越权删）。
void TestFlashPlanDialog::packageExtractCancelStopsAtFileBoundary()
{
    QTemporaryDir pkgDir, outDir;
    QVERIFY(pkgDir.isValid() && outDir.isValid());
    const QString pkg = pkgDir.filePath(QStringLiteral("fw.ops"));
    QVERIFY(writeOpsPackage(pkg, {QStringLiteral("a.img"), QStringLiteral("b.img")}));

    OppoExtractWorker worker;
    std::atomic<int> progressCount{0};
    QObject::connect(&worker, &OppoExtractWorker::progress, &worker,
                     [&](const QString &, int) {
                         progressCount.fetch_add(1);
                         worker.requestCancel();   // 原子标志；引擎在下一个条目边界读到
                     },
                     Qt::DirectConnection);

    worker.start(pkg, outDir.path());
    QVERIFY2(awaitExtract(worker), "解包未在 60s 内结束");

    QVERIFY(worker.cancelRequested());
    QVERIFY(!worker.ok());                                        // 取消 ≠ 成功
    QVERIFY2(worker.error().contains(QStringLiteral("用户取消")), qPrintable(worker.error()));
    QVERIFY2(worker.error().contains(QStringLiteral("已完成 1/2 个文件")), qPrintable(worker.error()));
    QCOMPARE(progressCount.load(), 1);                            // 取消后没有第二个文件的进度
    QVERIFY(QFile::exists(outDir.filePath(QStringLiteral("a.img"))));   // 当前文件跑完并校验完才停
    QVERIFY(!QFile::exists(outDir.filePath(QStringLiteral("b.img"))));  // 下一个条目不再开始
    QVERIFY(QDir(outDir.path()).exists());                        // 产物目录保留（调用方决定回收）
}

QTEST_MAIN(TestFlashPlanDialog)
#include "test_flash_plan_dialog.moc"
