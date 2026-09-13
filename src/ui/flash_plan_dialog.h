// src/ui/flash_plan_dialog.h
//
// Phase B Task 8：刷写计划预览与确认对话框。**只读**展示 edl::FlashPlan（计划由
// edl::buildPlanFromDir 构建，本类不解析、不改计划），交互上做两件事：
//   ① 把"要写什么"摊开给用户看（条目表 + 告警），② 用勾选框把"此路径真机未验证"变成
//   显式确认 —— 未勾选时「开始刷写」禁用（默认走 cancel 语义，不是默认开刷）。
//
// 确认语义：`confirmed()` == 对话框**被 accept** 过（UI 里唯一会 accept 的控件是「开始刷写」；
// 取消/关闭窗口走 reject）—— 调用方据此决定是否真的下发刷写。
#ifndef FLASH_PLAN_DIALOG_H
#define FLASH_PLAN_DIALOG_H

#include <QDialog>
#include <QString>
#include <QStringList>

#include "core/edl/flash_plan.h"

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStandardItemModel;
class QTableView;
class QTemporaryDir;

class FlashPlanDialog : public QDialog
{
    Q_OBJECT

public:
    explicit FlashPlanDialog(const edl::FlashPlan &plan, QWidget *parent = nullptr);

    bool confirmed() const { return m_confirmed; }

    QString planDir() const { return m_planDir; }
    void setPlanDir(const QString &dir) { m_planDir = dir; }

    // 解包告警（部分条目被跳过等）并入预览的告警列表 —— 必须在 exec() 之前调用。
    void addWarnings(const QStringList &warnings);

    // 目录入口（解包产物目录：rawprogram*.xml / settings.xml）：
    //   buildPlanFromDir → 预览 → 用户确认后 *outDir = dir 并返回 true。
    // 返回 false 时：*error 非空 = 构建计划失败（中文文案）；*error 为空 = 用户取消。
    static bool buildAndShow(const QString &dir, QWidget *parent, QString *outDir, QString *error);

    // 整包入口（.ofp/.ops）：Phase A 的 extractOFP/extractOPS 解包到 **tempDir**（由调用方持有：
    // 解包产物必须活过对话框与随后的刷写，本函数不销毁它）→ 走同一预览。
    // 失败/取消语义同 buildAndShow（解包的部分跳过告警并入预览 warnings）。
    static bool buildAndShowPackage(const QString &packagePath, QWidget *parent,
                                    QTemporaryDir *tempDir, QString *outDir, QString *error);

public slots:
    // 唯一置位 confirmed() 的路径（「开始刷写」→ accept；取消/关闭 → reject）
    void accept() override;

signals:
    // 用户点了「开始刷写」（planDir 为本次预览的来源目录；单独构造对话框时为空串）
    void startRequested(const QString &planDir);

private:
    void buildUi(const edl::FlashPlan &plan);
    void refreshWarnings();

    QLabel *m_summaryLabel = nullptr;
    QTableView *m_table = nullptr;
    QStandardItemModel *m_model = nullptr;
    QLabel *m_warningsTitle = nullptr;
    QListWidget *m_warningsList = nullptr;
    QCheckBox *m_ackCheck = nullptr;
    QPushButton *m_startButton = nullptr;

    QStringList m_warnings;      // 计划告警 + 解包告警（预览用，不去重）
    QString m_planDir;
    bool m_confirmed = false;
};

#endif // FLASH_PLAN_DIALOG_H
