#include "fs_browser_dialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QFileInfo>
#include <QFileDialog>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QDir>
#include <QHeaderView>
#include <algorithm>
#include <functional>

namespace {

// 文件大小人类可读（与 image_tool_panel 的 formatSize 同款实现，前端小工具）
QString formatSize(quint64 bytes)
{
    if (bytes < 1024ull)
        return QStringLiteral("%1 B").arg(bytes);
    const char *units[] = { "KB", "MB", "GB", "TB" };
    double v = double(bytes);
    int u = -1;
    do {
        v /= 1024.0;
        ++u;
    } while (v >= 1024.0 && u < 3);
    return QStringLiteral("%1 %2").arg(v, 0, 'f', u ? 1 : 0).arg(QLatin1String(units[u]));
}

} // namespace

FsBrowserDialog::FsBrowserDialog(const QString &imagePath, imgreg::Format format,
                                 ImageWorker *worker, QWidget *parent)
    : QDialog(parent)
    , m_imagePath(imagePath)
    , m_worker(worker)
{
    m_canReplace = (format == imgreg::Format::Ext4);
    setWindowTitle(QStringLiteral("文件系统浏览 - %1")
                       .arg(QFileInfo(imagePath).fileName()));
    resize(760, 520);

    setupUI();

    // worker（工作线程）→ 对话框（UI 线程），跨线程自动 QueuedConnection；
    // 多对话框并存时按 path 过滤（onFs* 各槽均校验 m_imagePath）
    connect(m_worker, &ImageWorker::fsOpened, this, &FsBrowserDialog::onFsOpened);
    connect(m_worker, &ImageWorker::fsExtracted, this, &FsBrowserDialog::onFsExtracted);
    connect(m_worker, &ImageWorker::fsReplaced, this, &FsBrowserDialog::onFsReplaced);
    connect(m_worker, &ImageWorker::fsRepacked, this, &FsBrowserDialog::onFsRepacked);

    setBusy(true); // 加载期间全禁（含替换按钮；加载后随选中项启用）
    setStatus(QStringLiteral("正在读取镜像并遍历文件树...\n%1")
                  .arg(m_canReplace
                           ? QStringLiteral("ext4 镜像：可替换文件（内存中修改，"
                                            "须「保存修改镜像」重打包落盘；带 "
                                            "metadata_csum 校验和的镜像由后端拒绝替换）")
                           : QStringLiteral("EROFS 只读镜像：仅可浏览与提取（后端 "
                                            "ErofsImage 无写回能力，压缩 LZ4 文件"
                                            "提取返回失败）")));
    m_worker->runFsOpen(imagePath);
}

FsBrowserDialog::~FsBrowserDialog()
{
    // 会话收尾：worker 串行执行，若仍有在途操作会在其后关闭；本对象析构后
    // worker 信号连接自动断开，迟到结果被丢弃（无副作用）
    if (m_worker && !m_imagePath.isEmpty())
        m_worker->runFsClose(m_imagePath);
}

