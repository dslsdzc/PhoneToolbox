#include "flash_panel.h"
#include "core/filename_parser.h"
#include "core/restart_tool.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QFrame>
#include <QCheckBox>
#include <QApplication>

FlashPanel::FlashPanel(QWidget *parent)
    : QWidget(parent)
    , m_flashTool(new FlashTool(this))
{
    setupUI();

    connect(m_flashTool, &FlashTool::outputMessage,
            this, &FlashPanel::onToolOutput);
    connect(m_flashTool, &FlashTool::flashProgress,
            this, &FlashPanel::onToolProgress);
}

void FlashPanel::setupUI()
{
    setAcceptDrops(true);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(5, 5, 5, 5);
    mainLayout->setSpacing(4);

    // 顶部：设备信息 + 选择文件 + 返回按钮
    QHBoxLayout *topLayout = new QHBoxLayout();
    m_deviceLabel = new QLabel("设备: 未连接", this);
    m_modeLabel = new QLabel("模式: -", this);
    m_selectFileBtn = new QPushButton("选择文件", this);
    m_backBtn = new QPushButton("返回", this);
    topLayout->addWidget(m_deviceLabel);
    topLayout->addWidget(m_modeLabel);
    topLayout->addStretch();
    topLayout->addWidget(m_selectFileBtn);

    // EDL 模式控件 (默认隐藏)
    m_edlSelectProgBtn = new QPushButton("选择 Programmer", this);
    m_edlConnectBtn = new QPushButton("连接 EDL", this);
    m_edlDisconnectBtn = new QPushButton("断开 EDL", this);
    m_edlStatusLabel = new QLabel("EDL: 未连接", this);
    m_edlSelectProgBtn->hide();
    m_edlConnectBtn->hide();
    m_edlDisconnectBtn->hide();
    m_edlStatusLabel->hide();

    topLayout->addWidget(m_edlSelectProgBtn);
    topLayout->addWidget(m_edlConnectBtn);
    topLayout->addWidget(m_edlDisconnectBtn);
    topLayout->addWidget(m_edlStatusLabel);

    // MTK 模式控件 (默认隐藏)
    m_mtkConnectBtn = new QPushButton("连接 MTK", this);
    m_mtkDisconnectBtn = new QPushButton("断开 MTK", this);
    m_mtkStatusLabel = new QLabel("MTK: 未连接", this);
    m_mtkConnectBtn->hide();
    m_mtkDisconnectBtn->hide();
    m_mtkStatusLabel->hide();

    topLayout->addWidget(m_mtkConnectBtn);
    topLayout->addWidget(m_mtkDisconnectBtn);
    topLayout->addWidget(m_mtkStatusLabel);

    topLayout->addWidget(m_backBtn);
    mainLayout->addLayout(topLayout);

    // 文件信息
    m_fileLabel = new QLabel(this);
    m_fileLabel->setWordWrap(true);
    m_fileLabel->hide();

    m_romInfoLabel = new QLabel(this);
    m_romInfoLabel->setWordWrap(true);
    m_romInfoLabel->setStyleSheet("color: palette(text); font-size: 12px;");
    m_romInfoLabel->hide();

    m_romSuggestionLabel = new QLabel(this);
    m_romSuggestionLabel->setWordWrap(true);
    m_romSuggestionLabel->setStyleSheet("color: #2a82da; font-size: 12px;");
    m_romSuggestionLabel->hide();

    mainLayout->addWidget(m_fileLabel);
    mainLayout->addWidget(m_romInfoLabel);
    mainLayout->addWidget(m_romSuggestionLabel);

    // 分区列表 (自动撑满)
    QGroupBox *partGroup = new QGroupBox("分区列表", this);
    QVBoxLayout *partLayout = new QVBoxLayout(partGroup);

    m_partitionList = new QListWidget(this);
    m_partitionList->setSelectionMode(QAbstractItemView::SingleSelection);

    m_refreshPartitionsBtn = new QPushButton("刷新分区", this);

    partLayout->addWidget(m_partitionList, 1);
    partLayout->addWidget(m_refreshPartitionsBtn);
    mainLayout->addWidget(partGroup, 1);

    // 操作按钮
    QHBoxLayout *actionLayout = new QHBoxLayout();
    m_flashBtn = new QPushButton("刷入", this);
    m_eraseBtn = new QPushButton("擦除", this);
    m_dumpBtn = new QPushButton("读取", this);
    m_updateBtn = new QPushButton("完整包", this);
    m_sideloadBtn = new QPushButton("Sideload", this);

    m_flashBtn->setEnabled(false);
    m_eraseBtn->setEnabled(false);
    m_dumpBtn->setEnabled(false);
    m_updateBtn->setEnabled(false);
    m_sideloadBtn->setEnabled(false);

    actionLayout->addWidget(m_flashBtn);
    actionLayout->addWidget(m_eraseBtn);
    actionLayout->addWidget(m_dumpBtn);
    actionLayout->addWidget(m_updateBtn);
    actionLayout->addWidget(m_sideloadBtn);
    mainLayout->addLayout(actionLayout);

    // 脚本工具
    QHBoxLayout *scriptLayout = new QHBoxLayout();
    m_runScriptBtn = new QPushButton("运行脚本", this);
    m_runScriptBtn->setEnabled(false);
    m_bootImgBtn = new QPushButton("启动临时镜像", this);
    m_bootImgBtn->setEnabled(false);
    m_unlockBtn = new QPushButton("解锁 BL", this);
    m_unlockBtn->setEnabled(false);
    m_lockBtn = new QPushButton("回锁 BL", this);
    m_lockBtn->setEnabled(false);
    m_frpBtn = new QPushButton("清除 FRP", this);
    m_frpBtn->setEnabled(false);
    m_brickRepairBtn = new QPushButton("死砖修复", this);
    m_brickRepairBtn->setEnabled(false);

    scriptLayout->addWidget(m_runScriptBtn);
    scriptLayout->addWidget(m_bootImgBtn);
    scriptLayout->addWidget(m_unlockBtn);
    scriptLayout->addWidget(m_lockBtn);
    scriptLayout->addWidget(m_frpBtn);
    scriptLayout->addWidget(m_brickRepairBtn);
    scriptLayout->addStretch();
    mainLayout->addLayout(scriptLayout);

    // 进度条
    m_progressBar = new QProgressBar(this);
    m_progressBar->setVisible(false);
    mainLayout->addWidget(m_progressBar);

    mainLayout->addStretch();

    // 信号连接
    connect(m_selectFileBtn, &QPushButton::clicked,
            this, &FlashPanel::onSelectFile);
    connect(m_backBtn, &QPushButton::clicked,
            this, &FlashPanel::switchToDeviceInfo);
    connect(m_refreshPartitionsBtn, &QPushButton::clicked,
            this, &FlashPanel::onRefreshPartitions);
    connect(m_flashBtn, &QPushButton::clicked,
            this, &FlashPanel::onFlashClicked);
    connect(m_eraseBtn, &QPushButton::clicked,
            this, &FlashPanel::onEraseClicked);
    connect(m_updateBtn, &QPushButton::clicked,
            this, &FlashPanel::onUpdateClicked);
    connect(m_sideloadBtn, &QPushButton::clicked,
            this, &FlashPanel::onSideloadClicked);
    connect(m_runScriptBtn, &QPushButton::clicked,
            this, &FlashPanel::onRunScriptClicked);
    connect(m_dumpBtn, &QPushButton::clicked,
            this, &FlashPanel::onDumpClicked);
    connect(m_bootImgBtn, &QPushButton::clicked,
            this, &FlashPanel::onBootImageClicked);
    connect(m_partitionList, &QListWidget::itemSelectionChanged,
            this, &FlashPanel::onPartitionSelectionChanged);

    // EDL 信号
    connect(m_edlSelectProgBtn, &QPushButton::clicked,
            this, &FlashPanel::onEdlSelectProgrammer);
    connect(m_edlConnectBtn, &QPushButton::clicked,
            this, &FlashPanel::onEdlConnect);
    connect(m_edlDisconnectBtn, &QPushButton::clicked,
            this, &FlashPanel::onEdlDisconnect);

    // MTK 信号
    connect(m_mtkConnectBtn, &QPushButton::clicked,
            this, &FlashPanel::onMtkConnect);
    connect(m_mtkDisconnectBtn, &QPushButton::clicked,
            this, &FlashPanel::onMtkDisconnect);

    // BL 解锁/回锁
    connect(m_unlockBtn, &QPushButton::clicked,
            this, &FlashPanel::onUnlockBootloader);
    connect(m_lockBtn, &QPushButton::clicked,
            this, &FlashPanel::onLockBootloader);

    // FRP 清除
    connect(m_frpBtn, &QPushButton::clicked,
            this, &FlashPanel::onFrpErase);

    // 死砖修复
    connect(m_brickRepairBtn, &QPushButton::clicked,
            this, &FlashPanel::onBrickRepairClicked);
}

