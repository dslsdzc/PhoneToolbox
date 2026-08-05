#include "image_tool_panel.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QFormLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QDateTime>
#include <QScrollBar>
#include <QMimeData>
#include <QUrl>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMenu>
#include <QAction>
#include <QPoint>

namespace {

// 格式枚举 → 展示名（与 registry.cpp detect() 的 detail 文案风格一致）
QString formatName(imgreg::Format f)
{
    switch (f) {
    case imgreg::Format::Payload:    return QStringLiteral("Payload OTA 包");
    case imgreg::Format::Zip:        return QStringLiteral("Zip 刷机包");
    case imgreg::Format::Tar:        return QStringLiteral("Tar 镜像");
    case imgreg::Format::TarMd5:     return QStringLiteral("Tar.md5 镜像");
    case imgreg::Format::Sparse:     return QStringLiteral("Sparse 稀疏镜像");
    case imgreg::Format::Super:      return QStringLiteral("Super 动态分区");
    case imgreg::Format::Boot:       return QStringLiteral("Boot 镜像");
    case imgreg::Format::VendorBoot: return QStringLiteral("Vendor_boot 镜像");
    case imgreg::Format::Vbmeta:     return QStringLiteral("Vbmeta 镜像");
    case imgreg::Format::Dtb:        return QStringLiteral("Dtb 镜像");
    case imgreg::Format::Br:         return QStringLiteral("Brotli 压缩流");
    case imgreg::Format::Lz4:        return QStringLiteral("LZ4 压缩流");
    case imgreg::Format::Xz:         return QStringLiteral("XZ 压缩流");
    case imgreg::Format::Gzip:       return QStringLiteral("Gzip 压缩流");
    case imgreg::Format::Zstd:       return QStringLiteral("Zstd 压缩流");
    case imgreg::Format::Brotli:     return QStringLiteral("Brotli 压缩流");
    case imgreg::Format::Dat:        return QStringLiteral("Dat 系统分区");
    case imgreg::Format::Pac:        return QStringLiteral("PAC 固件");
    case imgreg::Format::Kdz:        return QStringLiteral("KDZ 固件");
    case imgreg::Format::UpdateApp:  return QStringLiteral("Update.app 固件");
    case imgreg::Format::UpdateBin:  return QStringLiteral("Update.bin 固件");
    case imgreg::Format::Sin:        return QStringLiteral("SIN 镜像");
    case imgreg::Format::DiskGpt:    return QStringLiteral("GPT 磁盘镜像");
    case imgreg::Format::TwrpWin:    return QStringLiteral("TWRP 备份镜像");
    case imgreg::Format::Erofs:      return QStringLiteral("EROFS 分区镜像");
    case imgreg::Format::Ext4:       return QStringLiteral("Ext4 分区镜像");
    case imgreg::Format::RawImage:   return QStringLiteral("Raw 镜像");
    case imgreg::Format::Unknown:    return QStringLiteral("未知格式");
    }
    return QStringLiteral("未知格式");
}

} // namespace

ImageToolPanel::ImageToolPanel(QWidget *parent)
    : QWidget(parent)
    , m_fileLabel(nullptr)
    , m_formatLabel(nullptr)
    , m_detailLabel(nullptr)
    , m_backBtn(nullptr)
    , m_unpackBtn(nullptr)
    , m_packBtn(nullptr)
    , m_convertBtn(nullptr)
    , m_patchBtn(nullptr)
    , m_browseBtn(nullptr)
    , m_progressBar(nullptr)
    , m_logOutput(nullptr)
{
    setupUI();
    setupConnections();
    appendLog(QStringLiteral("提示: 将镜像文件拖入本面板，或点击「浏览...」选择文件"));
}

ImageToolPanel::~ImageToolPanel() = default;

