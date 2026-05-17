#include "system_tool_panel.h"
#include "core/adb_embedded.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QScrollArea>
#include <QFrame>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileDialog>
#include <QDateTime>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QCoreApplication>

static const char *kCategories[] = {
    "性能调优", "界面定制", "功能增强", "分区管理",
    "应用管理", "安全隐私", "开发调试"
};
static const int kCategoryCount = 7;

SystemToolPanel::SystemToolPanel(QWidget *parent)
    : QWidget(parent)
    , m_asyncProc(nullptr)
    , m_monitorProc(nullptr)
{
    setupUI();
    m_perfTimer = new QTimer(this);
    m_perfTimer->setInterval(500);
    connect(m_perfTimer, &QTimer::timeout, this, &SystemToolPanel::onPerformancePoll);
}

// ==================== 辅助方法 ====================

QString SystemToolPanel::executeAdb(const QStringList &args, int timeoutMs)
{
    if (!AdbEmbedded::instance().initialize())
        return "Error: ADB not initialized";
    QString adb = AdbEmbedded::instance().getAdbPath();
    if (adb.isEmpty()) return "Error: ADB path empty";
    QProcess proc;
    proc.setProgram(adb); proc.setArguments(args);
    proc.start();
    if (!proc.waitForFinished(timeoutMs)) { proc.kill(); return "Error: timeout"; }
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

QString SystemToolPanel::adbShell(const QString &cmd, int timeoutMs)
{
    QString dev = m_deviceInfo.serialNumber;
    if (dev.isEmpty()) return "Error: no device";
    return executeAdb({"-s", dev, "shell", cmd}, timeoutMs);
}

void SystemToolPanel::runAdbAsync(const QStringList &args)
{
    if (!AdbEmbedded::instance().initialize()) return;
    QString adb = AdbEmbedded::instance().getAdbPath();
    if (adb.isEmpty()) return;
    if (m_asyncProc) { m_asyncProc->kill(); m_asyncProc->deleteLater(); }
    m_asyncProc = new QProcess(this);
    m_asyncProc->setProgram(adb); m_asyncProc->setArguments(args);
    m_asyncProc->start();
}

void SystemToolPanel::appendOutput(const QString &msg, bool isError)
{
    emit outputMessage(msg, isError);
    QCoreApplication::processEvents();
}

void SystemToolPanel::logAndRefresh(const QString &label, const QString &result)
{
    appendOutput(label + ": " + result, result.contains("Error"));
}


// ==================== setupUI ====================

void SystemToolPanel::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(4,4,4,4); mainLayout->setSpacing(4);

    // Top bar
    QHBoxLayout *top = new QHBoxLayout();
    m_deviceLabel = new QLabel("设备: 未连接", this);
    m_rootLabel = new QLabel("", this);
    m_backBtn = new QPushButton("返回", this);
    top->addWidget(m_deviceLabel); top->addWidget(m_rootLabel);
    top->addStretch(); top->addWidget(m_backBtn);
    mainLayout->addLayout(top);

    // Body: category list (left) + pages (right)
    QHBoxLayout *body = new QHBoxLayout();
    m_categoryList = new QListWidget(this);
    m_categoryList->setFixedWidth(110);
    m_categoryList->setSpacing(2);
    for (int i = 0; i < kCategoryCount; i++)
        m_categoryList->addItem(QString::fromUtf8(kCategories[i]));

    m_categoryStack = new QStackedWidget(this);
    for (int i = 0; i < kCategoryCount; i++) {
        QWidget *page = createPage(i);
        m_pages.append(page);
        m_categoryStack->addWidget(page);
    }
    m_categoryStack->setCurrentIndex(0);

    body->addWidget(m_categoryList);
    body->addWidget(m_categoryStack, 1);
    mainLayout->addLayout(body, 1);

    // Connections
    connect(m_backBtn, &QPushButton::clicked, this, &SystemToolPanel::switchToDeviceInfo);
    connect(m_categoryList, &QListWidget::currentRowChanged, this, &SystemToolPanel::onCategoryChanged);
}

void SystemToolPanel::onCategoryChanged(int index)
{
    if (index >= 0 && index < kCategoryCount)
        m_categoryStack->setCurrentIndex(index);
}

// ==================== setDeviceInfo / clearDeviceInfo ====================

void SystemToolPanel::setDeviceInfo(const DeviceInfo &info)
{
    m_deviceInfo = info;
    m_deviceLabel->setText(QString("设备: %1").arg(
        info.serialNumber.isEmpty() ? "未知" : info.serialNumber));

    bool adbMode = (info.mode == DeviceDetector::MODE_ADB);
    bool recovery = (info.mode == DeviceDetector::MODE_RECOVERY);
    bool enabled = adbMode || recovery;

    m_rootLabel->setText(info.isRooted ? "已 Root" : "未 Root");
    m_rootLabel->setStyleSheet(info.isRooted ? "color:#2a82da;font-weight:bold;" : "color:#888;");

    // Enable widgets for ADB mode (most commands work without root via shell user)
    for (auto *page : m_pages) {
        auto btns = page->findChildren<QPushButton*>();
        for (auto *b : btns) {
            if (b != m_backBtn && b != m_perfToggleBtn) b->setEnabled(enabled);
        }
        auto lists = page->findChildren<QListWidget*>();
        for (auto *l : lists) l->setEnabled(enabled);
    }
    m_backBtn->setEnabled(true);
    if (m_perfToggleBtn) m_perfToggleBtn->setEnabled(enabled);

    // Auto-fill app list on first switch
    if (enabled && m_appList && m_appList->count() == 0)
        onAppRefreshList();
}

void SystemToolPanel::clearDeviceInfo()
{
    m_deviceInfo = DeviceInfo();
    m_deviceLabel->setText("设备: 未连接");
    m_rootLabel->setText("");
    for (auto *page : m_pages) {
        auto btns = page->findChildren<QPushButton*>();
        for (auto *b : btns) b->setEnabled(false);
        auto lists = page->findChildren<QListWidget*>();
        for (auto *l : lists) l->setEnabled(false);
    }
    auto statuses = findChildren<QLabel*>();
    for (auto *s : statuses) {
        if (s != m_deviceLabel && s != m_rootLabel)
            s->setText("");
    }
    if (m_appList) m_appList->clear();
    if (m_perfTimer) m_perfTimer->stop();
    stopMonitorProcess();
    if (m_perfToggleBtn) { m_perfToggleBtn->setChecked(false); m_perfToggleBtn->setText("▶ 开始监控"); }
    for (auto *chart : m_cpuCharts) {
        m_cpuGrid->removeWidget(chart);
        chart->deleteLater();
    }
    m_cpuCharts.clear();
    m_cpuCoreCount = 0;

    // Re-add single placeholder chart
    auto *placeholder = new LiveChartWidget(m_perfPage);
    placeholder->setTitle("CPU 频率");
    placeholder->setUnit("MHz");
    placeholder->setColor(QColor("#f38ba8"));
    placeholder->setYRange(0, 3000);
    placeholder->setMaxPoints(120);
    placeholder->setMinimumHeight(120);
    m_cpuGrid->addWidget(placeholder, 0, 0, 1, 4);
    m_cpuCharts.append(placeholder);
    m_cpuFullHistory.clear();
    m_gpuFullHistory.clear();
    m_tempFullHistory.clear();
    m_memFullHistory.clear();
    if (m_saveChartBtn) m_saveChartBtn->setEnabled(false);
    if (m_gpuChart) m_gpuChart->clearData();
    if (m_tempChart) m_tempChart->clearData();
    if (m_memChart) m_memChart->clearData();
}

// ==================== 页面构建 ====================

