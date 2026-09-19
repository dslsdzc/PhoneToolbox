// src/ui/agreement_dialog.h
//
// 首屏「使用须知与免责声明」（W2 spec §5 的第一行）：启动时若本版本尚未被接受，先弹这个；
// 用户不接受 → 调用方退出程序（不进入主界面）。正文是**单一副本**——`resources/legal/user-agreement.md`，
// 逐字取自 design spec §2 的代码块，改正文等于改对用户的责任约定，必须同时改 spec 与版本号。
//
// 为什么读失败要 fail-closed：这份对话框在"没读到正文"时如果还能点同意，用户就会在**没有看到条款**的
// 情况下留下"已同意"的留痕 —— 留痕的意义正好被抹掉。故「同意并继续」的启用条件是**两条都满足**：
// 正文读到了、且用户勾了确认框（用例钉住：不存在的路径与存在的空文件都不放行）。
//
// 控件用 objectName 暴露给用例（仓内既有对话框同款）：agreementBody / agreementError /
// agreeCheck / agreeButton / exitButton。
#pragma once

#include <QDialog>
#include <QString>

class QCheckBox;
class QLabel;
class QPushButton;
class QTextBrowser;

namespace agreementui {

// 正文资源路径（qrc 前缀 /legal + alias user-agreement.md）。ask() 用它；用例注入别的路径
// 来钉 fail-closed 分支。**别在别处硬编这个字符串** —— 三路比对（spec 正文 ↔ 资源 ↔ kAgreementVersion）
// 就是靠它落在同一个字面上。
inline constexpr const char *kAgreementResource = ":/legal/user-agreement.md";

class AgreementDialog : public QDialog
{
    Q_OBJECT

public:
    // resourcePath：正文来源（默认即 kAgreementResource；用例注入不存在的路径/空文件做负控制）。
    explicit AgreementDialog(const QString &resourcePath = QString::fromLatin1(kAgreementResource),
                             QWidget *parent = nullptr);

    // 正文是否读入成功。false 时「同意并继续」**恒禁用**（见文件头），用户只能退出。
    bool bodyLoaded() const { return m_bodyLoaded; }
    // 正文控件里**实际展示**的纯文本（markdown 渲染后）。用例的断言打在这里而不是重读一遍文件 ——
    // 否则"资源读到了但没接到控件上"也能过。
    QString bodyText() const;

private slots:
    void refreshAgreeEnabled();

private:
    // 读资源 → 渲染进正文控件。失败（不存在/打不开/空）→ 一行错误 + 保持禁用，绝不放行。
    void loadBody();

    QTextBrowser *m_body  = nullptr;
    QLabel       *m_error = nullptr;
    QCheckBox    *m_check = nullptr;
    QPushButton  *m_agree = nullptr;
    QPushButton  *m_exit  = nullptr;

    QString m_resourcePath;
    bool    m_bodyLoaded = false;
};

// 展示使用须知（只读滚动正文 + 一个必勾确认框）。返回 true = 用户同意。
// 用户点「退出」/关窗 → false（调用方负责退出程序）。
bool ask(QWidget *parent);

} // namespace agreementui
