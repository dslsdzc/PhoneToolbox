// src/ui/mtk_plan_dialog.h
//
// Phase D1 Task 11：MTK BROM 刷写计划的预览与确认对话框。**只读**展示 mtkplan::MtkFlashPlan
// （计划由 mtkplan::buildMtkPlan 构建，本类不解析、不改计划），并把"此路径真机未验证"变成显式确认。
// 确认语义与 Phase B/C 同款：confirmed() == 对话框被 accept 过（唯一 accept 路径是「开始刷写」）。
#ifndef MTK_PLAN_DIALOG_H
#define MTK_PLAN_DIALOG_H

#include <QDialog>
#include <QString>

#include "core/mtk_flash_plan.h"

class PlanPreviewWidget;

class MtkPlanDialog : public QDialog
{
    Q_OBJECT
public:
    explicit MtkPlanDialog(const mtkplan::MtkFlashPlan &plan, QWidget *parent = nullptr);

    bool confirmed() const { return m_confirmed; }

    // 预览 → 用户确认。返回 false 时：*error 非空 = 计划不可用（空计划，**弹窗前**返回）；
    // *error 为空 = 用户取消（不依赖调用方传进来的初值）。计划的渲染（表头/条目/摘要/告警）
    // 全走 mtkplan::plan* 三个函数 —— 与计划层同一份文案，UI 不另抄一套。
    static bool buildAndShow(const mtkplan::MtkFlashPlan &plan, QWidget *parent, QString *error);

public slots:
    // 唯一置位 confirmed() 的路径（「开始刷写」→ accept；取消/关闭 → reject）
    void accept() override;
    void reject() override;

private:
    void buildUi(const mtkplan::MtkFlashPlan &plan);

    PlanPreviewWidget *m_preview = nullptr;
    bool m_confirmed = false;
};

#endif // MTK_PLAN_DIALOG_H
