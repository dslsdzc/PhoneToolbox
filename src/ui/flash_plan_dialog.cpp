#include "flash_plan_dialog.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QProgressDialog>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QTemporaryDir>
#include <QVBoxLayout>

#include "image_engine/oppo_extract.h"

namespace {

// 计划条目 → 预览"分区"列文案：Program 用 label；Erase 在 rawprogram 里没有名字（模型注释），
// 用区间描述；Patch 的 partitionName 是 filename（解析层填的）。
QString entryName(const edl::PlanEntry &e)
{
    switch (e.action) {
    case edl::PlanEntry::Action::Program:
        return e.partitionName;
    case edl::PlanEntry::Action::Erase: {
        const QString range = e.startSectorExpr.isEmpty()
            ? (e.numSectors == 0 ? QStringLiteral("整 LUN")
                                 : QStringLiteral("%1+%2").arg(e.startSector).arg(e.numSectors))
            : e.startSectorExpr;
        return QStringLiteral("（擦除）%1").arg(range);
    }
    case edl::PlanEntry::Action::Patch:
        return QStringLiteral("（修补）%1").arg(e.partitionName);
    }
    return QString();
}

// 起始扇区列：表达式条目原样透传（设备侧求值，flash_plan.h 的模型契约）
QString entryStart(const edl::PlanEntry &e)
{
    if (!e.startSectorExpr.isEmpty())
        return e.startSectorExpr;
    return QString::number(e.startSector);
}

QString humanBytes(quint64 bytes)
{
    if (bytes >= 1024ull * 1024 * 1024)
        return QStringLiteral("%1 GiB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
    if (bytes >= 1024ull * 1024)
        return QStringLiteral("%1 MiB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
    if (bytes >= 1024ull)
        return QStringLiteral("%1 KiB").arg(bytes / 1024.0, 0, 'f', 0);
    return QStringLiteral("%1 B").arg(bytes);
}

QString entryBytes(const edl::PlanEntry &e)
{
    const quint64 bytes = e.rawBytes != 0
        ? e.rawBytes
        : e.numSectors * quint64(e.sectorSize ? e.sectorSize : 1);
    return bytes == 0 ? QStringLiteral("-") : humanBytes(bytes);
}

} // namespace

FlashPlanDialog::FlashPlanDialog(const edl::FlashPlan &plan, QWidget *parent)
    : QDialog(parent)
    , m_warnings(plan.warnings)
{
    buildUi(plan);
}

void FlashPlanDialog::buildUi(const edl::FlashPlan &plan)
{
    setWindowTitle(QStringLiteral("EDL 刷写计划预览"));
    setMinimumSize(880, 520);

    QVBoxLayout *layout = new QVBoxLayout(this);

    // 顶部摘要：来源 / 存储类型 / 条目数 / 总字节
    m_summaryLabel = new QLabel(this);
    m_summaryLabel->setTextFormat(Qt::RichText);
    m_summaryLabel->setText(QStringLiteral(
        "<b>来源：</b>%1　<b>存储类型：</b>%2　<b>条目数：</b>%3　<b>总字节：</b>%4")
        .arg(plan.source.isEmpty() ? QStringLiteral("（未标注）") : plan.source,
             plan.storageType.isEmpty() ? QStringLiteral("（未标注）") : plan.storageType)
        .arg(plan.entries.size())
        .arg(humanBytes(plan.totalBytes)));
    m_summaryLabel->setWordWrap(true);
    layout->addWidget(m_summaryLabel);

    // 条目表：分区 / LUN / 起始扇区 / 扇区数 / 大小 / 文件 / 校验
    m_model = new QStandardItemModel(plan.entries.size(), 7, this);
    m_model->setHorizontalHeaderLabels({
        QStringLiteral("分区"), QStringLiteral("LUN"), QStringLiteral("起始扇区"),
        QStringLiteral("扇区数"), QStringLiteral("大小"), QStringLiteral("文件"),
        QStringLiteral("校验")});
    for (int row = 0; row < plan.entries.size(); ++row) {
        const edl::PlanEntry &e = plan.entries.at(row);
        const QStringList cells = {
            entryName(e),
            QString::number(e.lun),
            entryStart(e),
            e.numSectors == 0 ? QStringLiteral("整 LUN") : QString::number(e.numSectors),
            entryBytes(e),
            e.imageFile == QLatin1String("DISK")
                ? QStringLiteral("（设备磁盘偏移，不下发文件）")
                : QFileInfo(e.imageFile).fileName(),
            e.sha256.isEmpty() ? QStringLiteral("-")
                               : QStringLiteral("%1…").arg(e.sha256.left(12)),
        };
        for (int col = 0; col < cells.size(); ++col) {
            QStandardItem *item = new QStandardItem(cells.at(col));
            item->setEditable(false);
            if (!e.imageFile.isEmpty() && e.imageFile != QLatin1String("DISK"))
                item->setToolTip(e.imageFile);        // 表里只显文件名，完整路径走 tooltip
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

    // 告警列表（计划层 warnings + 解包告警）
    m_warningsTitle = new QLabel(QStringLiteral("告警"), this);
    layout->addWidget(m_warningsTitle);
    m_warningsList = new QListWidget(this);
    m_warningsList->setObjectName(QStringLiteral("warningsList"));
    m_warningsList->setMaximumHeight(110);
    layout->addWidget(m_warningsList);
    refreshWarnings();

    // 真机未验证告知：勾选前「开始刷写」保持禁用（默认停手，不是默认开刷）
    m_ackCheck = new QCheckBox(QStringLiteral("我知晓此路径真机未验证"), this);
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

    connect(m_ackCheck, &QCheckBox::toggled,
            m_startButton, &QPushButton::setEnabled);
    connect(m_startButton, &QPushButton::clicked, this, [this]() {
        emit startRequested(m_planDir);
        accept();
    });
    connect(cancelButton, &QPushButton::clicked, this, &FlashPlanDialog::reject);
}

void FlashPlanDialog::refreshWarnings()
{
    m_warningsList->clear();
    m_warningsList->addItems(m_warnings);
    const bool any = !m_warnings.isEmpty();
    m_warningsTitle->setVisible(any);
    m_warningsList->setVisible(any);
}

void FlashPlanDialog::addWarnings(const QStringList &warnings)
{
    if (warnings.isEmpty())
        return;
    m_warnings.append(warnings);
    refreshWarnings();
}

void FlashPlanDialog::accept()
{
    m_confirmed = true;      // 只有走到 accept 的路径才算用户确认（reject/关闭窗口不置位）
    QDialog::accept();
}

bool FlashPlanDialog::buildAndShow(const QString &dir, QWidget *parent,
                                   QString *outDir, QString *error)
{
    edl::FlashPlan plan;
    QString planErr;
    if (!edl::buildPlanFromDir(dir, plan, &planErr)) {
        if (error)
            *error = planErr.isEmpty()
                ? QStringLiteral("构建刷写计划失败：%1").arg(dir)
                : planErr;
        return false;
    }

    FlashPlanDialog dlg(plan, parent);
    dlg.setPlanDir(dir);
    if (dlg.exec() != QDialog::Accepted || !dlg.confirmed())
        return false;        // 取消：*error 保持调用方原值（调用方按空 error 识别"用户取消"）

    if (outDir) *outDir = dir;
    return true;
}

bool FlashPlanDialog::buildAndShowPackage(const QString &packagePath, QWidget *parent,
                                          QTemporaryDir *tempDir, QString *outDir, QString *error)
{
    if (!tempDir || !tempDir->isValid()) {
        if (error)
            *error = QStringLiteral("无法创建解包临时目录，无法处理固件包");
        return false;
    }

    // 解包引擎按扩展名二选一（.ofp → extractOFP 内部再分 QC/MTK 变体；.ops → extractOPS）
    const QString lower = packagePath.toLower();
    const bool ops = lower.endsWith(QStringLiteral(".ops"));
    if (!ops && !lower.endsWith(QStringLiteral(".ofp"))) {
        if (error)
            *error = QStringLiteral("不是 OPPO 固件包（需 .ofp/.ops）：%1").arg(packagePath);
        return false;
    }

    // 模态进度条（引擎是同步一整趟写完的，没有取消钩子 —— 半途停下比写完更危险，
    // 故不给取消按钮，只报进度）
    QProgressDialog progress(QStringLiteral("正在解包固件包…"), QString(), 0, 100, parent);
    progress.setWindowTitle(QStringLiteral("解包"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setCancelButton(nullptr);
    progress.setMinimumDuration(0);
    const auto cb = [&progress](const QString &name, int percent) {
        progress.setLabelText(QStringLiteral("解包中：%1").arg(name));
        progress.setValue(percent);
        QCoreApplication::processEvents();      // 同步引擎 + 模态进度条：手动泵事件
    };

    QString extractErr;
    const bool ok = ops ? imgopp::extractOPS(packagePath, tempDir->path(), cb, &extractErr)
                        : imgopp::extractOFP(packagePath, tempDir->path(), cb, &extractErr);
    progress.close();
    // 与 ImageWorker 同款约定（oppo_extract.h 顶部）：ok==true 且 *error 非空 =
    // **部分条目被跳过**（不安全文件名/截断等）—— 必须让用户在预览里看见，不得静默
    if (!ok) {
        if (error)
            *error = extractErr.isEmpty()
                ? QStringLiteral("固件包解包失败：%1").arg(QFileInfo(packagePath).fileName())
                : extractErr;
        return false;
    }

    edl::FlashPlan plan;
    QString planErr;
    if (!edl::buildPlanFromDir(tempDir->path(), plan, &planErr)) {
        if (error)
            *error = planErr.isEmpty()
                ? QStringLiteral("解包产物中未找到可用的刷写计划（rawprogram*.xml / settings.xml）")
                : planErr;
        return false;
    }

    FlashPlanDialog dlg(plan, parent);
    dlg.setPlanDir(tempDir->path());
    if (!extractErr.isEmpty())
        dlg.addWarnings({QStringLiteral("解包告警：%1").arg(extractErr)});
    if (dlg.exec() != QDialog::Accepted || !dlg.confirmed())
        return false;

    if (outDir) *outDir = tempDir->path();
    return true;
}
