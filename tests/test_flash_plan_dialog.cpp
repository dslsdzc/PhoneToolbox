// tests/test_flash_plan_dialog.cpp
//
// Phase B Task 8：刷写计划预览对话框的**确认门控**（真机未验证告知 → 未勾选不得开刷）。
// 无 DISPLAY 环境（CI/headless）下靠 QT_QPA_PLATFORM=offscreen 跑（见 CMakeLists 的
// set_tests_properties），因此这里只断言**控件状态与确认语义**，不做像素级渲染断言。
//
// PB-B6：整包入口的解包从 GUI 线程搬到 OppoExtractWorker（工作线程）+ 条目边界取消。
// 对话框本体（buildAndShowPackage）会弹模态进度条并进入事件循环 —— 只要**有人在事件循环里
// 与它交互**就能驱动（本文件用 1ms 定时器在进度条可见时点一次取消按钮 / 发一次 Esc），
// 故取消路径已入库用例；仍然**不能**驱动的是"失败必须弹窗前返回"那条红线
// （buildAndShowFailsBeforeShowingDialog：无人交互 ⇒ 一旦弹窗就是挂死）。
#include <QtTest>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QListWidget>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QTableView>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <functional>

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
    // PB-B6 审查修复：对话框本体的取消路径（点按钮 / 按 Esc）+ 成功路径不被误判为取消
    void dialogCancelButtonStopsAtFileBoundary();
    void dialogEscapeAlsoRequestsCancel();
    void dialogSuccessNotMisreadAsCancel();
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

