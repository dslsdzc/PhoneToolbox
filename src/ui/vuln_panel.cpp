#include "vuln_panel.h"
#include "vuln_db/vuln_matcher.h"
#include "risk_confirm_dialog.h"        // W2-T5：二次确认**唯一闸门**（漏洞扫描/利用属 Tier3）
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QScrollBar>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QDateTime>
#include <QSet>
#include <QElapsedTimer>
#include <QCheckBox>
#include <QLineEdit>
#include <memory>

// W8-T6：MTK BROM 漏洞利用区（把 T1–T5 交付的能力接成可达）
#include "core/modes/mtk_brom.h"                          // BromSession / enumerateUsb / openLibusbUsb
#include "core/modes/mtk_brom_exploit_download_qt.h"      // makeQtExploitDownloader（生产下载器）
#include "ui/brom_exploit_ui.h"                           // 执行前判据（纯函数，逐条被用例钉住）

namespace {

// 生产执行器：打开 BROM 会话 → **核对设备自报芯片码** → 修正载荷 → 跑 kamakiri 触发序列。
//
// ⚠️ 真机路径（本仓无 MTK 真机 ⇒ **无自动化覆盖**，只有"编译 + 注入点替换"级证据）：
//    本函数的三处 fail-closed 都是**有意**的，别按"上游也这样"放宽：
//      ① 多台候选设备 → 拒（`devs.first()` 打错设备的代价是砖机，F5 的多设备警告同族）；
//      ② 设备自报 hw_code 与面板输入不符 → 拒（芯片表按 hw_code 取 watchdog/地址，写错芯片 = 砖机路径）；
//      ③ 任一步失败 → 中止、不重试、不复位（与本仓其余真机段同口径）。
bool runBromExploitOnDevice(const VulnPanel::BromRunRequest &req, QStringList *log, QString *error)
{
    auto note = [log](const QString &s) { if (log) *log << s; };
    auto fail = [error](const QString &why) { if (error) *error = why; return false; };

    QList<mtkbrom::BromDevice> devs;
    QString enumErr;
    if (!mtkbrom::enumerateUsb(devs, &enumErr))
        return fail(QStringLiteral("枚举 USB 失败：%1").arg(enumErr));
    if (devs.isEmpty())
        return fail(QStringLiteral("未检测到 MTK 协议设备（BROM / preloader）—— "
                                   "请让设备进入 BROM 模式后再插 USB"));
    if (devs.size() > 1)
        return fail(QStringLiteral("检测到 %1 台 MTK 协议设备：本入口只支持单台"
                                   "（自动挑一台的代价是打错设备）—— 请只连一台")
                        .arg(devs.size()));

    // 「这次打到了哪台」的**唯一可信记录**（控制方 2026-09-22 裁决①）：面板里"选中的设备"只是
    // "有没有设备"的判据，真正被打开的是**现场枚举到的这一台** —— 芯片码核对挡不住"两台同芯片、
    // 选了 A 只插着 B"，所以这里必须把设备身份落痕。
    // 位置在**打开句柄之前**：打开失败时也要留下"本来要打哪台"（出事后能回溯）。
    // `portName` 与设备列表的 ID 同形（都是 usb-<bus>-<addr>），可直接对表；**不是**持久序列号。
    const mtkbrom::BromDevice &target = devs.first();
    note(QStringLiteral("目标设备：%1 —— 本入口只作用于**现场枚举到的这一台**"
                        "（不是设备列表里选中的那台；芯片码将在握手后与设备自报值核对）")
             .arg(bromui::describeBromTarget(target.vid, target.pid, target.portName)));

    std::unique_ptr<mtkbrom::IBromUsb> usb;
    QString openErr;
    if (!mtkbrom::openLibusbUsb(devs.first(), usb, &openErr))
        return fail(QStringLiteral("打开 USB 通道失败：%1").arg(openErr));

    mtkbrom::BromSession session(std::move(usb), devs.first());
    QString connErr;
    if (!session.connect(&connErr))
        return fail(QStringLiteral("BROM 握手失败：%1").arg(connErr));
    note(QStringLiteral("已连接 BROM 会话（握手完成）"));

    quint16 devHw = 0, devHwVer = 0;
    QString hwErr;
    if (!session.getHwCode(devHw, devHwVer, &hwErr))
        return fail(QStringLiteral("读设备芯片码失败：%1").arg(hwErr));
    if (devHw != req.hwCode)
        return fail(QStringLiteral("设备自报 hw_code 0x%1 与面板填的 0x%2 不一致 —— 拒绝执行"
                                   "（按错的芯片写 watchdog / 载荷地址是砖机路径）")
                        .arg(devHw, 4, 16, QLatin1Char('0'))
                        .arg(req.hwCode, 4, 16, QLatin1Char('0')));
    note(QStringLiteral("设备自报芯片码 0x%1（hw_ver 0x%2）与面板输入一致")
             .arg(devHw, 4, 16, QLatin1Char('0')).arg(devHwVer, 4, 16, QLatin1Char('0')));

    const mtkbrom::ChipHwAddrs *addrs = mtkbrom::lookupChipHwAddrs(req.hwCode);
    if (!addrs)
        return fail(QStringLiteral("芯片 0x%1 不在硬件地址表内").arg(req.hwCode, 4, 16, QLatin1Char('0')));

    // 载荷修正：`runBromExploit` **要求**入参是 fixExploitPayload 的产物（传原始占位符载荷会把
    // 0x10007000 / 0x11002000 当真实地址写下去）。daMode = false 是 kamakiri 路径的上游取值
    // （exploit_handler.py 的 fix_payload(..., da=False)）—— 与 T3 用例同一口径。
    QString fixErr;
    const QByteArray fixed = mtkbrom::fixExploitPayload(req.payload, *addrs, /*daMode=*/false, log, &fixErr);
    if (fixed.isEmpty())
        return fail(QStringLiteral("载荷修正失败：%1").arg(fixErr));

    mtkbrom::ExploitOptions opt;
    opt.allowPatchedGeneration = req.allowPatchedGeneration;
    if (req.allowPatchedGeneration)
        note(QStringLiteral("⚠️ 已开启「允许已修补代际（V6）」—— 本仓对已修补代际的判据是粗判据"
                            "（来自公开报告，非逐设备实测），不保证有意义"));
    mtkbrom::ExploitResult out;
    QString runErr;
    const bool ok = mtkbrom::runBromExploit(session, fixed, *addrs, out, &runErr, opt);
    for (const QString &line : out.log)
        note(line);
    if (!ok)
        return fail(QStringLiteral("触发序列在阶段 `%1` 失败：%2").arg(out.stage, runErr));
    return true;
}

} // namespace