void FlashPanel::setDeviceInfo(const DeviceInfo &info)
{
    m_deviceInfo = info;
    m_deviceLabel->setText(QString("设备: %1").arg(
        info.serialNumber.isEmpty() ? "未知" : info.serialNumber));

    QString modeStr;
    bool fastbootMode = false;
    bool recoveryMode = false;
    switch (info.mode) {
    case DeviceDetector::MODE_FASTBOOT:
        modeStr = "Fastboot"; fastbootMode = true; break;
    case DeviceDetector::MODE_FASTBOOTD:
        modeStr = "Fastbootd"; fastbootMode = true; break;
    case DeviceDetector::MODE_ADB:
        modeStr = "ADB"; break;
    case DeviceDetector::MODE_RECOVERY:
        modeStr = "Recovery"; recoveryMode = true; break;
    case DeviceDetector::MODE_EDL_9008:
        modeStr = "EDL 9008"; break;
    case DeviceDetector::MODE_MTK_DA:
        modeStr = "MTK DA"; break;
    default:
        modeStr = "未知"; break;
    }
    m_modeLabel->setText(QString("模式: %1").arg(modeStr));

    m_edlSelectProgBtn->hide();
    m_edlConnectBtn->hide();
    m_edlDisconnectBtn->hide();
    m_edlStatusLabel->hide();
    m_mtkConnectBtn->hide();
    m_mtkDisconnectBtn->hide();
    m_mtkStatusLabel->hide();
    m_flashBtn->setEnabled(false);
    m_eraseBtn->setEnabled(false);
    m_dumpBtn->setEnabled(false);
    m_updateBtn->setEnabled(false);
    m_sideloadBtn->setEnabled(false);
    m_bootImgBtn->setEnabled(false);
    m_unlockBtn->setEnabled(false);
    m_lockBtn->setEnabled(false);
    m_frpBtn->setEnabled(false);
    m_brickRepairBtn->setEnabled(false);

    if (info.mode == DeviceDetector::MODE_EDL_9008) {
        // EDL 模式: 显示连接控件
        m_edlSelectProgBtn->show();
        m_edlConnectBtn->show();
        m_edlDisconnectBtn->show();
        m_edlStatusLabel->show();
        m_edlStatusLabel->setText(m_flashTool->edlIsConnected() ? "EDL: 已连接" : "EDL: 未连接");
        m_edlDisconnectBtn->setEnabled(m_flashTool->edlIsConnected());

        // 已连接 EDL Firehose 后显示分区操作
        if (m_flashTool->edlIsConnected()) {
            m_dumpBtn->setEnabled(true);
            m_frpBtn->setEnabled(true);
            m_brickRepairBtn->setEnabled(true);
            if (!m_currentFile.isEmpty() && m_currentFile.toLower().endsWith(".img"))
                m_flashBtn->setEnabled(m_partitionList->currentItem() != nullptr);
        }
        return; // EDL 不走后面的逻辑
    }

    if (info.mode == DeviceDetector::MODE_MTK_DA) {
        // MTK DA 模式: 显示连接控件
        m_mtkConnectBtn->show();
        m_mtkDisconnectBtn->show();
        m_mtkStatusLabel->show();
        m_mtkStatusLabel->setText(m_flashTool->mtkIsConnected() ? "MTK: 已连接" : "MTK: 未连接");
        m_mtkDisconnectBtn->setEnabled(m_flashTool->mtkIsConnected());

        // 已连接后显示分区操作
        if (m_flashTool->mtkIsConnected()) {
            m_dumpBtn->setEnabled(true);
            m_frpBtn->setEnabled(true);
            m_brickRepairBtn->setEnabled(true);
            if (!m_currentFile.isEmpty() && m_currentFile.toLower().endsWith(".img"))
                m_flashBtn->setEnabled(m_partitionList->currentItem() != nullptr);
        }

        // MTK BL 解锁/回锁需要连接
        if (m_flashTool->mtkIsConnected()) {
            m_unlockBtn->setEnabled(true);
            m_lockBtn->setEnabled(true);
        }
        return; // MTK 不走后面的逻辑
    }

    // 选中设备自动刷新分区
    onRefreshPartitions();

    if (fastbootMode) {
        m_unlockBtn->setEnabled(true);
        m_lockBtn->setEnabled(true);
        m_sideloadBtn->setEnabled(false);
        m_dumpBtn->setEnabled(true);
        m_bootImgBtn->setEnabled(!m_currentFile.isEmpty() &&
                                  (m_currentFile.toLower().endsWith(".img")));
        if (!m_currentFile.isEmpty()) {
            QString lower = m_currentFile.toLower();
            if (lower.endsWith(".zip"))
                m_updateBtn->setEnabled(true);
        }
        m_frpBtn->setEnabled(true);
    } else if (recoveryMode) {
        m_sideloadBtn->setEnabled(!m_currentFile.isEmpty() &&
                                   m_currentFile.toLower().endsWith(".zip"));
    } else if (m_deviceInfo.mode == DeviceDetector::MODE_ADB) {
        m_dumpBtn->setEnabled(true);
        if (m_deviceInfo.isRooted)
            m_frpBtn->setEnabled(true);
    }
}