// 冒烟/入库用例共用：第一条目 bigFirstBytes 字节（解包耗时可见）、第二条 512B。
// 与 writeOpsPackage 同一构造口径，只是把首条目撑大 —— "取消在条目边界生效"的对话框用例
// 需要"交互发生时第一条目还在搬运"这个时间窗（定时器 1ms 一拍，64 MiB 的搬运远长于此）。
bool writeOpsPackageBigFirst(const QString &path, qsizetype firstBytes)
{
    const QByteArray mboxBlob = imgopp::opsKeyCandidates().at(0).mboxBlob;
    QString xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                 "<ProFile>\n  <BasicInfo Project=\"PB-B6\" Version=\"V1\"/>\n"
                                 "  <Program>\n");
    xml += QStringLiteral("    <program filename=\"a.img\" FileOffsetInSrc=\"0\""
                          " SizeInByteInSrc=\"%1\"/>\n").arg(firstBytes);
    xml += QStringLiteral("    <program filename=\"b.img\" FileOffsetInSrc=\"%1\""
                          " SizeInByteInSrc=\"512\"/>\n").arg(firstBytes / 512);
    xml += QStringLiteral("  </Program>\n</ProFile>\n");
    const QByteArray xmlBytes = xml.toUtf8();
    const QByteArray padded = xmlBytes + QByteArray(0x10 - (xmlBytes.size() % 0x10), '\0');
    const QByteArray cipher = imgopp::opsEncrypt(padded, mboxBlob);

    const qsizetype settingsOff = firstBytes;          // 条目数据之后（0x200 对齐）
    QByteArray blob(settingsOff + 0x800, '\0');
    blob.replace(0, firstBytes, QByteArray(firstBytes, 'A'));
    blob.replace(settingsOff, cipher.size(), cipher);
    blob.replace(firstBytes + 512, 512, QByteArray(512, 'B'));
    const qsizetype tailBase = blob.size() - 0x200;
    ofptest::putLE32(blob, tailBase + 0x00, 2);
    ofptest::putLE32(blob, tailBase + 0x04, 1);
    ofptest::putLE32(blob, tailBase + 0x10, 0x7CEF);
    ofptest::putLE32(blob, tailBase + 0x14, quint32(settingsOff / 512));
    ofptest::putLE32(blob, tailBase + 0x18, quint32(xmlBytes.size()));
    ofptest::putFixed(blob, tailBase + 0x1C, 16, QStringLiteral("PB-B6"));
    ofptest::putFixed(blob, tailBase + 0x2C, 32, QStringLiteral("V1"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(blob) == blob.size();
}

// 驱动**真实对话框入口**（buildAndShowPackage）并与进度对话框交互一次的结果快照。
struct DialogRunResult
{
    bool ok = false;
    QString error;
    QString cancelNote;
    QStringList tempFiles;               // 对话框返回后临时目录里的文件（回收与否由调用方决定）
    int ticksAfterInteract = 0;          // 交互之后的定时器拍数（"取消后还在等"确有窗口）
    bool visibleAfterInteract = false;   // 其中至少一拍进度对话框仍可见 ⇒ 模态防线还在
};

// 定时器手法（原为一次性冒烟，经审查建议入库）：1ms 一拍，进度对话框一旦可见就调用
// interact()（点取消按钮 / 发 Esc），之后每拍记录它是否仍在屏幕上 —— 取消请求只在**条目
// 边界**生效，"当前文件跑完"之前对话框必须一直可见（Qt 的取消/Esc 都会自己 hide 它，
// 产品代码再把 show() 拉回来；若那段逻辑被删，本例可见性断言立刻变红）。
DialogRunResult runDialog(QTemporaryDir &tmp, const QString &pkg,
                          const std::function<void(QProgressDialog *)> &interact)
{
    DialogRunResult r;
    QWidget parent;                      // 进度对话框的 parent：findChild 才能可靠定位它
    QPointer<QProgressDialog> bar;
    QTimer poll;
    poll.setInterval(1);
    bool interacted = false;
    QObject::connect(&poll, &QTimer::timeout, &poll, [&] {
        if (!bar)
            bar = parent.findChild<QProgressDialog *>();
        if (!bar || !bar->isVisible())
            return;
        if (!interacted) {
            interacted = true;
            if (interact)                // 传空 = 只观察、不交互（成功路径用例）
                interact(bar);
            return;
        }
        ++r.ticksAfterInteract;
        if (bar->isVisible())
            r.visibleAfterInteract = true;
    });
    poll.start();
    QString outDir;
    r.ok = FlashPlanDialog::buildAndShowPackage(pkg, &parent, &tmp, &outDir, &r.error, &r.cancelNote);
    poll.stop();
    r.tempFiles = QDir(tmp.path()).entryList(QDir::Files);
    return r;
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

// ==================== PB-B6 审查修复：对话框本体的取消/成功路径 ====================

// **点取消按钮**驱动真实对话框入口：断言取消文案（停在条目边界）+ 临时目录内容与文案一致
// + 交互之后进度对话框一直可见（模态防线没被 Qt 的 hide 吃掉）。
// 判别力：把 handleCancel 的 show() 去掉 → visibleAfterInteract 变 false；把 canceled()
// 的连接去掉 → 走不到取消路径（error 变成计划构建失败）。两种都试过（见报告）。
void TestFlashPlanDialog::dialogCancelButtonStopsAtFileBoundary()
{
    QTemporaryDir tmp, pkgDir;
    QVERIFY(tmp.isValid() && pkgDir.isValid());
    const QString pkg = pkgDir.filePath(QStringLiteral("big.ops"));
    QVERIFY(writeOpsPackageBigFirst(pkg, 64 * 1024 * 1024));

    const DialogRunResult r = runDialog(tmp, pkg, [](QProgressDialog *bar) {
        auto *btn = bar->findChild<QPushButton *>();
        QVERIFY(btn);                       // 取消入口必须存在，否则本用例无意义
        btn->click();
    });

    QVERIFY(!r.ok);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));      // *error 空 = 用户取消
    QVERIFY2(r.cancelNote.contains(QStringLiteral("用户取消")), qPrintable(r.cancelNote));
    // 取消 = 停在条目边界：完成数只能是 0 或 1（解包没跑完），且盘上文件必须与文案一致
    const bool oneDone = r.cancelNote.contains(QStringLiteral("已完成 1/2"));
    QVERIFY2(oneDone || r.cancelNote.contains(QStringLiteral("已完成 0/2")), qPrintable(r.cancelNote));
    QCOMPARE(r.tempFiles.contains(QStringLiteral("a.img")), oneDone);
    QVERIFY(!r.tempFiles.contains(QStringLiteral("b.img")));   // 下一个条目不再开始
    // 取消之后、worker 结束之前：进度对话框仍在屏幕上（否则 WindowModal 失效 → 主窗口可交互）
    QVERIFY(r.ticksAfterInteract > 0);
    QVERIFY(r.visibleAfterInteract);
}