void ImageToolPanel::setupUI()
{
    setAcceptDrops(true);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);

    // 顶部：标题 + 返回
    QHBoxLayout *headerLayout = new QHBoxLayout();
    QLabel *title = new QLabel(QStringLiteral("镜像工具"), this);
    title->setStyleSheet("font-size: 16px; font-weight: bold;");
    m_backBtn = new QPushButton(QStringLiteral("← 返回"), this);
    m_backBtn->setMaximumWidth(80);
    headerLayout->addWidget(title);
    headerLayout->addStretch();
    headerLayout->addWidget(m_backBtn);
    mainLayout->addLayout(headerLayout);

    // 镜像信息卡（detectFinished 回来更新）
    QGroupBox *infoGroup = new QGroupBox(QStringLiteral("镜像信息"), this);
    QFormLayout *infoLayout = new QFormLayout(infoGroup);
    m_fileLabel = new QLabel(QStringLiteral("（拖入镜像文件或点击浏览）"), this);
    m_fileLabel->setWordWrap(true);
    m_formatLabel = new QLabel(QStringLiteral("—"), this);
    m_detailLabel = new QLabel(QStringLiteral("—"), this);
    m_detailLabel->setWordWrap(true);
    infoLayout->addRow(QStringLiteral("文件:"), m_fileLabel);
    infoLayout->addRow(QStringLiteral("格式:"), m_formatLabel);
    infoLayout->addRow(QStringLiteral("详情:"), m_detailLabel);
    mainLayout->addWidget(infoGroup);

    // 动作按钮组（按识别结果动态 enable）
    QGroupBox *actionGroup = new QGroupBox(QStringLiteral("操作"), this);
    QHBoxLayout *actionLayout = new QHBoxLayout(actionGroup);
    m_unpackBtn = new QPushButton(QStringLiteral("解包"), this);
    m_packBtn = new QPushButton(QStringLiteral("打包"), this);
    m_convertBtn = new QPushButton(QStringLiteral("转换"), this);
    m_patchBtn = new QPushButton(QStringLiteral("修补"), this);
    m_browseBtn = new QPushButton(QStringLiteral("浏览..."), this);
    m_unpackBtn->setEnabled(false);
    m_packBtn->setEnabled(false);
    m_convertBtn->setEnabled(false);
    m_patchBtn->setEnabled(false);
    actionLayout->addWidget(m_unpackBtn);
    actionLayout->addWidget(m_packBtn);
    actionLayout->addWidget(m_convertBtn);
    actionLayout->addWidget(m_patchBtn);
    actionLayout->addStretch();
    actionLayout->addWidget(m_browseBtn);
    mainLayout->addWidget(actionGroup);

    // 进度条
    m_progressBar = new QProgressBar(this);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setVisible(false);
    mainLayout->addWidget(m_progressBar);

    // 日志区（撑满）
    m_logOutput = new QTextEdit(this);
    m_logOutput->setReadOnly(true);
    m_logOutput->setPlaceholderText(QStringLiteral("操作日志"));
    mainLayout->addWidget(m_logOutput, 1);
}

void ImageToolPanel::setupConnections()
{
    connect(m_backBtn, &QPushButton::clicked, this, &ImageToolPanel::switchToDeviceInfo);
    connect(m_browseBtn, &QPushButton::clicked, this, &ImageToolPanel::onBrowseClicked);
    connect(m_unpackBtn, &QPushButton::clicked, this, &ImageToolPanel::onUnpackClicked);
    connect(m_packBtn, &QPushButton::clicked, this, &ImageToolPanel::onPackClicked);
    connect(m_convertBtn, &QPushButton::clicked, this, &ImageToolPanel::onConvertClicked);
    connect(m_patchBtn, &QPushButton::clicked, this, &ImageToolPanel::onPatchClicked);

    // worker（工作线程）→ 面板（UI 线程），跨线程自动 QueuedConnection
    connect(&m_worker, &ImageWorker::detectFinished,
            this, &ImageToolPanel::onDetectFinished);
    connect(&m_worker, &ImageWorker::unpackFinished,
            this, &ImageToolPanel::onUnpackFinished);
    connect(&m_worker, &ImageWorker::packFinished,
            this, &ImageToolPanel::onPackFinished);
    connect(&m_worker, &ImageWorker::convertFinished,
            this, &ImageToolPanel::onConvertFinished);
    connect(&m_worker, &ImageWorker::patchFinished,
            this, &ImageToolPanel::onPatchFinished);
    connect(&m_worker, &ImageWorker::progress,
            this, &ImageToolPanel::onWorkerProgress);
}