void FsBrowserDialog::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);

    // 顶部信息卡
    QGroupBox *infoGroup = new QGroupBox(QStringLiteral("镜像信息"), this);
    QHBoxLayout *infoLayout = new QHBoxLayout(infoGroup);
    const QFileInfo fi(m_imagePath);
    QLabel *infoLabel = new QLabel(QStringLiteral("%1 · %2 · %3")
                                       .arg(fi.fileName(),
                                            m_canReplace ? QStringLiteral("ext4 镜像")
                                                         : QStringLiteral("EROFS 镜像"),
                                            formatSize(quint64(fi.size()))),
                                   infoGroup);
    infoLayout->addWidget(infoLabel);
    mainLayout->addWidget(infoGroup);

    // 目录树（名称 / 大小）
    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(2);
    m_tree->setHeaderLabels({ QStringLiteral("名称"), QStringLiteral("大小") });
    m_tree->setRootIsDecorated(true);
    m_tree->setAlternatingRowColors(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->setSortingEnabled(false); // 保持引擎条目顺序（目录在前、文件在后，各按名排序）
    m_tree->setToolTip(QStringLiteral("双击文件用系统程序打开；双击目录展开/折叠"));
    mainLayout->addWidget(m_tree, 1);

    // 操作按钮行
    QHBoxLayout *btnLayout = new QHBoxLayout();
    m_openBtn = new QPushButton(QStringLiteral("打开"), this);
    m_extractBtn = new QPushButton(QStringLiteral("提取到..."), this);
    m_replaceBtn = new QPushButton(QStringLiteral("替换文件..."), this);
    m_saveBtn = new QPushButton(QStringLiteral("保存修改镜像..."), this);
    QPushButton *closeBtn = new QPushButton(QStringLiteral("关闭"), this);
    m_openBtn->setEnabled(false);
    m_extractBtn->setEnabled(false);
    m_replaceBtn->setEnabled(false);
    m_saveBtn->setEnabled(false);
    btnLayout->addWidget(m_openBtn);
    btnLayout->addWidget(m_extractBtn);
    btnLayout->addWidget(m_replaceBtn);
    btnLayout->addWidget(m_saveBtn);
    btnLayout->addStretch();
    btnLayout->addWidget(closeBtn);
    mainLayout->addLayout(btnLayout);

    // 状态行 + 不定进度条
    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet(QStringLiteral("color: #7f8c8d;"));
    mainLayout->addWidget(m_statusLabel);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0); // 不定模式（引擎无逐块进度回调）
    m_progress->setVisible(false);
    mainLayout->addWidget(m_progress);

    // 信号接线
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *item, int) {
                if (!item)
                    return;
                const QString inner = item->data(0, Qt::UserRole).toString();
                // 目录项不登记在 m_items（文件项表）→ 双击交给默认展开/折叠；
                // 文件项 → 提取到临时文件并用系统程序打开
                if (!m_items.contains(inner))
                    return;
                requestOpen(inner);
            });
    connect(m_tree, &QTreeWidget::currentItemChanged, this,
            [this](QTreeWidgetItem *, QTreeWidgetItem *) { updateActionButtons(); });
    connect(m_openBtn, &QPushButton::clicked, this, [this] {
        const QString inner = currentInnerPath();
        if (!inner.isEmpty())
            requestOpen(inner);
    });
    connect(m_extractBtn, &QPushButton::clicked, this, [this] {
        const QString inner = currentInnerPath();
        if (inner.isEmpty())
            return;
        const QString suggested = QDir::homePath() + QLatin1Char('/')
            + QFileInfo(inner).fileName();
        const QString outPath = QFileDialog::getSaveFileName(
            this, QStringLiteral("提取到"), suggested);
        if (outPath.isEmpty())
            return;
        m_opening = false;
        setBusy(true);
        setStatus(QStringLiteral("正在提取 %1 ...").arg(inner));
        m_worker->runFsExtractTo(m_imagePath, inner, outPath);
    });
    connect(m_replaceBtn, &QPushButton::clicked, this, [this] {
        const QString inner = currentInnerPath();
        if (inner.isEmpty() || !m_canReplace)
            return;
        const QString hostFile = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择替换用的新文件"),
            QFileInfo(m_imagePath).absolutePath());
        if (hostFile.isEmpty())
            return;
        setBusy(true);
        setStatus(QStringLiteral("正在替换 %1 ...").arg(inner));
        m_worker->runFsReplace(m_imagePath, inner, hostFile);
    });
    connect(m_saveBtn, &QPushButton::clicked, this, [this] {
        if (m_modifiedCount <= 0)
            return;
        const QString base = QFileInfo(m_imagePath).completeBaseName();
        const QString suggested = QFileInfo(m_imagePath).absolutePath() + QLatin1Char('/')
            + base + QStringLiteral("_modified.img");
        const QString outPath = QFileDialog::getSaveFileName(
            this, QStringLiteral("保存修改镜像"), suggested);
        if (outPath.isEmpty())
            return;
        setBusy(true);
        setStatus(QStringLiteral("正在重打包并保存（镜像较大时可能需要一段时间）..."));
        m_worker->runFsRepack(m_imagePath, outPath);
    });
}

