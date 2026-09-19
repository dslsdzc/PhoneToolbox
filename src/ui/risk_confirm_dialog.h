// src/ui/risk_confirm_dialog.h
//
// 高风险动作的**二次确认**对话框 + 唯一闸门 `riskui::ensureAccepted` —— W2「用户协议与风险分级门控」。
//
// 为什么要有"唯一闸门"这个函数：门控的失效形态几乎都是**调用点各自为政** —— 甲处弹了窗、乙处直接
// 执行、丙处弹了窗但用户取消后照样往下走。把"弹窗 → 记住用户确认过 → 返回放行与否"收进一个函数后，
// 每个入口只需要一行 `if (!riskui::ensureAccepted(...)) return;`，用例也只需要钉住这一个函数的语义；
// 生产接线一律走全限定名 `riskui::ensureAccepted`（Task 5 的接线按名计数）。
//
// 与首屏 `agreementui::ask()` 的分工：首屏是**每个版本一次**的使用须知全量确认（未接受则整个程序不
// 进入），本对话框是**每档一次**的具体风险确认（拒绝只影响本次动作，不影响其它功能）。
//
// 为什么正文读不到时也 fail-closed（与首屏同款裁决）：对话框的契约是"给用户看了完整条款再收确认"。
// 读不到正文却仍能点「继续」，用户就会在**没看到条款**的情况下留下 Tier2/Tier3 的留痕 —— 留痕的
// 意义正好被抹掉。故「继续」的启用条件是两条都满足：正文读到了、且用户勾了确认框。
// （生产上正文由 qrc 编入，这条分支只在资源缺失/被截断时触发；用例用注入路径做负控制。）
//
// 控件用 objectName 暴露给用例（仓内既有对话框同款）：riskTitle / riskWarning / agreementToggle /
// agreementBrowser / riskError / riskCheck / continueButton / cancelButton。
#pragma once

#include <QDialog>
#include <QString>

#include "ui/risk_ui.h"          // RiskTier（分级映射的唯一来源）

class QCheckBox;
class QLabel;
class QPushButton;
class QTextBrowser;
class QToolButton;

namespace legal { class ConsentStore; }

namespace riskui {

// 协议正文的单一副本（与 agreementui::kAgreementResource **同一个字面** —— 那是首屏与本对话框共同
// 展示的同一份条款；两边各写一份字符串就是"改了一处漏一处"的温床，用例钉两处字面相等）。
inline constexpr const char *kAgreementResource = ":/legal/user-agreement.md";

class RiskConfirmDialog : public QDialog
{
    Q_OBJECT

public:
    // tier：本对话框要确认的档（Tier2Boot / Tier3Diagnostic）。None **不该走到这里** ——
    // `ensureAccepted` 对 None 直接短路返回，不构造对话框（构造它没有可展示的内容）。
    // agreementPath：协议正文来源（默认即 kAgreementResource；用例注入不存在的路径做负控制）。
    explicit RiskConfirmDialog(RiskTier tier, QWidget *parent = nullptr,
                               const QString &agreementPath = QString::fromLatin1(kAgreementResource));

    // 正文是否读入成功。false 时「继续」**恒禁用**（见文件头），用户只能取消。
    bool bodyLoaded() const { return m_bodyLoaded; }

    // 本对话框实际确认的档（用例据此钉"弹的是不是请求的那一档"）。
    RiskTier tier() const { return m_tier; }

private slots:
    // 勾选框变化 → 重算「继续」可用性（正文 + 勾选两条）。
    void refreshContinueEnabled();
    // 协议入口展开/收起（默认收起：二次确认不该被一屏条款挤出风险要点）。
    void toggleAgreement();

private:
    // 读资源 → 渲染进正文控件。失败（不存在/打不开/空）→ 一行错误 + 保持禁用，绝不放行。
    void loadBody();

    QLabel       *m_title    = nullptr;
    QTextBrowser *m_body     = nullptr;
    QLabel       *m_error    = nullptr;
    QToolButton  *m_toggle   = nullptr;
    QCheckBox    *m_check    = nullptr;
    QPushButton  *m_continue = nullptr;
    QPushButton  *m_cancel   = nullptr;

    RiskTier m_tier;
    QString  m_agreementPath;
    bool     m_bodyLoaded = false;
};

// **唯一闸门**（本任务的对外契约，Task 5 的接线全靠它）：
//   * tier == None            → true，且**不弹窗、不写任何键**（该档无需确认）；
//   * 已接受过该 tier          → true，且**不再写**（留痕是"第一次确认"的凭证，不是每次动作的计数）；
//   * 否则弹二次确认：勾选并继续 → `acceptTier(tier)` 落盘并 true；
//                     取消/关窗   → false（**静默**：调用方据此不发起任何动作）。
// store == nullptr → 用进程内默认 store（UserScope QSettings；要求调用方已设 QApplication 的
// organizationName/applicationName，否则会落到 `Unknown Organization` 名下）。用例一律注入临时
// store（临时 ini），绝不碰真实用户配置。
bool ensureAccepted(QWidget *parent, RiskTier tier, legal::ConsentStore *store = nullptr);

} // namespace riskui
