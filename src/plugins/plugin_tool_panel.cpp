#include "src/plugins/plugin_tool_panel.h"

#include <QColor>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

#include "src/plugins/plugin_interface.h"

PluginToolPanel::PluginToolPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);

    m_summary = new QLabel(QStringLiteral("尚未扫描插件"), this);
    layout->addWidget(m_summary);

    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("插件"),
        QStringLiteral("加载状态"),
        QStringLiteral("简介"),
    });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->verticalHeader()->setVisible(false);
    m_table->setAlternatingRowColors(true);
    layout->addWidget(m_table, 1);

    m_hint = new QLabel(
        QStringLiteral("插件能力由对应功能自动调用（例如华为刷写在刷机流程中）。"
                       "加载失败的具体原因见下方输出日志。"),
        this);
    m_hint->setWordWrap(true);
    m_hint->setStyleSheet(QStringLiteral("color: #888; font-size: 11px;"));
    layout->addWidget(m_hint);
}

void PluginToolPanel::setRecords(const QList<PluginRecord> &records)
{
    m_table->setRowCount(0);

    int loadedCount = 0;
    int failedCount = 0;

    auto addRow = [this](const QString &name, const QString &status,
                         const QString &description, bool ok) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);

        auto *nameItem = new QTableWidgetItem(name);
        auto *statusItem = new QTableWidgetItem(status);
        auto *descItem = new QTableWidgetItem(
            description.isEmpty() ? QStringLiteral("—") : description);
        descItem->setToolTip(description);

        if (!ok)
            statusItem->setForeground(QColor(QStringLiteral("#c0392b")));

        m_table->setItem(row, 0, nameItem);
        m_table->setItem(row, 1, statusItem);
        m_table->setItem(row, 2, descItem);
    };

    // 每个插件一行。失败原因不塞进表格：它已经由 MainWindow 在启动时
    // 逐条写入下方输出日志；表格这里只回答"这个插件是谁、有没有加载成功"。
    for (const PluginRecord &record : records) {
        if (!record.loaded) {
            ++failedCount;
            addRow(record.name, QStringLiteral("加载失败"), record.description, false);
            continue;
        }
        ++loadedCount;
        addRow(record.name, QStringLiteral("已加载"), record.description, true);
    }

    if (records.isEmpty()) {
        m_summary->setText(
            QStringLiteral("未找到插件（plugins/ 目录为空或不存在）"));
    } else {
        m_summary->setText(QStringLiteral("已加载 %1 个插件，加载失败 %2 个")
                               .arg(loadedCount)
                               .arg(failedCount));
    }
}
