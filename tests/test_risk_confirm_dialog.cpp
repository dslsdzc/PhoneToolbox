// tests/test_risk_confirm_dialog.cpp
//
// W2 Task 4：二次确认对话框（`RiskConfirmDialog`）与**唯一闸门** `riskui::ensureAccepted` 的离线可测面。
// 无真机依赖：纯 offscreen（CMakeLists 的 ENVIRONMENT 与 test_agreement_dialog 同款）。
//
// 为什么闸门函数的语义值钱：它是全部高危入口（刷写 / 解锁 / exploit）**唯一**的放行判据。
// 失效形态有三种，各自必须在**本文件**变红：
//   ① 恒放行（`return true` 放在第一行 / 忘了问 store）—— 用户从没确认过也进入写操作；
//   ② 取消被当成同意（关窗走 accept 分支 / 返回值写反）—— 用户点了「取消」动作照样执行；
//   ③ 确认没落盘（只弹窗不写 store）—— 每次动作都重新弹窗，留痕形同虚设（协议第七条对用户的承诺）。
//
// 断言强度的取舍（本目标易写成本恒真的几处在下面点明）：
//   * 对话框要点槽从 `tierWarnings()` **取期望值**再逐条比对（用例里不抄文案表）：写死一份文案的
//     实现在改文案后会红，而"照抄一份到用例里"两边一起改就自洽地绿了。
//   * "不再写 / 没写"一律用 **QFile 读裸字节前后对比**（不是再开一把 QSettings 去读 —— Qt 会把
//     前一个实例未落盘的待写值刷盘，那样连"没 sync()"都能骗过）。且每个"无写入"断言后面都跟一条
//     **判别力正控**：真写一次，字节必须变 —— 否则"字节没变"可能只是这把尺子量不出东西。
//   * 闸门槽一律**注入临时 ini 的 store**（绝不构造默认 store：那会写进本机真实用户配置）。
//     `store == nullptr`（生产默认分支）在本文件**刻意不覆盖**，见文件末「未覆盖」。
//   * 弹窗类槽用看门狗 + 驱动计时器（test_agreement_dialog.cpp 同款）：驱动 lambda 先记下"真摸到了
//     哪个模态窗、是不是本档的 RiskConfirmDialog"，再操作按钮 —— 于是"该弹窗却没弹"与"弹错了档"
//     都会红，而不是靠 exec() 不返回把整个用例挂死。
//
// **与任务书的一处偏离（主动记录，便于复核/回退）**：任务书 Step 1 写着「"接受"分支需真人点按钮，
// **不驱动**」。本文件**驱动了**它（ensureAcceptedRecordsTierAndReturnsTrueWhenUserAccepts：由
// 计时器代点确认框 + 「继续」）。理由：① "取消"分支本来就必须靠驱动才走得到（不驱动则只剩"没弹窗"
// 可断言），两者是同一机制；② 不驱动的话，"勾选并继续 → acceptTier 落盘 → 返回 true"这条**唯一**的
// 留痕路径完全没有覆盖，而任务书同时又要求"用 QSettings 直读键值前后对比"钉住写入 —— 不驱动就只剩
// 一条恒真的断言可写。驱动只作用于**注入的临时 store**（真实用户配置一个字节都不碰）。若控制方
// 坚持不驱动，删掉该槽即可，其余 8 槽不受影响。
#include <QtTest>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <functional>
#include <memory>

#include "legal/consent_store.h"     // legal::kAgreementVersion（正文版本三路比对的第三路）
#include "ui/risk_confirm_dialog.h"

class TestRiskConfirmDialog : public QObject
{
    Q_OBJECT
private slots:
    void dialogShowsTierTitleAndWarningsFromMappings();
    void continueButtonGatedByCheckbox();
    void agreementEntranceIsCollapsedAndShowsBundledBody();
    void continueAcceptsAndCancelOrCloseRejects();
    void missingOrEmptyAgreementBodyBlocksContinue();
    void ensureAcceptedNoneReturnsTrueWithoutDialogOrWrite();
    void ensureAcceptedSkipsAcceptedTierWithoutRewriting();
    void ensureAcceptedRecordsTierAndReturnsTrueWhenUserAccepts();
    void ensureAcceptedReturnsFalseAndWritesNothingWhenUserCancels();
};