QWidget *SystemToolPanel::createPage(int cat)
{
    QWidget *page = new QWidget(this);
    QVBoxLayout *lay = new QVBoxLayout(page);
    lay->setContentsMargins(4,4,4,4); lay->setSpacing(6);

    auto addSection = [&](const QString &title) {
        auto *lb = new QLabel(title, page);
        lb->setStyleSheet("font-weight:bold;font-size:13px;color:#2a82da;");
        lay->addWidget(lb);
    };
    auto addBtn = [&](const QString &text, const QString &tooltip) -> QPushButton* {
        auto *b = new QPushButton(text, page);
        b->setToolTip(tooltip); b->setEnabled(false);
        b->setMinimumHeight(28);
        lay->addWidget(b);
        return b;
    };
    auto addStatus = [&]() -> QLabel* {
        auto *lb = new QLabel("", page);
        lb->setStyleSheet("color:#666;font-size:11px;padding:2px 0;");
        lb->setWordWrap(true);
        lay->addWidget(lb);
        return lb;
    };
    auto addHint = [&](const QString &text) {
        auto *lb = new QLabel(text, page);
        lb->setStyleSheet("color:#888;font-size:10px;font-style:italic;");
        lb->setWordWrap(true);
        lay->addWidget(lb);
    };

    switch (cat) {
    // ========== 0: 性能调优 ==========
    case 0: {
        m_perfPage = page;

        // CPU core grid — initially one placeholder chart, splits on device connect
        m_cpuGrid = new QGridLayout();
        m_cpuGrid->setSpacing(4);
        auto *placeholder = new LiveChartWidget(page);
        placeholder->setTitle("CPU 频率");
        placeholder->setUnit("MHz");
        placeholder->setColor(QColor("#f38ba8"));
        placeholder->setYRange(0, 3000);
        placeholder->setMaxPoints(120);
        placeholder->setMinimumHeight(120);
        m_cpuGrid->addWidget(placeholder, 0, 0, 1, 4);
        m_cpuCharts.append(placeholder);
        lay->addLayout(m_cpuGrid, 1);

        // GPU, Temp, Mem — bottom row (3 columns)
        QHBoxLayout *bottomRow = new QHBoxLayout();
        bottomRow->setSpacing(6);

        auto createChart = [&](const QString &title, const QString &unit,
                                const QColor &color, double minY, double maxY) -> LiveChartWidget* {
            auto *chart = new LiveChartWidget(page);
            chart->setTitle(title);
            chart->setUnit(unit);
            chart->setColor(color);
            chart->setYRange(minY, maxY);
            chart->setMaxPoints(120);
            chart->setMinimumHeight(110);
            bottomRow->addWidget(chart, 1);
            return chart;
        };

        m_gpuChart = createChart("GPU 频率", "MHz", QColor("#a6e3a1"), 0, 1000);
        m_tempChart = createChart("温度", "°C", QColor("#fab387"), 0, 100);
        m_memChart  = createChart("内存使用率", "%", QColor("#89b4fa"), 0, 100);
        lay->addLayout(bottomRow);

        // Control row
        QHBoxLayout *ctrlRow = new QHBoxLayout();
        m_perfToggleBtn = new QPushButton("▶ 开始监控", page);
        m_perfToggleBtn->setEnabled(false);
        m_perfToggleBtn->setCheckable(true);
        m_perfToggleBtn->setMinimumHeight(28);
        ctrlRow->addWidget(m_perfToggleBtn);

        auto *cpuGovBtn = new QPushButton("CPU Governor", page);
        cpuGovBtn->setObjectName("cpuGov");
        cpuGovBtn->setEnabled(false);
        cpuGovBtn->setToolTip("查看/设置 CPU 调度策略");
        cpuGovBtn->setMinimumHeight(28);
        ctrlRow->addWidget(cpuGovBtn);

        auto *memBtn = new QPushButton("内存优化", page);
        memBtn->setObjectName("memory");
        memBtn->setEnabled(false);
        memBtn->setToolTip("调整 swappiness");
        memBtn->setMinimumHeight(28);
        ctrlRow->addWidget(memBtn);

        auto *ioBtn = new QPushButton("I/O 调度", page);
        ioBtn->setObjectName("ioSched");
        ioBtn->setEnabled(false);
        ioBtn->setToolTip("更改 I/O 调度算法");
        ioBtn->setMinimumHeight(28);
        ctrlRow->addWidget(ioBtn);

        ctrlRow->addStretch();
        m_saveChartBtn = new QPushButton("保存截图", page);
        m_saveChartBtn->setToolTip("保存完整监控历史截图");
        m_saveChartBtn->setMinimumHeight(28);
        m_saveChartBtn->setEnabled(false);
        ctrlRow->addWidget(m_saveChartBtn);
        lay->addLayout(ctrlRow);

        // Status label
        m_perfStatus = new QLabel("", page);
        m_perfStatus->setStyleSheet("color:#6c7086;font-size:10px;");
        lay->addWidget(m_perfStatus);

        connect(m_perfToggleBtn, &QPushButton::clicked, this, &SystemToolPanel::onPerfTimerToggle);
        connect(m_saveChartBtn, &QPushButton::clicked, this, &SystemToolPanel::onSaveChart);
        connect(page->findChild<QPushButton*>("cpuGov"), &QPushButton::clicked, this, &SystemToolPanel::onCpuGovernor);
        connect(page->findChild<QPushButton*>("memory"), &QPushButton::clicked, this, &SystemToolPanel::onMemoryOptimize);
        connect(page->findChild<QPushButton*>("ioSched"), &QPushButton::clicked, this, &SystemToolPanel::onIOScheduler);
        break;
    }
    // ========== 1: 界面定制 ==========
    case 1: {
        addSection("显示");
        addBtn("调整 DPI", "更改显示密度 (density)，影响所有界面元素大小")->setObjectName("dpi");
        addBtn("动画速度", "调整窗口动画/过渡动画速度 (0.5x-10x)")->setObjectName("anim");
        m_dpiStatus = addStatus();

        addSection("状态栏与导航栏");
        addBtn("状态栏定制", "设置状态栏显示项 (电量百分比、时钟格式等)")->setObjectName("statusbar");
        addBtn("导航栏定制", "调整导航栏高度/布局/手势")->setObjectName("navbar");
        m_animStatus = addStatus();

        addSection("开机动画");
        addBtn("更换开机动画", "替换 /system/media/bootanimation.zip")->setObjectName("bootanim");
        addBtn("更换字体", "替换 /system/fonts/ 全局字体")->setObjectName("font");

        addHint("部分修改需要 /system 分区可读写。重启生效。");

        connect(page->findChild<QPushButton*>("dpi"), &QPushButton::clicked, this, &SystemToolPanel::onSetDpi);
        connect(page->findChild<QPushButton*>("anim"), &QPushButton::clicked, this, &SystemToolPanel::onAnimationScale);
        connect(page->findChild<QPushButton*>("statusbar"), &QPushButton::clicked, this, &SystemToolPanel::onStatusBar);
        connect(page->findChild<QPushButton*>("navbar"), &QPushButton::clicked, this, &SystemToolPanel::onNavBar);
        connect(page->findChild<QPushButton*>("bootanim"), &QPushButton::clicked, this, &SystemToolPanel::onBootAnimation);
        connect(page->findChild<QPushButton*>("font"), &QPushButton::clicked, this, &SystemToolPanel::onSetFont);
        break;
    }
    // ========== 2: 功能增强 ==========
    case 2: {
        addSection("Xposed / LSPosed 管理");
        addHint("Xposed/LSPosed 框架可以为系统注入模块以实现深度定制。");
        addHint("需先安装 LSPosed 框架 (Magisk 模块或 APK)。");
        addBtn("检查 Xposed 状态", "检测设备是否已安装 Xposed/LSPosed 框架")->setObjectName("xposedCheck");
        addBtn("LSPosed 管理器", "打开 LSPosed Manager (需已安装)")->setObjectName("xposedOpen");
        addSection("推荐模块");
        addHint("• PixelXpert — 深度系统 UI 定制\n• AndroidFaker — 伪造设备标识\n• XPrivacyLua — 隐私权限控制\n• 核心破解 — 签名验证绕过");

        connect(page->findChild<QPushButton*>("xposedCheck"), &QPushButton::clicked, this, [this](){
            QString r = adbShell("su -c 'ls /data/data/org.lsposed.manager 2>/dev/null && echo FOUND || echo NOT_FOUND'");
            appendOutput("Xposed/LSPosed: " + r, !r.contains("FOUND"));
        });
        connect(page->findChild<QPushButton*>("xposedOpen"), &QPushButton::clicked, this, [this](){
            adbShell("monkey -p org.lsposed.manager 1");
            appendOutput("尝试打开 LSPosed Manager...", false);
        });
        break;
    }
    // ========== 3: 分区管理 ==========
    case 3: {
        addSection("分区信息");
        addBtn("列出块设备", "显示 /dev/block/by-name/ 全部分区")->setObjectName("listBlock");
        addBtn("Super 分区", "查看 super 内部动态分区布局 (lpdump)")->setObjectName("superInfo");
        m_blockStatus = addStatus();

        addSection("挂载操作");
        addBtn("挂载 System (RW)", "以可读写方式重新挂载 /system 分区")->setObjectName("mountSys");
        addBtn("卸载分区", "安全卸载指定分区")->setObjectName("unmount");

        addHint("注意: 修改分区表(sgdisk/gpt)需要 EDL/MTK 模式，极其危险。");

        connect(page->findChild<QPushButton*>("listBlock"), &QPushButton::clicked, this, &SystemToolPanel::onListBlockDevices);
        connect(page->findChild<QPushButton*>("superInfo"), &QPushButton::clicked, this, &SystemToolPanel::onSuperInfo);
        connect(page->findChild<QPushButton*>("mountSys"), &QPushButton::clicked, this, &SystemToolPanel::onMountSystem);
        connect(page->findChild<QPushButton*>("unmount"), &QPushButton::clicked, this, &SystemToolPanel::onUnmountSystem);
        break;
    }
    // ========== 4: 应用管理 ==========
    case 4: {
        addSection("应用列表");

        // Search box
        m_searchBox = new QLineEdit(page);
        m_searchBox->setPlaceholderText("搜索应用 (包名或名称)...");
        m_searchBox->setClearButtonEnabled(true);
        m_searchBox->setEnabled(false);
        lay->addWidget(m_searchBox);

        // Icon toggle

        QHBoxLayout *filterRow = new QHBoxLayout();
        auto *refreshBtn = new QPushButton("刷新列表", page);
        auto *filterAllBtn = new QPushButton("全部", page);
        auto *filter3rdBtn = new QPushButton("三方", page);
        auto *filterSysBtn = new QPushButton("系统", page);
        refreshBtn->setEnabled(false);
        filterAllBtn->setCheckable(true); filterAllBtn->setChecked(true);
        filter3rdBtn->setCheckable(true); filterSysBtn->setCheckable(true);
        filterRow->addWidget(refreshBtn);
        filterRow->addWidget(filterAllBtn);
        filterRow->addWidget(filter3rdBtn);
        filterRow->addWidget(filterSysBtn);
        filterRow->addStretch();
        lay->addLayout(filterRow);

        QListWidget *appList = new QListWidget(page);
        appList->setObjectName("appList");
        appList->setSelectionMode(QAbstractItemView::SingleSelection);
        appList->setMinimumHeight(120);
        appList->setEnabled(false);
        lay->addWidget(appList, 1);

        addSection("操作");
        QHBoxLayout *actRow = new QHBoxLayout();
        auto *launchBtn = new QPushButton("启动", page);
        auto *freezeBtn = new QPushButton("冻结", page);
        auto *uninstBtn = new QPushButton("卸载", page);
        auto *extractBtn = new QPushButton("提取 APK", page);
        launchBtn->setObjectName("launchApp"); launchBtn->setEnabled(false);
        freezeBtn->setObjectName("freezeApp"); freezeBtn->setEnabled(false);
        uninstBtn->setObjectName("uninstApp"); uninstBtn->setEnabled(false);
        extractBtn->setObjectName("extractApp"); extractBtn->setEnabled(false);
        actRow->addWidget(launchBtn);
        actRow->addWidget(freezeBtn);
        actRow->addWidget(uninstBtn);
        actRow->addWidget(extractBtn);
        actRow->addStretch();
        lay->addLayout(actRow);

        m_appStatus = addStatus();
        m_appList = appList;

        // Search filter
        connect(m_searchBox, &QLineEdit::textChanged, this, [this](const QString &text) {
            for (int i = 0; i < m_appList->count(); i++) {
                auto *item = m_appList->item(i);
                bool match = text.isEmpty() ||
                    item->data(Qt::UserRole).toString().contains(text, Qt::CaseInsensitive) ||
                    item->data(Qt::UserRole + 2).toString().contains(text, Qt::CaseInsensitive);
                item->setHidden(!match);
            }
        });

connect(refreshBtn, &QPushButton::clicked, this, &SystemToolPanel::onAppRefreshList);
        connect(filterAllBtn, &QPushButton::clicked, this, [this](){ m_appFilter=0; onAppRefreshList(); });
        connect(filter3rdBtn, &QPushButton::clicked, this, [this](){ m_appFilter=1; onAppRefreshList(); });
        connect(filterSysBtn, &QPushButton::clicked, this, [this](){ m_appFilter=2; onAppRefreshList(); });
        connect(launchBtn, &QPushButton::clicked, this, &SystemToolPanel::onAppLaunch);
        connect(freezeBtn, &QPushButton::clicked, this, &SystemToolPanel::onAppFreezeToggle);
        connect(uninstBtn, &QPushButton::clicked, this, &SystemToolPanel::onAppUninstall);
        connect(extractBtn, &QPushButton::clicked, this, &SystemToolPanel::onAppExtract);
        connect(appList, &QListWidget::currentRowChanged, this, [this, launchBtn, freezeBtn, uninstBtn, extractBtn](){
            bool has = m_appList->currentItem() != nullptr;
            launchBtn->setEnabled(has);
            freezeBtn->setEnabled(has);
            uninstBtn->setEnabled(has);
            extractBtn->setEnabled(has);
        });
        break;
    }
    // ========== 5: 安全隐私 ==========
    case 5: {
        addSection("设备标识");
        addBtn("编辑 build.prop", "查看/修改 ro.* 系统属性")->setObjectName("buildprop");
        addBtn("伪造设备信息", "修改机型/品牌/IMEI 等标识 (需重启生效)")->setObjectName("spoof");
        m_buildPropStatus = addStatus();

        addSection("系统安全");
        addBtn("SELinux 模式", "切换 SELinux Enforcing/Permissive")->setObjectName("selinux");
        addBtn("SafetyNet 绕过", "修改系统属性以通过 CTS 认证")->setObjectName("safetynet");
        m_selinuxLabel = addStatus();

        addSection("签名");
        addBtn("启用签名伪造", "修改 system/etc/permissions 支持签名伪造 (需要 Xposed)")->setObjectName("sigspoof");
        addHint("部分操作需要重启生效。伪造 IMEI 在某些地区可能违法。");

        connect(page->findChild<QPushButton*>("buildprop"), &QPushButton::clicked, this, &SystemToolPanel::onEditBuildProp);
        connect(page->findChild<QPushButton*>("spoof"), &QPushButton::clicked, this, &SystemToolPanel::onSpoofDevice);
        connect(page->findChild<QPushButton*>("selinux"), &QPushButton::clicked, this, &SystemToolPanel::onSelinuxMode);
        connect(page->findChild<QPushButton*>("safetynet"), &QPushButton::clicked, this, &SystemToolPanel::onSafetynetBypass);
        connect(page->findChild<QPushButton*>("sigspoof"), &QPushButton::clicked, this, &SystemToolPanel::onSignatureSpoof);
        break;
    }
    // ========== 6: 开发调试 ==========
    case 6: {
        addSection("日志");
        addBtn("Logcat", "实时抓取系统日志 (输出到 Output 面板)")->setObjectName("logcat");
        addBtn("Bugreport", "生成完整错误报告 (耗时较长)")->setObjectName("bugreport");

        addSection("系统信息");
        addBtn("Dumpsys", "运行 dumpsys 并输出关键系统服务状态")->setObjectName("dumpsys");
        addBtn("Shizuku 状态", "检查 Shizuku 服务是否运行")->setObjectName("shizuku");
        m_shizukuStatus = addStatus();

        addSection("ROM 工具");
        addHint("ROM 解包/打包需要外部工具: \n  • payload_dumper — 解压 payload.bin\n  • img2simg/simg2img — sparse 镜像转换\n  • apktool — APK 反编译");

        connect(page->findChild<QPushButton*>("logcat"), &QPushButton::clicked, this, &SystemToolPanel::onLogcat);
        connect(page->findChild<QPushButton*>("bugreport"), &QPushButton::clicked, this, &SystemToolPanel::onTakeBugreport);
        connect(page->findChild<QPushButton*>("dumpsys"), &QPushButton::clicked, this, &SystemToolPanel::onDumpsys);
        connect(page->findChild<QPushButton*>("shizuku"), &QPushButton::clicked, this, &SystemToolPanel::onShizukuStatus);
        break;
    }
    }
    return page;
}