// 文件系统目录树：条目为扁平相对路径（listTree 语义），按父目录分组重建层级。
// 条目列表已含全部目录项 → 每目录一个节点；目录在前、文件在后，各按路径排序
// （与引擎输出顺序一致，确定性可测）。
void FsBrowserDialog::buildTree(const QList<imgfs::FsEntry> &entries)
{
    m_tree->clear();
    m_items.clear();

    // 父目录路径 → 子条目（"" = 根）
    QHash<QString, QList<imgfs::FsEntry>> byParent;
    for (const imgfs::FsEntry &e : entries) {
        const QString parent = QFileInfo(e.path).path();
        byParent[parent == QLatin1String(".") ? QString() : parent].append(e);
    }

    // 目录路径 → 节点（含未显式出现在列表中的中间目录兜底）
    QHash<QString, QTreeWidgetItem *> dirNodes;

    auto makeFileItem = [this](QTreeWidgetItem *parent, const imgfs::FsEntry &e) {
        QTreeWidgetItem *item = parent ? new QTreeWidgetItem(parent)
                                       : new QTreeWidgetItem(m_tree);
        item->setText(0, QFileInfo(e.path).fileName());
        item->setData(0, Qt::UserRole, e.path);   // 文件 innerPath
        item->setData(1, Qt::UserRole, e.size);
        item->setText(1, e.size ? formatSize(e.size) : QStringLiteral("0 B"));
        item->setToolTip(0, e.path);
        m_items.insert(e.path, item);
    };

    std::function<QTreeWidgetItem *(const QString &, QTreeWidgetItem *)> buildDir =
        [&](const QString &dirPath, QTreeWidgetItem *parentItem) -> QTreeWidgetItem * {
            QTreeWidgetItem *node = dirNodes.value(dirPath);
            if (!node) {
                node = new QTreeWidgetItem();
                node->setText(0, QFileInfo(dirPath).fileName());
                node->setData(0, Qt::UserRole, dirPath); // 目录 innerPath（仅定位用）
                node->setData(1, Qt::UserRole, QVariant());
                node->setToolTip(0, dirPath);
                dirNodes.insert(dirPath, node);
            }
            // 子条目：目录在前、文件在后，各按路径排序
            const QList<imgfs::FsEntry> children = byParent.value(dirPath);
            QList<imgfs::FsEntry> subDirs;
            QList<imgfs::FsEntry> files;
            for (const imgfs::FsEntry &e : children)
                (e.isDir ? subDirs : files).append(e);
            auto byPath = [](const imgfs::FsEntry &a, const imgfs::FsEntry &b) {
                return a.path < b.path;
            };
            std::sort(subDirs.begin(), subDirs.end(), byPath);
            std::sort(files.begin(), files.end(), byPath);
            for (const imgfs::FsEntry &d : std::as_const(subDirs))
                buildDir(d.path, node);
            for (const imgfs::FsEntry &f : std::as_const(files))
                makeFileItem(node, f);
            if (parentItem)
                parentItem->addChild(node);
            else
                m_tree->addTopLevelItem(node);
            return node;
        };

    // 根级（parent == ""）目录与文件
    const QList<imgfs::FsEntry> rootEntries = byParent.value(QString());
    for (const imgfs::FsEntry &e : rootEntries)
        if (e.isDir)
            buildDir(e.path, nullptr);
    for (const imgfs::FsEntry &e : rootEntries)
        if (!e.isDir)
            makeFileItem(nullptr, e);

    m_tree->expandToDepth(1);
    updateActionButtons();
}

QString FsBrowserDialog::currentInnerPath() const
{
    QTreeWidgetItem *cur = m_tree->currentItem();
    if (!cur)
        return QString();
    const QString inner = cur->data(0, Qt::UserRole).toString();
    // 仅文件项可操作（文件项登记在 m_items）
    return m_items.contains(inner) ? inner : QString();
}

void FsBrowserDialog::requestOpen(const QString &innerPath)
{
    m_opening = true;
    setBusy(true);
    setStatus(QStringLiteral("正在提取 %1 ...").arg(innerPath));
    m_worker->runFsExtractTo(m_imagePath, innerPath, QString());
}

void FsBrowserDialog::setBusy(bool busy)
{
    m_progress->setVisible(busy);
    m_tree->setEnabled(!busy);
    if (busy) {
        m_openBtn->setEnabled(false);
        m_extractBtn->setEnabled(false);
        m_replaceBtn->setEnabled(false);
        m_saveBtn->setEnabled(false);
    } else {
        updateActionButtons();
    }
}

void FsBrowserDialog::updateActionButtons()
{
    if (m_tree && !m_tree->isEnabled()) // busy 期间由 setBusy 全禁，不在此放开
        return;
    const bool hasFile = !currentInnerPath().isEmpty();
    m_openBtn->setEnabled(hasFile);
    m_extractBtn->setEnabled(hasFile);
    m_replaceBtn->setEnabled(hasFile && m_canReplace);
    m_saveBtn->setEnabled(m_modifiedCount > 0 && m_canReplace);
}

void FsBrowserDialog::setStatus(const QString &text, bool isError)
{
    m_statusLabel->setStyleSheet(isError ? QStringLiteral("color: #e74c3c;")
                                         : QStringLiteral("color: #7f8c8d;"));
    m_statusLabel->setText(text);
}

// ---------------- worker 结果回传 ----------------