VulnPanel::VulnPanel(QWidget *parent)
    : QWidget(parent)
    , m_db(new VulnDb(this))
    , m_engine(new ExploitEngine(this))
{
    // W8-T6：载荷解析的**生产默认**——来源清单（用户配置优先，否则内置副本）+ 不落盘缓存。
    // `cacheDir` 有意留空（只留内存）：与 FlashPanel 的 preloader 获取同款口径 —— 载荷二进制
    // 只在这一步的内存里，不往用户盘上落一份（本仓不分发载荷，也不额外留下一份副本）。
    m_bromPayloadOptions.sourceListPath = bromui::defaultSourceListPath();
    m_bromPayloadOptions.cacheDir.clear();

    setupUI();
    // W8-T6：初始态也要落原因（未选设备/无芯片码）—— 灰键不解释是本区最坏的形态。
    refreshBromGate();

    connect(m_engine, &ExploitEngine::outputLine,
            this, &VulnPanel::onEngineOutput);
    connect(m_engine, &ExploitEngine::progressChanged,
            this, &VulnPanel::onProgressChanged);
}

void VulnPanel::setBromRunner(const BromRunner &runner)
{
    m_bromRunner = runner;
}

void VulnPanel::setBromDownloader(const mtkbrom::ExploitDownloader &downloader)
{
    m_bromDownloader = downloader;
}

void VulnPanel::setBromPayloadOptions(const mtkbrom::ExploitPayloadOptions &opt)
{
    // **整体替换**（不合并）：避免"用例的临时清单 + 生产缓存目录"这类混合态把字节写进用户缓存。
    m_bromPayloadOptions = opt;
}

void VulnPanel::setConsentStore(legal::ConsentStore *store)
{
    m_consentStore = store;
}

VulnPanel::~VulnPanel() = default;

void VulnPanel::setupUI()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(8, 8, 8, 8);

    // ── Header: device info ──
    QHBoxLayout *headerLayout = new QHBoxLayout();

    m_backBtn = new QPushButton(QStringLiteral("← 返回"), this);
    m_backBtn->setMaximumWidth(80);

    m_deviceLabel = new QLabel(QStringLiteral("未选择设备"), this);
    m_deviceLabel->setStyleSheet("font-weight: bold; font-size: 13px;");

    m_rootLabel = new QLabel(this);

    m_riskLabel = new QLabel(this);
    m_riskLabel->setStyleSheet("color: #888; font-size: 11px;");

    headerLayout->addWidget(m_backBtn);
    headerLayout->addWidget(m_deviceLabel, 1);
    headerLayout->addWidget(m_rootLabel);
    headerLayout->addWidget(m_riskLabel);
    mainLayout->addLayout(headerLayout);

    // ── Action buttons ──
    QHBoxLayout *actionLayout = new QHBoxLayout();
    m_loadDbBtn = new QPushButton(QStringLiteral("加载数据库"), this);
    m_scanBtn = new QPushButton(QStringLiteral("扫描检测"), this);
    m_scanBtn->setEnabled(false);
    m_exploitAllBtn = new QPushButton(QStringLiteral("全部利用"), this);
    m_exploitAllBtn->setEnabled(false);

    actionLayout->addWidget(m_loadDbBtn);
    actionLayout->addWidget(m_scanBtn);
    actionLayout->addWidget(m_exploitAllBtn);
    actionLayout->addStretch();
    mainLayout->addLayout(actionLayout);

    // ── Vulnerability table ──
    QGroupBox *tableGroup = new QGroupBox(QStringLiteral("漏洞列表"), this);
    QVBoxLayout *tableLayout = new QVBoxLayout(tableGroup);

    m_vulnTable = new QTableWidget(0, 5, this);
    m_vulnTable->setHorizontalHeaderLabels({
        QString(),                  // 0: severity icon
        QStringLiteral("CVE ID"),    // 1
        QStringLiteral("描述 / 影响范围"), // 2
        QStringLiteral("状态"),      // 3
        QString()                   // 4: action button
    });
    m_vulnTable->setColumnWidth(0, 30);
    m_vulnTable->setColumnWidth(1, 150);
    m_vulnTable->horizontalHeader()->setStretchLastSection(false);
    m_vulnTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_vulnTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_vulnTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_vulnTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_vulnTable->verticalHeader()->setVisible(false);
    m_vulnTable->setAlternatingRowColors(true);
    m_vulnTable->setMinimumHeight(200);

    tableLayout->addWidget(m_vulnTable);
    mainLayout->addWidget(tableGroup, 1);

    // ── W8-T6：MTK BROM 漏洞利用区 ──
    mainLayout->addWidget(buildBromExploitGroup());

    // ── Log output ──
    QGroupBox *logGroup = new QGroupBox(QStringLiteral("输出日志"), this);
    QVBoxLayout *logLayout = new QVBoxLayout(logGroup);

    m_logOutput = new QTextEdit(this);
    m_logOutput->setReadOnly(true);
    m_logOutput->setStyleSheet("font-family: 'Courier New', monospace; font-size: 11px;");
    m_logOutput->setMinimumHeight(100);
    logLayout->addWidget(m_logOutput);
    mainLayout->addWidget(logGroup, 0);

    // ── Connections ──
    connect(m_backBtn, &QPushButton::clicked, this, &VulnPanel::switchToDeviceInfo);
    connect(m_loadDbBtn, &QPushButton::clicked, this, &VulnPanel::onLoadDatabase);
    connect(m_scanBtn, &QPushButton::clicked, this, &VulnPanel::onScan);
    connect(m_exploitAllBtn, &QPushButton::clicked, this, &VulnPanel::onExploitAll);
}