// =====================================================================
// ==================== 持久 ADB Shell 监控进程 ====================

void SystemToolPanel::startMonitorProcess()
{
    QString adb = AdbEmbedded::instance().getAdbPath();
    if (adb.isEmpty() || m_deviceInfo.serialNumber.isEmpty()) {
        appendOutput("无法启动监控: ADB 或设备未就绪", true);
        return;
    }
    if (m_monitorProc) { m_monitorProc->kill(); m_monitorProc->deleteLater(); }
    m_monitorProc = new QProcess(this);
    m_monitorProc->setProgram(adb);
    m_monitorProc->setArguments({"-s", m_deviceInfo.serialNumber, "shell"});
    m_monitorProc->start();
    if (!m_monitorProc->waitForStarted(5000)) {
        appendOutput("监控进程启动失败", true);
        m_monitorProc->deleteLater();
        m_monitorProc = nullptr;
        return;
    }
    // Flush initial shell banner/prompt
    m_monitorProc->waitForReadyRead(300);
    m_monitorProc->readAllStandardOutput();
}

void SystemToolPanel::stopMonitorProcess()
{
    if (m_monitorProc) {
        m_monitorProc->kill();
        m_monitorProc->deleteLater();
        m_monitorProc = nullptr;
    }
}

QString SystemToolPanel::monitorExec(const QString &cmd, int timeoutMs)
{
    if (!m_monitorProc || m_monitorProc->state() != QProcess::Running)
        return "Error: monitor process not running";

    // Discard stale output
    m_monitorProc->readAllStandardOutput();

    // Write command + end marker
    QByteArray input = cmd.toUtf8();
    if (!input.endsWith('\n')) input += '\n';
    input += "echo __MON_END__\n";
    m_monitorProc->write(input);

    // Read until end marker or timeout
    QString output;
    auto deadline = QDateTime::currentDateTime().addMSecs(timeoutMs);
    while (QDateTime::currentDateTime() < deadline) {
        if (m_monitorProc->waitForReadyRead(qMin(200, deadline.msecsTo(QDateTime::currentDateTime())))) {
            output += QString::fromUtf8(m_monitorProc->readAllStandardOutput());
            if (output.contains("__MON_END__")) {
                return output.section("__MON_END__", 0, 0).trimmed();
            }
        } else {
            break;
        }
    }

    appendOutput("监控进程响应超时，正在重启...", true);
    stopMonitorProcess();
    startMonitorProcess();
    return "Error: timeout";
}

//  1. 性能调优 — 实时监控
// =====================================================================

void SystemToolPanel::onPerfTimerToggle()
{
    if (!m_perfTimer) return;
    if (m_perfTimer->isActive()) {
        m_perfTimer->stop();
        m_perfToggleBtn->setText("▶ 开始监控");
        if (m_perfStatus) m_perfStatus->setText("监控已暂停");
        stopMonitorProcess();
    } else {
        startMonitorProcess();
        if (!m_monitorProc) return;
        m_perfTimer->start();
        m_perfToggleBtn->setText("⏸ 暂停");
        onPerformancePoll();
        if (m_perfStatus) m_perfStatus->setText("监控运行中 (0.5s 间隔)");
    }
}