void FlashPanel::clearDeviceInfo()
{
    m_deviceInfo = DeviceInfo();
    m_deviceLabel->setText("设备: 未连接");
    m_modeLabel->setText("模式: -");
    m_partitionList->clear();
    m_fileLabel->hide();
    m_romInfoLabel->hide();
    m_romSuggestionLabel->hide();
    m_progressBar->setVisible(false);
    m_flashBtn->setEnabled(false);
    m_eraseBtn->setEnabled(false);
    m_dumpBtn->setEnabled(false);
    m_updateBtn->setEnabled(false);
    m_sideloadBtn->setEnabled(false);
    m_runScriptBtn->setEnabled(false);
    m_bootImgBtn->setEnabled(false);
    m_brickRepairBtn->setEnabled(false);
}

// ==================== 拖放 ====================

void FlashPanel::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls()) {
        for (const QUrl &url : event->mimeData()->urls()) {
            QString path = url.toLocalFile().toLower();
            if (path.endsWith(".zip") || path.endsWith(".tar") ||
                path.endsWith(".tar.md5") || path.endsWith(".img") ||
                path.endsWith(".gz") || path.endsWith(".br")) {
                event->acceptProposedAction();
                return;
            }
            // 也接受脚本文件
            if (path.endsWith(".sh") || path.endsWith(".bat")) {
                event->acceptProposedAction();
                return;
            }
        }
    }
    event->ignore();
}

void FlashPanel::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasUrls()) return;

    for (const QUrl &url : event->mimeData()->urls()) {
        QString filePath = url.toLocalFile();
        if (!filePath.isEmpty()) {
            updateFileInfo(filePath);
            break;
        }
    }
}

// ==================== 文件处理 ====================

void FlashPanel::onSelectFile()
{
    QString filePath = QFileDialog::getOpenFileName(
        this, "选择刷机包",
        QString(),
        "刷机包 (*.zip *.tar *.tar.md5 *.img *.gz *.br);;"
        "脚本 (*.sh *.bat);;所有文件 (*)");

    if (!filePath.isEmpty()) {
        updateFileInfo(filePath);
    }
}

void FlashPanel::updateFileInfo(const QString &filePath)
{
    m_currentFile = filePath;
    QFileInfo fi(filePath);

    m_fileLabel->setText(QString("已选: %1 (%2)")
                         .arg(fi.fileName())
                         .arg(fi.size() > 1024 * 1024 * 1024
                              ? QString("%1 GB").arg(fi.size() / (1024.0 * 1024 * 1024), 0, 'f', 1)
                              : QString("%1 MB").arg(fi.size() / (1024.0 * 1024), 0, 'f', 0)));
    m_fileLabel->show();

    QString lower = fi.fileName().toLower();

    // 检测是否为刷机脚本
    if (lower.endsWith(".sh") || lower.endsWith(".bat")) {
        m_romInfoLabel->setText(QStringLiteral("刷机脚本，点击「运行脚本」执行"));
        m_romInfoLabel->show();
        m_romSuggestionLabel->hide();
        m_runScriptBtn->setEnabled(true);
        m_updateBtn->setEnabled(false);
        m_sideloadBtn->setEnabled(false);
        return;
    }

    m_runScriptBtn->setEnabled(false);

    // 解析文件名
    RomInfo rom = FilenameParser::parse(filePath);
    if (rom.valid) {
        QString info;
        if (!rom.manufacturer.isEmpty())
            info += QString("制造商: %1  ").arg(rom.manufacturer);
        if (!rom.model.isEmpty())
            info += QString("型号: %1  ").arg(rom.model);
        if (!rom.buildVersion.isEmpty())
            info += QString("版本: %1  ").arg(rom.buildVersion);
        if (!rom.androidVersion.isEmpty())
            info += QString("Android: %1").arg(rom.androidVersion);

        m_romInfoLabel->setText(info.trimmed());
        m_romInfoLabel->show();

        QString suggestion;
        if (lower.endsWith(".zip"))
            suggestion = QStringLiteral("zip 包可用「完整包」刷入，或解压后选 .img 单分区刷入");
        else if (lower.endsWith(".img"))
            suggestion = QStringLiteral("选中分区后点击「刷入」");
        else if (lower.endsWith(".br"))
            suggestion = "Brotli 压缩 (.br)，需要先解压为 .img";

        if (!suggestion.isEmpty()) {
            m_romSuggestionLabel->setText(suggestion);
            m_romSuggestionLabel->show();
        }
    } else {
        m_romInfoLabel->setText("未能识别机型信息");
        m_romInfoLabel->show();
        m_romSuggestionLabel->hide();
    }

    // 根据模式和文件类型启用按钮
    int mode = m_deviceInfo.mode;
    if (mode == DeviceDetector::MODE_FASTBOOT || mode == DeviceDetector::MODE_FASTBOOTD) {
        if (lower.endsWith(".zip"))
            m_updateBtn->setEnabled(true);
        if (lower.endsWith(".img") && m_partitionList->currentItem())
            m_flashBtn->setEnabled(true);
    } else if (mode == DeviceDetector::MODE_RECOVERY) {
        if (lower.endsWith(".zip"))
            m_sideloadBtn->setEnabled(true);
    }
}

// ==================== 分区 ====================