// W8-T6：MTK BROM 漏洞利用区（载荷来源 / 目标芯片 / 执行 / 日志 —— 日志落在本面板的输出日志区）。
//
// 三个输入为什么必须在**面板上显式给**而不是从设备/刷写链里取：
//   * 目标芯片：`runBromExploit` 要的是**芯片表里的硬件地址**（watchdog/uart/载荷地址），表按 hw_code 查；
//     设备自报的 hw_code 要到握手之后才有，而"按钮该不该亮"要在那之前就判（判不出来就是"未就绪"）。
//     故面板输入 = 用户的**声明**，执行时再与设备自报值核对（不符 = 拒，见 runBromExploitOnDevice）。
//   * 载荷：本仓**不分发**载荷二进制（清单只有 URL + sha256），所以载荷必须来自用户自己的文件或
//     显式开启的网络获取 —— 没有"内置一份"这个选项。
//   * daMode / var1：前者固定 false（kamakiri 路径的上游取值）；后者按"表优先 → 显式覆盖 → 响亮失败"，
//     本区**不暴露**覆盖口（缺 var1 的芯片由 runBromExploit 响亮失败，不静默 0）。
QWidget *VulnPanel::buildBromExploitGroup()
{
    QGroupBox *group = new QGroupBox(QStringLiteral("MTK BROM 漏洞利用（kamakiri 路径）"), this);
    QVBoxLayout *v = new QVBoxLayout(group);

    // —— 目标芯片（hw_code）——
    QHBoxLayout *chipRow = new QHBoxLayout();
    chipRow->addWidget(new QLabel(QStringLiteral("目标芯片 hw_code："), group));
    m_bromHwCodeEdit = new QLineEdit(group);
    m_bromHwCodeEdit->setObjectName(QStringLiteral("bromHwCodeEdit"));
    m_bromHwCodeEdit->setPlaceholderText(QStringLiteral("如 0x6735"));
    m_bromHwCodeEdit->setMaximumWidth(120);
    chipRow->addWidget(m_bromHwCodeEdit);
    chipRow->addStretch();
    v->addLayout(chipRow);

    m_bromChipStatus = new QLabel(group);
    m_bromChipStatus->setObjectName(QStringLiteral("bromChipStatus"));
    m_bromChipStatus->setWordWrap(true);
    m_bromChipStatus->setStyleSheet(QStringLiteral("font-size: 11px;"));
    v->addWidget(m_bromChipStatus);

    // —— 载荷来源 ——
    QHBoxLayout *payloadRow = new QHBoxLayout();
    payloadRow->addWidget(new QLabel(QStringLiteral("载荷来源："), group));
    m_bromPayloadPathEdit = new QLineEdit(group);
    m_bromPayloadPathEdit->setObjectName(QStringLiteral("bromPayloadPathEdit"));
    m_bromPayloadPathEdit->setPlaceholderText(
        QStringLiteral("载荷文件（留空 = 按来源清单获取，需勾选下面的网络开关）"));
    payloadRow->addWidget(m_bromPayloadPathEdit, 1);
    auto *payloadBrowse = new QPushButton(QStringLiteral("选择…"), group);
    payloadRow->addWidget(payloadBrowse);
    m_bromAllowNetworkCheck = new QCheckBox(QStringLiteral("允许网络获取"), group);
    m_bromAllowNetworkCheck->setObjectName(QStringLiteral("bromAllowNetworkCheck"));
    m_bromAllowNetworkCheck->setToolTip(QStringLiteral(
        "默认关闭（关着时下载器一次都不会被调用）。开启后按来源清单下载，"
        "并逐条做 sha256 fail-closed 校验 —— 校验不过的字节一律丢弃。"));
    payloadRow->addWidget(m_bromAllowNetworkCheck);
    v->addLayout(payloadRow);

    QHBoxLayout *resolveRow = new QHBoxLayout();
    m_bromResolveBtn = new QPushButton(QStringLiteral("解析载荷"), group);
    m_bromResolveBtn->setObjectName(QStringLiteral("bromResolveButton"));
    resolveRow->addWidget(m_bromResolveBtn);
    m_bromPayloadStatus = new QLabel(group);
    m_bromPayloadStatus->setObjectName(QStringLiteral("bromPayloadStatus"));
    m_bromPayloadStatus->setWordWrap(true);
    m_bromPayloadStatus->setStyleSheet(QStringLiteral("font-size: 11px;"));
    resolveRow->addWidget(m_bromPayloadStatus, 1);
    v->addLayout(resolveRow);

    // —— 执行 ——
    QHBoxLayout *runRow = new QHBoxLayout();
    m_bromRunBtn = new QPushButton(QStringLiteral("执行利用"), group);
    m_bromRunBtn->setObjectName(QStringLiteral("bromRunButton"));
    m_bromRunBtn->setEnabled(false);      // 判据由 refreshBromGate 给（初始必然未就绪）
    runRow->addWidget(m_bromRunBtn);
    m_bromAllowPatchedCheck = new QCheckBox(QStringLiteral("允许已修补代际（V6）"), group);
    m_bromAllowPatchedCheck->setObjectName(QStringLiteral("bromAllowPatchedCheck"));
    m_bromAllowPatchedCheck->setToolTip(QStringLiteral(
        "默认关 = 已修补代际（V6 / XML）直接早退（fail-closed）。本仓没有逐设备的 BROM 补丁判据，"
        "这里用的是来自公开报告的粗判据（会有假阳性）—— 开启只代表「允许尝试」，不保证有意义。"));
    runRow->addWidget(m_bromAllowPatchedCheck);
    runRow->addStretch();
    v->addLayout(runRow);

    m_bromRunReason = new QLabel(group);
    m_bromRunReason->setObjectName(QStringLiteral("bromRunReason"));
    m_bromRunReason->setWordWrap(true);
    m_bromRunReason->setStyleSheet(QStringLiteral("font-size: 11px; color: #e67e22;"));
    v->addWidget(m_bromRunReason);

    auto *note = new QLabel(QStringLiteral(
        "⚠️ 执行**作用于当前 USB 上枚举到的 BROM 设备**（多台时直接拒绝）—— 不是左侧设备列表里选中的"
        "那台；芯片码会在握手后与设备自报值核对（不符即拒）。执行日志会记下目标设备的 VID:PID 与端口。\n"
        "本仓库不分发任何利用载荷二进制：来源清单只存 URL + sha256，载荷来自你自己的文件"
        "或你显式开启的网络获取。真机未验证 —— 详见功能清单里的诚实边界。"), group);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: #888; font-size: 11px;"));
    v->addWidget(note);

    // —— 连接 ——
    connect(m_bromHwCodeEdit, &QLineEdit::textChanged,
            this, &VulnPanel::onBromChipCodeChanged);
    connect(m_bromResolveBtn, &QPushButton::clicked,
            this, &VulnPanel::onBromResolvePayload);
    connect(m_bromRunBtn, &QPushButton::clicked,
            this, &VulnPanel::onBromExploitRun);
    connect(payloadBrowse, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择利用载荷文件"), QString(),
            QStringLiteral("载荷 (*.bin);;所有文件 (*)"));
        if (!path.isEmpty())
            m_bromPayloadPathEdit->setText(path);
    });

    return group;
}