void ImageToolPanel::dragEnterEvent(QDragEnterEvent *event)
{
    // 接受带本地文件 URL 的拖入（至少 1 个本地路径；isLocalFile 规范判定）
    if (event->mimeData()->hasUrls()) {
        for (const QUrl &url : event->mimeData()->urls()) {
            if (url.isLocalFile()) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    event->ignore();
}

void ImageToolPanel::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }
    // 取第一个本地文件路径
    for (const QUrl &url : event->mimeData()->urls()) {
        if (!url.isLocalFile())
            continue;
        const QString path = url.toLocalFile();
        if (!path.isEmpty()) {
            event->acceptProposedAction();
            startDetect(path);
            return;
        }
    }
    event->ignore();
}

void ImageToolPanel::onBrowseClicked()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("选择镜像文件"));
    if (!path.isEmpty())
        startDetect(path);
}

void ImageToolPanel::startDetect(const QString &path)
{
    if (path.isEmpty())
        return;

    m_currentFile = path;
    m_detected = imgreg::Detected{};
    m_fileLabel->setText(QFileInfo(path).fileName());
    m_formatLabel->setText(QStringLiteral("识别中..."));
    m_detailLabel->setText(QStringLiteral("正在读取文件头..."));
    m_unpackBtn->setEnabled(false);
    m_packBtn->setEnabled(false);
    m_convertBtn->setEnabled(false);
    m_patchBtn->setEnabled(false);
    m_progressBar->setValue(0);
    m_progressBar->setFormat(QString()); // 清理上次操作的 format 残留
    m_progressBar->setVisible(true);
    appendLog(QStringLiteral("开始识别: %1").arg(path));

    m_worker.runDetect(path); // 工作线程执行，UI 不阻塞
}

void ImageToolPanel::onDetectFinished(const QString &path, const imgreg::Detected &detected)
{
    m_detected = detected;
    m_formatLabel->setText(formatName(detected.format));
    m_detailLabel->setText(detected.detail.isEmpty() ? QStringLiteral("—") : detected.detail);
    updateButtonsFor(detected);

    if (detected.format == imgreg::Format::Unknown) {
        const QString reason = detected.detail.isEmpty()
            ? QStringLiteral("无法识别的文件格式")
            : detected.detail;
        appendLog(QStringLiteral("识别失败: %1").arg(reason), true);
        return;
    }
    appendLog(QStringLiteral("已识别: %1 → %2")
                  .arg(QFileInfo(path).fileName(), formatName(detected.format)));
}

