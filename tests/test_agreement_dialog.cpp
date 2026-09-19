// tests/test_agreement_dialog.cpp
//
// W2 Task 3：首屏「使用须知」对话框的离线可测面 —— 资源真的编进来了、正文与
// legal::kAgreementVersion 三路一致、勾选门控、以及"读不到正文就绝不同意"（fail-closed）。
// 无真机依赖：本用例是纯 offscreen 的（CMakeLists 的 ENVIRONMENT 与 test_eub_recovery_dialog 同款）。
//
// 断言强度的取舍（本目标易写成本恒真的几处在下面点明）：
//   * 资源槽打的是 `QFile(":/legal/user-agreement.md")` —— 它只有把 `${QRC_FILES}` 编进**本目标**
//     才可能绿。这不是装饰：不编进去时本槽红（RED 证据见 .superpowers/sdd/w2-task-3-logs/）。
//   * 版本槽不是"文本里有「版本 1」"就算数：从正文**首行**解析出的数字要与 legal::kAgreementVersion
//     相等（`toInt` 的 ok 单独查 —— 解析失败会静默给 0），且对话框**实际展示**的文本里也要有同一字面
//     （否则"资源对了但没接到控件上"也能过）。
//   * 门控槽查的是 findChild 拿到的**真实按钮控件**的 isEnabled()（不是另算一份门控表达式 —— 否则
//     "忘了刷新按钮"的实现照样绿），并跑"勾选 → 取消勾选"的反向序（只在首次 toggled 时 enable 的实现
//     会在这里挂）。
//   * fail-closed 槽用**两个注入子例**（不存在的路径 / 存在的空文件）：只查 bodyLoaded() 而不同时
//     查按钮 enabled 的断言过不了 —— 而"读不到也能同意"正是本任务点名要挡的形态。
#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <functional>
#include <memory>

#include "legal/consent_store.h"     // legal::kAgreementVersion（三路比对的第三路）
#include "ui/agreement_dialog.h"

class TestAgreementDialog : public QObject
{
    Q_OBJECT
private slots:
    void resourceIsBundledAndNonEmpty();
    void versionInBodyMatchesImplementationConstant();
    void bodyPinsKeySentences();
    void bodyViewIsReadOnlyWithMinimumHeight();
    void agreeButtonGatedByCheckbox();
    void missingOrEmptyBodyBlocksAgreement();
    void exitButtonAndCloseBothReject();
    void askReturnsFalseWhenUserExits();
    void askReturnsTrueAfterCheckingAndAgreeing();
};