void VulnPanel::setDeviceInfo(const DeviceInfo &info)
{
    m_deviceInfo = info;
    m_deviceLabel->setText(QStringLiteral("%1 · Android %2 · 补丁 %3")
        .arg(info.model.isEmpty() ? info.serialNumber : info.model)
        .arg(info.androidVersion.isEmpty() ? "?" : info.androidVersion)
        .arg(info.securityPatch.isEmpty() ? "未知" : info.securityPatch));

    m_rootLabel->setText(info.isRooted
        ? QStringLiteral("已Root")
        : QStringLiteral("未Root"));
    m_rootLabel->setStyleSheet(info.isRooted
        ? "color: #c0392b; font-weight: bold;"
        : "color: #666;");

    m_engine->setDeviceSerial(info.serialNumber);
    m_engine->setRootEnabled(info.isRooted);

    // Update risk score if we have entries
    if (!m_currentEntries.isEmpty() && !m_deviceInfo.serialNumber.isEmpty()) {
        VulnMatcher matcher(m_currentEntries);
        QList<MatchResult> results = matcher.matchDevice(m_deviceInfo);
        int risk = VulnMatcher::calculateRiskScore(results);
        m_riskLabel->setText(QStringLiteral("风险评分: %1").arg(risk));

        // Color code
        QString color = risk >= 70 ? "#c0392b" : (risk >= 40 ? "#e67e22" : "#27ae60");
        m_riskLabel->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold;").arg(color));
    }

    updateButtonStates();
    refreshBromGate();   // W8-T6：设备来了/换了 → 「执行利用」的判据跟着重算
}

void VulnPanel::clearDeviceInfo()
{
    m_deviceInfo = DeviceInfo();
    m_deviceLabel->setText(QStringLiteral("未选择设备"));
    m_rootLabel->setText("");
    m_riskLabel->setText("");

    updateButtonStates();
    refreshBromGate();   // W8-T6：拔线/取消选中 → 执行键必须**当场禁用**（不留"拔线后仍可点"）
}

void VulnPanel::updateButtonStates()
{
    bool hasDevice = !m_deviceInfo.serialNumber.isEmpty();
    bool hasDb = !m_db->getAllEntries().isEmpty();
    m_scanBtn->setEnabled(hasDevice && hasDb);
}

void VulnPanel::appendLog(const QString &msg, bool isError)
{
    if (msg.isEmpty()) {
        m_logOutput->insertHtml("<br>");
        m_logOutput->verticalScrollBar()->setValue(m_logOutput->verticalScrollBar()->maximum());
        return;
    }

    QString timestamp = QDateTime::currentDateTime().toString("[HH:mm:ss] ");
    QString color = isError ? "#e74c3c" : "#2c3e50";

    // Dim section header lines
    if (msg.contains("────") || msg.contains("──"))
        color = "#7f8c8d";

    // Highlight success (✔) in green, warning (⚠) in orange
    if (msg.contains("✔"))
        color = "#27ae60";
    else if (msg.contains("✘"))
        color = "#e74c3c";
    else if (msg.contains("⚠"))
        color = "#e67e22";

    QString html = QString("<span style='color: %1;'>%2%3</span><br>")
        .arg(color, timestamp, msg.toHtmlEscaped());
    m_logOutput->insertHtml(html);
    m_logOutput->verticalScrollBar()->setValue(m_logOutput->verticalScrollBar()->maximum());
    emit outputMessage(msg, isError);
}

void VulnPanel::setButtonsEnabled(bool enabled)
{
    m_loadDbBtn->setEnabled(enabled);
    m_scanBtn->setEnabled(enabled && !m_db->getAllEntries().isEmpty());
    m_exploitAllBtn->setEnabled(enabled);
}

QString VulnPanel::stateIcon(VulnEntry::State state) const
{
    switch (state) {
    case VulnEntry::VULNERABLE:     return QStringLiteral("⚠"); // ⚠
    case VulnEntry::NOT_VULNERABLE: return QStringLiteral("✔"); // ✔
    case VulnEntry::EXPLOITED:      return QStringLiteral("✔"); // ✔
    case VulnEntry::FAILED:         return QStringLiteral("✘"); // ✘
    default:                        return QStringLiteral("○"); // ○
    }
}

QString VulnPanel::severityIcon(const QString &severity) const
{
    QString s = severity.toUpper();
    if (s == "CRITICAL") return QStringLiteral("★"); // ★
    if (s == "HIGH")     return QStringLiteral("▲"); // ▲
    if (s == "MEDIUM")   return QStringLiteral("●"); // ●
    if (s == "LOW")      return QStringLiteral("○"); // ○
    return QStringLiteral("○");
}

int VulnPanel::findRowByCveId(const QString &cveId) const
{
    for (int row = 0; row < m_vulnTable->rowCount(); ++row) {
        auto *item = m_vulnTable->item(row, 1);
        if (item && item->text() == cveId)
            return row;
    }
    return -1;
}