void FlashPanel::onRefreshPartitions()
{
    if (m_deviceInfo.serialNumber.isEmpty()) return;

    int mode = m_deviceInfo.mode;
    bool fastbootMode = (mode == DeviceDetector::MODE_FASTBOOT ||
                         mode == DeviceDetector::MODE_FASTBOOTD);

    m_partitionList->clear();
    m_partitions.clear();

    if (mode == DeviceDetector::MODE_EDL_9008) {
        // EDL 模式: 通过 Firehose 获取分区
        if (!m_flashTool->edlIsConnected()) {
            emit outputMessage("EDL 未连接，请先连接 EDL", false);
            return;
        }
        QList<EDLPartition> edlParts = m_flashTool->edlListPartitions();
        for (const auto &p : edlParts) {
            QString display = p.name;
            if (p.numSectors > 0)
                display += QString("  [%1 sectors]").arg(p.numSectors);
            QListWidgetItem *item = new QListWidgetItem(display, m_partitionList);
            item->setData(Qt::UserRole, p.name);
            m_partitions.append(p.name);
        }
        emit outputMessage(QString("EDL 检测到 %1 个分区").arg(m_partitions.size()), false);
        return;
    }

    if (mode == DeviceDetector::MODE_MTK_DA) {
        // MTK 模式: 通过 mtkclient 获取分区
        if (!m_flashTool->mtkIsConnected()) {
            emit outputMessage("MTK 未连接，请先连接 MTK", false);
            return;
        }
        QList<MtkPartition> mtkParts = m_flashTool->mtkListPartitions();
        for (const auto &p : mtkParts) {
            QString display = p.name;
            if (p.length > 0)
                display += QString("  [%1 bytes]").arg(p.length);
            QListWidgetItem *item = new QListWidgetItem(display, m_partitionList);
            item->setData(Qt::UserRole, p.name);
            m_partitions.append(p.name);
        }
        emit outputMessage(QString("MTK 检测到 %1 个分区").arg(m_partitions.size()), false);
        return;
    }

    QList<FlashTool::PartitionInfo> details;
    if (fastbootMode) {
        details = m_flashTool->getPartitionDetails(m_deviceInfo.serialNumber);
    } else {
        details = m_flashTool->getPartitionDetailsAdb(m_deviceInfo.serialNumber);
    }
    for (const auto &pd : details) {
        QString display = pd.name;
        if (!pd.size.isEmpty())
            display += QString("  [%1]").arg(pd.size);
        if (!pd.type.isEmpty())
            display += QString("  %1").arg(pd.type);
        QListWidgetItem *item = new QListWidgetItem(display, m_partitionList);
        item->setData(Qt::UserRole, pd.name);
        m_partitions.append(pd.name);
    }
    emit outputMessage(QString("已检测到 %1 个分区").arg(m_partitions.size()), false);
}

void FlashPanel::onPartitionSelectionChanged()
{
    QListWidgetItem *item = m_partitionList->currentItem();
    if (item) {
        QString part = item->data(Qt::UserRole).toString();
        QString lower = m_currentFile.toLower();

        if (m_deviceInfo.mode == DeviceDetector::MODE_EDL_9008) {
            // EDL 模式: 只启用刷入和读取
            m_flashBtn->setEnabled(m_flashTool->edlIsConnected() && lower.endsWith(".img"));
            m_eraseBtn->setEnabled(false);
        } else if (m_deviceInfo.mode == DeviceDetector::MODE_MTK_DA) {
            // MTK 模式: 只启用刷入和读取
            m_flashBtn->setEnabled(m_flashTool->mtkIsConnected() && lower.endsWith(".img"));
            m_eraseBtn->setEnabled(false);
        } else {
            m_flashBtn->setEnabled(lower.endsWith(".img"));
            m_eraseBtn->setEnabled(true);
        }
    } else {
        m_flashBtn->setEnabled(false);
        m_eraseBtn->setEnabled(false);
    }
}

// ==================== 操作（含确认和BL检测） ====================