void SystemToolPanel::onPerformancePoll()
{
    if (m_polling) return;
    m_polling = true;

    if (m_deviceInfo.serialNumber.isEmpty()) {
        m_perfTimer->stop();
        m_perfToggleBtn->setText("▶ 开始监控");
        m_polling = false;
        return;
    }

    // Detect core count and split into per-core charts on first poll
    if (m_cpuCoreCount == 0) {
        QString countResult = monitorExec(
            "ls -d /sys/devices/system/cpu/cpu[0-9]* 2>/dev/null | wc -l", 5000);
        if (!countResult.isEmpty() && !countResult.contains("Error"))
            m_cpuCoreCount = countResult.trimmed().toInt();
        if (m_cpuCoreCount <= 0 || m_cpuCoreCount > 64)
            m_cpuCoreCount = 1;

        if (m_cpuCoreCount > 1) {
            // Replace placeholder chart with per-core charts
            if (!m_cpuCharts.isEmpty()) {
                m_cpuGrid->removeWidget(m_cpuCharts[0]);
                m_cpuCharts[0]->deleteLater();
                m_cpuCharts.clear();
            }

            static const QColor kCpuColors[] = {
                QColor("#f38ba8"), QColor("#a6e3a1"), QColor("#fab387"),
                QColor("#89b4fa"), QColor("#cba6f7"), QColor("#94e2d5"),
                QColor("#f9e2af"), QColor("#74c7ec"), QColor("#eba0ac"),
                QColor("#b4befe"), QColor("#89dceb"), QColor("#a6adc8"),
            };
            const int colorCount = sizeof(kCpuColors) / sizeof(kCpuColors[0]);
            const int cols = 4;

            m_cpuFullHistory.clear();
            m_cpuFullHistory.resize(m_cpuCoreCount);
            m_gpuFullHistory.clear();
            m_tempFullHistory.clear();
            m_memFullHistory.clear();

            for (int i = 0; i < m_cpuCoreCount; i++) {
                auto *chart = new LiveChartWidget(m_perfPage);
                chart->setTitle(QString("CPU%1").arg(i));
                chart->setUnit("MHz");
                chart->setColor(kCpuColors[i % colorCount]);
                chart->setYRange(0, 3000);
                chart->setMaxPoints(120);
                chart->setMinimumHeight(120);
                m_cpuGrid->addWidget(chart, i / cols, i % cols);
                m_cpuCharts.append(chart);
            }
        }

        appendOutput(QString("检测到 %1 个 CPU 核心，已初始化监控").arg(m_cpuCoreCount), false);

        // Try to force GPU clock on (Qualcomm) — no-op if path doesn't exist
        monitorExec("echo 1 > /sys/class/kgsl/kgsl-3d0/force_clk_on 2>/dev/null; echo ok", 2000);

        // Detect GPU type and max freq for proper Y range
        QString gpuDetect = monitorExec(
            "echo 'name:'; "
            "cat /sys/class/kgsl/kgsl-3d0/gpu_model 2>/dev/null || "
            "cat /sys/class/kgsl/kgsl-3d0/gpu_available_frequencies 2>/dev/null | head -1 || "
            "cat /sys/class/misc/mali0/device/gpu_type 2>/dev/null || "
            "cat /sys/kernel/gpu/gpu_freq_table 2>/dev/null | head -1 || "
            "echo 'GPU'; "
            "echo '---max:'; "
            "cat /sys/class/kgsl/kgsl-3d0/devfreq/max_freq 2>/dev/null || "
            "cat /sys/class/kgsl/kgsl-3d0/max_gpuclk 2>/dev/null || "
            "cat /sys/class/misc/mali0/device/devfreq/max_freq 2>/dev/null || "
            "cat /sys/kernel/gpu/gpu_max_freq 2>/dev/null || "
            "cat /sys/class/devfreq/*gpu*/max_freq 2>/dev/null | head -1 || echo 0"
        , 5000);
        QStringList gpuParts = gpuDetect.split("---max:");
        QString gpuName = gpuParts.value(0).remove("name:").trimmed();
        double gpuMaxMHz = gpuParts.value(1).trimmed().toDouble() / 1000.0;
        if (!gpuName.isEmpty() && m_gpuChart && !gpuName.contains("Error")) {
            if (!gpuName.startsWith("GPU"))
                m_gpuChart->setTitle(gpuName);
            appendOutput(QString("检测到 GPU: %1").arg(gpuName), false);
        }
        if (gpuMaxMHz > 100 && m_gpuChart)
            m_gpuChart->setYRange(0, gpuMaxMHz);

        // Detect which GPU freq path works — cache the FULL path for subsequent polls
        m_gpuFreqPath = monitorExec(
            "r=$(cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null) && echo \"/sys/class/kgsl/kgsl-3d0/gpuclk:$r\" && exit 0; "
            "r=$(cat /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq 2>/dev/null) && echo \"/sys/class/kgsl/kgsl-3d0/devfreq/cur_freq:$r\" && exit 0; "
            "r=$(cat /sys/class/misc/mali0/device/devfreq/cur_freq 2>/dev/null) && echo \"/sys/class/misc/mali0/device/devfreq/cur_freq:$r\" && exit 0; "
            "r=$(cat /sys/devices/platform/13000000.mali/devfreq/cur_freq 2>/dev/null) && echo \"/sys/devices/platform/13000000.mali/devfreq/cur_freq:$r\" && exit 0; "
            "r=$(cat /sys/kernel/gpu/gpu_freq_table 2>/dev/null) && echo \"/sys/kernel/gpu/gpu_freq_table:$r\" && exit 0; "
            "r=$(cat /sys/devices/platform/*gpu*/devfreq/cur_freq 2>/dev/null) && echo \"/sys/devices/platform/*gpu*/devfreq/cur_freq:$r\" && exit 0; "
            "r=$(cat /sys/class/devfreq/*gpu*/cur_freq 2>/dev/null) && echo \"/sys/class/devfreq/*gpu*/cur_freq:$r\" && exit 0; "
            "r=$(cat /sys/class/devfreq/*/cur_freq 2>/dev/null | head -1) && echo \"/sys/class/devfreq/*/cur_freq:$r\" && exit 0; "
            "r=$(awk -F'[: ]' '/freq=/{print $2; exit}' /proc/gpufreq/gpufreq_opp_dump 2>/dev/null) && echo \"/proc/gpufreq/gpufreq_opp_dump:$r\" && exit 0; "
            "echo '/none:0'"
        , 5000);
        // m_gpuFreqPath is "fullpath:value" — keep just the path
        {
            int colon = m_gpuFreqPath.indexOf(':');
            if (colon > 0) {
                QString path = m_gpuFreqPath.left(colon);
                if (path != "/none") {
                    appendOutput(QString("GPU 路径已缓存: %1").arg(path), false);
                    m_gpuFreqPath = path;
                } else {
                    m_gpuFreqPath.clear();
                }
            } else {
                m_gpuFreqPath.clear();
            }
        }

        // Enumerate thermal zones and pick the best match
        QString zoneRaw = monitorExec(
            "for z in /sys/class/thermal/thermal_zone*; do "
            "idx=${z#/sys/class/thermal/thermal_zone}; "
            "echo \"$idx:$(cat $z/type 2>/dev/null):$(cat $z/temp 2>/dev/null)\"; done"
        , 5000);
        int bestScore = -1, bestZone = -1;
        QString bestName;
        for (const auto &line : zoneRaw.split('\n', Qt::SkipEmptyParts)) {
            QStringList parts = line.split(':');
            if (parts.size() < 3) continue;
            QString type = parts[1].toLower();
            int score = 0;
            if (type.contains("cpu") || type.contains("tsens") || type.contains("ap") || type.contains("soc"))
                score = 10;
            else if (type.contains("bat") || type.contains("bms") || type.contains("charger"))
                score = 8;
            else if (type.contains("board") || type.contains("skin") || type.contains("case") || type.contains("back") || type.contains("xo_therm"))
                score = 5;
            else if (type.contains("gpu") || type.contains("gpuss") || type.contains("kgsl"))
                score = 3;
            if (score > bestScore) { bestScore = score; bestZone = parts[0].toInt(); bestName = parts[1]; }
        }
        m_tempZoneIndex = bestZone;
        if (m_tempChart) {
            if (bestScore >= 0) {
                m_tempChart->setTitle(bestName);
                m_tempChart->setYRange(0, 100);
                m_tempChart->show();
                appendOutput(QString("温度传感器: %1 (zone%2)").arg(bestName).arg(bestZone), false);
            } else {
                m_tempChart->setTitle("温度 N/A");
                m_tempChart->setVisible(false);
                appendOutput("未检测到温度传感器，已隐藏温度图表", false);
            }
        }

        // Init memory chart as multi-series (RAM + Swap)
        if (m_memChart) {
            m_memChart->clearSeries();
            m_memChart->addSeries("内存", QColor("#89b4fa"));
            m_memChart->addSeries("Swap", QColor("#cba6f7"));
        }
        m_swapFullHistory.clear();
    }

    // Build per-core freq read commands
    QString cpuCmd;
    for (int i = 0; i < m_cpuCoreCount; i++) {
        cpuCmd += QString(
            "f_cur=$(cat /sys/devices/system/cpu/cpu%1/cpufreq/cpuinfo_cur_freq 2>/dev/null || "
            "cat /sys/devices/system/cpu/cpu%1/cpufreq/scaling_cur_freq 2>/dev/null || echo 0); "
            "f_max=$(cat /sys/devices/system/cpu/cpu%1/cpufreq/scaling_max_freq 2>/dev/null || echo 0); "
            "echo \"$f_cur $f_max\"; "
        ).arg(i);
    }

    // Batch collect all metrics in one shell call
    // Use cached GPU path if available, otherwise full fallback chain
    QString gpuSection;
    if (!m_gpuFreqPath.isEmpty()) {
        gpuSection = QString("gpu_freq=$(cat %1 2>/dev/null || echo 0); echo \"$gpu_freq\"; ")
            .arg(m_gpuFreqPath);
    } else {
        gpuSection =
            "gpu_freq=$(cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null || "
            "cat /sys/class/kgsl/kgsl-3d0/devfreq/cur_freq 2>/dev/null || "
            "cat /sys/class/misc/mali0/device/devfreq/cur_freq 2>/dev/null || "
            "cat /sys/devices/platform/13000000.mali/devfreq/cur_freq 2>/dev/null || "
            "cat /sys/kernel/gpu/gpu_freq_table 2>/dev/null | head -1 || "
            "cat /sys/devices/platform/*gpu*/devfreq/cur_freq 2>/dev/null | head -1 || "
            "cat /sys/class/devfreq/*gpu*/cur_freq 2>/dev/null | head -1 || "
            "cat /sys/class/devfreq/*/cur_freq 2>/dev/null | head -1 || "
            "awk -F'[: ]' '/freq=/{print $2; exit}' /proc/gpufreq/gpufreq_opp_dump 2>/dev/null || echo 0); "
            "echo \"$gpu_freq\"; ";
    }
    QString cmd = cpuCmd + "echo '---'; " + gpuSection
        + "echo '---'; "
        + (m_tempZoneIndex >= 0
            ? QString("cat /sys/class/thermal/thermal_zone%1/temp 2>/dev/null || ").arg(m_tempZoneIndex)
            : "")
        + "echo 0; "
        "echo '---'; "
        "awk '/MemTotal/{t=$2} /MemAvailable/{a=$2} /SwapTotal/{s=$2} /SwapFree/{f=$2} "
        "END{printf \"%.1f %.1f\", (1-a/t)*100, (s>0?(1-f/s)*100:0)}' "
        "/proc/meminfo 2>/dev/null || echo \"0 0\"; "
        "echo '---'; "
        "cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo ''";

    QString raw = monitorExec(cmd, 5000);
    if (raw.isEmpty() || raw.contains("Error")) { m_polling = false; return; }

    QStringList parts = raw.split("---");
    if (parts.size() < 5) { m_polling = false; return; }

    // CPU freqs — each line is "cur_khz max_khz"
    QString cpuSection = parts[0].trimmed();
    QStringList freqLines = cpuSection.split('\n', Qt::SkipEmptyParts);
    int count = qMin(freqLines.size(), m_cpuCoreCount);
    for (int i = 0; i < count; i++) {
        QStringList vals = freqLines[i].trimmed().split(' ', Qt::SkipEmptyParts);
        double curMHz = vals.value(0).toDouble() / 1000.0;
        double maxMHz = vals.value(1).toDouble() / 1000.0;
        if (i < m_cpuCharts.size()) {
            if (maxMHz > 0.0)
                m_cpuCharts[i]->setYRange(0.0, maxMHz);
            m_cpuCharts[i]->addValue(curMHz);
        }
        // Record full history
        if (i < m_cpuFullHistory.size())
            m_cpuFullHistory[i].append(curMHz);
    }

    // GPU, Temp, Mem (+Swap)
    double gpuFreq = parts[1].trimmed().toDouble() / 1000.0;
    double temp    = parts[2].trimmed().toDouble() / 1000.0;
    QStringList memParts = parts[3].trimmed().split(' ', Qt::SkipEmptyParts);
    double memPct  = memParts.value(0).toDouble();
    double swapPct = memParts.value(1).toDouble();
    QString gov    = parts[4].trimmed();

    m_gpuChart->addValue(gpuFreq);
    m_tempChart->addValue(temp);
    m_memChart->addDataPoint(0, memPct);
    m_memChart->addDataPoint(1, swapPct);
    m_gpuFullHistory.append(gpuFreq);
    m_tempFullHistory.append(temp);
    m_memFullHistory.append(memPct);
    m_swapFullHistory.append(swapPct);

    if (m_perfStatus && !gov.isEmpty())
        m_perfStatus->setText(QString("Governor: %1").arg(gov));

    m_polling = false;
}