namespace {

// 三把键名**逐字手写** = 对外契约（与 tests/test_consent_store.cpp 同一份字面）。
const QString kTier2Key = QStringLiteral("legal/tier2Version");
const QString kTier3Key = QStringLiteral("legal/tier3Version");

// 读**裸字节**（QFile，不是第二把 QSettings —— 见文件头）。
QString rawIni(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
}

// 临时 ini + 注入它的 store。QTemporaryDir 保证不碰真实用户配置，作用域退出即清。
struct StoreFixture {
    QTemporaryDir dir;
    QString       iniPath;
    std::unique_ptr<QSettings>     settings;
    std::unique_ptr<legal::ConsentStore> store;

    // seedKey 非空 → 先写下"上一次运行留下的留痕"（模拟已接受过 / 旧版本）。
    bool open(const QString &seedKey = QString(), int seedValue = 0)
    {
        if (!dir.isValid())
            return false;
        iniPath = dir.filePath(QStringLiteral("consent.ini"));
        if (!seedKey.isEmpty()) {
            QSettings seed(iniPath, QSettings::IniFormat);
            seed.setValue(seedKey, seedValue);
            seed.sync();
        }
        settings = std::make_unique<QSettings>(iniPath, QSettings::IniFormat);
        store    = std::make_unique<legal::ConsentStore>(settings.get());
        return true;
    }
};

struct DriveOutcome {
    bool    returned         = false;   // ensureAccepted 的返回值
    bool    watchdogFired    = false;   // 看门狗收的场（闸门没返回 → 断言失败，而不是 ctest 超时）
    int     driverFired      = 0;       // 驱动 lambda 真摸到模态窗的次数（0 = 没弹窗）
    QString modalTitle;                 // 第一次摸到的模态窗标题（诊断用）
    bool    modalIsRiskDlg   = false;   // 弹出来的是不是 RiskConfirmDialog（不是别的窗/没有窗）
    int     modalTier        = -1;      // 该对话框**自报**的档（-1 = 没摸到）
};

// 驱动一次 `ensureAccepted` 的模态循环：driver 在事件循环起来后对**当前模态窗**执行操作。
// 驱动计时器与看门狗都是栈对象：作用域退出即析构，连接随之断开，不会在别的用例的事件循环里复活。
DriveOutcome driveEnsure(QWidget *parent, riskui::RiskTier tier, legal::ConsentStore *store,
                         const std::function<void(QWidget *)> &driver)
{
    DriveOutcome out;

    QTimer driverTimer;
    driverTimer.setSingleShot(true);
    QObject::connect(&driverTimer, &QTimer::timeout, [driver, &out] {
        if (QWidget *w = QApplication::activeModalWidget()) {
            ++out.driverFired;
            out.modalTitle = w->windowTitle();
            // 弹出来的必须是**本档**的 RiskConfirmDialog：查的是对话框自报的 tier（比对标题字符串
            // 更强的判据 —— 弹了别的窗口 / 弹了别档的窗，这里都直接记下差异）。
            if (auto *rc = qobject_cast<riskui::RiskConfirmDialog *>(w)) {
                out.modalIsRiskDlg = true;
                out.modalTier      = int(rc->tier());
            }
            driver(w);
        }
    });
    driverTimer.start(0);

    QTimer watchdog;
    watchdog.setSingleShot(true);
    QObject::connect(&watchdog, &QTimer::timeout, [&out] {
        out.watchdogFired = true;
        if (auto *dlg = qobject_cast<QDialog *>(QApplication::activeModalWidget()))
            dlg->reject();
    });
    watchdog.start(5000);

    out.returned = riskui::ensureAccepted(parent, tier, store);
    watchdog.stop();
    return out;
}

// 驱动 lambda：勾选确认框并点「继续」（"用户接受"那条路径）。
void checkAndContinue(QWidget *w)
{
    if (auto *box = w->findChild<QCheckBox *>(QStringLiteral("riskCheck")))
        box->setChecked(true);
    if (auto *btn = w->findChild<QPushButton *>(QStringLiteral("continueButton")))
        btn->click();
}

// 驱动 lambda：点「取消」。
void cancel(QWidget *w)
{
    if (auto *btn = w->findChild<QPushButton *>(QStringLiteral("cancelButton")))
        btn->click();
}

} // namespace

