#include "flash_panel.h"
#include "flash_plan_dialog.h"          // Phase B Task 8：EDL 刷写计划预览
#include "samsung_plan_dialog.h"        // Phase C Task 9：三星 Odin 刷写计划预览
#include "mtk_plan_dialog.h"            // Phase D1 Task 11：MTK BROM 刷写计划预览
#include "core/filename_parser.h"
#include "core/mtk_flash_plan.h"        // D1 Task 11：MTK 入口的计划构建（scatter 参照表 + 镜像）
#include "core/restart_tool.h"
#include <utility>   // std::as_const（遍历 Qt 容器，不得用 qAsConst）
#include <QFile>                        // D1 Task 11：MTK 入口读 scatter
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
#include <QEventLoop>                   // processEvents 的标志（ExcludeUserInputEvents，刷写期间只重绘）
#include <QThread>
#include <QRegularExpression>
#include <QMap>
#include <QDateTime>
#include <QDir>

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
    // Phase B Task 8：EDL 刷写计划（解包产物目录 / .ofp/.ops 整包 → 计划预览 → 按计划刷写）
    m_edlPlanBtn = new QPushButton("EDL 刷写计划…", this);
    m_edlPlanBtn->setToolTip("按 rawprogram/patch XML 计划刷写（自动 Sahara 引导，设备须停留在 9008）");
    m_edlStatusLabel = new QLabel("EDL: 未连接", this);
    m_edlSelectProgBtn->hide();
    m_edlConnectBtn->hide();
    m_edlDisconnectBtn->hide();
    m_edlPlanBtn->hide();
    m_edlStatusLabel->hide();

    topLayout->addWidget(m_edlSelectProgBtn);
    topLayout->addWidget(m_edlConnectBtn);
    topLayout->addWidget(m_edlDisconnectBtn);
    topLayout->addWidget(m_edlPlanBtn);
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
    connect(m_edlPlanBtn, &QPushButton::clicked,
            this, &FlashPanel::onEdlPlanFlash);

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
    case DeviceDetector::MODE_MTK_BROM:
        modeStr = "MTK BROM"; break;
    case DeviceDetector::MODE_HUAWEI_USB_UPDATE:
        modeStr = "华为 USB Update"; break;
    case DeviceDetector::MODE_SPD:
        modeStr = "展锐"; break;
    case DeviceDetector::MODE_SAMSUNG_ODIN:
        modeStr = "三星 (Odin)"; break;
    default:
        modeStr = "未知"; break;
    }
    m_modeLabel->setText(QString("模式: %1").arg(modeStr));

    m_edlSelectProgBtn->hide();
    m_edlConnectBtn->hide();
    m_edlDisconnectBtn->hide();
    m_edlPlanBtn->hide();
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
    // 提示也一并复位：下面 EDL 分支会给这两个按钮挂"为什么不可用"的提示，
    // 不复位就会跟着按钮状态漂到别的模式（Fastboot/MTK 下它们是可用的）
    m_frpBtn->setToolTip(QString());
    m_brickRepairBtn->setToolTip(QString());

    if (info.mode == DeviceDetector::MODE_EDL_9008) {
        // EDL 模式: 显示连接控件
        m_edlSelectProgBtn->show();
        m_edlConnectBtn->show();
        m_edlDisconnectBtn->show();
        // 刷写计划入口不需要"已连接"：通道自己走 Sahara 引导（设备须停留在 9008，
        // 先点过「连接 EDL」反而会把设备带到 Firehose —— 提示见处理函数）
        m_edlPlanBtn->show();
        m_edlStatusLabel->show();
        m_edlStatusLabel->setText(m_flashTool->edlIsConnected() ? "EDL: 已连接" : "EDL: 未连接");
        m_edlDisconnectBtn->setEnabled(m_flashTool->edlIsConnected());

        // 已连接 EDL Firehose 后显示分区操作。「清除 FRP」「死砖修复」在 EDL 下点不出任何结果：
        // 两者都要"分区名 → 分区"，而 Firehose 只回报 lun<N>（各自的处理函数会弹指路框，
        // 见 onClearFrpClicked / onBrickRepairClicked 的 EDL 早退）—— 与其让用户先点一次再被拒，
        // 不如在这里就置灰并说明原因（tooltip 指明替代入口）。读回（m_dumpBtn）不受影响。
        if (m_flashTool->edlIsConnected()) {
            m_dumpBtn->setEnabled(true);
            m_frpBtn->setToolTip(QStringLiteral(
                "EDL 模式不支持：Firehose 只能枚举 lun<N>，拿不到 GPT 分区名。"
                "请改用「EDL 刷写计划…」（计划里 label=frp 的条目）"));
            m_brickRepairBtn->setToolTip(QStringLiteral(
                "EDL 模式不支持：Firehose 只能枚举 lun<N>，无法把镜像文件名映射到分区。"
                "请改用「EDL 刷写计划…」"));
            if (!m_currentFile.isEmpty() && m_currentFile.toLower().endsWith(".img"))
                m_flashBtn->setEnabled(m_partitionList->currentItem() != nullptr);
        } else {
            // 未连接时也让"为什么现在点不了"可见（连上后如上面所述仍然点不了，原因不同）
            m_frpBtn->setToolTip(QStringLiteral("需先「连接 EDL」；连接后 EDL 模式仍不支持按分区名清除 FRP，"
                                               "请改用「EDL 刷写计划…」"));
            m_brickRepairBtn->setToolTip(QStringLiteral("需先「连接 EDL」；连接后 EDL 模式仍不支持目录批量刷写，"
                                                       "请改用「EDL 刷写计划…」"));
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

    // F5: 协议通道模式（MTK BROM / 华为 USB Update / 展锐 / 三星 Odin）——整包刷写通道。
    // 分区列表不适用：MTK BROM 的镜像与 DA 在「刷入」入口里选择（含计划预览），
    // 华为/展锐为整包通道，三星侧分区来自包内 PIT（刷写入口里弹预览）；
    // 跳过 onRefreshPartitions，避免对协议设备(如 usb-1-2)发 ADB 查询。
    // 「刷入」按所选设备模式走协议通道文件参数对话框。
    if (m_deviceInfo.mode == DeviceDetector::MODE_MTK_BROM ||
        m_deviceInfo.mode == DeviceDetector::MODE_HUAWEI_USB_UPDATE ||
        m_deviceInfo.mode == DeviceDetector::MODE_SPD ||
        m_deviceInfo.mode == DeviceDetector::MODE_SAMSUNG_ODIN) {
        m_partitionList->clear();
        m_partitions.clear();
        m_flashBtn->setEnabled(true);
        m_flashBtn->setToolTip(QStringLiteral("协议通道整包/按计划刷写（按模式选择 update.app / pac+FDL / DA+镜像 / 三星 tar.md5）"));
        return;
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
    // F5: 协议通道整包刷写（MTK BROM / 华为 USB Update / 展锐）——
    // 优先于分区刷写；通道按所选设备模式分派，ADB 条目不会进入此分支
    // （flashChannelForMode 对非协议模式返回空串）。
    //
    // Phase B Task 8：`oppo-edl`（9008）**不在这里截胡** —— EDL 的分区列表是真实可选条目（lun<N>），
    // 「刷入」必须保持"写所选分区"的语义（与 onDumpClicked 的「读取」一致，见 :920-941；
    // 两条路径行为一致是硬要求）。
    // 若在此截胡：本段没有 oppo-edl 的参数分支 → params 空 → 通道报"缺少 planDir"直接 return，
    // 下面对 EDL 的分区写分支将**永久不可达**（Task 8 审查 Important）。
    // 整包按计划刷写走专用入口「EDL 刷写计划…」（onEdlPlanFlash），与本按钮互不干扰。
    const DeviceDetector::DeviceMode deviceMode =
        static_cast<DeviceDetector::DeviceMode>(m_deviceInfo.mode);
    const QString channel = FlashTool::flashChannelForMode(deviceMode);
    if (FlashTool::isPackageChannelMode(deviceMode)) {
        const QString deviceId = m_deviceInfo.serialNumber;
        QVariantMap params;
        if (channel == QStringLiteral("huawei-usb-update")) {
            const QString appPath = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择 update.app"), QString(),
                QStringLiteral("华为固件 (*.app)"));
            if (appPath.isEmpty()) return;
            params.insert(QStringLiteral("updateApp"), appPath);
        } else if (channel == QStringLiteral("spd")) {
            const QString pacPath = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择 pac 固件"), QString(),
                QStringLiteral("展锐固件 (*.pac)"));
            if (pacPath.isEmpty()) return;
            const QString fdl1 = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择 FDL1 二进制"));
            if (fdl1.isEmpty()) return;
            const QString fdl2 = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择 FDL2 二进制"));
            if (fdl2.isEmpty()) return;
            params.insert(QStringLiteral("pacPath"), pacPath);
            params.insert(QStringLiteral("fdl1Path"), fdl1);
            params.insert(QStringLiteral("fdl2Path"), fdl2);
        } else if (channel == QStringLiteral("mtk-brom")) {
            // 文件入口顺序即用户心智：DA → 镜像 → scatter(可选) → preloader(可选) → 预览 → 确认
            const QString daPath = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择 DA 二进制（MTK_AllInOne_DA_*.bin）"), QString(),
                QStringLiteral("MTK DA (*.bin);;所有文件 (*)"));
            if (daPath.isEmpty()) return;
            const QStringList images = QFileDialog::getOpenFileNames(
                this, QStringLiteral("选择要刷入的镜像（可多选：boot.img / super.img / preloader.bin…）"),
                QString(), QStringLiteral("镜像 (*.img *.bin);;所有文件 (*)"));
            if (images.isEmpty()) return;

            // 分区参照表（可选）：有 scatter 就能提前看到分区名与大小，并对"放不下"提前告警；
            // 没有则目标分区名由镜像文件名推导 —— **写入判据始终以设备实读分区表为准**。
            QList<mtkplan::PartitionRef> refs;
            const QMessageBox::StandardButton wantScatter = QMessageBox::question(
                this, QStringLiteral("分区参照表"),
                QStringLiteral("是否提供 scatter 文件（Android_scatter.txt 或 MTK 的 *.xml）？\n\n"
                               "提供 → 预览可见分区名与分区大小（提前发现「放不下」）；\n"
                               "不提供 → 分区名由镜像文件名推导，刷写时按设备分区表校验。"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (wantScatter == QMessageBox::Yes) {
                const QString scatter = QFileDialog::getOpenFileName(
                    this, QStringLiteral("选择 scatter 文件"), QString(),
                    QStringLiteral("scatter (*.txt *.xml);;所有文件 (*)"));
                if (scatter.isEmpty()) return;
                QFile sf(scatter);
                if (!sf.open(QIODevice::ReadOnly)) {
                    emit outputMessage(QStringLiteral("无法读取 scatter：%1").arg(scatter), true);
                    return;
                }
                // 两种方言（D1 文本 / D2 XML）由计划层自动识别；XML 的 EMMC/UFS 双副本在那里调和，
                // 分歧/只有一份可用的说明逐条落到日志（**不猜**：预览是咨询性的，写入判据以设备实读为准）。
                QString parseErr;
                QStringList scatterLog;
                if (!mtkplan::parseScatterAnyDialect(QString::fromUtf8(sf.readAll()), refs, &scatterLog, &parseErr)) {
                    emit outputMessage(QStringLiteral("scatter 解析失败：%1").arg(parseErr), true);
                    return;
                }
                for (const QString &line : std::as_const(scatterLog))
                    emit outputMessage(line, false);
            }

            // preloader（EMI/DRAM 初始化用）：显式选择优先；否则在镜像目录里自动找（唯一命中才用）
            QString preloaderPath;
            QStringList firmwareDirs;
            const QMessageBox::StandardButton wantPre = QMessageBox::question(
                this, QStringLiteral("preloader"),
                QStringLiteral("是否指定 preloader？（用于 DRAM/EMI 初始化）\n\n"
                               "选「是」→ 手动选择文件；\n"
                               "选「否」→ 自动在镜像所在目录查找 preloader*.bin（唯一命中才用）。"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            if (wantPre == QMessageBox::Yes) {
                preloaderPath = QFileDialog::getOpenFileName(
                    this, QStringLiteral("选择 preloader"), QString(),
                    QStringLiteral("preloader (*.bin);;所有文件 (*)"));
                if (preloaderPath.isEmpty()) return;
            } else {
                firmwareDirs << QFileInfo(images.first()).absolutePath();
            }
            // 网络获取：**默认关闭**（默认按钮就是 No）—— 显式 opt-in 才带上参数
            bool allowNetwork = false;
            if (preloaderPath.isEmpty()) {
                const QMessageBox::StandardButton net = QMessageBox::question(
                    this, QStringLiteral("网络获取 preloader"),
                    QStringLiteral("是否允许从网络获取 preloader？\n\n"
                                   "默认关闭。开启后按本机来源清单（mtk_preloader_sources.json，"
                                   "含 URL 与 sha256）下载并校验；\n"
                                   "注意：错误的 preloader 有砖机风险，请自行确认来源可信。"),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                allowNetwork = (net == QMessageBox::Yes);
            }

            // 预览：镜像文件名 → 目标分区（scatter 是参照表，不是判据）→ 计划不可用/用户取消都在这里收口
            mtkplan::MtkFlashPlan plan;
            QString planErr;
            if (!mtkplan::buildMtkPlan(refs, images, plan, &planErr)) {
                emit outputMessage(QStringLiteral("MTK 刷写计划构建失败：%1").arg(planErr), true);
                return;
            }
            if (!MtkPlanDialog::buildAndShow(plan, this, &planErr)) {
                if (!planErr.isEmpty())
                    emit outputMessage(QStringLiteral("MTK 刷写计划不可用：%1").arg(planErr), true);
                return;                      // *error 空 = 用户取消（静默返回，与 EDL/三星入口同口径）
            }
            // 键名与 FlashTool::parseBromParams 一一对应（flash_tool.h 的通道参数表）：
            // 缺 daPath/imagePaths = 通道侧响亮失败；preloaderCacheDir 本入口不设（空 = 只留内存）
            params.insert(QStringLiteral("daPath"), daPath);
            params.insert(QStringLiteral("imagePaths"), images);
            if (!preloaderPath.isEmpty())
                params.insert(QStringLiteral("preloaderPath"), preloaderPath);
            if (!firmwareDirs.isEmpty())
                params.insert(QStringLiteral("firmwareDirs"), firmwareDirs);
            if (allowNetwork)
                params.insert(QStringLiteral("allowNetworkPreloader"), true);
        } else if (channel == QStringLiteral("samsung-odin")) {
            // Phase C：选包 → 计划预览（含两类不匹配告警 + 未验证勾选）→ 确认后把**文件列表**
            // 交给通道（通道内自行解析 PIT 并重建计划 —— 与 oppo-edl 通道传 planDir 同款口径）
            const QStringList tars = QFileDialog::getOpenFileNames(
                this, QStringLiteral("选择三星固件包（BL/AP/CP/CSC 的 .tar.md5，可多选）"), QString(),
                QStringLiteral("三星固件包 (*.tar.md5 *.tar);;所有文件 (*)"));
            if (tars.isEmpty()) return;
            QString pitPath;
            const QMessageBox::StandardButton wantPit = QMessageBox::question(
                this, QStringLiteral("PIT"),
                QStringLiteral("是否显式指定 PIT 文件？\n\n"
                               "选「否」用包内 .pit（推荐，通常来自 CSC 包）；\n"
                               "包内没有 .pit 时必须选「是」并指定文件。"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (wantPit == QMessageBox::Yes) {
                pitPath = QFileDialog::getOpenFileName(this, QStringLiteral("选择 PIT 文件"), QString(),
                                                       QStringLiteral("PIT (*.pit);;所有文件 (*)"));
                if (pitPath.isEmpty()) return;
            }
            QString planErr;
            if (!SamsungPlanDialog::buildAndShow(tars, pitPath, this, &planErr)) {
                if (!planErr.isEmpty())
                    emit outputMessage(QStringLiteral("三星刷写计划构建失败：%1").arg(planErr), true);
                return;                     // *error 空 = 用户取消（静默返回，与 EDL 计划入口同口径）
            }
            params.insert(QStringLiteral("tarMd5Files"), tars);
            if (!pitPath.isEmpty())
                params.insert(QStringLiteral("pitPath"), pitPath);
        }

        // 已知显示伪影（F5-1）：VID 通配检测（华为 0x12D1 / 展锐 0x1782）
        // 会让同设备同时以 ADB 模式列出。协议条目的 ID 为 usb-<bus>-<addr>，
        // 无法与 ADB serial 关联，故不做身份匹配；按所选条目模式分派保证
        // ADB 条目不会进入协议通道——用户选择协议条目即表明意图，放行并记录。
        if (channel == QStringLiteral("huawei-usb-update") ||
            channel == QStringLiteral("spd")) {
            emit outputMessage(QStringLiteral(
                "提示：该设备可能同时以 ADB 模式列出（VID 通配检测）；"
                "本次按所选协议条目走 %1 通道，请确认设备处于对应协议模式")
                .arg(channel), false);
        }

        m_progressBar->setVisible(true);
        m_progressBar->setValue(0);
        m_flashBtn->setEnabled(false);

        // 通道刷写是同步一整趟（DA 握手 → 逐分区写 → reset），不泵事件则进度条与日志
        // **全程不重绘**（进度只在返回后跳变）。与 EDL 计划路径同款：ExcludeUserInputEvents
        // 只放行重绘/定时器，把"刷写途中用户再点按钮"的重入从队列层面排除；连接是本段专有，
        // 退出即断开（EDL 计划路径见 onEdlPlanFlash）。
        const QMetaObject::Connection pump = connect(
            m_flashTool, &FlashTool::flashProgress, this, [](int) {
                QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            });

        QString error;
        const bool ok = m_flashTool->flashFullPackage(deviceId,
                static_cast<DeviceDetector::DeviceMode>(m_deviceInfo.mode),
                params, &error);
        disconnect(pump);

        if (!ok)
            emit outputMessage(QStringLiteral("刷写失败: %1").arg(error), true);
        else
            emit outputMessage(QStringLiteral("刷写完成"), false);

        m_progressBar->setVisible(false);
        m_flashBtn->setEnabled(true);
        return;
    }

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

// Phase B Task 8：EDL 刷写计划入口（oppo-edl 通道）。两种来源：解包产物目录（rawprogram*/patch*/
// settings.xml）或整包（.ofp/.ops，先解包到临时目录）。预览确认后按计划整包刷写。
void FlashPanel::onEdlPlanFlash()
{
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("EDL 刷写计划"));
    box.setIcon(QMessageBox::Question);
    box.setText(QStringLiteral("请选择计划来源："));
    box.setInformativeText(QStringLiteral(
        "解包产物目录：含 rawprogram*.xml / settings.xml 的目录（推荐）\n"
        "整包：.ofp / .ops 原包（先解包到临时目录再预览）\n\n"
        "注意：刷写会重新执行 Sahara 引导，设备须停留在 EDL 9008 状态；"
        "若已点过「连接 EDL」（设备已在 Firehose），请重新插拔回到 9008。"));
    QPushButton *dirBtn = box.addButton(QStringLiteral("选择目录…"), QMessageBox::AcceptRole);
    QPushButton *pkgBtn = box.addButton(QStringLiteral("选择整包 (.ofp/.ops)…"),
                                        QMessageBox::ActionRole);
    box.addButton(QStringLiteral("取消"), QMessageBox::RejectRole);
    box.exec();

    QString planDir;
    QString error;
    QString cancelNote;                 // 取消详情（仅整包入口会填；见 buildAndShowPackage 注释）
    bool picked = false;
    if (box.clickedButton() == dirBtn) {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择解包产物目录"), QString(), QFileDialog::ShowDirsOnly);
        if (dir.isEmpty()) {
            emit outputMessage(QStringLiteral("已取消 EDL 刷写计划（未选择目录）"), false);
            return;
        }
        picked = FlashPlanDialog::buildAndShow(dir, this, &planDir, &error);
        m_edlPlanTempDir.reset();          // 目录入口用不上上次整包的解包产物
    } else if (box.clickedButton() == pkgBtn) {
        const QString pkg = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择 OPPO 固件包"), QString(),
            QStringLiteral("OPPO 固件包 (*.ofp *.ops);;所有文件 (*)"));
        if (pkg.isEmpty()) {
            emit outputMessage(QStringLiteral("已取消 EDL 刷写计划（未选择固件包）"), false);
            return;
        }
        // 解包产物必须活过本次刷写：临时目录由面板持有（重建一次 = 丢弃上一次的产物）
        m_edlPlanTempDir = std::make_unique<QTemporaryDir>();
        if (!m_edlPlanTempDir->isValid()) {
            m_edlPlanTempDir.reset();
            emit outputMessage(QStringLiteral("无法创建解包临时目录，已取消"), true);
            return;
        }
        // cancelNote：解包被取消时的如实详情（"已完成 N/M 个文件"等；PB-B6 —— 取消在条目
        // 边界生效，日志要能说明停在哪，而不是只说一句"已取消"）
        picked = FlashPlanDialog::buildAndShowPackage(pkg, this, m_edlPlanTempDir.get(),
                                                      &planDir, &error, &cancelNote);
    } else {
        return;                            // 取消
    }

    // 解包产物常有数 GB：清理策略一处说清 ——
    //   * 用户取消（error 为空）→ 立即回收；
    //   * **构建/解包失败（error 非空）→ 保留并把路径告诉用户**（Task 8 审查 Minor：此时 reset 等于
    //     让用户为同一个包再解一遍；保留后可直接改包/重选，或手动清理）；
    //   * 刷写成功 → 回收；刷写失败 → 保留（便于就地重试）；
    //   * 下一次选包会整体替换（旧目录随 unique_ptr 析构删除）。
    if (!picked) {
        if (error.isEmpty()) {
            // 取消（含整包解包期间的取消：那时临时目录里可能已有半套产物）→ 立即回收。
            // cancelNote 把"取消发生在哪一步"如实带上（PB-B6：解包的取消在条目边界生效）。
            emit outputMessage(cancelNote.isEmpty()
                                   ? QStringLiteral("已取消 EDL 刷写计划")
                                   : QStringLiteral("已取消 EDL 刷写计划（%1）；解包临时目录已回收")
                                         .arg(cancelNote),
                               false);
            if (m_edlPlanTempDir)
                m_edlPlanTempDir.reset();
        } else {
            emit outputMessage(QStringLiteral("EDL 刷写计划不可用: %1").arg(error), true);
            if (m_edlPlanTempDir)
                emit outputMessage(QStringLiteral("解包产物保留在：%1（改包后可重选该目录，"
                                                  "避免重新解包；也可手动删除）")
                                       .arg(m_edlPlanTempDir->path()), false);
        }
        return;
    }

    emit outputMessage(QStringLiteral("EDL 刷写计划已确认，开始刷写：%1").arg(planDir), false);
    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);
    m_edlPlanBtn->setEnabled(false);

    // 刷写是同步一整趟（Sahara → 逐条目写 → reset），不泵事件则进度条与日志**全程不重绘**
    // （死砖循环是手动 processEvents，见 onBrickRepairClicked 的执行段）。这里用
    // **ExcludeUserInputEvents**：只放行重绘/定时器，鼠标键盘事件一律不进队列 ——
    // 从根上排除"刷写途中用户再点按钮造成重入"，比逐个禁用按钮更可靠（且连接是本段专有，
    // 退出即断开，不影响其它路径的既有行为）。
    const QMetaObject::Connection pump = connect(
        m_flashTool, &FlashTool::flashProgress, this, [](int) {
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        });

    QVariantMap params;
    params.insert(QStringLiteral("planDir"), planDir);
    QString runErr;
    const bool ok = m_flashTool->flashFullPackage(m_deviceInfo.serialNumber,
                                                  DeviceDetector::MODE_EDL_9008, params, &runErr);
    disconnect(pump);

    if (!ok)
        emit outputMessage(QStringLiteral("刷写失败: %1").arg(runErr), true);
    else
        emit outputMessage(QStringLiteral("刷写完成"), false);

    m_progressBar->setVisible(false);
    m_edlPlanBtn->setEnabled(true);

    if (ok)
        m_edlPlanTempDir.reset();          // 成功即回收（镜像已写完，不再需要）
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

    // Phase B Task 8 前置：EDL 模式下按分区名找 FRP 必然失败（Firehose 只回报 lun<N>）——
    // 不再走完确认框再失败，直接给出可操作的指引（核心层 eraseFRP 仍保留同口径报错兜底）
    if (mode == DeviceDetector::MODE_EDL_9008) {
        emit outputMessage(QStringLiteral(
            "EDL 模式无法按分区名定位 FRP：请改用「EDL 刷写计划…」（计划中 label=frp 的条目）"), true);
        QMessageBox::information(this, QStringLiteral("清除 FRP"),
            QStringLiteral("EDL 模式请改用「EDL 刷写计划…」清除 FRP：\n\n"
                "Firehose 只能枚举 lun<N>，拿不到 GPT 分区名，无法按名字定位 frp 分区。\n"
                "计划方式下，XML 里 label=frp 的条目自带 LUN/起始扇区/扇区数，可精确定位。\n\n"
                "（Fastboot / ADB Root / MTK 模式的「清除 FRP」不受影响。）"));
        return;
    }

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

// ==================== 死砖修复辅助方法 ====================

static int sortPriority(const QString &filePath)
{
    QString n = QFileInfo(filePath).completeBaseName().toLower();
    if (n.contains("gpt") || n.contains("partition")) return 0;
    if (n.contains("sbl") || n.contains("xbl") || n.contains("preloader")) return 1;
    if (n.contains("abl") || n.contains("lk")) return 2;
    if (n.contains("tz") || n.contains("hyp") || n.contains("keymaster")) return 3;
    if (n.contains("boot") && !n.contains("vendor") && !n.contains("super")) return 4;
    if (n.contains("vbmeta") || n.contains("dtbo") || n.contains("dpm")) return 5;
    return 6;
}

static bool compareByPriority(const QString &a, const QString &b)
{
    return sortPriority(a) < sortPriority(b);
}

static bool checkImageSize(const QString &imagePath, quint64 partitionBytes)
{
    if (partitionBytes == 0) return true; // unknown size, skip check
    QFileInfo fi(imagePath);
    return fi.size() <= (qint64)partitionBytes;
}

static bool detectSparseImage(const QString &imagePath)
{
    QFile f(imagePath);
    if (!f.open(QIODevice::ReadOnly)) return false;
    char magic[4];
    if (f.read(magic, 4) < 4) { f.close(); return false; }
    f.close();
    // Android sparse image magic: 0xED26FF3A (LE)
    return (static_cast<unsigned char>(magic[0]) == 0x3A &&
            static_cast<unsigned char>(magic[1]) == 0xFF &&
            static_cast<unsigned char>(magic[2]) == 0x26 &&
            static_cast<unsigned char>(magic[3]) == 0xED);
}

bool FlashPanel::ensureEDLConnected()
{
    if (m_flashTool->edlIsConnected()) return true;

    emit outputMessage("EDL 连接已断开，尝试重连...", false);

    if (m_programmerPath.isEmpty()) {
        emit outputMessage("需要 Firehose Programmer 文件", false);
        onEdlSelectProgrammer();
        if (m_programmerPath.isEmpty()) {
            emit outputMessage("未选择 Programmer，无法连接 EDL", true);
            return false;
        }
    }

    int attempts = 3;
    for (int i = 0; i < attempts; i++) {
        emit outputMessage(QString("连接 EDL... (尝试 %1/%2)").arg(i + 1).arg(attempts), false);
        m_flashTool->edlConnect(m_programmerPath);
        if (m_flashTool->edlIsConnected()) {
            m_flashTool->edlListPartitions();
            emit outputMessage("EDL 重连成功", false);
            return true;
        }
        QThread::msleep(1500);
    }

    emit outputMessage("EDL 连接失败，请检查设备与 Programmer 是否匹配", true);
    return false;
}

bool FlashPanel::ensureMTKConnected()
{
    if (m_flashTool->mtkIsConnected()) return true;

    emit outputMessage("MTK 连接已断开，尝试重连...", false);

    int attempts = 3;
    for (int i = 0; i < attempts; i++) {
        emit outputMessage(QString("连接 MTK... (尝试 %1/%2)").arg(i + 1).arg(attempts), false);
        if (m_flashTool->mtkConnect()) {
            m_flashTool->mtkListPartitions();
            emit outputMessage("MTK 重连成功", false);
            return true;
        }
        QThread::msleep(1500);
    }

    emit outputMessage("MTK 连接失败", true);
    return false;
}

QString FlashPanel::validateFirmwareDirectory(const QString &dir)
{
    QDir firmwareDir(dir);
    QStringList allFiles = firmwareDir.entryList({"*.img", "*.bin", "*.elf"},
        QDir::Files, QDir::Name);

    if (allFiles.isEmpty())
        return "目录中没有镜像文件（.img / .bin / .elf）";

    QStringList lowerFiles;
    for (const auto &f : allFiles)
        lowerFiles << f.toLower();

    bool hasGpt = false, hasPreloader = false, hasSbl = false, hasAbl = false;
    for (const auto &f : lowerFiles) {
        if (f.contains("gpt") || f.contains("partition")) hasGpt = true;
        if (f.contains("preloader")) hasPreloader = true;
        if (f.contains("sbl") || f.contains("xbl")) hasSbl = true;
        if (f.contains("abl") || f.contains("lk")) hasAbl = true;
    }

    if (!hasGpt)
        return "未检测到 GPT 分区表文件（gpt.bin / partition.bin）\n救砖必须包含 GPT！";

    if (!hasPreloader && !hasSbl && !hasAbl)
        return "未检测到引导加载器镜像（preloader / sbl / xbl / abl / lk）\n请确认这是完整的线刷包";

    // Platform & feature detection
    if (hasPreloader)
        emit outputMessage("◈ 检测到 MTK 平台（preloader）", false);
    else if (hasSbl || hasAbl)
        emit outputMessage("◈ 检测到 Qualcomm 平台（SBL/ABL）", false);

    int slotCount = 0;
    for (const auto &f : lowerFiles)
        if (f.contains("_a") || f.contains("_b")) slotCount++;
    if (slotCount > 3)
        emit outputMessage(QString("◈ 检测到 A/B slot 文件 (%1 个)").arg(slotCount), false);

    for (const auto &f : lowerFiles) {
        if (f.contains("super")) {
            emit outputMessage("◈ 检测到 super 镜像（动态分区）", false);
            break;
        }
    }

    emit outputMessage(QString("◈ 固件目录验证通过: %1 个镜像文件").arg(allFiles.size()), false);
    return {};
}

QStringList FlashPanel::matchPartitionFiles(const QString &dir, const QList<EDLPartition> &parts)
{
    QDir firmwareDir(dir);
    QStringList allFiles = firmwareDir.entryList({"*.img", "*.bin", "*.elf"},
        QDir::Files, QDir::Name);

    QMap<QString, QString> bestFile;   // partition name -> file path
    QMap<QString, int> bestScore;

    for (const auto &f : allFiles) {
        QString base = QFileInfo(f).completeBaseName().toLower();
        QString clean = base;
        clean.remove(QRegularExpression("_(a|b)$"));
        clean.remove(QRegularExpression("_(image)$"));
        clean.remove(QRegularExpression("^(img_)"));

        int topScore = 0;
        QString topPart;
        for (const auto &p : parts) {
            QString pLower = p.name.toLower();
            QString pClean = pLower;
            pClean.remove(QRegularExpression("_(a|b)$"));

            int score = 0;
            if (clean == pLower) score = 100;
            else if (clean == pClean) score = 80;
            else if (clean.contains(pLower) || pLower.contains(clean)) score = 50;

            if (score > topScore) {
                topScore = score;
                topPart = p.name;
            }
        }

        if (topScore >= 50) {
            QString fp = firmwareDir.filePath(f);
            auto it = bestFile.constFind(topPart);
            if (it == bestFile.constEnd() || topScore > bestScore.value(topPart, 0)) {
                bestFile[topPart] = fp;
                bestScore[topPart] = topScore;
            }
        }
    }

    QStringList matched;
    QStringList partsList = bestFile.keys();
    std::sort(partsList.begin(), partsList.end(),
        [&](const QString &a, const QString &b) {
            return sortPriority(bestFile[a]) < sortPriority(bestFile[b]);
        });
    for (const auto &p : partsList)
        matched << bestFile[p];

    emit outputMessage(QString("分区匹配: %1/%2 个文件已匹配").arg(matched.size()).arg(allFiles.size()), false);
    return matched;
}

QStringList FlashPanel::matchPartitionFilesMtk(const QString &dir, const QList<MtkPartition> &parts)
{
    QList<EDLPartition> edlParts;
    for (const auto &mp : parts) {
        EDLPartition ep;
        ep.name = mp.name;
        ep.startSector = mp.offset;
        ep.numSectors = mp.length;
        ep.sectorSize = 1;
        edlParts.append(ep);
    }
    return matchPartitionFiles(dir, edlParts);
}

bool FlashPanel::writePartitionWithRetry(const QString &partName,
    std::function<bool()> writeFn, int maxRetries)
{
    for (int attempt = 0; attempt <= maxRetries; attempt++) {
        if (attempt > 0) {
            emit outputMessage(QString("  ⚡ 重试 (%1/%2)...").arg(attempt).arg(maxRetries), false);
            if (!ensureEDLConnected() && !ensureMTKConnected()) {
                emit outputMessage("  重连失败，无法重试", true);
                return false;
            }
        }
        if (writeFn()) {
            emit outputMessage(QString("  ✓ %1 写入成功").arg(partName), false);
            return true;
        }
        if (attempt < maxRetries)
            QThread::msleep(500);
    }
    emit outputMessage(QString("  ✗ %1 写入失败（已重试 %2 次）").arg(partName).arg(maxRetries), true);
    return false;
}

QStringList FlashPanel::backupCriticalPartitions(const QString &backupDir)
{
    QStringList critical = {"gpt", "boot", "vbmeta", "persist"};
    QStringList backedUp;

    QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString dir = backupDir + "/brick_backup_" + ts;
    QDir().mkpath(dir);

    emit outputMessage("备份关键分区...", false);

    int total = critical.size();
    for (int i = 0; i < total; i++) {
        emit outputMessage(QString("  [%1/%2] 备份 %3...").arg(i + 1).arg(total).arg(critical[i]), false);
        bool ok = false;
        if (m_deviceInfo.mode == DeviceDetector::MODE_EDL_9008) {
            auto parts = m_flashTool->edlListPartitions();
            for (const auto &p : parts) {
                QString pl = p.name.toLower();
                if (pl == critical[i] || pl == critical[i] + "_a") {
                    QString outPath = dir + "/" + p.name + "_backup.img";
                    ok = m_flashTool->edlReadPartition(p, outPath);
                    if (ok) backedUp << outPath;
                    break;
                }
            }
        } else if (m_deviceInfo.mode == DeviceDetector::MODE_MTK_DA) {
            QString outPath = dir + "/" + critical[i] + "_backup.img";
            ok = m_flashTool->mtkReadPartition(critical[i], outPath);
            if (ok) backedUp << outPath;
        }
        if (!ok)
            emit outputMessage(QString("  ⚠ 备份 %1 失败（分区可能不存在）").arg(critical[i]), true);
    }

    emit outputMessage(QString("备份完成: %1 个分区已保存").arg(backedUp.size()), false);
    return backedUp;
}

void FlashPanel::saveProgressFile(const QString &dir, int completedIndex)
{
    QString safe = QFileInfo(dir).fileName();
    safe.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
    QString path = QDir::tempPath() + "/brick_progress_" + safe + ".txt";
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QByteArray::number(completedIndex));
        f.close();
    }
}

int FlashPanel::loadProgressFile(const QString &dir)
{
    QString safe = QFileInfo(dir).fileName();
    safe.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
    QString path = QDir::tempPath() + "/brick_progress_" + safe + ".txt";
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        int idx = f.readAll().trimmed().toInt();
        f.close();
        return idx;
    }
    return -1;
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

    // EDL 的"目录批量刷写"已确定性失效（Phase B Task 7 审查发现，Q2 改动引入的 UI 级回归）：
    // 本流程拿镜像 basename（boot/vbmeta…）去比 EDLPartition::name，而 Firehose 只枚举得出
    // "lun<N>"（无 GPT 分区名）—— 匹配必然全部落空、每个镜像都报"EDL 未能匹配分区"。
    // 处置（Task 8 前置，二选一）：**明确拒绝并指路刷写计划**（不静默失败）。选它而不是"改走计划
    // 条目"的理由：本流程是"裸镜像目录 + 按名打分匹配 + 断点续传文件"的形状，目录里没有
    // rawprogram/patch XML 或 GPT，给不出 validatePlan 需要的 LUN/起始扇区/扇区数几何 ——
    // 改成计划驱动等于把救砖流程重写成另一个功能（超出本任务范围，且计划层已覆盖该场景）。
    // MTK 不受影响：mtkListPartitions 给的是真实分区名。
    // 下游仍留有 EDL 侧旧分支（连接/匹配/备份/写/断开）：它们与 MTK 共用同一流程骨架，逐个摘除会
    // 牵连 backupCriticalPartitions / matchPartitionFiles 等只服务本流程的成员 —— 待计划方式接入
    // 后按整块清理；本入口（唯一调用点）已在源头拒绝，不会执行到它们。
    if (edlMode) {
        emit outputMessage(QStringLiteral(
            "EDL 模式已不支持「死砖修复」目录批量刷写：Firehose 只回报 lun<N>，"
            "无法把镜像文件名映射到分区。请改用「EDL 刷写计划…」"), true);
        QMessageBox::information(this, QStringLiteral("死砖修复"),
            QStringLiteral("EDL 模式请改用「EDL 刷写计划…」：\n\n"
                "  1. 先解包官方线刷包（.ofp/.ops）得到 rawprogram*.xml + 镜像目录\n"
                "  2. 点「EDL 刷写计划…」选择该目录（或直接选整包）\n"
                "  3. 预览条目并确认后按计划刷写\n\n"
                "计划以 XML/GPT 的分区名与几何为准（含 LUN / 起始扇区 / 扇区数），"
                "不再依赖文件名匹配。\n"
                "MTK 模式的死砖修复不受影响，仍可继续使用。"));
        return;
    }

    // Phase 1: 确保连接（带自动重试）
    if (edlMode && !ensureEDLConnected()) return;
    if (mtkMode && !ensureMTKConnected()) return;

    // Phase 2: 选择并验证固件目录
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

    QString validationError = validateFirmwareDirectory(dir);
    if (!validationError.isEmpty()) {
        QMessageBox::StandardButton ignore = QMessageBox::warning(this,
            "固件目录验证失败",
            validationError + "\n\n是否仍要继续？",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ignore != QMessageBox::Yes) return;
    }

    // Phase 3: 可选备份
    QMessageBox::StandardButton doBackup = QMessageBox::question(this,
        "备份关键分区",
        "是否在刷写前备份当前设备的关键分区（GPT / boot / vbmeta / persist）？\n"
        "备份后若修复失败可用来恢复。",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (doBackup == QMessageBox::Yes) {
        QString backupDir = QFileDialog::getExistingDirectory(this, "选择备份保存目录", dir);
        if (!backupDir.isEmpty())
            backupCriticalPartitions(backupDir);
    }

    // Phase 4: 扫描并匹配分区（带权重评分）
    emit outputMessage("======== 死砖修复开始 ========", false);
    emit outputMessage("正在扫描并匹配分区镜像...", false);

    QStringList foundFiles;
    if (edlMode) {
        auto parts = m_flashTool->edlListPartitions();
        foundFiles = matchPartitionFiles(dir, parts);
    } else {
        auto parts = m_flashTool->mtkListPartitions();
        foundFiles = matchPartitionFilesMtk(dir, parts);
    }

    // 匹配失败时回退到关键词搜索
    if (foundFiles.isEmpty()) {
        emit outputMessage("分区匹配失败，回退到关键词搜索...", false);
        QStringList searchNames = {
            "gpt", "partition", "sbl", "sbl1", "xbl", "abl", "lk",
            "preloader", "boot", "vbmeta", "dtbo", "dpm", "tz",
            "hyp", "keymaster", "cmnlib", "devcfg", "storsec"
        };
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
            QString singleFile = QFileDialog::getOpenFileName(this,
                "选择镜像文件", dir,
                "镜像 (*.img *.bin *.elf);;所有文件 (*)");
            if (singleFile.isEmpty()) return;
            foundFiles << singleFile;
        }
    }

    // Phase 5: 排序
    std::sort(foundFiles.begin(), foundFiles.end(), compareByPriority);

    // Phase 6: 断点续传检查
    int startIndex = loadProgressFile(dir);
    if (startIndex > 0 && startIndex < foundFiles.size()) {
        QMessageBox::StandardButton resume = QMessageBox::question(this,
            "检测到上次进度",
            QString("上次修复进度: %1/%2\n是否从中断处继续？")
                .arg(startIndex).arg(foundFiles.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        if (resume != QMessageBox::Yes) startIndex = 0;
    } else {
        startIndex = 0;
    }

    // 显示文件列表并确认
    QString fileList;
    for (int i = startIndex; i < foundFiles.size(); i++)
        fileList += "  " + QFileInfo(foundFiles[i]).fileName() + "\n";

    QString modeStr = edlMode ? "EDL Firehose" : "MTK DA";
    QMessageBox::StandardButton confirm = QMessageBox::warning(this,
        "确认死砖修复",
        QString("即将通过 %1 按序写入以下 %2 个文件：\n\n%3\n"
                "⚠️ 写入错误的分区镜像将导致设备永久损坏！\n"
                "请确保文件来源可靠（官方线刷包）。\n\n是否继续？")
            .arg(modeStr).arg(foundFiles.size() - startIndex).arg(fileList),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (confirm != QMessageBox::Yes) return;

    // Phase 7: 执行写入
    m_brickRepairBtn->setEnabled(false);
    m_progressBar->setVisible(true);
    m_progressBar->setValue(0);

    // 预收集分区大小用于校验
    QMap<QString, quint64> partSizeMap;
    if (edlMode) {
        auto parts = m_flashTool->edlListPartitions();
        for (const auto &p : parts)
            partSizeMap[p.name.toLower()] = p.numSectors * p.sectorSize;
    } else {
        auto parts = m_flashTool->mtkListPartitions();
        for (const auto &p : parts)
            partSizeMap[p.name.toLower()] = p.length;
    }

    int total = foundFiles.size();
    bool allOk = true;

    for (int i = startIndex; i < total; i++) {
        const auto &filePath = foundFiles[i];
        QString fileName = QFileInfo(filePath).fileName();
        QString partBase = QFileInfo(filePath).completeBaseName();

        emit outputMessage(QString("[%1/%2] %3...").arg(i + 1).arg(total).arg(fileName), false);
        m_progressBar->setValue(i * 100 / total);
        QCoreApplication::processEvents();

        // 镜像大小校验
        QString pLower = partBase.toLower();
        pLower.remove(QRegularExpression("_(a|b)$"));
        if (!checkImageSize(filePath, partSizeMap.value(pLower, 0))) {
            emit outputMessage(QString("  ⚠ %1 镜像超出分区容量，跳过").arg(fileName), true);
            allOk = false;
            saveProgressFile(dir, i + 1);
            continue;
        }

        // 稀疏镜像检测
        if (detectSparseImage(filePath))
            emit outputMessage("  ℹ 稀疏镜像格式", false);

        // 写入（带重试）
        bool ok = writePartitionWithRetry(partBase, [&]() {
            if (edlMode) {
                auto parts = m_flashTool->edlListPartitions();
                QString pLower2 = partBase.toLower();
                // Try exact match first, then partial
                for (const auto &p : parts) {
                    QString pl = p.name.toLower();
                    QString pClean = pl;
                    pClean.remove(QRegularExpression("_(a|b)$"));
                    if (pClean == pLower2 || pl == pLower2 ||
                        pLower2.contains(pl) || pl.contains(pLower2)) {
                        return m_flashTool->edlWritePartition(p, filePath);
                    }
                }
                emit outputMessage(QString("  EDL 未能匹配分区 %1").arg(partBase), true);
                return false;
            } else {
                return m_flashTool->mtkWritePartition(partBase, filePath);
            }
        });

        if (ok) {
            saveProgressFile(dir, i + 1);
        } else {
            allOk = false;
            saveProgressFile(dir, i);
            emit outputMessage(QString("  ✗ %1 写入失败").arg(fileName), true);
            QMessageBox::critical(this, "写入失败",
                QString("分区 %1 写入失败。\n\n"
                    "可能的原因：\n"
                    "  1. 镜像与分区不匹配（名称/大小）\n"
                    "  2. EDL/MTK 连接已断开\n"
                    "  3. 分区受保护或不存在\n\n"
                    "修复问题后可再次点击「死砖修复」继续。")
                .arg(fileName));
            break;
        }
    }

    m_progressBar->setValue(100);

    // Phase 8: 清理
    if (allOk) {
        QString safe = QFileInfo(dir).fileName();
        safe.replace(QRegularExpression("[^a-zA-Z0-9_-]"), "_");
        QFile::remove(QDir::tempPath() + "/brick_progress_" + safe + ".txt");
    }

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
            "部分分区写入失败。\n\n"
            "已保存当前进度，下次点击「死砖修复」可选择继续。\n"
            "请查看输出日志获取详细信息。");
    }

    m_brickRepairBtn->setEnabled(true);
}