void SystemToolPanel::onCpuGovernor()
{
    QString r = adbShell("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null; "
                         "echo '---'; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors 2>/dev/null");
    if (r.isEmpty() || r.contains("Error")) {
        appendOutput("CPU governor 信息获取失败", true); return; }
    auto parts = r.split("---");
    QString cur = parts.value(0).trimmed();
    QString avail = parts.value(1).trimmed();
    if (m_perfStatus) m_perfStatus->setText(QString("当前: %1  可用: %2").arg(cur, avail));

    bool ok = false;
    QString gov = QInputDialog::getItem(this, "CPU Governor", "选择调度策略:",
        avail.split(' ', Qt::SkipEmptyParts), 0, false, &ok);
    if (!ok) return;
    adbShell(QString("su -c 'echo %1 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor; "
                      "for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; "
                      "do echo %1 > $c 2>/dev/null; done; echo DONE'").arg(gov));
    appendOutput(QString("CPU governor 已设为 %1").arg(gov), false);
}

void SystemToolPanel::onGpuGovernor()
{
    QString r = adbShell("cat /sys/class/kgsl/kgsl-3d0/devfreq/available_governors 2>/dev/null || "
                         "cat /sys/class/kgsl/kgsl-3d0/governor 2>/dev/null || echo 'N/A'");
    if (r.contains("N/A")) {
        r = adbShell("cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null && echo '---' && "
                     "cat /proc/gpufreq/gpufreq_opp_dump 2>/dev/null || echo 'GPU 路径不可用'");
    }
    if (m_perfStatus) m_perfStatus->setText("GPU: " + r);
    appendOutput("GPU 信息: " + r, r.contains("Error"));
}

void SystemToolPanel::onThermalControl()
{
    QString r = adbShell("su -c 'ls /system/etc/thermal-engine.conf /vendor/etc/thermal-engine.conf "
                         "/system/etc/thermal-engine-* 2>/dev/null || echo NOT_FOUND'");
    if (r.contains("NOT_FOUND")) {
        appendOutput("未找到温控配置文件", true);
        if (m_perfStatus) m_perfStatus->setText("未找到温控配置");
        return;
    }
    if (m_perfStatus) m_perfStatus->setText("温控文件:\n" + r);

    QMessageBox::StandardButton ret = QMessageBox::warning(this, "温控配置",
        QString("找到温控配置:\n%1\n\n"
                "解除降频限制方法:\n"
                "  1. 备份原文件后删除 thermal-engine.conf\n"
                "  2. 或将文件内容中的 temperature/threshold 改高\n\n"
                "⚠️ 解除温控可能导致设备过热损坏！\n\n"
                "是否备份当前温控配置到 /sdcard/？").arg(r),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret == QMessageBox::Yes) {
        QString bak = adbShell("su -c 'cp /system/etc/thermal-engine.conf /sdcard/thermal.conf.bak 2>/dev/null; "
                               "cp /vendor/etc/thermal-engine.conf /sdcard/thermal.conf.bak 2>/dev/null; echo OK'");
        appendOutput("温控备份: " + bak, bak.contains("Error"));
    }
}

void SystemToolPanel::onMemoryOptimize()
{
    QString r = adbShell("su -c 'echo -n \"swappiness: \"; cat /proc/sys/vm/swappiness; "
                         "echo -n \"vfs_cache: \"; cat /proc/sys/vm/vfs_cache_pressure; "
                         "echo -n \"dirty_ratio: \"; cat /proc/sys/vm/dirty_ratio'");
    if (m_perfStatus) m_perfStatus->setText(r);

    QStringList opts = {"普通 (swappiness=60)", "性能 (swappiness=100)", "省电 (swappiness=10)", "自定义"};
    bool ok = false;
    QString sel = QInputDialog::getItem(this, "内存优化", "选择策略:", opts, 0, false, &ok);
    if (!ok) return;
    QString val;
    if (sel.startsWith("普通")) val = "60";
    else if (sel.startsWith("性能")) val = "100";
    else if (sel.startsWith("省电")) val = "10";
    else {
        val = QInputDialog::getText(this, "swappiness", "值 (0-100):", QLineEdit::Normal, "60", &ok);
        if (!ok) return;
    }
    adbShell(QString("su -c 'echo %1 > /proc/sys/vm/swappiness; echo DONE'").arg(val));
    appendOutput(QString("swappiness 已设为 %1").arg(val), false);
}

void SystemToolPanel::onIOScheduler()
{
    QString r = adbShell("su -c 'cat /sys/block/mmcblk0/queue/scheduler 2>/dev/null || "
                         "cat /sys/block/sda/queue/scheduler 2>/dev/null || echo N/A'");
    if (m_perfStatus) m_perfStatus->setText("I/O 调度: " + r);
    if (r.contains("N/A")) { appendOutput("I/O 调度器信息不可用", true); return; }

    // Extract available schedulers from brackets
    QString cur, avail;
    QRegularExpression re(R"(\[([^\]]+)\])");
    auto m = re.match(r);
    if (m.hasMatch()) cur = m.captured(1);
    avail = r.remove('[').remove(']').trimmed();

    bool ok = false;
    QString sched = QInputDialog::getItem(this, "I/O 调度", "选择算法:",
        avail.split(' ', Qt::SkipEmptyParts), 0, false, &ok);
    if (!ok) return;
    adbShell(QString("su -c 'echo %1 > /sys/block/mmcblk0/queue/scheduler 2>/dev/null; "
                     "echo %1 > /sys/block/sda/queue/scheduler 2>/dev/null; echo DONE'").arg(sched));
    appendOutput(QString("I/O 调度器已设为 %1").arg(sched), false);
}

// =====================================================================
//  2. 界面定制
// =====================================================================

void SystemToolPanel::onSetDpi()
{
    QString cur = adbShell("wm density 2>/dev/null || getprop ro.sf.lcd_density");
    m_dpiStatus->setText("当前 DPI: " + cur);

    bool ok = false;
    QString dpi = QInputDialog::getText(this, "调整 DPI", "输入显示密度 (默认 420):",
        QLineEdit::Normal, cur.section(' ', -1), &ok);
    if (!ok || dpi.isEmpty()) return;

    adbShell(QString("wm density %1 && echo OK").arg(dpi));
    m_dpiStatus->setText("DPI: " + dpi);
    appendOutput(QString("DPI 已设为 %1 (重启后可能恢复)").arg(dpi), false);
}