bool FlashPanel::checkBootloaderUnlock(const QString &deviceId)
{
    int status = m_flashTool->checkBootloaderStatus(deviceId);
    if (status == 1) return true; // 已解锁

    if (status == 0) {
        QMessageBox::StandardButton reply = QMessageBox::warning(
            this, "Bootloader 未解锁",
            "设备 Bootloader 处于锁定状态，刷入可能失败！\n\n"
            "是否继续？",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        return reply == QMessageBox::Yes;
    }

    // status == -1 (未知)
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, "Bootloader 状态未知",
        "无法检测 Bootloader 解锁状态。\n\n是否继续刷入？",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    return reply == QMessageBox::Yes;
}

void FlashPanel::onFlashClicked()
{
    QListWidgetItem *item = m_partitionList->currentItem();
    if (!item || m_currentFile.isEmpty()) return;

    QString partition = item->data(Qt::UserRole).toString();
    QString deviceId = m_deviceInfo.serialNumber;
    int mode = m_deviceInfo.mode;
    bool fastbootMode = (mode == DeviceDetector::MODE_FASTBOOT ||
                         mode == DeviceDetector::MODE_FASTBOOTD);
    bool adbMode = (mode == DeviceDetector::MODE_ADB);
    bool edlMode = (mode == DeviceDetector::MODE_EDL_9008);

    bool mtkMode = (mode == DeviceDetector::MODE_MTK_DA);

    if (!fastbootMode && !adbMode && !edlMode && !mtkMode) {
        QMessageBox::information(this, "提示",
            "当前模式不支持刷入操作。\n需要 Fastboot / ADB(需Root) / EDL / MTK 模式。");
        return;
    }

    QString modeLabel = fastbootMode ? "Fastboot" :
                        edlMode ? "EDL" :
                        mtkMode ? "MTK" : "ADB + Root";
    QMessageBox::StandardButton confirm = QMessageBox::question(
        this, "确认刷入",
        QString("即将通过 %1 刷入:\n  分区: %2\n  文件: %3\n\n此操作不可逆，是否继续？")
        .arg(modeLabel, partition, QFileInfo(m_currentFile).fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (confirm != QMessageBox::Yes) return;

    // BL锁检测 (fastboot 模式需要)
    if (fastbootMode && !checkBootloaderUnlock(deviceId)) return;

    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    m_flashBtn->setEnabled(false);
    m_eraseBtn->setEnabled(false);

    if (edlMode) {
        // 查找 EDL 分区信息
        QList<EDLPartition> parts = m_flashTool->edlListPartitions();
        for (const EDLPartition &p : parts) {
            if (p.name == partition) {
                m_flashTool->edlWritePartition(p, m_currentFile);
                return;
            }
        }
        emit outputMessage(QString("EDL 未找到分区: %1").arg(partition), true);
    } else if (mtkMode) {
        m_flashTool->mtkWritePartition(partition, m_currentFile);
    } else if (fastbootMode) {
        m_flashTool->flashPartition(deviceId, partition, m_currentFile);
    } else {
        m_flashTool->adbFlashPartition(deviceId, partition, m_currentFile);
    }

    // MTK 和 EDL 模式刷完后不询问重启
    if (edlMode || mtkMode) return;

    // 刷完后询问重启
    QMessageBox::StandardButton reboot = QMessageBox::question(
        this, "刷入完成", "是否重启设备？",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (reboot == QMessageBox::Yes) {
        RestartTool rest;
        rest.restartDevice(deviceId,
            static_cast<DeviceDetector::DeviceMode>(m_deviceInfo.mode),
            RestartTool::MODE_SYSTEM);
    }
}

void FlashPanel::onEraseClicked()
{
    QListWidgetItem *item = m_partitionList->currentItem();
    if (!item) return;

    QString partition = item->text();
    QString deviceId = m_deviceInfo.serialNumber;

    QMessageBox::StandardButton confirm = QMessageBox::warning(
        this, "确认擦除",
        QString("即将擦除分区: %1\n\n此操作不可逆！数据将永久丢失！\n是否继续？")
        .arg(partition),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (confirm != QMessageBox::Yes) return;

    if (!checkBootloaderUnlock(deviceId)) return;

    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    m_flashBtn->setEnabled(false);
    m_eraseBtn->setEnabled(false);

    m_flashTool->erasePartition(deviceId, partition);
}


void FlashPanel::onUpdateClicked()
{
    if (m_currentFile.isEmpty()) return;

    QString deviceId = m_deviceInfo.serialNumber;

    QMessageBox::StandardButton confirm = QMessageBox::question(
        this, "确认刷入完整包",
        QString("即将刷入完整包: %1\n\n此操作不可逆，是否继续？")
        .arg(QFileInfo(m_currentFile).fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (confirm != QMessageBox::Yes) return;

    if (!checkBootloaderUnlock(deviceId)) return;

    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    m_flashBtn->setEnabled(false);
    m_eraseBtn->setEnabled(false);
    m_updateBtn->setEnabled(false);

    m_flashTool->flashUpdate(deviceId, m_currentFile);

    // 刷完后询问重启
    QMessageBox::StandardButton reboot = QMessageBox::question(
        this, "刷入完成", "是否重启设备？",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
    if (reboot == QMessageBox::Yes) {
        RestartTool rest;
        rest.restartDevice(deviceId,
            static_cast<DeviceDetector::DeviceMode>(m_deviceInfo.mode),
            RestartTool::MODE_SYSTEM);
    }
}

void FlashPanel::onSideloadClicked()
{
    if (m_currentFile.isEmpty()) return;

    QString deviceId = m_deviceInfo.serialNumber;

    QMessageBox::StandardButton confirm = QMessageBox::question(
        this, "确认 ADB Sideload",
        QString("将通过 ADB Sideload 刷入:\n%1\n\n"
                "请确保设备已在 Recovery 模式下并选择\n"
                "'Apply update from ADB' 或类似选项。\n\n是否继续？")
        .arg(QFileInfo(m_currentFile).fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (confirm != QMessageBox::Yes) return;

    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    m_sideloadBtn->setEnabled(false);

    m_flashTool->adbSideload(deviceId, m_currentFile);
}

void FlashPanel::onRunScriptClicked()
{
    if (m_currentFile.isEmpty()) return;

    QString deviceId = m_deviceInfo.serialNumber;

    QMessageBox::StandardButton confirm = QMessageBox::question(
        this, "确认运行刷机脚本",
        QString("即将运行刷机脚本: %1\n\n"
                "脚本将自动执行刷机操作，是否继续？")
        .arg(QFileInfo(m_currentFile).fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (confirm != QMessageBox::Yes) return;

    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    m_runScriptBtn->setEnabled(false);

    m_flashTool->runFlashScript(m_currentFile, deviceId);
}

void FlashPanel::onDumpClicked()
{
    QListWidgetItem *item = m_partitionList->currentItem();
    if (!item) {
        QMessageBox::information(this, "提示", "请先在分区列表中选择一个分区");
        return;
    }

    QString partition = item->data(Qt::UserRole).toString();
    QString deviceId = m_deviceInfo.serialNumber;

    QString savePath = QFileDialog::getSaveFileName(
        this, QString("保存分区镜像 - %1").arg(partition),
        QString("%1_dump.img").arg(partition),
        "镜像文件 (*.img);;所有文件 (*)");

    if (savePath.isEmpty()) return;

    // 模式检测
    int mode = m_deviceInfo.mode;
    bool fastbootMode = (mode == DeviceDetector::MODE_FASTBOOT ||
                         mode == DeviceDetector::MODE_FASTBOOTD);
    bool edlMode = (mode == DeviceDetector::MODE_EDL_9008);
    bool mtkMode = (mode == DeviceDetector::MODE_MTK_DA);

    if (edlMode) {
        // EDL 模式: 直接通过 Firehose 读取
        QMessageBox::StandardButton confirm = QMessageBox::question(
            this, "确认读取分区 (EDL)",
            QString("即将通过 EDL Firehose 读取分区: %1\n\n保存到: %2\n\n是否继续？")
            .arg(partition, savePath),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

        if (confirm != QMessageBox::Yes) return;

        m_progressBar->setVisible(true);
        m_progressBar->setValue(0);
        m_dumpBtn->setEnabled(false);

        // 查找 EDL 分区信息
        QList<EDLPartition> parts = m_flashTool->edlListPartitions();
        for (const EDLPartition &p : parts) {
            if (p.name == partition) {
                m_flashTool->edlReadPartition(p, savePath);
                return;
            }
        }
        emit outputMessage(QString("EDL 未找到分区: %1").arg(partition), true);
        return;
    }

    if (mtkMode) {
        // MTK 模式: 通过 mtkclient 读取
        QMessageBox::StandardButton confirm = QMessageBox::question(
            this, "确认读取分区 (MTK)",
            QString("即将通过 MTK DA 读取分区: %1\n\n保存到: %2\n\n是否继续？")
            .arg(partition, savePath),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

        if (confirm != QMessageBox::Yes) return;

        m_progressBar->setVisible(true);
        m_progressBar->setValue(0);
        m_dumpBtn->setEnabled(false);

        m_flashTool->mtkReadPartition(partition, savePath);
        return;
    }

    if (fastbootMode) {
        // Fastboot 模式: 两种选择
        int blStatus = m_flashTool->checkBootloaderStatus(deviceId);
        if (blStatus == 0) {
            QMessageBox::warning(this, "Bootloader 未解锁",
                "Fastboot 模式下读取分区需要解锁 Bootloader。\n\n"
                "请先解锁 Bootloader 或使用「启动临时镜像」功能。");
            return;
        }

        QMessageBox msgBox(this);
        msgBox.setWindowTitle("Fastboot 模式");
        msgBox.setText("Fastboot 模式下没有 ADB，选择读取方式:");
        QPushButton *recoveryBtn = msgBox.addButton("启动临时 Recovery", QMessageBox::AcceptRole);
        QPushButton *dumpMemBtn = msgBox.addButton("高级: 内存转储", QMessageBox::ActionRole);
        msgBox.addButton("取消", QMessageBox::RejectRole);
        msgBox.exec();

        if (msgBox.clickedButton() == recoveryBtn) {
            emit outputMessage("请使用「启动临时镜像」功能 boot Recovery 镜像", false);
            return;
        }

        if (msgBox.clickedButton() == dumpMemBtn) {
            // 内存转储: 需要地址和大小
            bool okAddr = false, okSize = false;
            QString addr = QInputDialog::getText(this, "内存转储",
                "输入起始地址 (十六进制, 如 0x10000000):",
                QLineEdit::Normal, "0x", &okAddr);
            if (!okAddr || addr.isEmpty()) return;

            QString sz = QInputDialog::getText(this, "内存转储",
                "输入大小 (十六进制字节, 如 0x100000 即 1MB):",
                QLineEdit::Normal, "0x100000", &okSize);
            if (!okSize || sz.isEmpty()) return;

            QString savePath = QFileDialog::getSaveFileName(this,
                "保存内存转储", "mem_dump.bin", "原始数据 (*.bin *.dump);;所有文件 (*)");
            if (savePath.isEmpty()) return;

            emit outputMessage(QString("内存转储: addr=%1 size=%2").arg(addr, sz), false);
            m_progressBar->setVisible(true);
            m_progressBar->setValue(0);
            m_dumpBtn->setEnabled(false);
            m_flashTool->fastbootDumpMem(deviceId, addr, sz, savePath);
            return;
        }
        return;
    } else if (mode == DeviceDetector::MODE_ADB && !m_deviceInfo.isRooted) {
        // ADB 无 root: 警告
        QMessageBox::StandardButton cont = QMessageBox::question(
            this, "无 Root 权限",
            "设备未获取 Root 权限，直接读取可能失败。\n\n是否继续尝试？",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (cont != QMessageBox::Yes) return;
    }

    QMessageBox::StandardButton confirm = QMessageBox::question(
        this, "确认读取分区",
        QString("即将读取分区: %1\n\n"
                "该操作不会修改设备，属于只读操作。\n"
                "保存到: %2\n\n是否继续？")
        .arg(partition, savePath),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (confirm != QMessageBox::Yes) return;

    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    m_dumpBtn->setEnabled(false);

    m_flashTool->dumpPartition(deviceId, mode, partition, savePath);
}

void FlashPanel::onBootImageClicked()
{
    if (m_currentFile.isEmpty()) {
        QString imgPath = QFileDialog::getOpenFileName(
            this, "选择 Recovery/启动镜像",
            QString(), "镜像文件 (*.img);;所有文件 (*)");
        if (imgPath.isEmpty()) return;
        updateFileInfo(imgPath);
    }

    QString deviceId = m_deviceInfo.serialNumber;

    QMessageBox::StandardButton confirm = QMessageBox::question(
        this, "确认启动临时镜像",
        QString("即将通过 fastboot boot 启动:\n%1\n\n"
                "设备将会临时从该镜像启动，不会写入设备。\n"
                "重启后即恢复。\n\n是否继续？")
        .arg(QFileInfo(m_currentFile).fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (confirm != QMessageBox::Yes) return;

    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    m_bootImgBtn->setEnabled(false);
    m_brickRepairBtn->setEnabled(false);

    m_flashTool->fastbootBoot(deviceId, m_currentFile);
}

// ==================== EDL 模式 ====================

void FlashPanel::onEdlSelectProgrammer()
{
    QString path = QFileDialog::getOpenFileName(
        this, "选择 Programmer ELF",
        QString(),
        "Programmer (*.elf *.bin *.mbn);;所有文件 (*)");
    if (!path.isEmpty()) {
        m_programmerPath = path;
        m_edlStatusLabel->setText(QString("Programmer: %1").arg(QFileInfo(path).fileName()));
    }
}

void FlashPanel::onEdlConnect()
{
    if (m_programmerPath.isEmpty()) {
        QMessageBox::information(this, "提示", "请先选择 Programmer ELF 文件");
        return;
    }

    QMessageBox::StandardButton confirm = QMessageBox::question(
        this, "确认连接 EDL",
        QString("即将连接 EDL 设备并加载 Programmer:\n%1\n\n"
                "确保设备已进入 EDL 模式 (9008)。\n是否继续？")
        .arg(QFileInfo(m_programmerPath).fileName()),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (confirm != QMessageBox::Yes) return;

    m_edlConnectBtn->setEnabled(false);
    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);

    if (m_flashTool->edlConnect(m_programmerPath)) {
        m_edlStatusLabel->setText("EDL: 已连接 (Firehose)");
        m_edlDisconnectBtn->setEnabled(true);
        m_dumpBtn->setEnabled(true);

        // 自动刷新分区
        onRefreshPartitions();
    } else {
        m_edlStatusLabel->setText("EDL: 连接失败");
    }

    m_edlConnectBtn->setEnabled(true);
}

void FlashPanel::onEdlDisconnect()
{
    m_flashTool->edlDisconnect();
    m_edlStatusLabel->setText("EDL: 未连接");
    m_edlDisconnectBtn->setEnabled(false);
    m_dumpBtn->setEnabled(false);
    m_flashBtn->setEnabled(false);
    m_partitionList->clear();
}

// ==================== MTK 模式 ====================

void FlashPanel::onMtkConnect()
{
    QMessageBox::StandardButton confirm = QMessageBox::question(
        this, "确认连接 MTK",
        "即将连接 MTK DA 设备。\n\n"
        "请确保设备已进入 BROM/Preloader 模式\n"
        "（通常为短接测试点或按住特定按键后插入 USB）。\n\n"
        "是否继续？",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);

    if (confirm != QMessageBox::Yes) return;

    m_mtkConnectBtn->setEnabled(false);
    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);

    if (m_flashTool->mtkConnect()) {
        m_mtkStatusLabel->setText("MTK: 已连接");
        m_mtkDisconnectBtn->setEnabled(true);
        m_dumpBtn->setEnabled(true);
        m_unlockBtn->setEnabled(true);
        m_lockBtn->setEnabled(true);

        // 自动刷新分区
        onRefreshPartitions();
    } else {
        m_mtkStatusLabel->setText("MTK: 连接失败");
    }

    m_mtkConnectBtn->setEnabled(true);
}

void FlashPanel::onMtkDisconnect()
{
    m_flashTool->mtkDisconnect();
    m_mtkStatusLabel->setText("MTK: 未连接");
    m_mtkDisconnectBtn->setEnabled(false);
    m_dumpBtn->setEnabled(false);
    m_flashBtn->setEnabled(false);
    m_unlockBtn->setEnabled(false);
    m_lockBtn->setEnabled(false);
    m_partitionList->clear();
}

// ==================== Bootloader 解锁/回锁 ====================

void FlashPanel::onUnlockBootloader()
{
    QString deviceId = m_deviceInfo.serialNumber;
    if (deviceId.isEmpty()) return;

    bool mtkMode = (m_deviceInfo.mode == DeviceDetector::MODE_MTK_DA);

    QMessageBox msgBox(this);
    msgBox.setWindowTitle("警告: 解锁 Bootloader");
    msgBox.setText(
        mtkMode ?
        "通过 MTK DA 解锁 Bootloader (seccfg) 将:\n"
        "  • 清除设备所有数据\n"
        "  • 可能移除 FRP 锁\n\n"
        "建议提前备份重要数据。\n\n"
        "是否继续解锁？"
        :
        "解锁 Bootloader 将:\n"
        "  • 清除设备所有数据 (恢复出厂设置)\n"
        "  • 可能导致部分保修失效\n"
        "  • 部分银行/支付应用可能无法使用\n\n"
        "建议提前备份重要数据。\n\n"
        "是否继续解锁？");
    msgBox.setIcon(QMessageBox::Warning);
    QPushButton *confirmBtn = msgBox.addButton("确认解锁", QMessageBox::AcceptRole);
    msgBox.addButton("取消", QMessageBox::RejectRole);
    msgBox.exec();

    if (msgBox.clickedButton() != confirmBtn) return;

    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    m_unlockBtn->setEnabled(false);
    m_lockBtn->setEnabled(false);

    if (mtkMode)
        m_flashTool->mtkSeccfgUnlock();
    else
        m_flashTool->unlockBootloader(deviceId);
}

void FlashPanel::onLockBootloader()
{
    QString deviceId = m_deviceInfo.serialNumber;
    if (deviceId.isEmpty()) return;

    bool mtkMode = (m_deviceInfo.mode == DeviceDetector::MODE_MTK_DA);

    QMessageBox msgBox(this);
    msgBox.setWindowTitle("警告: 回锁 Bootloader");
    msgBox.setText(
        mtkMode ?
        "通过 MTK DA 回锁 Bootloader (seccfg) 将:\n"
        "  • 恢复 FRP 锁定状态\n"
        "  • 重新锁定 Bootloader\n\n"
        "是否继续回锁？"
        :
        "回锁 Bootloader 将:\n"
        "  • 清除设备所有数据\n"
        "  • 恢复出厂状态\n"
        "  • 回锁后将无法刷入非官方镜像\n\n"
        "通常用于售后送修或出售设备前。\n\n"
        "是否继续回锁？");
    msgBox.setIcon(QMessageBox::Warning);
    QPushButton *confirmBtn = msgBox.addButton("确认回锁", QMessageBox::AcceptRole);
    msgBox.addButton("取消", QMessageBox::RejectRole);
    msgBox.exec();

    if (msgBox.clickedButton() != confirmBtn) return;

    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    m_unlockBtn->setEnabled(false);
    m_lockBtn->setEnabled(false);

    if (mtkMode)
        m_flashTool->mtkSeccfgLock();
    else
        m_flashTool->lockBootloader(deviceId);
}

// ==================== FRP 清除 ====================

void FlashPanel::onFrpErase()
{
    QString deviceId = m_deviceInfo.serialNumber;
    int mode = m_deviceInfo.mode;

    if (deviceId.isEmpty()) return;

    QMessageBox msgBox(this);
    msgBox.setWindowTitle("警告: 清除 FRP");
    msgBox.setText(
        "清除 FRP (出厂重置保护) 将:\n"
        "  • 移除 Google 账户锁/激活锁\n"
        "  • 不会清除用户数据\n"
        "  • 不会影响系统运行\n\n"
        "建议提前备份重要数据。\n\n"
        "是否继续清除 FRP？");
    msgBox.setIcon(QMessageBox::Warning);
    QPushButton *confirmBtn = msgBox.addButton("确认清除", QMessageBox::AcceptRole);
    msgBox.addButton("取消", QMessageBox::RejectRole);
    msgBox.exec();

    if (msgBox.clickedButton() != confirmBtn) return;

    if (mode == DeviceDetector::MODE_FASTBOOT || mode == DeviceDetector::MODE_FASTBOOTD) {
        // Fastboot 模式: 需要解锁 BL
        int blStatus = m_flashTool->checkBootloaderStatus(deviceId);
        if (blStatus == 0) {
            QMessageBox::StandardButton cont = QMessageBox::warning(
                this, "Bootloader 未解锁",
                "Fastboot 模式下清除 FRP 需要解锁 Bootloader。\n\n"
                "是否先尝试擦除 FRP 分区（可能失败）？",
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (cont != QMessageBox::Yes) return;
        }
    }

    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);

    m_flashTool->eraseFRP(deviceId, mode);
}

// ==================== 工具回调 ====================

void FlashPanel::onToolOutput(const QString &msg, bool isError)
{
    emit outputMessage(msg, isError);
}

void FlashPanel::onToolProgress(int percent)
{
    m_progressBar->setValue(percent);
    if (percent >= 100 || percent == 0) {
        m_flashBtn->setEnabled(m_partitionList->currentItem() &&
                               !m_currentFile.isEmpty() &&
                               m_currentFile.toLower().endsWith(".img"));
        m_eraseBtn->setEnabled(m_partitionList->currentItem() != nullptr);
        m_dumpBtn->setEnabled(m_partitionList->currentItem() != nullptr);
        m_updateBtn->setEnabled(!m_currentFile.isEmpty() &&
                                m_currentFile.toLower().endsWith(".zip"));
        m_sideloadBtn->setEnabled(!m_currentFile.isEmpty() &&
                                  m_currentFile.toLower().endsWith(".zip"));
        m_runScriptBtn->setEnabled(!m_currentFile.isEmpty() &&
                                   (m_currentFile.toLower().endsWith(".sh") ||
                                    m_currentFile.toLower().endsWith(".bat")));
        m_bootImgBtn->setEnabled(!m_currentFile.isEmpty() &&
                                 (m_currentFile.toLower().endsWith(".img")));
    }
}

void FlashPanel::onBrickRepairClicked()
{
    int mode = m_deviceInfo.mode;
    bool edlMode = (mode == DeviceDetector::MODE_EDL_9008);
    bool mtkMode = (mode == DeviceDetector::MODE_MTK_DA);

    if (!edlMode && !mtkMode) {
        QMessageBox::information(this, "死砖修复",
            "死砖修复需要设备处于深度刷写模式：\n\n"
            "  1. 短接设备主板上的测试点进入 EDL 或 MTK BROM 模式\n"
            "  2. 连接 USB 数据线\n"
            "  3. 等设备被 PhoneToolbox 识别\n\n"
            "当前模式不支持。请先短接测试点进入深刷模式。");
        return;
    }

    // 确保已连接 Firehose / DA
    if (edlMode && !m_flashTool->edlIsConnected()) {
        if (m_programmerPath.isEmpty()) {
            QMessageBox::information(this, "选择 Programmer",
                "请先选择与设备 SoC 匹配的 Firehose Programmer ELF 文件。\n\n"
                "Programmer 通常包含在官方线刷包中，文件名类似 prog_*.elf。");
            onEdlSelectProgrammer();
            if (m_programmerPath.isEmpty()) return;
        }
        QMessageBox::StandardButton ret = QMessageBox::question(this, "连接 EDL",
            "是否加载 Programmer 并连接 EDL 设备？",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (ret == QMessageBox::Yes) onEdlConnect();
        if (!m_flashTool->edlIsConnected()) {
            QMessageBox::warning(this, "连接失败", "EDL 连接失败，请检查设备与 Programmer 是否匹配。");
            return;
        }
    } else if (mtkMode && !m_flashTool->mtkIsConnected()) {
        QMessageBox::StandardButton ret = QMessageBox::question(this, "连接 MTK",
            "是否连接 MTK DA？",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (ret == QMessageBox::Yes) onMtkConnect();
        if (!m_flashTool->mtkIsConnected()) return;
    }

    // 选择救砖包目录
    QString dir = QFileDialog::getExistingDirectory(this, "选择救砖包目录",
        QString(), QFileDialog::ShowDirsOnly);
    if (dir.isEmpty()) {
        QMessageBox::information(this, "提示",
            "请选择一个包含分区镜像文件的目录。\n\n"
            "救砖包应从官方线刷包中提取，通常包含：\n"
            "  gpt.bin, sbl1.bin, abl.elf, boot.img,\n"
            "  vbmeta.img, dtbo.img, system.img 等");
        return;
    }

    // 扫描目录中的镜像文件
    QStringList searchNames = {
        "gpt", "partition", "sbl", "sbl1", "xbl", "abl", "lk",
        "preloader", "boot", "vbmeta", "dtbo", "dpm", "tz",
        "hyp", "keymaster", "cmnlib", "devcfg", "storsec"
    };

    QStringList foundFiles;
    QDir firmwareDir(dir);
    QStringList allFiles = firmwareDir.entryList({"*.img", "*.bin", "*.elf"},
        QDir::Files, QDir::Name);
    for (const auto &f : allFiles) {
        QString base = QFileInfo(f).completeBaseName().toLower();
        for (const auto &need : searchNames) {
            if (base.contains(need)) {
                foundFiles << firmwareDir.filePath(f);
                break;
            }
        }
    }

    if (foundFiles.isEmpty()) {
        // 退一步：允许用户选单文件
        QString singleFile = QFileDialog::getOpenFileName(this,
            "选择镜像文件", dir,
            "镜像 (*.img *.bin *.elf);;所有文件 (*)");
        if (singleFile.isEmpty()) return;
        foundFiles << singleFile;
    }

    // 排序：GPT 优先，boot 相关次之，system/vendor最后
    auto sortKey = [](const QString &f) {
        QString n = QFileInfo(f).completeBaseName().toLower();
        if (n.contains("gpt") || n.contains("partition")) return 0;
        if (n.contains("sbl") || n.contains("xbl") || n.contains("preloader")) return 1;
        if (n.contains("abl") || n.contains("lk")) return 2;
        if (n.contains("tz") || n.contains("hyp") || n.contains("keymaster")) return 3;
        if (n.contains("boot")) return 4;
        if (n.contains("vbmeta") || n.contains("dtbo") || n.contains("dpm")) return 5;
        return 6;
    };
    std::sort(foundFiles.begin(), foundFiles.end(),
        [&](const QString &a, const QString &b) { return sortKey(a) < sortKey(b); });

    // 显示将要写入的文件列表
    QString fileList;
    for (const auto &f : foundFiles)
        fileList += "  " + QFileInfo(f).fileName() + "\n";

    QString modeStr = edlMode ? "EDL Firehose" : "MTK DA";
    QMessageBox::StandardButton confirm = QMessageBox::warning(this,
        "确认死砖修复",
        QString("即将通过 %1 按序写入以下 %2 个文件：\n\n%3\n"
                "⚠️ 写入错误的分区镜像将导致设备永久损坏！\n"
                "请确保文件来源可靠（官方线刷包）。\n\n是否继续？")
            .arg(modeStr).arg(foundFiles.size()).arg(fileList),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (confirm != QMessageBox::Yes) return;

    // 执行修复
    m_brickRepairBtn->setEnabled(false);
    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    emit outputMessage("======== 死砖修复开始 ========", false);

    int total = foundFiles.size();
    bool allOk = true;

    for (int i = 0; i < total; i++) {
        const auto &filePath = foundFiles[i];
        QString partBase = QFileInfo(filePath).completeBaseName();
        emit outputMessage(QString("[%1/%2] 写入 %3...")
            .arg(i + 1).arg(total).arg(QFileInfo(filePath).fileName()), false);
        m_progressBar->setValue(i * 100 / total);
        QCoreApplication::processEvents();

        bool ok = false;
        if (edlMode) {
            auto parts = m_flashTool->edlListPartitions();
            for (const auto &p : parts) {
                if (partBase.toLower().contains(p.name.toLower()) ||
                    p.name.toLower().contains(partBase.toLower())) {
                    ok = m_flashTool->edlWritePartition(p, filePath);
                    break;
                }
            }
            if (!ok) {
                emit outputMessage(QString("EDL 未能匹配分区 %1").arg(partBase), true);
                allOk = false;
            }
        } else if (mtkMode) {
            ok = m_flashTool->mtkWritePartition(partBase, filePath);
            if (!ok) {
                emit outputMessage(QString("MTK 写入 %1 失败").arg(partBase), true);
                allOk = false;
            }
        }
        if (ok)
            emit outputMessage(QString("  ✓ %1 写入成功").arg(QFileInfo(filePath).fileName()), false);
    }

    m_progressBar->setValue(100);

    if (allOk) {
        emit outputMessage("======== 死砖修复完成 ========", false);
        QMessageBox::StandardButton act = QMessageBox::question(this,
            "修复完成",
            "所有分区已成功写入。\n\n"
            "是否断开深刷连接并重启设备？\n"
            "（如果仍无法开机，可能需要继续刷入 system/vendor 分区）",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (act == QMessageBox::Yes) {
            if (edlMode) {
                m_flashTool->edlDisconnect();
                RestartTool rest;
                rest.restartDevice(m_deviceInfo.serialNumber,
                    DeviceDetector::MODE_EDL_9008, RestartTool::MODE_SYSTEM);
            } else if (mtkMode) {
                m_flashTool->mtkDisconnect();
                emit outputMessage("MTK 已断开。请拔插 USB 重启设备。", false);
            }
        }
    } else {
        emit outputMessage("======== 修复部分失败，请检查日志 ========", true);
        QMessageBox::warning(this, "修复未完成",
            "部分分区写入失败，请查看输出日志获取详细信息。");
    }
}