void ImageToolPanel::updateButtonsFor(const imgreg::Detected &detected)
{
    const imgreg::Format f = detected.format;
    // 解包：容器/分区类镜像（纯压缩流不直接解包；Raw 本身即已解包格式）
    m_unpackBtn->setEnabled(f == imgreg::Format::Payload || f == imgreg::Format::Zip ||
                            f == imgreg::Format::Tar || f == imgreg::Format::TarMd5 ||
                            f == imgreg::Format::Sparse || f == imgreg::Format::Super ||
                            f == imgreg::Format::Boot || f == imgreg::Format::VendorBoot ||
                            f == imgreg::Format::Vbmeta || f == imgreg::Format::Dtb ||
                            f == imgreg::Format::Dat || f == imgreg::Format::Pac ||
                            f == imgreg::Format::Kdz || f == imgreg::Format::UpdateApp ||
                            f == imgreg::Format::UpdateBin || f == imgreg::Format::Sin ||
                            f == imgreg::Format::DiskGpt || f == imgreg::Format::TwrpWin ||
                            f == imgreg::Format::Erofs || f == imgreg::Format::Ext4);
    // 打包（D6 接线）：sparse→raw、镜像集→tar、payload 全量等
    m_packBtn->setEnabled(f == imgreg::Format::Sparse || f == imgreg::Format::RawImage ||
                          f == imgreg::Format::Tar || f == imgreg::Format::Payload);
    // 转换（D3 接线）：sparse↔raw
    m_convertBtn->setEnabled(f == imgreg::Format::Sparse || f == imgreg::Format::RawImage);
    // 修补（D4 接线）：boot 类镜像
    m_patchBtn->setEnabled(f == imgreg::Format::Boot || f == imgreg::Format::VendorBoot);
}

void ImageToolPanel::onWorkerProgress(int percent, const QString &stage)
{
    if (!m_progressBar->isVisible())
        m_progressBar->setVisible(true);
    m_progressBar->setValue(percent);
    m_progressBar->setFormat(QStringLiteral("%1 %2%").arg(stage).arg(percent));
    if (percent >= 100) {
        m_progressBar->setVisible(false);
        m_progressBar->setFormat(QString()); // 清理 format 残留，下次操作从默认格式开始
    }
}

void ImageToolPanel::onUnpackClicked()
{
    if (m_currentFile.isEmpty() || m_detected.format == imgreg::Format::Unknown)
        return;
    // D3 接线：弹目录选择框选输出目录（默认源文件所在目录）
    const QString outDir = QFileDialog::getExistingDirectory(
        this, QStringLiteral("选择解包输出目录"),
        QFileInfo(m_currentFile).absolutePath());
    if (outDir.isEmpty()) {
        appendLog(QStringLiteral("已取消解包（未选择输出目录）"));
        return;
    }
    appendLog(QStringLiteral("开始解包: %1 → %2").arg(m_currentFile, outDir));
    m_worker.runUnpack(m_currentFile, outDir, m_detected);
}

void ImageToolPanel::onPackClicked()
{
    if (m_currentFile.isEmpty() || m_detected.format == imgreg::Format::Unknown)
        return;
    appendLog(QStringLiteral("开始打包: %1").arg(m_currentFile));
    // D6 接线：输出路径/输入清单由后续任务提供；当前为占位参数
    m_worker.runPack(m_currentFile + QStringLiteral(".tar"),
                     QStringList{m_currentFile}, m_detected);
}

void ImageToolPanel::onConvertClicked()
{
    if (m_currentFile.isEmpty() || m_detected.format == imgreg::Format::Unknown)
        return;

    // D3 接线：目标类型选择（当前仅 sparse↔raw 两向，菜单内单选）
    const bool toSparse = (m_detected.format == imgreg::Format::RawImage);
    QMenu menu(this);
    menu.addAction(toSparse ? QStringLiteral("转换为 Sparse 镜像 (img2simg)")
                            : QStringLiteral("转换为 Raw 镜像 (simg2img)"));
    QAction *chosen = menu.exec(m_convertBtn->mapToGlobal(QPoint(0, m_convertBtn->height() + 2)));
    if (!chosen) {
        appendLog(QStringLiteral("已取消转换（未选择目标类型）"));
        return;
    }

    const QString base = QFileInfo(m_currentFile).completeBaseName();
    const QString suggested = QFileInfo(m_currentFile).absolutePath() + QLatin1Char('/')
        + base + (toSparse ? QStringLiteral(".sparse") : QStringLiteral(".raw"));
    const QString outPath = QFileDialog::getSaveFileName(
        this, toSparse ? QStringLiteral("选择 Sparse 输出文件")
                       : QStringLiteral("选择 Raw 输出文件"),
        suggested);
    if (outPath.isEmpty()) {
        appendLog(QStringLiteral("已取消转换（未选择输出文件）"));
        return;
    }

    appendLog(QStringLiteral("开始转换: %1 → %2").arg(m_currentFile, outPath));
    m_worker.runConvert(m_currentFile, outPath, m_detected);
}