void FsBrowserDialog::onFsOpened(const QString &path, bool ok,
                                 const QList<imgfs::FsEntry> &entries,
                                 const QString &error)
{
    if (path != m_imagePath)
        return; // 其他会话的打开结果（多对话框并存防御）
    setBusy(false);
    if (!ok) {
        m_tree->clear();
        setStatus(error.isEmpty() ? QStringLiteral("打开失败（未知错误）") : error, true);
        emit outputMessage(QStringLiteral("[文件浏览] 打开失败: %1").arg(error), true);
        return;
    }
    buildTree(entries);
    int dirs = 0;
    int files = 0;
    for (const imgfs::FsEntry &e : entries)
        (e.isDir ? dirs : files)++;
    setStatus(QStringLiteral("已加载 %1 个条目（目录 %2 / 文件 %3）· %4")
                  .arg(entries.size())
                  .arg(dirs)
                  .arg(files)
                  .arg(m_canReplace ? QStringLiteral("可替换文件（修改后须保存）")
                                    : QStringLiteral("只读镜像")));
    emit outputMessage(QStringLiteral("[文件浏览] %1 已打开（%2 个条目）")
                           .arg(QFileInfo(m_imagePath).fileName())
                           .arg(entries.size()), false);
}

void FsBrowserDialog::onFsExtracted(const QString &path, const QString &innerPath,
                                    const QString &outFile, bool ok,
                                    const QString &error)
{
    if (path != m_imagePath)
        return;
    setBusy(false);
    if (!ok) {
        m_opening = false;
        setStatus(error, true);
        emit outputMessage(QStringLiteral("[文件浏览] 提取 %1 失败: %2")
                               .arg(innerPath, error), true);
        return;
    }
    if (m_opening) {
        m_opening = false;
        const QUrl url = QUrl::fromLocalFile(outFile);
        if (!QDesktopServices::openUrl(url)) {
            setStatus(QStringLiteral("无法用系统程序打开 %1（可在文件管理器中手动打开）")
                          .arg(outFile), true);
            emit outputMessage(QStringLiteral("[文件浏览] 系统程序打开失败: %1").arg(outFile),
                               true);
            return;
        }
        setStatus(QStringLiteral("已用系统程序打开: %1").arg(outFile));
    } else {
        setStatus(QStringLiteral("已提取到: %1").arg(outFile));
    }
    emit outputMessage(QStringLiteral("[文件浏览] 已提取 %1 → %2").arg(innerPath, outFile),
                       false);
}

void FsBrowserDialog::onFsReplaced(const QString &path, const QString &innerPath,
                                   const QString &hostFile, bool ok,
                                   const QString &error)
{
    if (path != m_imagePath)
        return;
    setBusy(false);
    if (!ok) {
        setStatus(error, true);
        emit outputMessage(QStringLiteral("[文件浏览] 替换 %1 失败: %2")
                               .arg(innerPath, error), true);
        QMessageBox::warning(
            this, QStringLiteral("替换失败"),
            error + QStringLiteral("\n\n可能原因：EROFS 只读镜像 / ext4 带 "
                                   "metadata_csum 校验和 / 目录或符号链接 / "
                                   "legacy block map 布局（FsImage::replace 无错误明细，"
                                   "后端扩展待办）。"));
        return;
    }
    ++m_modifiedCount;
    updateActionButtons(); // setBusy(false) 时计数尚未递增，此处重新求值
    // 刷新大小列：新内容大小 = 宿主文件大小（会话内存中已替换）
    const quint64 newSize = quint64(QFileInfo(hostFile).size());
    QTreeWidgetItem *item = m_items.value(innerPath);
    if (item) {
        item->setData(1, Qt::UserRole, newSize);
        item->setText(1, formatSize(newSize));
    }
    setStatus(QStringLiteral("已替换 %1（%2）—— 已修改 %3 个文件，"
                             "保存修改镜像后落盘")
                  .arg(innerPath, formatSize(newSize)).arg(m_modifiedCount));
    emit outputMessage(QStringLiteral("[文件浏览] 已替换 %1 ← %2（%3）")
                           .arg(innerPath, hostFile, formatSize(newSize)), false);
}

void FsBrowserDialog::onFsRepacked(const QString &path, const QString &outPath,
                                   bool ok, const QString &error)
{
    if (path != m_imagePath)
        return;
    if (!ok) {
        setBusy(false); // 失败：修改未落盘，保持"已修改"状态（可重试保存）
        setStatus(error, true);
        emit outputMessage(QStringLiteral("[文件浏览] 保存失败: %1").arg(error), true);
        QMessageBox::warning(this, QStringLiteral("保存失败"), error);
        return;
    }
    m_modifiedCount = 0; // 已落盘：后续修改须再次保存（先清零，setBusy 内按 0 求值）
    setBusy(false);
    setStatus(QStringLiteral("已保存修改镜像: %1").arg(outPath));
    emit outputMessage(QStringLiteral("[文件浏览] 已保存修改镜像: %1").arg(outPath),
                       false);
}