void TestRiskConfirmDialog::dialogShowsTierTitleAndWarningsFromMappings()
{
    // 期望值**从分级映射取**（riskui::tierTitle / tierWarnings 是文案的唯一来源），不在用例里抄一份。
    const QVector<riskui::RiskTier> tiers = { riskui::RiskTier::Tier2Boot,
                                              riskui::RiskTier::Tier3Diagnostic };
    QVERIFY2(riskui::tierWarnings(tiers.at(0)) != riskui::tierWarnings(tiers.at(1)),
             "两档要点一模一样 —— 分档失去意义（多半是复制粘贴时没换文案表）");

    for (const riskui::RiskTier tier : tiers) {
        riskui::RiskConfirmDialog dlg(tier);
        QCOMPARE(dlg.tier(), tier);

        auto *title = dlg.findChild<QLabel *>(QStringLiteral("riskTitle"));
        QVERIFY(title);
        QCOMPARE(title->text(), riskui::tierTitle(tier));
        QVERIFY2(dlg.windowTitle().contains(riskui::tierTitle(tier)),
                 "窗口标题里没有档名 —— 用户在多任务/任务栏里认不出弹的是哪一档");

        const QStringList expected = riskui::tierWarnings(tier);
        QVERIFY2(!expected.isEmpty(), "该档要点为空 —— 对话框会展示一片空白");

        const QList<QLabel *> warningLabels = dlg.findChildren<QLabel *>(QStringLiteral("riskWarning"));
        QCOMPARE(warningLabels.size(), int(expected.size()));   // 逐条，不多不少
        QStringList shown;
        for (const QLabel *l : warningLabels)
            shown << l->text();
        QCOMPARE(shown, expected);                              // 逐条、同序
    }
}

void TestRiskConfirmDialog::continueButtonGatedByCheckbox()
{
    riskui::RiskConfirmDialog dlg(riskui::RiskTier::Tier2Boot);
    auto *check    = dlg.findChild<QCheckBox *>(QStringLiteral("riskCheck"));
    auto *cont     = dlg.findChild<QPushButton *>(QStringLiteral("continueButton"));
    QVERIFY(check && cont);
    QCOMPARE(check->text(), QStringLiteral("我已了解上述风险"));

    QVERIFY2(!check->isChecked(), "确认框初始应是未勾选（预勾选 = 用户没看就确认）");
    QVERIFY2(!cont->isEnabled(), "未勾选时「继续」必须禁用");

    check->setChecked(true);
    QVERIFY2(cont->isEnabled(), "勾选后「继续」必须可用");

    // 反向序：再取消勾选要回到禁用（只在首次 toggled 时 enable 的实现会在这里挂）。
    check->setChecked(false);
    QVERIFY2(!cont->isEnabled(), "取消勾选后「继续」必须回禁用");
}

void TestRiskConfirmDialog::agreementEntranceIsCollapsedAndShowsBundledBody()
{
    riskui::RiskConfirmDialog dlg(riskui::RiskTier::Tier2Boot);
    auto *toggle  = dlg.findChild<QToolButton *>(QStringLiteral("agreementToggle"));
    auto *browser = dlg.findChild<QTextBrowser *>(QStringLiteral("agreementBrowser"));
    QVERIFY(toggle && browser);
    QVERIFY2(browser->isReadOnly(), "协议正文控件不是只读的");
    QVERIFY2(browser->verticalScrollBarPolicy() != Qt::ScrollBarAlwaysOff,
             "协议正文不允许纵向滚动 —— 窗口放不下时用户看不到下半份正文");
    QVERIFY2(dlg.bodyLoaded(), "资源 :/legal/user-agreement.md 没读到 —— qrc 没编进本测试目标？");

    dlg.show();   // isVisible() 对未 show 的窗口恒 false，必须先 show 再断言"默认收起"
    QVERIFY2(!browser->isVisible(), "协议正文默认应收起（二次确认不该被一屏条款挤出风险要点）");

    toggle->setChecked(true);
    QVERIFY2(browser->isVisible(), "展开协议入口后正文应可见");

    // 展示的必须是**首屏那份**正文（同一份资源）：渲染后的形态单独列锚，与
    // test_agreement_dialog.cpp 的 shownAnchors 同源 —— 本对话框自己再抄一份别处的文本就会红。
    const QString shown = browser->toPlainText();
    QVERIFY(!shown.isEmpty());
    QVERIFY2(shown.contains(QStringLiteral("版本 %1").arg(legal::kAgreementVersion)),
             "协议入口展示的正文版本号与 legal::kAgreementVersion 不一致");
    QVERIFY2(shown.contains(QStringLiteral("四、以下行为即使你同意本须知也不被许可")),
             "协议入口展示的不是 :/legal/user-agreement.md（缺 §四 小节标题）");
    QVERIFY2(shown.contains(QStringLiteral("不联网、不上传")),
             "协议入口展示的不是 :/legal/user-agreement.md（缺 §七 本地记录承诺）");

    toggle->setChecked(false);
    QVERIFY2(!browser->isVisible(), "再点一次应收起协议正文");
}

