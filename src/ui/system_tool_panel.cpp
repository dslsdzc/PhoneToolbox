#include "system_tool_panel.h"
#include "core/adb_embedded.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QMessageBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QCheckBox>
#include <QPainter>
#include <QMap>
#include <QScrollArea>
#include <QFrame>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>

static const char *kCategories[] = {
    "性能调优", "界面定制", "功能增强", "分区管理",
    "应用管理", "安全隐私", "开发调试"
};
static const int kCategoryCount = 7;

SystemToolPanel::SystemToolPanel(QWidget *parent)
    : QWidget(parent)
    , m_asyncProc(nullptr)
{
    setupUI();
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

QIcon SystemToolPanel::generateAppIcon(QListWidgetItem *item)
{
    if (!item) return QIcon();
    QString name = item->data(Qt::UserRole + 2).toString();
    if (name.isEmpty()) name = item->data(Qt::UserRole).toString();

    static const QColor colors[] = {
        QColor("#e57373"), QColor("#f06292"), QColor("#ba68c8"),
        QColor("#9575cd"), QColor("#7986cb"), QColor("#64b5f6"),
        QColor("#4fc3f7"), QColor("#4dd0e1"), QColor("#4db6ac"),
        QColor("#81c784"), QColor("#aed581"), QColor("#ffd54f"),
        QColor("#ffb74d"), QColor("#ff8a65"), QColor("#a1887f"),
    };
    static const int colorCount = sizeof(colors) / sizeof(colors[0]);

    const int sz = 36;
    QPixmap pix(sz, sz);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(colors[qHash(name) % colorCount]);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(0, 0, sz, sz, 6, 6);
    p.setPen(Qt::white);
    QFont f = p.font();
    f.setPixelSize(20);
    f.setBold(true);
    p.setFont(f);
    p.drawText(QRect(0, 0, sz, sz), Qt::AlignCenter, name.left(1).toUpper());
    p.end();
    return QIcon(pix);
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
            if (b != m_backBtn) b->setEnabled(enabled);
        }
        auto lists = page->findChildren<QListWidget*>();
        for (auto *l : lists) l->setEnabled(enabled);
    }
    m_backBtn->setEnabled(true);

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
        addSection("CPU 调频");
        addBtn("CPU Governor", "查看/设置 CPU 调度策略 (performance/schedutil/interactive)")->setObjectName("cpuGov");
        addBtn("CPU 频率上限", "调低/调高最高频率以省电/提性能")->setObjectName("cpuFreqMax");
        m_cpuStatus = addStatus();

        addSection("GPU 调频");
        addBtn("GPU Governor", "查看/设置 GPU 调度策略")->setObjectName("gpuGov");
        m_gpuStatus = addStatus();

        addSection("温控策略");
        addBtn("温控配置", "查看/解除温控降频限制 (thermal-engine.conf)")->setObjectName("thermal");
        m_thermalStatus = addStatus();

        addSection("内存与 I/O");
        addBtn("内存优化", "调整 swappiness 及 LMK 杀进程阈值")->setObjectName("memory");
        addBtn("I/O 调度", "查看/更改存储设备 I/O 调度算法")->setObjectName("ioSched");
        m_memoryStatus = addStatus();
        m_ioStatus = addStatus();

        lay->addStretch();

        connect(page->findChild<QPushButton*>("cpuGov"), &QPushButton::clicked, this, &SystemToolPanel::onCpuGovernor);
        connect(page->findChild<QPushButton*>("gpuGov"), &QPushButton::clicked, this, &SystemToolPanel::onGpuGovernor);
        connect(page->findChild<QPushButton*>("thermal"), &QPushButton::clicked, this, &SystemToolPanel::onThermalControl);
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
        m_showIconsCheck = new QCheckBox("显示图标", page);
        m_showIconsCheck->setEnabled(false);
        lay->addWidget(m_showIconsCheck);

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

        // Icon toggle
        connect(m_showIconsCheck, &QCheckBox::toggled, this, [this](bool show) {
            for (int i = 0; i < m_appList->count(); i++) {
                auto *item = m_appList->item(i);
                item->setIcon(show ? generateAppIcon(item) : QIcon());
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
//  1. 性能调优
// =====================================================================

void SystemToolPanel::onCpuGovernor()
{
    QString r = adbShell("cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null; "
                         "echo '---'; cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors 2>/dev/null");
    if (r.isEmpty() || r.contains("Error")) {
        appendOutput("CPU governor 信息获取失败", true); return; }
    auto parts = r.split("---");
    QString cur = parts.value(0).trimmed();
    QString avail = parts.value(1).trimmed();
    m_cpuStatus->setText(QString("当前: %1  可用: %2").arg(cur, avail));

    bool ok = false;
    QString gov = QInputDialog::getItem(this, "CPU Governor", "选择调度策略:",
        avail.split(' ', Qt::SkipEmptyParts), 0, false, &ok);
    if (!ok) return;
    adbShell(QString("su -c 'echo %1 > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor; "
                      "for c in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; "
                      "do echo %1 > $c 2>/dev/null; done; echo DONE'").arg(gov));
    m_cpuStatus->setText(QString("CPU: %1").arg(gov));
    appendOutput(QString("CPU governor 已设为 %1").arg(gov), false);
}

void SystemToolPanel::onGpuGovernor()
{
    QString r = adbShell("cat /sys/class/kgsl/kgsl-3d0/devfreq/available_governors 2>/dev/null || "
                         "cat /sys/class/kgsl/kgsl-3d0/governor 2>/dev/null || echo 'N/A'");
    if (r.contains("N/A")) {
        // Try alternative paths
        r = adbShell("cat /sys/class/kgsl/kgsl-3d0/gpuclk 2>/dev/null && echo '---' && "
                     "cat /proc/gpufreq/gpufreq_opp_dump 2>/dev/null || echo 'GPU 路径不可用'");
    }
    m_gpuStatus->setText("GPU: " + r);
    appendOutput("GPU 信息: " + r, r.contains("Error"));
}

void SystemToolPanel::onThermalControl()
{
    QString r = adbShell("su -c 'ls /system/etc/thermal-engine.conf /vendor/etc/thermal-engine.conf "
                         "/system/etc/thermal-engine-* 2>/dev/null || echo NOT_FOUND'");
    if (r.contains("NOT_FOUND")) {
        appendOutput("未找到温控配置文件", true);
        m_thermalStatus->setText("未找到温控配置");
        return;
    }
    m_thermalStatus->setText("温控文件:\n" + r);

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
    m_memoryStatus->setText(r);

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
    m_memoryStatus->setText(QString("swappiness=%1").arg(val));
    appendOutput(QString("swappiness 已设为 %1").arg(val), false);
}

void SystemToolPanel::onIOScheduler()
{
    QString r = adbShell("su -c 'cat /sys/block/mmcblk0/queue/scheduler 2>/dev/null || "
                         "cat /sys/block/sda/queue/scheduler 2>/dev/null || echo N/A'");
    m_ioStatus->setText("I/O 调度: " + r);
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
    m_ioStatus->setText("I/O: " + sched);
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

    // Step 1: Get package list
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

    // Step 2: Fetch display name mapping from dumpsys package
    QMap<QString, QString> labelMap;
    QString labelOutput = adbShell(
        "dumpsys package 2>/dev/null | grep -E 'Package \\[|application-label:'");
    if (!labelOutput.isEmpty() && !labelOutput.contains("Error")) {
        QString currentPkg;
        const auto &labelLines = labelOutput.split('\n', Qt::SkipEmptyParts);
        for (const auto &line : labelLines) {
            if (line.contains("Package [")) {
                int start = line.indexOf('[');
                int end = line.indexOf(']', start);
                if (start >= 0 && end > start)
                    currentPkg = line.mid(start + 1, end - start - 1);
            } else if (line.contains("application-label:") && !currentPkg.isEmpty()) {
                int start = line.indexOf('\'');
                if (start >= 0) {
                    int end = line.indexOf('\'', start + 1);
                    if (end > start)
                        labelMap[currentPkg] = line.mid(start + 1, end - start - 1);
                }
            }
        }
    }

    // Step 3: Populate list with display names
    QStringList lines = r.split('\n', Qt::SkipEmptyParts);
    int count = 0;
    for (const auto &line : lines) {
        if (!line.startsWith("package:")) continue;
        QString pkg = line.section('=', -1).trimmed();
        if (pkg.isEmpty()) continue;
        QString apkPath = line.section(':', 1).section('=', 0, 0).trimmed();

        QString displayName = labelMap.value(pkg, pkg);

        auto *item = new QListWidgetItem(displayName, m_appList);
        item->setData(Qt::UserRole, pkg);
        item->setData(Qt::UserRole + 1, apkPath);
        item->setData(Qt::UserRole + 2, displayName);
        item->setToolTip(QString("%1 (%2)").arg(displayName, pkg));
        count++;
    }

    // Step 4: Re-apply search filter and icon state
    if (m_searchBox && !m_searchBox->text().isEmpty()) {
        QString text = m_searchBox->text();
        for (int i = 0; i < m_appList->count(); i++) {
            auto *item = m_appList->item(i);
            bool match = item->data(Qt::UserRole).toString().contains(text, Qt::CaseInsensitive) ||
                         item->data(Qt::UserRole + 2).toString().contains(text, Qt::CaseInsensitive);
            item->setHidden(!match);
        }
    }
    if (m_showIconsCheck && m_showIconsCheck->isChecked()) {
        for (int i = 0; i < m_appList->count(); i++)
            m_appList->item(i)->setIcon(generateAppIcon(m_appList->item(i)));
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
