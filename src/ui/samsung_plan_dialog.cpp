#include "samsung_plan_dialog.h"

#include <QFileInfo>
#include <QVBoxLayout>

#include "plan_preview_widget.h"

namespace {

// "来源包"列：表里只显文件名，完整路径走 tooltip（与 FlashPlanDialog 的「文件」列同款口径）。
// fileIndex 越界（计划层契约：-1 = 无来源）给 "-"，不静默显示成第一个包。
QString sourceFileLabel(const odin::SamsungPlan &plan, int fileIndex)
{
    if (fileIndex < 0 || fileIndex >= plan.files.size())
        return QStringLiteral("-");
    return QFileInfo(plan.files.at(fileIndex).path).fileName();
}

QString sourceFileTooltip(const odin::SamsungPlan &plan, int fileIndex)
{
    if (fileIndex < 0 || fileIndex >= plan.files.size())
        return QString();
    return plan.files.at(fileIndex).path;
}

} // namespace

SamsungPlanDialog::SamsungPlanDialog(const odin::SamsungPlan &plan, QWidget *parent)
    : QDialog(parent)
{
    buildUi(plan);
}

void SamsungPlanDialog::buildUi(const odin::SamsungPlan &plan)
{
    setWindowTitle(QStringLiteral("三星 Odin 刷写计划预览"));
    setMinimumSize(880, 520);

    QVBoxLayout *layout = new QVBoxLayout(this);

    // 顶部摘要：PIT 来源 / 包数 / 条目数 / 总字节
    const QString summary = QStringLiteral(
        "<b>PIT：</b>%1　<b>包：</b>%2　<b>条目：</b>%3　<b>总字节：</b>%4")
        .arg(plan.pitSource.isEmpty() ? QStringLiteral("（未标注）") : plan.pitSource)
        .arg(plan.files.size())
        .arg(plan.entries.size())
        .arg(planBytesText(plan.totalBytes));

    // 条目表：分区 / 镜像 / 大小 / 来源包 / 匹配规则
    const QStringList headers = {
        QStringLiteral("分区"), QStringLiteral("镜像"), QStringLiteral("大小"),
        QStringLiteral("来源包"), QStringLiteral("匹配规则")};
    QList<QStringList> rows;
    QStringList tips;
    for (const odin::SamsungPlanEntry &e : plan.entries) {
        rows.append(QStringList{
            e.partition,
            e.imageFile,
            planBytesText(e.sizeBytes),
            sourceFileLabel(plan, e.fileIndex),
            e.matchRule,
        });
        tips.append(sourceFileTooltip(plan, e.fileIndex));
    }

    m_preview = new PlanPreviewWidget(headers, rows, summary, plan.warnings,
                                      QStringLiteral("我知晓此路径真机未验证（设备侧未联调）"),
                                      this);
    m_preview->setColumnTooltips(3, tips);
    layout->addWidget(m_preview);

    connect(m_preview, &PlanPreviewWidget::startRequested, this, &SamsungPlanDialog::accept);
    connect(m_preview, &PlanPreviewWidget::cancelRequested, this, &SamsungPlanDialog::reject);
}

void SamsungPlanDialog::accept()
{
    m_confirmed = true;      // 只有走到 accept 的路径才算用户确认（reject/关闭窗口不置位）
    QDialog::accept();
}

bool SamsungPlanDialog::buildAndShow(const QStringList &tarMd5Files, const QString &pitPath,
                                     QWidget *parent, QString *error)
{
    // PIT 显式优先（用户明确给了文件就用它，不再猜包内）；否则取包内唯一 .pit
    // —— 与 flash_tool.cpp 的 samsung-odin 通道同一口径（通道侧会自行重建计划）。
    odin::PitTable pit;
    QString pitSource;
    if (!pitPath.isEmpty()) {
        QString pitErr;
        if (!odin::parsePitFile(pitPath, pit, &pitErr)) {
            if (error) *error = QStringLiteral("读取 PIT 失败：%1").arg(pitErr);
            return false;
        }
        pitSource = QStringLiteral("用户指定 PIT：%1").arg(QFileInfo(pitPath).fileName());
    } else {
        QString pitErr;
        if (!odin::loadPitFromPackage(tarMd5Files, pit, &pitSource, &pitErr)) {
            if (error) *error = pitErr;
            return false;
        }
    }

    odin::SamsungPlan plan;
    QString planErr;
    if (!odin::buildSamsungPlan(tarMd5Files, pit, plan, &planErr, pitSource)) {
        if (error) *error = QStringLiteral("构建刷写计划失败：%1").arg(planErr);
        return false;
    }

    SamsungPlanDialog dlg(plan, parent);
    if (dlg.exec() != QDialog::Accepted || !dlg.confirmed()) {
        if (error) error->clear();   // 显式清空：*error 为空 = 用户取消（不赖调用方初值）
        return false;
    }
    return true;
}
