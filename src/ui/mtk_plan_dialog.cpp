#include "ui/mtk_plan_dialog.h"

#include <QVBoxLayout>

#include "ui/plan_preview_widget.h"

namespace {

// 真机未验证的**显式确认**文案（未勾选 = 「开始刷写」禁用）。
// 勾选框是纯文本控件（QCheckBox 不做富文本），故不写 Markdown 强调符 —— 会原样显示成星号。
QString ackText()
{
    return QStringLiteral("我已知晓：MTK BROM（LEGACY 代）刷写路径在本机未经真机验证，"
                          "分区写入有风险，出错自负。");
}

} // namespace

MtkPlanDialog::MtkPlanDialog(const mtkplan::MtkFlashPlan &plan, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("MTK BROM 刷写计划"));
    buildUi(plan);
}

void MtkPlanDialog::buildUi(const mtkplan::MtkFlashPlan &plan)
{
    m_preview = new PlanPreviewWidget(mtkplan::planHeaders(), mtkplan::planRows(plan),
                                      mtkplan::planSummaryHtml(plan), plan.warnings, ackText(), this);
    connect(m_preview, &PlanPreviewWidget::startRequested, this, &MtkPlanDialog::accept);
    connect(m_preview, &PlanPreviewWidget::cancelRequested, this, &MtkPlanDialog::reject);
    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_preview);
    resize(760, 520);
}

void MtkPlanDialog::accept()
{
    m_confirmed = true;      // 只有走到 accept 的路径才算用户确认（reject/关闭窗口不置位）
    QDialog::accept();
}

void MtkPlanDialog::reject()
{
    QDialog::reject();
}

bool MtkPlanDialog::buildAndShow(const mtkplan::MtkFlashPlan &plan, QWidget *parent, QString *error)
{
    // 空计划 = 没有东西可写：必须在**弹窗前**失败且给出非空原因 —— 调用方按
    // "false + error 空 = 用户取消 / 非空 = 失败"分派，弹一个"0 个分区"的预览框
    // 既让用户无从下手，也会把失败静默成"用户取消"。
    if (plan.entries.isEmpty()) {
        if (error)
            *error = QStringLiteral("计划里没有任何可写入的分区（检查镜像文件名与 scatter 分区名是否对得上）");
        return false;
    }
    if (error)
        error->clear();                     // 用户取消路径：error 保持空（调用方据此区分）
    MtkPlanDialog dlg(plan, parent);
    return dlg.exec() == QDialog::Accepted && dlg.confirmed();
}