void VulnPanel::updateEntryInTable(int row, const VulnEntry &entry)
{
    // Column 0: severity icon
    auto *iconItem = new QTableWidgetItem(severityIcon(entry.severity));
    iconItem->setTextAlignment(Qt::AlignCenter);
    QString sevColor = "#27ae60"; // default green
    QString s = entry.severity.toUpper();
    if (s == "CRITICAL") sevColor = "#e74c3c";
    else if (s == "HIGH") sevColor = "#e67e22";
    else if (s == "MEDIUM") sevColor = "#f39c12";
    iconItem->setForeground(QColor(sevColor));
    iconItem->setToolTip(entry.severity);
    m_vulnTable->setItem(row, 0, iconItem);

    // Column 1: CVE ID
    m_vulnTable->setItem(row, 1, new QTableWidgetItem(entry.id));

    // Column 2: summary + affected info
    QString affectedStr;
    if (!entry.affected.androidVersionMin.isEmpty() || !entry.affected.androidVersionMax.isEmpty()) {
        affectedStr = QStringLiteral("Android %1-%2")
            .arg(entry.affected.androidVersionMin.isEmpty() ? "?" : entry.affected.androidVersionMin)
            .arg(entry.affected.androidVersionMax.isEmpty() ? "?" : entry.affected.androidVersionMax);
    }
    if (!entry.affected.patchLevelBefore.isEmpty()) {
        if (!affectedStr.isEmpty()) affectedStr += " · ";
        affectedStr += QStringLiteral("补丁< %1").arg(entry.affected.patchLevelBefore);
    }
    if (!entry.affected.platforms.isEmpty()) {
        if (!affectedStr.isEmpty()) affectedStr += " · ";
        affectedStr += entry.affected.platforms.join(", ");
    }
    QString descText = entry.summary;
    if (!affectedStr.isEmpty())
        descText += "\n" + affectedStr;
    auto *descItem = new QTableWidgetItem(descText);
    descItem->setToolTip(affectedStr);
    m_vulnTable->setItem(row, 2, descItem);

    // Column 3: state
    QString stateText;
    QString stateColor = "#666";
    switch (entry.state) {
    case VulnEntry::VULNERABLE:
        stateText = QStringLiteral("存在漏洞");
        stateColor = "#e74c3c";
        break;
    case VulnEntry::NOT_VULNERABLE:
        stateText = QStringLiteral("不受影响");
        stateColor = "#27ae60";
        break;
    case VulnEntry::EXPLOITED:
        stateText = QStringLiteral("已利用");
        stateColor = "#8e44ad";
        break;
    case VulnEntry::FAILED:
        stateText = QStringLiteral("失败");
        stateColor = "#c0392b";
        break;
    default:
        stateText = QStringLiteral("待检测");
        stateColor = "#95a5a6";
        break;
    }
    auto *stateItem = new QTableWidgetItem(stateIcon(entry.state) + " " + stateText);
    stateItem->setForeground(QColor(stateColor));
    m_vulnTable->setItem(row, 3, stateItem);

    // Column 4: exploit button
    auto *btnWidget = new QWidget();
    auto *btnLayout = new QHBoxLayout(btnWidget);
    btnLayout->setContentsMargins(2, 2, 2, 2);
    auto *exploitBtn = new QPushButton(QStringLiteral("利用"), btnWidget);
    exploitBtn->setMaximumWidth(60);
    exploitBtn->setEnabled(entry.state == VulnEntry::VULNERABLE);
    QString cveId = entry.id;
    connect(exploitBtn, &QPushButton::clicked, this, [this, cveId]() {
        onExploitSingle(cveId);
    });
    btnLayout->addWidget(exploitBtn);
    btnLayout->addStretch();
    m_vulnTable->setCellWidget(row, 4, btnWidget);
}

// ── Slots ──

void VulnPanel::onLoadDatabase()
{
    QString path = QFileDialog::getOpenFileName(this,
        QStringLiteral("选择漏洞数据库文件"),
        QString(),
        QStringLiteral("漏洞数据库 (*.json);;所有文件 (*)"));
    if (path.isEmpty())
        return;

    QFileInfo fi(path);
    appendLog(QStringLiteral("加载漏洞数据库: %1").arg(fi.fileName()));

    if (!m_db->loadFromFile(path)) {
        appendLog(QStringLiteral("加载失败: 文件格式错误或无法读取"), true);
        QMessageBox::warning(this,
            QStringLiteral("加载失败"),
            QStringLiteral("无法加载漏洞数据库文件。\n请确保文件是有效的 JSON 格式。"));
        return;
    }

    // Update table
    m_vulnTable->setRowCount(0);
    m_currentEntries = m_db->getAllEntries();

    for (const auto &e : m_currentEntries) {
        int row = m_vulnTable->rowCount();
        m_vulnTable->insertRow(row);
        updateEntryInTable(row, e);
    }

    // Severity breakdown
    int c = 0, h = 0, m = 0, l = 0;
    for (const auto &e : m_currentEntries) {
        QString s = e.severity.toUpper();
        if (s == "CRITICAL") c++;
        else if (s == "HIGH") h++;
        else if (s == "MEDIUM") m++;
        else if (s == "LOW") l++;
    }
    appendLog(QStringLiteral("已加载 %1 个条目 | ★ CRITICAL: %2  ▲ HIGH: %3  ● MEDIUM: %4  ○ LOW: %5")
        .arg(m_currentEntries.size()).arg(c).arg(h).arg(m).arg(l));

    updateButtonStates();
}