void TestRiskConfirmDialog::continueAcceptsAndCancelOrCloseRejects()
{
    // 未勾选时点「继续」：什么也不该发生（用例打的是真实按钮控件的启用态与对话框结果）。
    {
        riskui::RiskConfirmDialog dlg(riskui::RiskTier::Tier2Boot);
        dlg.show();
        auto *cont = dlg.findChild<QPushButton *>(QStringLiteral("continueButton"));
        QVERIFY(cont);
        cont->click();
        QVERIFY2(dlg.result() != int(QDialog::Accepted), "未勾选时点「继续」竟被当成接受");
    }
    // 勾选后点「继续」→ Accepted（store 的写入由闸门槽覆盖，这里只钉对话框结果）。
    {
        riskui::RiskConfirmDialog dlg(riskui::RiskTier::Tier3Diagnostic);
        dlg.show();
        auto *check = dlg.findChild<QCheckBox *>(QStringLiteral("riskCheck"));
        auto *cont  = dlg.findChild<QPushButton *>(QStringLiteral("continueButton"));
        QVERIFY(check && cont);
        check->setChecked(true);
        cont->click();
        QCOMPARE(dlg.result(), int(QDialog::Accepted));
    }
    // 点「取消」→ Rejected。
    {
        riskui::RiskConfirmDialog dlg(riskui::RiskTier::Tier2Boot);
        dlg.show();
        auto *cancelBtn = dlg.findChild<QPushButton *>(QStringLiteral("cancelButton"));
        QVERIFY(cancelBtn);
        cancelBtn->click();
        QCOMPARE(dlg.result(), int(QDialog::Rejected));
    }
    // 关窗（标题栏 ×）= 拒绝 —— 绝不能当成同意（闸门据此不发起任何动作）。
    {
        riskui::RiskConfirmDialog dlg(riskui::RiskTier::Tier2Boot);
        dlg.show();
        QVERIFY(dlg.close());
        QCOMPARE(dlg.result(), int(QDialog::Rejected));
    }
}

void TestRiskConfirmDialog::missingOrEmptyAgreementBodyBlocksContinue()
{
    // 子例 1：路径不存在（资源没编进来 / 装包残缺）。
    {
        riskui::RiskConfirmDialog dlg(riskui::RiskTier::Tier2Boot, nullptr,
                                      QStringLiteral(":/legal/definitely-not-bundled.md"));
        QVERIFY(!dlg.bodyLoaded());
        auto *check = dlg.findChild<QCheckBox *>(QStringLiteral("riskCheck"));
        auto *cont  = dlg.findChild<QPushButton *>(QStringLiteral("continueButton"));
        auto *err   = dlg.findChild<QLabel *>(QStringLiteral("riskError"));
        QVERIFY(check && cont && err);
        QVERIFY2(!err->text().isEmpty(), "读不到正文时必须给出一行错误");
        check->setChecked(true);                 // 用户勾了也不行
        QVERIFY2(!cont->isEnabled(), "读不到正文时「继续」必须禁用");
        cont->click();
        QVERIFY(dlg.result() != int(QDialog::Accepted));
    }
    // 子例 2：文件在但是空的（占位/被截断）—— 同一 fail-closed 口径，避免"存在即放行"。
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString empty = tmp.filePath(QStringLiteral("user-agreement.md"));
        QFile f(empty);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        riskui::RiskConfirmDialog dlg(riskui::RiskTier::Tier2Boot, nullptr, empty);
        QVERIFY(!dlg.bodyLoaded());
        auto *check = dlg.findChild<QCheckBox *>(QStringLiteral("riskCheck"));
        auto *cont  = dlg.findChild<QPushButton *>(QStringLiteral("continueButton"));
        QVERIFY(check && cont);
        check->setChecked(true);
        QVERIFY2(!cont->isEnabled(), "正文为空时「继续」必须禁用（存在 ≠ 可用）");
    }
}