void SystemToolPanel::onAnimationScale()
{
    QString cur = adbShell("settings get global window_animation_scale 2>/dev/null || echo 1.0");
    m_animStatus->setText("当前动画: " + cur + "x");

    QStringList scales = {"0.0x (关闭)", "0.5x", "1.0x (默认)", "1.5x", "2.0x", "5.0x", "10.0x"};
    bool ok = false;
    QString sel = QInputDialog::getItem(this, "动画速度", "选择速度:", scales, 2, false, &ok);
    if (!ok) return;
    double val = sel.left(sel.indexOf('x')).toDouble();
    QString sv = QString::number(val);
    adbShell(QString("settings put global window_animation_scale %1; "
                     "settings put global transition_animation_scale %1; "
                     "settings put global animator_duration_scale %1; echo OK").arg(sv));
    m_animStatus->setText("动画: " + sv + "x");
    appendOutput(QString("动画速度已设为 %1x").arg(sv), false);
}

void SystemToolPanel::onStatusBar()
{
    QStringList opts = {"显示电量百分比", "显示秒数", "隐藏状态栏", "恢复默认"};
    bool ok = false;
    QString sel = QInputDialog::getItem(this, "状态栏定制", "选项:", opts, 0, false, &ok);
    if (!ok) return;

    if (sel == "显示电量百分比")
        adbShell("settings put system show_battery_percent 1; echo OK");
    else if (sel == "显示秒数")
        adbShell("settings put system statusbar_clock_show_seconds 1; echo OK");
    else if (sel == "隐藏状态栏")
        adbShell("settings put global policy_control immersive.status=*; echo OK");
    else
        adbShell("settings put global policy_control null; echo OK");
    appendOutput("状态栏: " + sel, false);
}

void SystemToolPanel::onNavBar()
{
    QStringList opts = {
        "默认 (3键)", "手势导航", "小导航栏 (dp)", "隐藏导航栏"
    };
    bool ok = false;
    QString sel = QInputDialog::getItem(this, "导航栏定制", "选项:", opts, 0, false, &ok);
    if (!ok) return;

    if (sel.contains("手势"))
        adbShell("settings put secure navigation_mode 2; echo OK");
    else if (sel.contains("小"))
        adbShell("settings put secure navigation_bar_height 36; echo OK");
    else if (sel.contains("隐藏"))
        adbShell("settings put global policy_control immersive.navigation=*; echo OK");
    else
        adbShell("settings put secure navigation_mode 0; settings put secure navigation_bar_height 0; echo OK");
    appendOutput("导航栏: " + sel + " (重启生效)", false);
}

void SystemToolPanel::onBootAnimation()
{
    QMessageBox::information(this, "更换开机动画",
        "操作步骤:\n\n"
        "1. 准备 bootanimation.zip 文件\n"
        "2. 将文件推送到 /sdcard/\n"
        "3. 执行以下命令:\n\n"
        "   adb root\n"
        "   adb shell mount -o rw,remount /system\n"
        "   adb push bootanimation.zip /system/media/\n"
        "   adb shell chmod 644 /system/media/bootanimation.zip\n"
        "   adb reboot\n\n"
        "支持 720p/1080p/1440p 分辨率。\n"
        "可用 '一键推送' 功能选择本地文件。");

    bool ok = false;
    QString path = QInputDialog::getText(this, "选择本地文件",
        "输入 bootanimation.zip 本地路径:", QLineEdit::Normal, "", &ok);
    if (!ok || path.isEmpty()) return;

    QString r = executeAdb({"-s", m_deviceInfo.serialNumber, "push", path, "/sdcard/bootanimation.zip"});
    if (r.contains("error")) { appendOutput("推送失败: " + r, true); return; }
    r = adbShell("su -c 'mount -o rw,remount /system 2>/dev/null; "
                 "cp /sdcard/bootanimation.zip /system/media/bootanimation.zip 2>/dev/null; "
                 "chmod 644 /system/media/bootanimation.zip; sync; echo DONE'");
    appendOutput("开机动画: " + r, r.contains("Error"));
}

void SystemToolPanel::onSetFont()
{
    QMessageBox::information(this, "更换字体",
        "全局字体文件位于 /system/fonts/ 目录。\n\n"
        "推荐方法:\n"
        "1. 使用 Magisk 字体模块 (不需修改 system 分区)\n"
        "2. 或将自定义字体改名后在 TWRP 中替换\n\n"
        "需替换的关键文件:\n"
        "  • Roboto-Regular.ttf\n"
        "  • Roboto-Bold.ttf\n"
        "  • NotoSans*.ttf\n\n"
        "此操作需要 /system 可读写及 Root 权限。");
}

// =====================================================================
//  4. 分区管理
// =====================================================================

void SystemToolPanel::onListBlockDevices()
{
    appendOutput("正在读取分区表...", false);
    QString r = adbShell("su -c 'ls -la /dev/block/by-name/ 2>/dev/null | head -80'");
    if (r.isEmpty() || r.contains("Error") || r.contains("No such"))
        r = adbShell("ls -la /dev/block/by-name/ 2>/dev/null || echo 'N/A'");
    m_blockStatus->setText(r.length() > 200 ? r.left(200) + "..." : r);
    appendOutput("分区表:\n" + r, false);
}

void SystemToolPanel::onSuperInfo()
{
    appendOutput("读取 super 分区信息...", false);
    QString r = adbShell("su -c 'lpdump --slot 2>/dev/null || lpdump 2>/dev/null || "
                         "sgdisk -p /dev/block/by-name/super 2>/dev/null || echo N/A'");
    if (r.contains("N/A")) {
        r = adbShell("su -c 'cat /proc/partitions | head -30; echo ---; df -h /dev/block/*'");
    }
    appendOutput("Super 分区信息:\n" + r, false);
}

void SystemToolPanel::onMountSystem()
{
    QString r = adbShell("su -c 'mount -o rw,remount /system 2>/dev/null && echo OK || "
                         "mount -o rw,remount / 2>/dev/null && echo OK || echo FAIL'");
    appendOutput("挂载 /system: " + r, !r.contains("OK"));
}

void SystemToolPanel::onUnmountSystem()
{
    bool ok = false;
    QString path = QInputDialog::getText(this, "卸载分区", "输入分区挂载点:",
        QLineEdit::Normal, "/system", &ok);
    if (!ok || path.isEmpty()) return;
    QString r = adbShell(QString("su -c 'umount %1 2>/dev/null && echo OK || echo FAIL'").arg(path));
    appendOutput(QString("卸载 %1: %2").arg(path, r), !r.contains("OK"));
}

// =====================================================================
//  5. 应用管理
// =====================================================================

void SystemToolPanel::onAppRefreshList()
{
    if (!m_appList) return;
    m_appList->clear();
    appendOutput("正在获取应用列表...", false);

    // Step 1: Get package list (format: package:/path/apk=com.example.name)
    QString cmd;
    switch (m_appFilter) {
    case 1: cmd = "pm list packages -3 -f 2>/dev/null"; break;
    case 2: cmd = "pm list packages -s -f 2>/dev/null"; break;
    default: cmd = "pm list packages -f 2>/dev/null"; break;
    }
    QString r = adbShell(cmd);
    if (r.isEmpty() || r.contains("Error")) {
        appendOutput("获取应用列表失败", true);
        m_appStatus->setText("获取失败");
        return;
    }

    // Step 2: Populate list with package names
    QStringList lines = r.split('\n', Qt::SkipEmptyParts);
    int count = 0;
    for (const auto &line : lines) {
        if (!line.startsWith("package:")) continue;
        QString pkg = line.section('=', -1).trimmed();
        if (pkg.isEmpty()) continue;
        QString apkPath = line.section(':', 1).section('=', 0, 0).trimmed();

        auto *item = new QListWidgetItem(pkg, m_appList);
        item->setData(Qt::UserRole, pkg);
        item->setData(Qt::UserRole + 1, apkPath);
        item->setData(Qt::UserRole + 2, pkg);
        item->setToolTip(pkg);
        count++;
    }

    // Step 4: Re-apply search filter
    if (m_searchBox && !m_searchBox->text().isEmpty()) {
        QString text = m_searchBox->text();
        for (int i = 0; i < m_appList->count(); i++) {
            auto *item = m_appList->item(i);
            bool match = item->data(Qt::UserRole).toString().contains(text, Qt::CaseInsensitive) ||
                         item->data(Qt::UserRole + 2).toString().contains(text, Qt::CaseInsensitive);
            item->setHidden(!match);
        }
    }

    m_appStatus->setText(QString("共 %1 个应用").arg(count));
    appendOutput(QString("应用列表: %1 个").arg(count), false);
}

void SystemToolPanel::onAppLaunch()
{
    if (!m_appList || !m_appList->currentItem()) return;
    QString pkg = m_appList->currentItem()->data(Qt::UserRole).toString();
    appendOutput(QString("启动: %1").arg(pkg), false);
    // Use monkey to launch the main activity
    QString r = adbShell(QString("monkey -p %1 -c android.intent.category.LAUNCHER 1 2>/dev/null; echo OK").arg(pkg));
    appendOutput(QString("启动 %1: %2").arg(pkg, r), r.contains("Error"));
}