void VulnPanel::onScan()
{
    // W2-T5：扫描也纳入（spec §5：判据出自 exploit DB，属诊断与安全研究）
    if (!riskui::ensureAccepted(this, riskui::riskTierForFeature(riskui::RiskFeature::VulnExploit), m_consentStore)) return;
    if (m_deviceInfo.serialNumber.isEmpty()) {
        appendLog(QStringLiteral("请先连接设备"), true);
        return;
    }

    if (m_db->getAllEntries().isEmpty()) {
        appendLog(QStringLiteral("请先加载漏洞数据库"), true);
        return;
    }

    if (m_isScanning) return;
    m_isScanning = true;
    setButtonsEnabled(false);

    QElapsedTimer timer;
    timer.start();

    // ── Device info ──
    appendLog(QStringLiteral("开始扫描检测 ──────────────────────"));
    appendLog(QStringLiteral("  %1 · Android %2 · SDK %3 · 补丁 %4%5")
        .arg(m_deviceInfo.model.isEmpty() ? m_deviceInfo.serialNumber : m_deviceInfo.model)
        .arg(m_deviceInfo.androidVersion.isEmpty() ? "?" : m_deviceInfo.androidVersion)
        .arg(m_deviceInfo.sdkVersion.isEmpty() ? "?" : m_deviceInfo.sdkVersion)
        .arg(m_deviceInfo.securityPatch.isEmpty() ? "未知" : m_deviceInfo.securityPatch)
        .arg(m_deviceInfo.isRooted ? " · 已Root" : ""));

    // 1. VulnMatcher: filter by device info
    VulnMatcher matcher(m_currentEntries);
    QList<MatchResult> matched = matcher.matchDevice(m_deviceInfo);
    int riskScore = VulnMatcher::calculateRiskScore(matched);

    // Update state in entries: mark NOT_VULNERABLE based on matcher
    QSet<QString> vulnerableIds;
    for (const auto &mr : matched) {
        if (mr.isVulnerable)
            vulnerableIds.insert(mr.cveId);
    }

    int matchedCount = 0;
    for (auto &e : m_currentEntries) {
        if (vulnerableIds.contains(e.id)) {
            e.state = VulnEntry::UNKNOWN;
            matchedCount++;
        } else {
            e.state = VulnEntry::NOT_VULNERABLE;
        }
    }

    // 2. Run detect scripts for potentially vulnerable entries
    QList<VulnEntry> toDetect;
    for (const auto &e : m_currentEntries) {
        if (e.state == VulnEntry::UNKNOWN)
            toDetect.append(e);
    }

    appendLog(QStringLiteral("  匹配 %1/%2 个条目").arg(matchedCount).arg(m_currentEntries.size()));

    if (toDetect.isEmpty()) {
        appendLog(QStringLiteral("  (所有条目因版本/补丁/平台不匹配已排除)"));
    } else {
        appendLog(QStringLiteral("  命中 %1 个, 执行检测脚本...").arg(toDetect.size()));

        // Show severity summary of matches
        int mc = 0, mh = 0, mm = 0;
        for (const auto &mr : matched) {
            if (!mr.isVulnerable) continue;
            QString s = mr.severity.toUpper();
            if (s == "CRITICAL") mc++;
            else if (s == "HIGH") mh++;
            else if (s == "MEDIUM") mm++;
        }
        if (mc || mh || mm) {
            QStringList parts;
            if (mc) parts << QStringLiteral("★ CRITICAL: %1").arg(mc);
            if (mh) parts << QStringLiteral("▲ HIGH: %1").arg(mh);
            if (mm) parts << QStringLiteral("● MEDIUM: %1").arg(mm);
            appendLog(QStringLiteral("  严重度分布: %1").arg(parts.join("  ")));
        }

        QList<ExploitResult> detectResults = m_engine->runDetectionBatch(toDetect);

        int vulnCount = 0;
        for (int i = 0; i < toDetect.size() && i < detectResults.size(); ++i) {
            int idx = -1;
            for (int j = 0; j < m_currentEntries.size(); ++j) {
                if (m_currentEntries[j].id == toDetect[i].id) {
                    idx = j;
                    break;
                }
            }
            if (idx >= 0) {
                m_currentEntries[idx].state = detectResults[i].state;
                if (detectResults[i].state == VulnEntry::VULNERABLE) {
                    vulnCount++;
                    appendLog(QStringLiteral("  ⚠ %1: 存在漏洞").arg(toDetect[i].id));
                } else {
                    appendLog(QStringLiteral("  ✔ %1: 不受影响").arg(toDetect[i].id));
                }
            }
        }
        appendLog(QStringLiteral("扫描完成 · %1 个漏洞 · 耗时 %2ms")
            .arg(vulnCount).arg(timer.elapsed()));
    }

    // Color the risk score
    QString riskColor = riskScore >= 70 ? "#c0392b" : (riskScore >= 40 ? "#e67e22" : "#27ae60");
    m_riskLabel->setText(QStringLiteral("风险评分: %1").arg(riskScore));
    m_riskLabel->setStyleSheet(QString("color: %1; font-size: 11px; font-weight: bold;").arg(riskColor));

    // 3. Refresh table
    for (int row = 0; row < m_currentEntries.size(); ++row) {
        updateEntryInTable(row, m_currentEntries[row]);
    }

    // Check if any vulnerable
    bool hasVuln = false;
    for (const auto &e : m_currentEntries) {
        if (e.state == VulnEntry::VULNERABLE) {
            hasVuln = true;
            break;
        }
    }
    m_exploitAllBtn->setEnabled(hasVuln);

    m_isScanning = false;
    setButtonsEnabled(true);
}

void VulnPanel::onExploitAll()
{
    // W2-T5：批量利用 —— Tier3 诊断与安全研究
    if (!riskui::ensureAccepted(this, riskui::riskTierForFeature(riskui::RiskFeature::VulnExploit), m_consentStore)) return;
    if (m_deviceInfo.serialNumber.isEmpty()) {
        appendLog(QStringLiteral("请先连接设备"), true);
        return;
    }

    if (m_isExploiting) return;
    m_isExploiting = true;
    setButtonsEnabled(false);

    QElapsedTimer timer;
    timer.start();

    // Collect vulnerable entries
    QList<VulnEntry> vulnEntries;
    for (auto &e : m_currentEntries) {
        if (e.state == VulnEntry::VULNERABLE)
            vulnEntries.append(e);
    }

    if (vulnEntries.isEmpty()) {
        appendLog(QStringLiteral("没有可利用的漏洞"), true);
        m_isExploiting = false;
        setButtonsEnabled(true);
        return;
    }

    appendLog(QStringLiteral("开始利用 %1 个漏洞 ──────────────────").arg(vulnEntries.size()));

    int successCount = 0, failCount = 0;
    for (const auto &e : vulnEntries) {
        appendLog(QStringLiteral("  ── %1 [%2] CVSS %3 ──")
            .arg(e.id, e.severity).arg(e.cvssScore, 0, 'f', 1));

        ExploitResult result = m_engine->runFull(e);

        int idx = -1;
        for (int i = 0; i < m_currentEntries.size(); ++i) {
            if (m_currentEntries[i].id == e.id) {
                idx = i;
                break;
            }
        }

        if (idx >= 0) {
            m_currentEntries[idx].state = result.state;
            updateEntryInTable(idx, m_currentEntries[idx]);
        }

        if (result.state == VulnEntry::EXPLOITED) {
            successCount++;
            appendLog(QStringLiteral("  ✔ %1: 利用成功").arg(e.id));
        } else {
            failCount++;
            appendLog(QStringLiteral("  ✘ %1: %2").arg(e.id, result.error), true);
        }
        appendLog(QString()); // separator
    }

    appendLog(QStringLiteral("全部完成 · %1 成功, %2 失败 · 耗时 %3ms")
        .arg(successCount).arg(failCount).arg(timer.elapsed()));
    m_isExploiting = false;
    setButtonsEnabled(true);
}