namespace {

// 工厂而不是直接构造：`AgreementDialog dlg(QString::fromLatin1(x));` 会被解析成函数声明
//（most-vexing-parse，本用例第一版就栽在这条上），且类在 agreementui 命名空间里。
std::unique_ptr<agreementui::AgreementDialog>
makeDlg(const QString &path = QString::fromLatin1(agreementui::kAgreementResource))
{
    return std::make_unique<agreementui::AgreementDialog>(path);
}

// ask() 的模态循环里驱动一次点击。driver 在事件循环起来后执行；watchdog 防"门控坏掉 → exec() 不
// 返回"把整个用例挂死（看门狗收场时置 *watchdogFired，调用方据此断言失败而不是等 ctest 超时）。
// 两个 QTimer 都是栈对象：作用域退出即析构，连接随之断开，lambda 不会在别的用例的事件循环里复活。
bool driveAsk(const std::function<void(QWidget *)> &driver, bool *watchdogFired)
{
    *watchdogFired = false;

    QTimer driverTimer;
    driverTimer.setSingleShot(true);
    QObject::connect(&driverTimer, &QTimer::timeout, [driver] {
        if (QWidget *w = QApplication::activeModalWidget())
            driver(w);
    });
    driverTimer.start(0);

    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [watchdogFired] {
        *watchdogFired = true;
        if (auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            dlg->reject();
    });
    watchdog.start(5000);

    const bool accepted = agreementui::ask(nullptr);
    watchdog.stop();
    return accepted;
}

// 从正文**首行**解析「版本 N」。返回 false = 首行不是那个标题、或数字没解析出来
//（toInt 的 ok 单独返回 —— 解析失败会静默给 0，与"版本 0"不可分辨）。
bool versionFromFirstLine(const QString &text, int *version)
{
    const QString firstLine = text.section(QLatin1Char('\n'), 0, 0);
    if (!firstLine.startsWith(QStringLiteral("# PhoneToolbox 使用须知与免责声明")))
        return false;
    const QRegularExpression re(QStringLiteral("版本\\s*([0-9]+)"));
    const QRegularExpressionMatch m = re.match(firstLine);
    if (!m.hasMatch())
        return false;
    bool ok = false;
    *version = m.captured(1).toInt(&ok);
    return ok;
}

QString bundledBodyText()
{
    QFile f(QString::fromLatin1(agreementui::kAgreementResource));
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

} // namespace

void TestAgreementDialog::resourceIsBundledAndNonEmpty()
{
    auto dlg = makeDlg();
    QVERIFY2(dlg->bodyLoaded(), "资源 :/legal/user-agreement.md 没读到 —— qrc 没编进本测试目标？");

    QFile f(QString::fromLatin1(agreementui::kAgreementResource));
    QVERIFY2(f.exists(), ":/legal/user-agreement.md 不存在（resources.qrc 的 /legal 前缀/alias 错了？）");
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    QVERIFY2(raw.size() > 500, "正文短得不像协议全文（占位文本？）");
    QVERIFY(!QString::fromUtf8(raw).trimmed().isEmpty());
}

void TestAgreementDialog::versionInBodyMatchesImplementationConstant()
{
    const QString raw = bundledBodyText();
    QVERIFY(!raw.isEmpty());

    int version = -1;
    QVERIFY2(versionFromFirstLine(raw, &version),
             "正文首行不是「# PhoneToolbox 使用须知与免责声明（版本 N）」—— 正文被改写过？");
    QCOMPARE(version, legal::kAgreementVersion);   // 三路一致钉：资源 ↔ 常量

    // 第三路：对话框**实际展示**的文本里也要有同一个字面（资源对了但没接到控件上也过不了）。
    auto dlg = makeDlg();
    QVERIFY(dlg->bodyText().contains(QStringLiteral("版本 %1").arg(legal::kAgreementVersion)));
}

void TestAgreementDialog::bodyPinsKeySentences()
{
    // 三条关键句各钉一条条款：不内置厂商二进制（§一）、对他人设备违法（§二）、GPLv3 权利不受限（§六）。
    // 少任何一条都说明正文被替换/截断过。
    const QStringList keys = { QStringLiteral("不包含"), QStringLiteral("违法"), QStringLiteral("GPLv3") };
    const QString raw = bundledBodyText();
    QVERIFY(!raw.isEmpty());

    auto dlg = makeDlg();
    QVERIFY(dlg->bodyLoaded());
    const QString shown = dlg->bodyText();   // 屏幕上那份（markdown 渲染后的纯文本）
    QVERIFY(!shown.isEmpty());
    for (const QString &k : keys) {
        QVERIFY2(raw.contains(k), qPrintable(QStringLiteral("资源正文缺关键句：%1").arg(k)));
        QVERIFY2(shown.contains(k), qPrintable(QStringLiteral("对话框展示的正文缺关键句：%1").arg(k)));
    }
}

void TestAgreementDialog::bodyViewIsReadOnlyWithMinimumHeight()
{
    auto dlg = makeDlg();
    auto *body = dlg->findChild<QTextBrowser *>(QStringLiteral("agreementBody"));
    QVERIFY(body);
    QVERIFY2(body->isReadOnly(), "正文控件不是只读的");
    QVERIFY2(body->minimumHeight() >= 320, "正文区最小高度没到 320px（任务书的排版约束）");
    QVERIFY2(body->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOff,
             "正文区不允许纵向滚动 —— 窗口放不下时用户看不到下半份正文");
}

void TestAgreementDialog::agreeButtonGatedByCheckbox()
{
    auto dlg = makeDlg();
    auto *check = dlg->findChild<QCheckBox *>(QStringLiteral("agreeCheck"));
    auto *agree = dlg->findChild<QPushButton *>(QStringLiteral("agreeButton"));
    QVERIFY(check && agree);
    QCOMPARE(check->text(), QStringLiteral("我已阅读并同意上述使用须知"));

    QVERIFY2(!check->isChecked(), "确认框初始应是未勾选（预勾选 = 用户没读就同意）");
    QVERIFY2(!agree->isEnabled(), "未勾选时「同意并继续」必须禁用");

    check->setChecked(true);
    QVERIFY2(agree->isEnabled(), "勾选后「同意并继续」必须可用");

    // 反向序：再取消勾选要回到禁用（只在第一次 toggled 时 enable 的实现会在这里挂）。
    check->setChecked(false);
    QVERIFY2(!agree->isEnabled(), "取消勾选后「同意并继续」必须回禁用");
}

void TestAgreementDialog::missingOrEmptyBodyBlocksAgreement()
{
    // 子例 1：路径不存在（资源没编进来 / 装包残缺）。
    {
        auto dlg = makeDlg(QStringLiteral(":/legal/definitely-not-bundled.md"));
        QVERIFY(!dlg->bodyLoaded());
        auto *check = dlg->findChild<QCheckBox *>(QStringLiteral("agreeCheck"));
        auto *agree = dlg->findChild<QPushButton *>(QStringLiteral("agreeButton"));
        auto *err = dlg->findChild<QLabel *>(QStringLiteral("agreementError"));
        QVERIFY(check && agree && err);
        QVERIFY2(!err->text().isEmpty(), "读不到正文时必须给出一行错误");
        check->setChecked(true);                 // 用户勾了也不行
        QVERIFY2(!agree->isEnabled(), "读不到正文时「同意并继续」必须禁用");
        agree->click();
        QVERIFY(dlg->result() != QDialog::Accepted);
    }

    // 子例 2：文件在但是空的（占位/被截断）—— 同一 fail-closed 口径，避免"存在即放行"。
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString empty = tmp.filePath(QStringLiteral("user-agreement.md"));
        QFile f(empty);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        auto dlg = makeDlg(empty);
        QVERIFY(!dlg->bodyLoaded());
        auto *check = dlg->findChild<QCheckBox *>(QStringLiteral("agreeCheck"));
        auto *agree = dlg->findChild<QPushButton *>(QStringLiteral("agreeButton"));
        QVERIFY(check && agree);
        check->setChecked(true);
        QVERIFY2(!agree->isEnabled(), "正文为空时「同意并继续」必须禁用（存在 ≠ 可用）");
    }
}

