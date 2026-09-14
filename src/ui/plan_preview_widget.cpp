#include "plan_preview_widget.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QVBoxLayout>

PlanPreviewWidget::PlanPreviewWidget(const QStringList &headers, const QList<QStringList> &rows,
                                     const QString &summaryHtml, const QStringList &warnings,
                                     const QString &ackText, QWidget *parent)
    : QWidget(parent), m_warnings(warnings)
{
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setTextFormat(Qt::RichText);
    m_summaryLabel->setText(summaryHtml);
    m_summaryLabel->setWordWrap(true);
    layout->addWidget(m_summaryLabel);

    // 告警在表格**之上**：两类不匹配（PIT 有而包内无 / 包内有而 PIT 无）是最该先看到的信息
    // （三星 spec §4「warnings 在预览里置顶」）；条目多时表格会把下方的告警挤出屏幕。
    m_warningsTitle = new QLabel(QStringLiteral("告警"), this);
    layout->addWidget(m_warningsTitle);
    m_warningsList = new QListWidget(this);
    m_warningsList->setObjectName(QStringLiteral("warningsList"));
    m_warningsList->setMaximumHeight(110);
    layout->addWidget(m_warningsList);
    refreshWarnings();

    m_model = new QStandardItemModel(int(rows.size()), int(headers.size()), this);
    m_model->setHorizontalHeaderLabels(headers);
    for (int row = 0; row < rows.size(); ++row) {
        const QStringList &cells = rows.at(row);
        for (int col = 0; col < headers.size(); ++col) {
            QStandardItem *item = new QStandardItem(col < cells.size() ? cells.at(col) : QString());
            item->setEditable(false);
            m_model->setItem(row, col, item);
        }
    }

    m_table = new QTableView(this);
    m_table->setObjectName(QStringLiteral("planTable"));
    m_table->setModel(m_model);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->resizeColumnsToContents();
    layout->addWidget(m_table, 1);

    // 真机未验证告知：勾选前「开始刷写」保持禁用（默认停手，不是默认开刷）
    m_ackCheck = new QCheckBox(ackText, this);
    m_ackCheck->setObjectName(QStringLiteral("ackCheck"));
    layout->addWidget(m_ackCheck);

    QHBoxLayout *buttonLayout = new QHBoxLayout();
    m_startButton = new QPushButton(QStringLiteral("开始刷写"), this);
    m_startButton->setObjectName(QStringLiteral("startButton"));
    m_startButton->setEnabled(false);
    QPushButton *cancelButton = new QPushButton(QStringLiteral("取消"), this);
    buttonLayout->addStretch();
    buttonLayout->addWidget(m_startButton);
    buttonLayout->addWidget(cancelButton);
    layout->addLayout(buttonLayout);

    connect(m_ackCheck, &QCheckBox::toggled, m_startButton, &QPushButton::setEnabled);
    connect(m_startButton, &QPushButton::clicked, this, &PlanPreviewWidget::startRequested);
    connect(cancelButton, &QPushButton::clicked, this, &PlanPreviewWidget::cancelRequested);
}

void PlanPreviewWidget::refreshWarnings()
{
    m_warningsList->clear();
    m_warningsList->addItems(m_warnings);
    const bool any = !m_warnings.isEmpty();
    m_warningsTitle->setVisible(any);
    m_warningsList->setVisible(any);
}

void PlanPreviewWidget::addWarnings(const QStringList &warnings)
{
    if (warnings.isEmpty())
        return;
    m_warnings.append(warnings);
    refreshWarnings();
}

void PlanPreviewWidget::setColumnTooltips(int column, const QStringList &tips)
{
    if (column < 0 || column >= m_model->columnCount())
        return;
    const int rows = qMin(int(tips.size()), m_model->rowCount());
    for (int row = 0; row < rows; ++row) {
        if (tips.at(row).isEmpty())
            continue;
        if (QStandardItem *item = m_model->item(row, column))
            item->setToolTip(tips.at(row));
    }
}

bool PlanPreviewWidget::acknowledged() const
{
    return m_ackCheck->isChecked();
}