void TestRiskConfirmDialog::ensureAcceptedNoneReturnsTrueWithoutDialogOrWrite()
{
    StoreFixture fx;
    QVERIFY(fx.open(QStringLiteral("legal/agreementVersion"), legal::kAgreementVersion));
    const QString before = rawIni(fx.iniPath);
    QVERIFY2(!before.isEmpty(), "前置留痕没落盘 —— 后面的“字节没变”就失去判别力");

    const DriveOutcome out = driveEnsure(nullptr, riskui::RiskTier::None, fx.store.get(),
                                         [](QWidget *) { QFAIL("None 档不该弹窗"); });
    QVERIFY2(!out.watchdogFired, "看门狗收的场：ensureAccepted 没有返回");
    QVERIFY2(out.returned, "None 档必须直接放行（它无需确认）");
    QCOMPARE(out.driverFired, 0);                 // 不弹窗
    QCOMPARE(rawIni(fx.iniPath), before);         // 不写：连键都不该出现

    // 判别力正控：同一把尺子对"真写一次"必须量得出变化。
    fx.store->acceptTier(riskui::RiskTier::Tier2Boot);
    QVERIFY2(rawIni(fx.iniPath) != before, "写了一次却读不出变化 —— 上面的“没写”断言是恒真的");
}

void TestRiskConfirmDialog::ensureAcceptedSkipsAcceptedTierWithoutRewriting()
{
    // 留痕写成**更高的版本号**（判据是 >=，用户已接受更新版）：若闸门重写，值会被改回当前版本。
    StoreFixture fx;
    QVERIFY(fx.open(kTier2Key, legal::kAgreementVersion + 1));
    const QString before = rawIni(fx.iniPath);
    QVERIFY2(!before.isEmpty(), "前置留痕没落盘 —— 后面的“字节没变”就失去判别力");

    const DriveOutcome out = driveEnsure(nullptr, riskui::RiskTier::Tier2Boot, fx.store.get(),
                                         [](QWidget *) { QFAIL("已接受过的档不该再弹窗"); });
    QVERIFY2(!out.watchdogFired, "看门狗收的场：ensureAccepted 没有返回");
    QVERIFY2(out.returned, "已接受过该 tier 必须直接放行");
    QCOMPARE(out.driverFired, 0);                 // 不弹窗
    QCOMPARE(rawIni(fx.iniPath), before);         // 不再写（值仍是 +1，没被改写成当前版本）

    // 判别力：把留痕撤掉（模拟"没确认过"）→ **同一次调用**必须改判并弹窗（说明上面那次不弹窗
    // 是因为读了 store，而不是实现里恒不弹 / 恒放行）。
    {
        QSettings clear(fx.iniPath, QSettings::IniFormat);
        clear.remove(kTier2Key);
        clear.sync();
    }
    QVERIFY2(!rawIni(fx.iniPath).contains(kTier2Key), "撤留痕没落盘，下面的反向子例不成立");
    {
        QSettings settings2(fx.iniPath, QSettings::IniFormat);
        legal::ConsentStore store2(&settings2);
        const DriveOutcome out2 = driveEnsure(nullptr, riskui::RiskTier::Tier2Boot, &store2, cancel);
        QVERIFY2(!out2.watchdogFired, "看门狗收的场：闸门没有返回");
        QCOMPARE(out2.driverFired, 1);                       // 撤掉留痕后必须弹窗
        QVERIFY2(out2.modalIsRiskDlg, "弹出来的不是 RiskConfirmDialog");
        QCOMPARE(out2.modalTier, int(riskui::RiskTier::Tier2Boot));
        QVERIFY2(!out2.returned, "取消必须返回 false");
    }
}

