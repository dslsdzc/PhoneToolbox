// src/ui/plan_preview_widget.h
//
// 计划预览的通用控件（从 Phase B 的 FlashPlanDialog 抽出）。职责三条：
//   ① 把"要写什么"摊开（摘要 + 告警 + 表格）；② 用勾选框把"此路径真机未验证"变成**显式确认**
//   （未勾选时「开始刷写」禁用）；③ 把"开始/取消"两个动作以信号抛出（谁接谁负责）。
// 不解析任何计划模型 —— 表格内容由调用方翻译好传进来。
#ifndef PLAN_PREVIEW_WIDGET_H
#define PLAN_PREVIEW_WIDGET_H

#include <QString>
#include <QStringList>
#include <QWidget>

class QCheckBox;
class QLabel;
class QListWidget;
class QPushButton;
class QStandardItemModel;
class QTableView;

// 字节数人性化（两个对话框共用）
inline QString planBytesText(quint64 bytes)
{
    if (bytes >= 1024ull * 1024 * 1024)
        return QStringLiteral("%1 GiB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
    if (bytes >= 1024ull * 1024)
        return QStringLiteral("%1 MiB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
    if (bytes >= 1024ull)
        return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 0);
    return QStringLiteral("%1 B").arg(bytes);
}

class PlanPreviewWidget : public QWidget
{
    Q_OBJECT
public:
    // headers/rows = 表格内容（行数 = rows.size()，列数 = headers.size()）；
    // summaryHtml = 顶部摘要（RichText）；warnings = 告警（**渲染在表格之上**）；
    // ackText = 勾选框文案。
    PlanPreviewWidget(const QStringList &headers, const QList<QStringList> &rows,
                      const QString &summaryHtml, const QStringList &warnings,
                      const QString &ackText, QWidget *parent = nullptr);

    void addWarnings(const QStringList &warnings);            // 只追加、不去重
    void setColumnTooltips(int column, const QStringList &tips); // 逐行 tooltip（不足的行不给）
    bool acknowledged() const;

signals:
    void startRequested();     // 用户点了「开始刷写」（勾选框已由控件门控）
    void cancelRequested();    // 用户点了「取消」→ 调用方 reject()

private:
    void refreshWarnings();

    QLabel *m_summaryLabel = nullptr;
    QLabel *m_warningsTitle = nullptr;
    QListWidget *m_warningsList = nullptr;
    QTableView *m_table = nullptr;
    QStandardItemModel *m_model = nullptr;
    QCheckBox *m_ackCheck = nullptr;
    QPushButton *m_startButton = nullptr;
    QStringList m_warnings;
};

#endif // PLAN_PREVIEW_WIDGET_H
