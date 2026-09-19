// src/ui/risk_confirm_dialog.cpp —— 二次确认对话框与唯一闸门（见 .h 文件头的分层与 fail-closed 取舍）
#include "ui/risk_confirm_dialog.h"

#include <QCheckBox>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>
#include <memory>

#include "legal/consent_store.h"

namespace riskui {

RiskConfirmDialog::RiskConfirmDialog(RiskTier tier, QWidget *parent, const QString &agreementPath)
    : QDialog(parent), m_tier(tier), m_agreementPath(agreementPath)
{
    // 标题里带上档名：任务栏/多窗口下用户要能一眼认出弹的是哪一档（用例钉住它含 tierTitle）。
    setWindowTitle(QStringLiteral("风险确认 —— %1").arg(tierTitle(tier)));
    setModal(true);

    m_title = new QLabel(tierTitle(tier), this);
    m_title->setObjectName(QStringLiteral("riskTitle"));
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);

    auto *root = new QVBoxLayout(this);
    root->addWidget(m_title);

    // 风险要点**逐条**一个 QLabel（同名 objectName），文本逐字来自 tierWarnings()：
    // 不在 UI 层改写/截断文案 —— 改文案只改分级映射一处（那是对用户的唯一承诺面）。
    const QStringList warnings = tierWarnings(tier);
    for (const QString &w : warnings) {
        auto *item = new QLabel(w, this);
        item->setObjectName(QStringLiteral("riskWarning"));
        item->setWordWrap(true);
        root->addWidget(item);
    }

    // 协议入口：默认收起（二次确认的首要内容是上面那几条**本次动作**的风险，不该被一屏条款挤掉）。
    m_toggle = new QToolButton(this);
    m_toggle->setObjectName(QStringLiteral("agreementToggle"));
    m_toggle->setText(QStringLiteral("查看完整使用须知"));
    m_toggle->setCheckable(true);
    m_toggle->setChecked(false);

    m_body = new QTextBrowser(this);
    m_body->setObjectName(QStringLiteral("agreementBrowser"));
    m_body->setReadOnly(true);          // 正文是给用户看的条款，不是可编辑控件
    m_body->setOpenExternalLinks(false);
    m_body->setMinimumHeight(200);      // 展开后保住可读面积，随字体/DPI 自适应
    m_body->setVisible(false);          // 收起态：父窗口 show() 之后仍是隐藏的

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("riskError"));
    m_error->setWordWrap(true);

    m_check = new QCheckBox(QStringLiteral("我已了解上述风险"), this);
    m_check->setObjectName(QStringLiteral("riskCheck"));

    m_continue = new QPushButton(QStringLiteral("继续"), this);
    m_continue->setObjectName(QStringLiteral("continueButton"));

    m_cancel = new QPushButton(QStringLiteral("取消"), this);
    m_cancel->setObjectName(QStringLiteral("cancelButton"));

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_continue);
    buttons->addWidget(m_cancel);

    root->addWidget(m_toggle);
    root->addWidget(m_body, 1);
    root->addWidget(m_error);
    root->addWidget(m_check);
    root->addLayout(buttons);

    connect(m_check, &QCheckBox::toggled, this, &RiskConfirmDialog::refreshContinueEnabled);
    connect(m_toggle, &QToolButton::toggled, this, &RiskConfirmDialog::toggleAgreement);
    connect(m_continue, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    // 关窗（标题栏 ×）= QDialog 默认的 reject —— 正是"用户取消"，闸门据此返回 false。
    // **有意不设 default 按钮**：Enter 不该替用户确认一次高危动作。

    loadBody();
}

void RiskConfirmDialog::toggleAgreement()
{
    m_body->setVisible(m_toggle->isChecked());
}

void RiskConfirmDialog::loadBody()
{
    QFile f(m_agreementPath);
    bool ok = f.exists() && f.open(QIODevice::ReadOnly);
    QString text;
    if (ok)
        text = QString::fromUtf8(f.readAll());
    if (ok && !text.trimmed().isEmpty()) {
        m_bodyLoaded = true;
        // markdown 渲染：用户看到的是标题/加粗/列表，而不是 `**` 记号本身（toPlainText 给出渲染后文本）。
        m_body->setMarkdown(text);
        m_error->clear();
    } else {
        // 失败一律 fail-closed：正文控件给出可读的占位，错误行说明原因，「继续」由
        // refreshContinueEnabled() 保持禁用（它同时看 m_bodyLoaded）。
        m_bodyLoaded = false;
        m_body->setPlainText(QStringLiteral("（无法读取使用须知正文，请联系作者获取完整副本。）"));
        m_error->setText(QStringLiteral("无法读取使用须知正文（%1）—— 「继续」已禁用。")
                             .arg(m_agreementPath));
    }
    refreshContinueEnabled();
}

void RiskConfirmDialog::refreshContinueEnabled()
{
    // 两条独立条件：正文读到了 **且** 用户勾了确认框。任一不满足都不放行。
    m_continue->setEnabled(m_bodyLoaded && m_check->isChecked());
}

bool ensureAccepted(QWidget *parent, RiskTier tier, legal::ConsentStore *store)
{
    // None：该档无需确认 —— 不弹窗、不写键（留痕无从谈起，调用方也不该为它弹窗）。
    if (tier == RiskTier::None)
        return true;

    // 默认 store 只在真的需要时构造（`store == nullptr` 的生产分支）：注入 store 的调用
    // （全部用例）由此**完全不碰**本机用户配置。留痕落在磁盘上，不依赖这个实例存活。
    std::unique_ptr<legal::ConsentStore> ownedStore;
    if (!store) {
        ownedStore = std::make_unique<legal::ConsentStore>();
        store = ownedStore.get();
    }

    // 已接受过（判据 `>= kAgreementVersion`）→ 直接放行，且**不再写**：留痕是"第一次确认"的凭证，
    // 不是每次动作的计数器；重写还会把用户接受过的**更高**版本号降级成当前版本。
    if (store->hasAcceptedTier(tier))
        return true;

    RiskConfirmDialog dlg(tier, parent);
    if (dlg.exec() != QDialog::Accepted)
        return false;   // 取消/关窗 = **静默**拒绝：调用方据此不发起任何动作

    store->acceptTier(tier);
    return true;
}

} // namespace riskui
