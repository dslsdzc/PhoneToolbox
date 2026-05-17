#include "vuln_panel.h"
#include "vuln_db/vuln_matcher.h"
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

VulnPanel::VulnPanel(QWidget *parent)
    : QWidget(parent)
    , m_db(new VulnDb(this))
    , m_engine(new ExploitEngine(this))
{
    setupUI();

    connect(m_engine, &ExploitEngine::outputLine,
            this, &VulnPanel::onEngineOutput);
    connect(m_engine, &ExploitEngine::progressChanged,
            this, &VulnPanel::onProgressChanged);
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
}

void VulnPanel::clearDeviceInfo()
{
    m_deviceInfo = DeviceInfo();
    m_deviceLabel->setText(QStringLiteral("未选择设备"));
    m_rootLabel->setText("");
    m_riskLabel->setText("");

    updateButtonStates();
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
    QString phaseName;
    switch (phase) {
    case ExploitPhase::DETECT:  phaseName = QStringLiteral("检测"); break;
    case ExploitPhase::EXPLOIT: phaseName = QStringLiteral("利用"); break;
    case ExploitPhase::VERIFY:  phaseName = QStringLiteral("验证"); break;
    }
    appendLog(QStringLiteral("    → %1 阶段...").arg(phaseName));
}
