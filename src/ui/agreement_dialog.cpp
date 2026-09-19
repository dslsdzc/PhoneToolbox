// src/ui/agreement_dialog.cpp —— 首屏使用须知（见 .h 文件头的 fail-closed 取舍）
#include "ui/agreement_dialog.h"

#include <QCheckBox>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace agreementui {

AgreementDialog::AgreementDialog(const QString &resourcePath, QWidget *parent)
    : QDialog(parent), m_resourcePath(resourcePath)
{
    setWindowTitle(QStringLiteral("PhoneToolbox 使用须知与免责声明"));
    setModal(true);

    m_body = new QTextBrowser(this);
    m_body->setObjectName(QStringLiteral("agreementBody"));
    // 只读：QTextBrowser 默认即只读，这里写死一次 —— 正文是给用户看的条款，不是可编辑控件。
    m_body->setReadOnly(true);
    m_body->setOpenExternalLinks(false);
    // 正文区最小高度 320px：窗口小时靠它保住可读面积（不 resize 写死整窗尺寸，随字体/DPI 自适应）。
    m_body->setMinimumHeight(320);

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("agreementError"));
    m_error->setWordWrap(true);

    m_check = new QCheckBox(QStringLiteral("我已阅读并同意上述使用须知"), this);
    m_check->setObjectName(QStringLiteral("agreeCheck"));

    m_agree = new QPushButton(QStringLiteral("同意并继续"), this);
    m_agree->setObjectName(QStringLiteral("agreeButton"));
    m_agree->setDefault(true);

    m_exit = new QPushButton(QStringLiteral("退出"), this);
    m_exit->setObjectName(QStringLiteral("exitButton"));

    auto *buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_agree);
    buttons->addWidget(m_exit);

    auto *root = new QVBoxLayout(this);
    root->addWidget(m_body, 1);
    root->addWidget(m_error);
    root->addWidget(m_check);
    root->addLayout(buttons);

    connect(m_check, &QCheckBox::toggled, this, &AgreementDialog::refreshAgreeEnabled);
    connect(m_agree, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_exit, &QPushButton::clicked, this, &QDialog::reject);
    // 关窗（标题栏 ×）= QDialog 默认的 reject —— 正是"拒绝"，调用方据此退出程序。

    loadBody();
}

QString AgreementDialog::bodyText() const
{
    return m_body ? m_body->toPlainText() : QString();
}

void AgreementDialog::loadBody()
{
    QFile f(m_resourcePath);
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
        // 失败一律 fail-closed：正文控件给出可读的占位，错误行说明原因，同意按钮由
        // refreshAgreeEnabled() 保持禁用（它同时看 m_bodyLoaded）。
        m_bodyLoaded = false;
        m_body->setPlainText(QStringLiteral("（无法读取使用须知正文，请联系作者获取完整副本。）"));
        m_error->setText(QStringLiteral("无法读取使用须知正文（%1）—— 「同意并继续」已禁用。")
                             .arg(m_resourcePath));
    }
    refreshAgreeEnabled();
}

void AgreementDialog::refreshAgreeEnabled()
{
    // 两条独立条件：正文读到了 **且** 用户勾了确认框。任一不满足都不放行。
    m_agree->setEnabled(m_bodyLoaded && m_check->isChecked());
}

bool ask(QWidget *parent)
{
    AgreementDialog dlg(QString::fromLatin1(kAgreementResource), parent);
    return dlg.exec() == QDialog::Accepted;
}

} // namespace agreementui