void ImageToolPanel::onPatchClicked()
{
    if (m_currentFile.isEmpty() || m_detected.format == imgreg::Format::Unknown)
        return;
    appendLog(QStringLiteral("开始修补: %1").arg(m_currentFile));
    // D4 接线：PatchConfig 由后续任务提供；当前为默认配置占位
    patcher::PatchConfig config;
    m_worker.runPatch(m_currentFile, config);
}

void ImageToolPanel::onUnpackFinished(bool ok, const QStringList &outputs, const QString &error)
{
    // D2 吸收：收尾清理进度条（含失败路径，防卡 0%）
    m_progressBar->setVisible(false);
    m_progressBar->setFormat(QString());
    if (!ok) {
        appendLog(QStringLiteral("解包失败: %1").arg(error), true);
        return;
    }
    appendLog(QStringLiteral("解包完成: 共 %1 个产物").arg(outputs.size()));
    const int shown = qMin(outputs.size(), 50);
    for (int i = 0; i < shown; ++i)
        appendLog(QStringLiteral("  ✔ %1").arg(outputs.at(i)));
    if (outputs.size() > shown)
        appendLog(QStringLiteral("  ... 其余 %1 项未列出").arg(outputs.size() - shown));
}

void ImageToolPanel::onPackFinished(bool ok, const QString &output, const QString &error)
{
    m_progressBar->setVisible(false);
    m_progressBar->setFormat(QString());
    if (ok)
        appendLog(QStringLiteral("打包完成: %1").arg(output));
    else
        appendLog(QStringLiteral("打包失败: %1").arg(error), true);
}

void ImageToolPanel::onConvertFinished(bool ok, const QString &output, const QString &error)
{
    m_progressBar->setVisible(false);
    m_progressBar->setFormat(QString());
    if (ok)
        appendLog(QStringLiteral("转换完成: %1").arg(output));
    else
        appendLog(QStringLiteral("转换失败: %1").arg(error), true);
}

void ImageToolPanel::onPatchFinished(bool ok, const QString &output, const QString &error)
{
    m_progressBar->setVisible(false);
    m_progressBar->setFormat(QString());
    if (ok)
        appendLog(QStringLiteral("修补完成: %1").arg(output));
    else
        appendLog(QStringLiteral("修补失败: %1").arg(error), true);
}

void ImageToolPanel::appendLog(const QString &msg, bool isError)
{
    if (msg.isEmpty()) {
        m_logOutput->insertHtml(QStringLiteral("<br>"));
        m_logOutput->verticalScrollBar()->setValue(m_logOutput->verticalScrollBar()->maximum());
        return;
    }

    const QString timestamp = QDateTime::currentDateTime().toString("[HH:mm:ss] ");
    QString color = isError ? QStringLiteral("#e74c3c") : QStringLiteral("#2c3e50");
    if (msg.contains(QStringLiteral("✔")))
        color = QStringLiteral("#27ae60");
    else if (msg.contains(QStringLiteral("✘")))
        color = QStringLiteral("#e74c3c");
    else if (msg.contains(QStringLiteral("⚠")))
        color = QStringLiteral("#e67e22");

    m_logOutput->insertHtml(QStringLiteral("<span style='color: %1;'>%2%3</span><br>")
                                .arg(color, timestamp, msg.toHtmlEscaped()));
    m_logOutput->verticalScrollBar()->setValue(m_logOutput->verticalScrollBar()->maximum());
    emit outputMessage(msg, isError);
}