void VulnPanel::onExploitSingle(const QString &cveId)
{
    // W2-T5：单条利用 —— Tier3 诊断与安全研究（与批量利用同一档：闸门一次即两处都放行）
    if (!riskui::ensureAccepted(this, riskui::riskTierForFeature(riskui::RiskFeature::VulnExploit), m_consentStore)) return;
    if (m_deviceInfo.serialNumber.isEmpty()) {
        appendLog(QStringLiteral("请先连接设备"), true);
        return;
    }

    int row = findRowByCveId(cveId);
    if (row < 0) return;

    VulnEntry entry;
    for (const auto &e : m_currentEntries) {
        if (e.id == cveId) {
            entry = e;
            break;
        }
    }

    if (entry.id.isEmpty() || entry.state != VulnEntry::VULNERABLE) {
        appendLog(QStringLiteral("%1: 不是可被利用的状态").arg(cveId), true);
        return;
    }

    appendLog(QStringLiteral("  利用 %1  [%2] CVSS %3")
        .arg(cveId, entry.severity).arg(entry.cvssScore, 0, 'f', 1));

    ExploitResult result = m_engine->runFull(entry);

    for (auto &e : m_currentEntries) {
        if (e.id == cveId) {
            e.state = result.state;
            updateEntryInTable(row, e);
            break;
        }
    }

    if (result.state == VulnEntry::EXPLOITED) {
        appendLog(QStringLiteral("  ✔ %1: 利用成功").arg(cveId));
    } else {
        appendLog(QStringLiteral("  ✘ %1: %2").arg(cveId, result.error), true);
    }
}

void VulnPanel::onEngineOutput(const QString &line)
{
    // Forward engine output to our log (but skip raw ADB noise)
    if (line.startsWith("adb ")) return; // the raw command line
    appendLog("  " + line);
}

void VulnPanel::onProgressChanged(const QString &entryId, ExploitPhase phase)
{
    Q_UNUSED(entryId) // 进度文案只按阶段显示；参数保留以匹配 ExploitEngine::progressChanged 信号签名
    QString phaseName;
    switch (phase) {
    case ExploitPhase::DETECT:  phaseName = QStringLiteral("检测"); break;
    case ExploitPhase::EXPLOIT: phaseName = QStringLiteral("利用"); break;
    case ExploitPhase::VERIFY:  phaseName = QStringLiteral("验证"); break;
    }
    appendLog(QStringLiteral("    → %1 阶段...").arg(phaseName));
}

// ==================== W8-T6：MTK BROM 漏洞利用区 ====================

// 判据全部来自 `bromui::evaluateRunGate`（纯函数，用例逐条钉住）——本函数只做"算 → 落控件"。
void VulnPanel::refreshBromGate()
{
    bromui::GateInputs in;
    in.deviceSelected = !m_deviceInfo.serialNumber.isEmpty();
    in.hwCodeText     = m_bromHwCodeEdit->text();
    in.payload        = m_bromPayloadState;
    in.payloadError   = m_bromPayloadError;

    const bromui::RunGate gate = bromui::evaluateRunGate(in);

    m_bromChipStatus->setText(gate.chipSummary);
    m_bromRunBtn->setEnabled(gate.canRun);
    // 原因文案两处同源（按钮 tooltip + 下方状态行）：灰着不解释是本区最坏的形态 ——
    // 用户会以为"没设备/坏了"，而不是"芯片码填错了"。
    // 口径（控制方 2026-09-22 裁决①）：**说清作用于哪台** —— 本入口现场枚举 USB（多台即拒），
    // 打的是枚举到的那台，不是设备列表里选中的那台（芯片码会在握手后与设备自报值核对）。
    const QString reason = gate.canRun
        ? QStringLiteral("就绪：作用于**当前 USB 上枚举到的 BROM 设备**（芯片码现场核对，执行日志会记下 "
                         "VID:PID 与端口）—— 不是设备列表里选中的那台；执行前会弹一次"
                         "「诊断与安全研究」（Tier 3）的风险确认")
        : gate.reason;
    m_bromRunBtn->setToolTip(reason);
    m_bromRunReason->setText(reason);
}

void VulnPanel::setBromPayloadState(bromui::PayloadState state, const QString &errorText)
{
    m_bromPayloadState = state;
    m_bromPayloadError = errorText;
    if (state != bromui::PayloadState::Verified)
        m_bromPayload.clear();          // 非放行态不保留字节（fail-closed：判据与数据不脱钩）

    switch (state) {
    case bromui::PayloadState::None:
        m_bromPayloadStatus->setText(QStringLiteral("载荷：未解析"));
        break;
    case bromui::PayloadState::Verified:
        m_bromPayloadStatus->setText(QStringLiteral("载荷：✔ 已就绪（%1 字节）").arg(m_bromPayload.size()));
        break;
    case bromui::PayloadState::Rejected:
        m_bromPayloadStatus->setText(QStringLiteral("载荷：✘ %1").arg(errorText));
        break;
    }
    refreshBromGate();
}