void TestRiskConfirmDialog::ensureAcceptedRecordsTierAndReturnsTrueWhenUserAccepts()
{
    // 本槽**驱动**了"接受"分支（任务书原写"不驱动"）—— 取舍与理由见文件头的「与任务书的一处偏离」。
    StoreFixture fx;
    QVERIFY(fx.open());
    const QString before = rawIni(fx.iniPath);

    const DriveOutcome out = driveEnsure(nullptr, riskui::RiskTier::Tier3Diagnostic, fx.store.get(),
                                         checkAndContinue);
    QVERIFY2(!out.watchdogFired, "看门狗收的场：勾选后点「继续」没有让闸门的模态循环结束");
    QCOMPARE(out.driverFired, 1);
    QVERIFY2(out.modalIsRiskDlg, "弹出来的不是 RiskConfirmDialog");
    QCOMPARE(out.modalTier, int(riskui::RiskTier::Tier3Diagnostic));   // 弹的是请求的那一档
    QVERIFY2(out.returned, "用户勾选并继续 → 必须放行");

    // 留痕**逐字**落盘：键名 = 该档那把，值 = 当前协议版本（经"另一把 QSettings + 裸字节"两路读）。
    const QString after = rawIni(fx.iniPath);
    QVERIFY2(after != before, "接受之后 ini 字节没变 —— 用户确认没落盘（每次动作都要重弹）");
    QVERIFY2(after.contains(QStringLiteral("tier3Version=%1").arg(legal::kAgreementVersion)),
             qPrintable(QStringLiteral("裸字节里没有 tier3Version=当前版本：%1").arg(after)));
    QVERIFY2(!after.contains(QStringLiteral("tier2Version=")),
             "确认 Tier3 竟同时写下了 Tier2 的留痕 —— 两档必须是两次独立确认");

    QSettings reread(fx.iniPath, QSettings::IniFormat);
    QCOMPARE(reread.value(kTier3Key, -1).toInt(), legal::kAgreementVersion);
    QVERIFY(fx.store->hasAcceptedTier(riskui::RiskTier::Tier3Diagnostic));
    QVERIFY2(!fx.store->hasAcceptedTier(riskui::RiskTier::Tier2Boot), "Tier2 不该被顺带放行");
}

void TestRiskConfirmDialog::ensureAcceptedReturnsFalseAndWritesNothingWhenUserCancels()
{
    StoreFixture fx;
    QVERIFY(fx.open());
    const QString before = rawIni(fx.iniPath);

    const DriveOutcome out = driveEnsure(nullptr, riskui::RiskTier::Tier2Boot, fx.store.get(), cancel);
    QVERIFY2(!out.watchdogFired, "看门狗收的场：点「取消」没有让闸门的模态循环结束");
    QCOMPARE(out.driverFired, 1);                     // 未接受过 → 必须弹窗
    QVERIFY2(out.modalIsRiskDlg, "弹出来的不是 RiskConfirmDialog");
    QCOMPARE(out.modalTier, int(riskui::RiskTier::Tier2Boot));
    QVERIFY2(!out.returned, "取消必须静默返回 false（调用方据此不发起任何动作）");
    QCOMPARE(rawIni(fx.iniPath), before);             // 取消 = 不留痕
    QVERIFY2(!fx.store->hasAcceptedTier(riskui::RiskTier::Tier2Boot), "取消后 store 竟认了");

    // 判别力正控：同一把尺子对"真写一次"必须量得出变化。
    fx.store->acceptTier(riskui::RiskTier::Tier2Boot);
    QVERIFY2(rawIni(fx.iniPath) != before, "写了一次却读不出变化 —— 上面的“没写”断言是恒真的");
}

// ---------------------------------------------------------------------------
// 未覆盖（如实记录，别把本文件的绿灯读成"全部分支都验过"）：
//   * `ensureAccepted(..., store == nullptr)`（生产默认分支 = UserScope QSettings）：本文件一律注入
//     临时 ini。走默认分支意味着**可能读/写本机真实用户配置**（这台机器上若恰好已接受过该档，
//     行为还与"用例机干净"时不同 → 断言会随环境漂移），故**刻意不覆盖**。该分支的实现面只有
//     "构造默认 store 后走同一条 store 指针"，其余语义均由本文件在注入 store 上钉住；
//     接线（Task 5）落地后应在真机上人工过一遍"首次弹窗 → 勾选继续 → 再进不弹"。
//   * `RiskTier::None` **构造对话框**：`ensureAccepted` 对 None 直接短路，对话框不会以 None 被构造；
//     本文件因此不钉"None 的对话框长什么样"（它没有可展示的标题与要点）。
//   * `ensureAccepted` 与首屏 `agreementui::ask()` 的先后关系（启动期门控）：属 Task 5 接线，
//     不在本用例的目标内。
// ---------------------------------------------------------------------------

QTEST_MAIN(TestRiskConfirmDialog)
#include "test_risk_confirm_dialog.moc"
