#include "flash_plan_dialog.h"

#include <QCheckBox>
#include <QDir>
#include <QEventLoop>
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

#include "oppo_extract_worker.h"

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
    if (dlg.exec() != QDialog::Accepted || !dlg.confirmed()) {
        if (error) error->clear();   // 显式清空：调用方以"*error 为空 = 用户取消"判定（不赖调用方初值）
        return false;
    }

    if (outDir) *outDir = dir;
    return true;
}

bool FlashPlanDialog::buildAndShowPackage(const QString &packagePath, QWidget *parent,
                                          QTemporaryDir *tempDir, QString *outDir, QString *error,
                                          QString *cancelNote)
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

    // PB-B6：解包在工作线程执行（OppoExtractWorker），进度经 queued 信号回到本线程 →
    // 进度条**真实推进**，而模态对话框自己的事件循环让界面保持响应（旧实现是"GUI 线程
    // 同步跑 + 手动 processEvents(ExcludeUserInputEvents) 泵"，GB 级包全程不可交互）。
    // 模态仍是 WindowModal：**响应 ≠ 可重入** —— 解包期间不放行主窗口输入（与旧实现的
    // ExcludeUserInputEvents 同一安全口径），用户能做的只有"看进度 / 取消"。
    OppoExtractWorker worker;
    // 取消文案必须诚实：解包引擎没有"半途停"的钩子，取消在**条目（文件）边界**生效 ——
    // 当前文件还会写完并校验完，然后才停（oppo_extract.h 的 ExtractCancel / worker 类注释）。
    QProgressDialog progress(QStringLiteral("正在解包固件包…"),
                             QStringLiteral("取消（当前文件完成后生效）"), 0, 100, parent);
    progress.setWindowTitle(QStringLiteral("解包"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    // autoClose/autoReset 一律关掉：进度条达到 100（或取消）时不让 QProgressDialog 自己
    // hide/reset —— 关不关它都拦不住"取消时 hide"（见下面 handler 里的 show()），但关掉能
    // 让"跑完"的收尾完全由本函数控制（先 progress.close() 再取结果），语义只有一处。
    progress.setAutoClose(false);
    progress.setAutoReset(false);

    connect(&worker, &OppoExtractWorker::progress, &progress,
            [&progress](const QString &name, int percent) {
                progress.setLabelText(QStringLiteral("解包中：%1").arg(name));
                progress.setValue(percent);
            });
    // DirectConnection：接收者与发射者都在 GUI 线程、lambda 只置原子标志 + 改本线程的控件，
    // 不需要（也不能）经工作线程的事件队列 —— 那边正忙于解包，排队等于取消永不生效。
    connect(&progress, &QProgressDialog::canceled, &progress,
            [&worker, &progress] {
                worker.requestCancel();       // 原子标志；引擎在下一个条目边界读到
                progress.setLabelText(QStringLiteral("正在取消…（当前文件完成后停止）"));
                progress.setCancelButton(nullptr);   // 请求已发出，再点没有意义
                // Qt 的 QProgressDialog 在取消时会自己 reset()+hide()（**与 autoClose/autoReset
                // 无关**：实测 6.11 上取消按钮点击 → visible 变 false、value 回 -1），而"取消
                // 只在文件边界生效"恰恰要求这段时间**看得见** —— 故把它拉回屏幕，让用户看到
                // "说明"而不是一个凭空消失、稍后又被进度更新弹回来的对话框。
                progress.show();
            },
            Qt::DirectConnection);

    QEventLoop loop;
    connect(&worker, &OppoExtractWorker::finished, &loop, &QEventLoop::quit);
    worker.start(packagePath, tempDir->path());
    progress.show();
    // 守卫循环：queued 的 finished 可能早于 exec() 到达（模态进度条的 show()/setValue()
    // 内部会自己 processEvents）—— 那时 quit() 对尚未运行的循环是空操作，裸 exec() 会
    // 一直等下去。用 isFinished() 兜住"已完成但 quit 早到"的情形。
    while (!worker.isFinished())
        loop.exec();
    progress.close();
    worker.waitForFinished();       // 取结果前 join（结果字段由工作线程写）

    if (worker.cancelRequested()) {
        // 取消路径与既有口径一致：*error 为空 = 用户取消（调用方据此回收临时目录）。
        // 详情走 cancelNote：这一趟被停在哪（完成 N/M 个文件）对日志与用户都有意义。
        if (cancelNote)
            *cancelNote = worker.ok()
                ? QStringLiteral("取消请求到达时解包已跑完（产物未使用）")
                : (worker.error().isEmpty() ? QStringLiteral("解包已取消") : worker.error());
        if (error) error->clear();
        return false;
    }

    const QString extractErr = worker.error();
    // 与 ImageWorker 同款约定（oppo_extract.h 顶部）：ok==true 且 *error 非空 =
    // **部分条目被跳过**（不安全文件名/截断等）—— 必须让用户在预览里看见，不得静默
    if (!worker.ok()) {
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
    if (dlg.exec() != QDialog::Accepted || !dlg.confirmed()) {
        if (error) error->clear();   // 与 buildAndShow 同口径：*error 为空 = 用户取消
        return false;
    }

    if (outDir) *outDir = tempDir->path();
    return true;
}
