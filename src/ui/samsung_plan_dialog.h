// src/ui/samsung_plan_dialog.h
//
// Phase C Task 9：三星（Odin）刷写计划的预览与确认对话框。与 Phase B 的 FlashPlanDialog
// 同一形状 —— 预览骨架（摘要 + 告警 + 条目表 + 未验证勾选 + 开始/取消）都走 PlanPreviewWidget，
// 差异只在数据来源与文案：本类展示 odin::SamsungPlan（PIT 条目 ↔ 包内镜像），不做任何解析。
//
// 确认语义：`confirmed()` == 对话框**被 accept** 过（UI 里唯一会 accept 的控件是「开始刷写」；
// 取消/关闭窗口走 reject）—— 调用方据此决定是否真的下发刷写。
#ifndef SAMSUNG_PLAN_DIALOG_H
#define SAMSUNG_PLAN_DIALOG_H

#include <QDialog>
#include <QString>
#include <QStringList>

#include "core/odin/samsung_plan.h"

class PlanPreviewWidget;

class SamsungPlanDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SamsungPlanDialog(const odin::SamsungPlan &plan, QWidget *parent = nullptr);

    bool confirmed() const { return m_confirmed; }

    // 整包入口（BL/AP/CP/CSC 的 .tar.md5 集合）：
    //   PIT 显式指定优先 → 否则取包内**唯一** .pit → buildSamsungPlan → 预览 → 用户确认。
    // 返回 false 时：*error 非空 = PIT/计划构建失败（中文文案）；*error 为空 = 用户取消
    // （取消路径**显式 error->clear()**，不依赖调用方传进来的初值）。PIT 来源串已由本函数
    // 填进计划的 pitSource（显式 = "用户指定 PIT：x.pit"，包内 = "路径（包内 条目名）"）。
    static bool buildAndShow(const QStringList &tarMd5Files, const QString &pitPath,
                             QWidget *parent, QString *error);

public slots:
    // 唯一置位 confirmed() 的路径（「开始刷写」→ accept；取消/关闭 → reject）
    void accept() override;

private:
    void buildUi(const odin::SamsungPlan &plan);

    PlanPreviewWidget *m_preview = nullptr;
    bool m_confirmed = false;
};

#endif // SAMSUNG_PLAN_DIALOG_H