void SystemToolPanel::onAppFreezeToggle()
{
    if (!m_appList || !m_appList->currentItem()) return;
    QString pkg = m_appList->currentItem()->data(Qt::UserRole).toString();

    // Check current state
    QString state = adbShell(QString("pm list packages -e 2>/dev/null | grep -c '%1' || echo 0").arg(pkg));
    bool isEnabled = state.contains('1') || state.contains(pkg);

    QString action = isEnabled ? "冻结" : "解冻";
    QMessageBox::StandardButton confirm = QMessageBox::question(this,
        QString("%1应用").arg(action),
        QString("即将 %1: %2\n\n是否继续？").arg(action, pkg),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

    if (confirm != QMessageBox::Yes) return;

    QString r;
    if (isEnabled)
        r = adbShell(QString("pm hide %1 2>/dev/null || pm disable %1 2>/dev/null; echo OK").arg(pkg));
    else
        r = adbShell(QString("pm unhide %1 2>/dev/null || pm enable %1 2>/dev/null; echo OK").arg(pkg));

    appendOutput(QString("%1 %2: %3").arg(action, pkg, r), !r.contains("OK"));
    if (r.contains("OK")) onAppRefreshList();
}

void SystemToolPanel::onAppUninstall()
{
    if (!m_appList || !m_appList->currentItem()) return;
    QString pkg = m_appList->currentItem()->data(Qt::UserRole).toString();

    QMessageBox::StandardButton confirm = QMessageBox::warning(this, "确认卸载",
        QString("即将卸载: %1\n\n对于系统应用,此操作不可逆！\n如需恢复须重新刷入固件。\n\n是否继续？").arg(pkg),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (confirm != QMessageBox::Yes) return;

    QString r = adbShell(QString("su -c 'pm uninstall -k --user 0 %1 2>/dev/null; echo DONE'").arg(pkg));
    appendOutput(QString("卸载 %1: %2").arg(pkg, r), r.contains("Error"));
    if (r.contains("DONE")) onAppRefreshList();
}

void SystemToolPanel::onAppExtract()
{
    if (!m_appList || !m_appList->currentItem()) return;
    QString pkg = m_appList->currentItem()->data(Qt::UserRole).toString();
    QString apkPath = m_appList->currentItem()->data(Qt::UserRole + 1).toString();

    if (apkPath.isEmpty()) {
        apkPath = adbShell(QString("pm path %1 2>/dev/null | cut -d: -f2").arg(pkg));
    }
    if (apkPath.isEmpty()) {
        appendOutput(QString("未找到 %1 的 APK 路径").arg(pkg), true);
        return;
    }

    QString localDir = "extracted_apks";
    QDir().mkpath(localDir);
    QStringList paths = apkPath.split('\n', Qt::SkipEmptyParts);
    for (const auto &p : paths) {
        QString name = p.section('/', -1);
        if (name.isEmpty()) name = pkg + ".apk";
        QString result = executeAdb({"-s", m_deviceInfo.serialNumber, "pull", p, localDir + "/" + name});
        appendOutput(QString("提取 %1: %2").arg(name, result), result.contains("Error"));
    }
    appendOutput(QString("APK 已保存到 %1/").arg(localDir), false);
}

// =====================================================================
//  6. 安全隐私
// =====================================================================

void SystemToolPanel::onEditBuildProp()
{
    QString r = adbShell("su -c 'cat /system/build.prop 2>/dev/null | head -40'");
    if (r.isEmpty() || r.contains("Error")) {
        r = adbShell("getprop ro.build.display.id");
        appendOutput("build.prop 读取失败，只读到了: " + r, true);
        return;
    }
    m_buildPropStatus->setText("build.prop 已读取 (前40行)");
    appendOutput("build.prop:\n" + r, false);

    QMessageBox::StandardButton ret = QMessageBox::question(this, "编辑 build.prop",
        "build.prop 已读取到日志。\n\n是否在设备上直接编辑？\n(需要 Root + vi 编辑器)",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret == QMessageBox::Yes) {
        adbShell("su -c 'mount -o rw,remount /system 2>/dev/null; vi /system/build.prop' &");
        appendOutput("已在设备上打开 vi 编辑器 (通过 ADB shell)", false);
    }
}

void SystemToolPanel::onSpoofDevice()
{
    QString cur = adbShell("getprop ro.product.model; echo ---; getprop ro.product.manufacturer; "
                           "echo ---; getprop ro.serialno");
    auto parts = cur.split("---");
    QString model = parts.value(0).trimmed();
    QString manuf = parts.value(1).trimmed();

    QMessageBox::information(this, "伪造设备信息",
        QString("当前:\n  型号: %1\n  制造商: %2\n\n"
                "可通过修改 build.prop 中的以下属性: \n"
                "  ro.product.model\n  ro.product.manufacturer\n"
                "  ro.product.device\n  ro.build.fingerprint\n\n"
                "修改后需重启生效。需要 Xposed 模块 (AndroidFaker) 实现动态修改。")
            .arg(model, manuf));
}

void SystemToolPanel::onSelinuxMode()
{
    QString cur = adbShell("getenforce 2>/dev/null");
    m_selinuxLabel->setText("SELinux: " + cur);

    QStringList opts = {"切换为 Permissive (宽松)", "切换为 Enforcing (强制)", "查看状态"};
    bool ok = false;
    QString sel = QInputDialog::getItem(this, "SELinux 模式", "选项:", opts, 0, false, &ok);
    if (!ok) return;

    if (sel.contains("Permissive")) {
        QString r = adbShell("su -c 'setenforce 0; echo DONE'");
        m_selinuxLabel->setText("SELinux: Permissive");
        appendOutput("SELinux 已设为 Permissive", r.contains("Error"));
    } else if (sel.contains("Enforcing")) {
        QString r = adbShell("su -c 'setenforce 1; echo DONE'");
        m_selinuxLabel->setText("SELinux: Enforcing");
        appendOutput("SELinux 已设为 Enforcing", r.contains("Error"));
    } else {
        QString r = adbShell("getenforce; sestatus 2>/dev/null || true");
        appendOutput("SELinux 状态:\n" + r, false);
    }
}

void SystemToolPanel::onSafetynetBypass()
{
    QStringList opts = {
        "检查 CTS 状态",
        "隐藏 Root (Magisk Hide)",
        "修改 fingerprint",
        "重置为默认"
    };
    bool ok = false;
    QString sel = QInputDialog::getItem(this, "SafetyNet 绕过", "选项:", opts, 0, false, &ok);
    if (!ok) return;

    if (sel.contains("检查")) {
        QString r = adbShell("su -c 'gsf -v 2>/dev/null || "
                             "dumpsys deviceidle | grep -i cts || "
                             "echo \"使用 YASNAC 或 SafetyNet Playground 检测\"'");
        appendOutput("CTS 检查: " + r, false);
    } else if (sel.contains("隐藏")) {
        adbShell("su -c 'magiskhide --add com.google.android.gms 2>/dev/null; "
                 "magiskhide --add com.google.android.gms.unstable 2>/dev/null; echo DONE'");
        appendOutput("Magisk Hide 已添加 Google Play 服务", false);
    } else if (sel.contains("fingerprint")) {
        bool ok2 = false;
        QString fp = QInputDialog::getText(this, "修改 fingerprint",
            "输入新的 ro.build.fingerprint:", QLineEdit::Normal, "", &ok2);
        if (ok2 && !fp.isEmpty()) {
            adbShell(QString("su -c 'setprop ro.build.fingerprint \"%1\"; echo DONE'").arg(fp));
            appendOutput("fingerprint 已修改 (重启后恢复)", false);
        }
    }
}

void SystemToolPanel::onSignatureSpoof()
{
    appendOutput("检查签名伪造支持...", false);
    QString r = adbShell("su -c 'ls /system/etc/permissions/org.aware.* 2>/dev/null || "
                         "ls /data/data/org.lsposed.manager 2>/dev/null || echo NEED_XPOSED'");
    if (r.contains("NEED_XPOSED")) {
        appendOutput("签名伪造需要 LSPosed 框架及核心破解模块", false);
        QMessageBox::information(this, "签名伪造",
            "实现签名伪造的步骤:\n\n"
            "1. 安装 LSPosed 框架 (Magisk 模块)\n"
            "2. 安装核心破解 (Core Patch) 模块\n"
            "3. 在 LSPosed 中启用模块\n"
            "4. 重启生效\n\n"
            "完成后可安装签名不一致的 APK。");
    } else {
        appendOutput("已检测到签名伪造支持:", false);
        appendOutput(r, false);
    }
}

// =====================================================================
//  7. 开发调试
// =====================================================================

void SystemToolPanel::onLogcat()
{
    QStringList opts = {"抓取全部日志 (20行)", "抓取错误日志", "清空日志", "持续输出 (按设备时间)"};
    bool ok = false;
    QString sel = QInputDialog::getItem(this, "Logcat", "选项:", opts, 0, false, &ok);
    if (!ok) return;

    if (sel.contains("全部")) {
        QString r = adbShell("logcat -t 20 2>/dev/null | head -30");
        appendOutput("Logcat (最近20行):\n" + r, false);
    } else if (sel.contains("错误")) {
        QString r = adbShell("logcat -t 50 *:E 2>/dev/null | head -40");
        appendOutput("Logcat (错误):\n" + r, false);
    } else if (sel.contains("清空")) {
        adbShell("logcat -c 2>/dev/null");
        appendOutput("日志已清空", false);
    } else {
        // Continuous output - grab last 15 lines and show
        QString r = adbShell("logcat -t 15 2>/dev/null");
        appendOutput("Logcat (最新):\n" + r, false);
        appendOutput("持续抓取: 运行 'adb logcat' 在终端查看", false);
    }
}

void SystemToolPanel::onTakeBugreport()
{
    appendOutput("正在生成 Bugreport... (可能需要 30 秒以上)", false);
    QString r = adbShell("bugreportz 2>/dev/null | head -5 || bugreport 2>/dev/null | head -5 || echo FAILED");
    appendOutput("Bugreport: " + r, r.contains("FAILED"));
    if (!r.contains("FAILED")) {
        QString path = r.section(' ', -1).trimmed();
        if (!path.isEmpty() && path.startsWith('/')) {
            QString fn = path.section('/', -1);
            executeAdb({"-s", m_deviceInfo.serialNumber, "pull", path, fn});
            appendOutput(QString("Bugreport 已下载: %1").arg(fn), false);
        }
    }
}

void SystemToolPanel::onDumpsys()
{
    QStringList services = {"batterystats", "meminfo", "cpuinfo", "diskstats", "package", "power", "window"};
    bool ok = false;
    QString svc = QInputDialog::getItem(this, "Dumpsys", "选择服务:",
        services, 0, false, &ok);
    if (!ok) return;

    QString r = adbShell(QString("dumpsys %1 2>/dev/null | head -40").arg(svc));
    appendOutput(QString("dumpsys %1:\n%2").arg(svc, r), false);
}

void SystemToolPanel::onShizukuStatus()
{
    QString r = adbShell("dumpsys package | grep -i shizuku 2>/dev/null || "
                         "ps -A | grep -i shizuku 2>/dev/null || echo NOT_FOUND");
    m_shizukuStatus->setText(r.contains("NOT_FOUND") ? "Shizuku: 未运行" : "Shizuku: 检测到");
    appendOutput("Shizuku 状态: " + r, r.contains("NOT_FOUND"));
}

// =====================================================================
//  Save Chart — 渲染完整历史数据为 PNG
// =====================================================================

static void drawHistoryStrip(QPainter &p, const QRect &r,
                             const QVector<double> &data,
                             const QString &title, const QString &unit,
                             const QColor &color,
                             double minY, double maxY,
                             bool overlay = false)
{
    if (data.isEmpty()) return;

    const int mL = 80, mR = 120, mT = 4, mB = 4;
    QRect chart(r.x() + mL, r.y() + mT, r.width() - mL - mR, r.height() - mT - mB);
    double yRange = maxY - minY;
    if (yRange <= 0) yRange = 1;

    if (!overlay) {
        // Background
        p.save();
        p.setBrush(QColor("#181825"));
        p.setPen(QPen(QColor("#313244"), 1));
        p.drawRoundedRect(r.adjusted(1, 1, -1, -1), 4, 4);
        p.restore();

        // Title
        p.save();
        QFont tf = p.font(); tf.setPixelSize(11); tf.setBold(true);
        p.setFont(tf);
        p.setPen(QColor("#cdd6f4"));
        p.drawText(r.x() + 4, r.y() + 2, mL - 8, 20, Qt::AlignLeft | Qt::AlignBottom,
                   title + (unit.isEmpty() ? "" : " (" + unit + ")"));
        p.restore();

        // Y-axis labels
        p.save();
        QFont af = p.font(); af.setPixelSize(8);
        p.setFont(af);
        p.setPen(QColor("#6c7086"));
        for (int i = 0; i <= 4; i++) {
            int y = chart.top() + chart.height() * i / 4;
            double val = maxY - (maxY - minY) * i / 4;
            p.drawText(r.x() + 4, y - 5, mL - 12, 10, Qt::AlignRight | Qt::AlignVCenter,
                       QString::number(val, 'f', 1));
        }
        p.restore();

        // Grid
        p.save();
        p.setPen(QPen(QColor("#313244"), 1));
        for (int i = 0; i <= 4; i++) {
            int y = chart.top() + chart.height() * i / 4;
            p.drawLine(chart.left(), y, chart.right(), y);
        }
        p.restore();
    }

    // Line
    p.save();
    p.setPen(QPen(color, overlay ? 1.0 : 1.5));
    int n = data.size();
    double stepX = n > 1 ? static_cast<double>(chart.width()) / (n - 1) : chart.width();
    QPainterPath path;
    path.moveTo(chart.left(), chart.bottom() - (data[0] - minY) / yRange * chart.height());
    for (int i = 1; i < n; i++) {
        double x = chart.left() + i * stepX;
        double y = qBound(static_cast<double>(chart.top()),
                          chart.bottom() - (data[i] - minY) / yRange * chart.height(),
                          static_cast<double>(chart.bottom()));
        path.lineTo(x, y);
    }
    p.drawPath(path);
    p.restore();

    if (!overlay) {
        // Stats
        p.save();
        QFont sf = p.font(); sf.setPixelSize(10);
        p.setFont(sf);
        p.setPen(QColor("#a6adc8"));
        double sum = 0, mn = data[0], mx = data[0];
        for (double v : data) { sum += v; if (v < mn) mn = v; if (v > mx) mx = v; }
        double avg = sum / n;
        int sx = r.right() - mR + 10, sy = r.top() + 10;
        p.drawText(sx, sy, mR - 14, 16, Qt::AlignLeft, QString("最低: %1").arg(mn, 0, 'f', 1));
        p.drawText(sx, sy + 18, mR - 14, 16, Qt::AlignLeft, QString("平均: %1").arg(avg, 0, 'f', 1));
        p.drawText(sx, sy + 36, mR - 14, 16, Qt::AlignLeft, QString("最高: %1").arg(mx, 0, 'f', 1));
        p.restore();
    }
}

void SystemToolPanel::onSaveChart()
{
    // Check if any data exists
    bool hasData = false;
    for (const auto &h : m_cpuFullHistory) {
        if (!h.isEmpty()) { hasData = true; break; }
    }
    if (!hasData && m_gpuFullHistory.isEmpty() && m_tempFullHistory.isEmpty() && m_memFullHistory.isEmpty()) {
        appendOutput("没有监控数据可保存，请先开始监控", true);
        return;
    }

    QString defaultName = QString("perf_%1_%2.png")
        .arg(m_deviceInfo.serialNumber.isEmpty() ? "device" : m_deviceInfo.serialNumber)
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));

    QString filePath = QFileDialog::getSaveFileName(this, "保存监控截图",
        defaultName, "PNG 图片 (*.png)");
    if (filePath.isEmpty()) return;

    const int stripHeight = 120;
    const int headerHeight = 80;
    const int margin = 20;
    const int chartWidth = 1200;
    int totalStrips = m_cpuCharts.size();
    if (m_gpuChart) totalStrips++;
    if (m_tempChart) totalStrips++;
    if (m_memChart) totalStrips++;
    int totalHeight = headerHeight + totalStrips * stripHeight + (totalStrips + 2) * margin;

    QImage image(chartWidth + 2 * margin, totalHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor("#1e1e2e"));

    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);

    // Header
    QFont hf = p.font(); hf.setPixelSize(14); hf.setBold(true);
    p.setFont(hf);
    p.setPen(QColor("#cdd6f4"));

    int totalPoints = m_cpuFullHistory.isEmpty() ? 0 : m_cpuFullHistory[0].size();
    // Fallback to GPU history size
    if (totalPoints == 0) totalPoints = m_gpuFullHistory.size();

    QString hdrLine1 = QString("设备: %1  |  Root: %2  |  记录时间: %3")
        .arg(m_deviceInfo.serialNumber.isEmpty() ? "未知" : m_deviceInfo.serialNumber)
        .arg(m_deviceInfo.isRooted ? "是" : "否")
        .arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"));
    p.drawText(margin, margin, chartWidth, 30, Qt::AlignLeft | Qt::AlignVCenter, hdrLine1);

    QFont sf2 = p.font(); sf2.setPixelSize(12); sf2.setBold(false);
    p.setFont(sf2);
    p.setPen(QColor("#6c7086"));
    double dur = totalPoints * 0.5;
    QString hdrLine2 = QString("采样间隔: 0.5s  |  总采样: %1 点  |  时长: %2秒 (%3分%4秒)")
        .arg(totalPoints)
        .arg(static_cast<int>(dur))
        .arg(static_cast<int>(dur / 60))
        .arg(static_cast<int>(dur) % 60);
    p.drawText(margin, margin + 25, chartWidth, 30, Qt::AlignLeft | Qt::AlignVCenter, hdrLine2);

    // Strips
    int yOff = headerHeight + margin;

    for (int i = 0; i < m_cpuCharts.size(); i++) {
        if (i < m_cpuFullHistory.size()) {
            drawHistoryStrip(p, QRect(margin, yOff, chartWidth, stripHeight - 6),
                           m_cpuFullHistory[i],
                           m_cpuCharts[i]->chartTitle(),
                           m_cpuCharts[i]->chartUnit(),
                           m_cpuCharts[i]->chartColor(),
                           m_cpuCharts[i]->minYValue(),
                           m_cpuCharts[i]->maxYValue());
        }
        yOff += stripHeight;
    }

    if (m_gpuChart) {
        drawHistoryStrip(p, QRect(margin, yOff, chartWidth, stripHeight - 6),
                       m_gpuFullHistory, m_gpuChart->chartTitle(),
                       m_gpuChart->chartUnit(), m_gpuChart->chartColor(),
                       m_gpuChart->minYValue(), m_gpuChart->maxYValue());
        yOff += stripHeight;
    }
    if (m_tempChart) {
        drawHistoryStrip(p, QRect(margin, yOff, chartWidth, stripHeight - 6),
                       m_tempFullHistory, m_tempChart->chartTitle(),
                       m_tempChart->chartUnit(), m_tempChart->chartColor(),
                       m_tempChart->minYValue(), m_tempChart->maxYValue());
        yOff += stripHeight;
    }
    if (m_memChart) {
        drawHistoryStrip(p, QRect(margin, yOff, chartWidth, stripHeight - 6),
                       m_memFullHistory, "内存 (RAM)",
                       m_memChart->chartUnit(), QColor("#89b4fa"),
                       m_memChart->minYValue(), m_memChart->maxYValue());
        // Overlay swap line
        if (!m_swapFullHistory.isEmpty()) {
            drawHistoryStrip(p, QRect(margin, yOff, chartWidth, stripHeight - 6),
                           m_swapFullHistory, "",
                           "", QColor("#cba6f7"),
                           m_memChart->minYValue(), m_memChart->maxYValue(),
                           true);
        }
        yOff += stripHeight;
    }

    p.end();

    if (image.save(filePath, "PNG")) {
        appendOutput(QString("监控截图已保存: %1 (%2x%3)")
            .arg(filePath).arg(image.width()).arg(image.height()), false);
    } else {
        appendOutput("保存截图失败: " + filePath, true);
    }
}