// **按 Esc**（不点按钮）：实测 Qt 6.11 下 Esc → QDialog::reject() 只发 rejected()、不发
// canceled()，且顺手 hide 对话框 —— 只接 canceled() 的实现会让 Esc 静默绕过取消请求并
// 丢掉模态（本轮审查抓到的口子）。本用例即那条口子的回归钉子：Esc 必须与点按钮**同效**。
void TestFlashPlanDialog::dialogEscapeAlsoRequestsCancel()
{
    QTemporaryDir tmp, pkgDir;
    QVERIFY(tmp.isValid() && pkgDir.isValid());
    const QString pkg = pkgDir.filePath(QStringLiteral("big.ops"));
    QVERIFY(writeOpsPackageBigFirst(pkg, 64 * 1024 * 1024));

    const DialogRunResult r = runDialog(tmp, pkg, [](QProgressDialog *bar) {
        // **必须直接投递 QKeyEvent，不能用 QTest::keyClick**：QtTest 的 keyClick 走的是
        // 窗口系统注入路径，在 offscreen 下端到端的效果是"关窗"（→ closeEvent → **canceled()**），
        // 于是即使产品没接 rejected() 用例也会绿 —— 那样就测不到本口子。真实桌面上的 Esc 是
        // 投递给焦点控件的按键事件 → QDialog::keyPressEvent → reject() → 只发 rejected()
        // （实测；见 qpd 探针），故这里按同样路由注入（sendEvent 与真实按键同一条 keyPressEvent）。
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(bar, &esc);
    });

    QVERIFY(!r.ok);
    QVERIFY2(r.error.isEmpty(), qPrintable(r.error));      // 取消 ≠ 失败（空 error 是取消口径）
    QVERIFY2(r.cancelNote.contains(QStringLiteral("用户取消")), qPrintable(r.cancelNote));
    const bool oneDone = r.cancelNote.contains(QStringLiteral("已完成 1/2"));
    QVERIFY2(oneDone || r.cancelNote.contains(QStringLiteral("已完成 0/2")), qPrintable(r.cancelNote));
    QCOMPARE(r.tempFiles.contains(QStringLiteral("a.img")), oneDone);
    QVERIFY(!r.tempFiles.contains(QStringLiteral("b.img")));
    QVERIFY(r.ticksAfterInteract > 0);
    QVERIFY(r.visibleAfterInteract);       // Esc 之后对话框必须仍可见（模态保持）
}

// **成功路径不得被误判成取消**（本轮自查发现的真 bug）：`QProgressDialog::closeEvent` 会发
// `canceled()`，故本函数末尾的 `progress.close()` 会把 cancelRequested 置位 —— 若不把
// "收尾阶段"与"用户取消"分开，每次**成功**解包都会返回"已取消（产物未使用）"：临时目录被
// 回收、刷写永不开始，且没有任何报错（静默丢结果）。
// 本用例不交互：让解包跑完 → 断言走的是**失败/成功**路径（本夹具的产物没有
// rawprogram/settings.xml，所以是"解包成功、构建计划失败"= error 非空、无取消文案）。
// 判别力：去掉 settled 冻结 → cancelNote 变成"取消请求到达时解包已跑完（产物未使用）"、
// error 为空，本用例立刻变红。
void TestFlashPlanDialog::dialogSuccessNotMisreadAsCancel()
{
    QTemporaryDir tmp, pkgDir;
    QVERIFY(tmp.isValid() && pkgDir.isValid());
    const QString pkg = pkgDir.filePath(QStringLiteral("fw.ops"));
    QVERIFY(writeOpsPackage(pkg, {QStringLiteral("a.img"), QStringLiteral("b.img")}));

    const DialogRunResult r = runDialog(tmp, pkg, {});     // 不交互：等它自己跑完

    QVERIFY(!r.ok);
    QVERIFY2(!r.error.isEmpty(), "解包成功后的计划构建失败必须给出 *error —— 空即被误判成取消");
    QVERIFY(r.cancelNote.isEmpty());                       // 没有任何"取消"成分
    QVERIFY2(r.tempFiles.contains(QStringLiteral("a.img"))
                 && r.tempFiles.contains(QStringLiteral("b.img")),
             qPrintable(r.tempFiles.join(QStringLiteral(","))));
}

QTEST_MAIN(TestFlashPlanDialog)
#include "test_flash_plan_dialog.moc"