void TestAgreementDialog::exitButtonAndCloseBothReject()
{
    auto dlg = makeDlg();
    auto *exit = dlg->findChild<QPushButton *>(QStringLiteral("exitButton"));
    QVERIFY(exit);
    QCOMPARE(exit->text(), QStringLiteral("退出"));

    dlg->show();
    exit->click();
    QCOMPARE(dlg->result(), int(QDialog::Rejected));
    QVERIFY2(!dlg->isVisible(), "点「退出」后对话框应关闭");

    // 关窗（标题栏 ×）= 拒绝：调用方据此退出程序，绝不能当成同意。
    auto dlg2 = makeDlg();
    dlg2->show();
    QVERIFY(dlg2->close());
    QCOMPARE(dlg2->result(), int(QDialog::Rejected));
}

void TestAgreementDialog::askReturnsFalseWhenUserExits()
{
    bool watchdogFired = false;
    const bool accepted = driveAsk([](QWidget *w) {
        if (auto *btn = w->findChild<QPushButton *>(QStringLiteral("exitButton")))
            btn->click();
    }, &watchdogFired);
    QVERIFY2(!watchdogFired, "看门狗收的场：点「退出」没有让 ask() 的模态循环结束");
    QVERIFY(!accepted);
}

void TestAgreementDialog::askReturnsTrueAfterCheckingAndAgreeing()
{
    bool watchdogFired = false;
    const bool accepted = driveAsk([](QWidget *w) {
        if (auto *box = w->findChild<QCheckBox *>(QStringLiteral("agreeCheck")))
            box->setChecked(true);
        if (auto *btn = w->findChild<QPushButton *>(QStringLiteral("agreeButton")))
            btn->click();
    }, &watchdogFired);
    QVERIFY2(!watchdogFired, "看门狗收的场：勾选后点「同意并继续」没有让 ask() 的模态循环结束");
    QVERIFY(accepted);
}

QTEST_MAIN(TestAgreementDialog)
#include "test_agreement_dialog.moc"