void VulnPanel::onBromChipCodeChanged()
{
    // 芯片码一变，已解析的载荷**一律作废**：来源是按芯片提示挑的（逐芯片条目优先于通用条目，
    // 见 resolveExploitPayload），换了芯片还留着旧字节 = 拿 A 芯片的载荷去跑 B 芯片。
    // 代价是改一次芯片码要重新解析一次；用缓存（若用户配了 cacheDir）不会重复下载。
    if (m_bromPayloadState != bromui::PayloadState::None) {
        const bool wasVerified = (m_bromPayloadState == bromui::PayloadState::Verified);
        setBromPayloadState(bromui::PayloadState::None, QString());
        if (wasVerified)
            appendLog(QStringLiteral("目标芯片已改动 —— 已解析的载荷作废，请重新点「解析载荷」"), false);
    }
    refreshBromGate();
}

void VulnPanel::onBromResolvePayload()
{
    // 本入口**不过 tier3 门控**：它只读本地文件、或按用户**显式**勾选的网络开关取一份字节，
    // 不碰设备、不改设备状态（W2 的界线：改设备状态才弹二次确认）。设备动作只在「执行利用」那道，
    // 那里过 Tier 3（见 onBromExploitRun）。
    mtkbrom::ExploitPayloadOptions opt = m_bromPayloadOptions;
    opt.explicitPath = m_bromPayloadPathEdit->text().trimmed();   // 用户当场给的文件优先
    opt.allowNetwork = m_bromAllowNetworkCheck->isChecked();      // 用户当场的选择优先
    if (opt.sourceListPath.isEmpty())
        opt.sourceListPath = bromui::defaultSourceListPath();

    // 芯片提示的形态必须与清单的 chipHint 一致（`0x%04x`）—— 形态对不上会**静默**少试逐芯片来源。
    quint16 hwCode = 0;
    const QString hint = bromui::parseHwCodeText(m_bromHwCodeEdit->text(), &hwCode, nullptr)
                             ? bromui::formatHwCodeHint(hwCode)
                             : QString();

    appendLog(QStringLiteral("解析 MTK BROM 载荷（芯片提示 %1；来源 %2；网络 %3）")
                  .arg(hint.isEmpty() ? QStringLiteral("（无）") : hint,
                       opt.explicitPath.isEmpty() ? QStringLiteral("来源清单")
                                                  : QStringLiteral("显式文件"),
                       opt.allowNetwork ? QStringLiteral("开") : QStringLiteral("关（默认）")));

    QByteArray payload;
    QStringList log;
    QString err;
    const mtkbrom::ExploitDownloader downloader =
        m_bromDownloader ? m_bromDownloader : mtkbrom::makeQtExploitDownloader();
    const bool ok = mtkbrom::resolveExploitPayload(opt, hint, payload, &log, &err, downloader);

    for (const QString &line : log)
        appendLog(QStringLiteral("  %1").arg(line), false);

    if (!ok) {
        appendLog(QStringLiteral("  ✘ %1").arg(err), true);
        setBromPayloadState(bromui::PayloadState::Rejected, err);
        return;
    }
    m_bromPayload = payload;
    setBromPayloadState(bromui::PayloadState::Verified, QString());
    appendLog(QStringLiteral("  ✔ 载荷就绪：%1 字节 —— 可以执行（执行前会弹一次风险确认）").arg(payload.size()));
}

void VulnPanel::onBromExploitRun()
{
    // W8-T6：执行前过 W2 的 tier 3 二次确认 —— **复用唯一闸门** `riskui::ensureAccepted`，
    // 不另起一套。取档用 `riskTierForFeature(VulnExploit)`（== Tier3Diagnostic）：BROM 载荷与
    // 扫描/批量利用/单条利用**同族同档**（绕过安全机制，spec §5 的 VulnPanel 行）。
    if (!riskui::ensureAccepted(this, riskui::riskTierForFeature(riskui::RiskFeature::VulnExploit),
                                m_consentStore))
        return;
    if (!m_bromRunBtn->isEnabled())
        return;   // 双保险：判据在 refreshBromGate；按钮灰着也挡不住程序化调用（与 EUB onStart 同款）

    quint16 hwCode = 0;
    QString perr;
    if (!bromui::parseHwCodeText(m_bromHwCodeEdit->text(), &hwCode, &perr)) {
        appendLog(QStringLiteral("芯片码不可用：%1").arg(perr), true);
        return;
    }

    // 首行就把"作用于哪台"写清楚（控制方 2026-09-22 裁决①）：**当前 USB 上枚举到的**那台，
    // 不是设备列表里选中的那台 —— 执行器随后会把该设备的 VID:PID/端口落进日志。
    appendLog(QStringLiteral("开始 MTK BROM 漏洞利用（kamakiri 路径） —— 目标芯片 0x%1；"
                             "作用于**当前 USB 上枚举到的 BROM 设备**（不是设备列表里选中的那台；"
                             "芯片码在握手后现场核对）")
                  .arg(hwCode, 4, 16, QLatin1Char('0')));

    BromRunRequest req;
    req.hwCode = hwCode;
    req.payload = m_bromPayload;
    req.allowPatchedGeneration = m_bromAllowPatchedCheck->isChecked();

    QElapsedTimer timer;
    timer.start();
    QStringList log;
    QString err;
    const BromRunner runner = m_bromRunner ? m_bromRunner : BromRunner(runBromExploitOnDevice);
    const bool ok = runner(req, &log, &err);

    for (const QString &line : log)
        appendLog(QStringLiteral("  %1").arg(line), false);

    if (ok) {
        // 判据只有 ack：照实说 —— 本仓没有"设备真的进入可利用状态"的证据链（无真机）。
        appendLog(QStringLiteral("✔ 触发序列完成（耗时 %1ms）。判据只有 **ack**：载荷已回 ack、"
                                 "触发控制传输已发出 —— 设备是否真的进入可利用状态，本仓无真机验证。")
                      .arg(timer.elapsed()), false);
    } else {
        appendLog(QStringLiteral("✘ 执行失败（耗时 %1ms）：%2").arg(timer.elapsed()).arg(err), true);
    }
    refreshBromGate();
}
