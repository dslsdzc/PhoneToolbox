#include "vuln_panel.h"
#include "vuln_db/vuln_matcher.h"
#include "risk_confirm_dialog.h"        // W2-T5：二次确认**唯一闸门**（漏洞扫描/利用属 Tier3）
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QScrollBar>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>

#include "core/device_detector.h"
#include <QMessageBox>
#include <QDateTime>
#include <QSet>
#include <QElapsedTimer>
#include <QThread>
#include <QCheckBox>
#include <QDialog>
#include <QFormLayout>
#include <QTextBrowser>

#include "core/lua_sandbox.h"
#include <QCryptographicHash>   // M5：载荷指纹（本机实算的 sha256 前 8 位）
#include <QLineEdit>
#include <memory>

// W8-T6：MTK BROM 漏洞利用区（把 T1–T5 交付的能力接成可达）
#include "core/modes/mtk_brom.h"                          // BromSession / enumerateUsb / openLibusbUsb
#include "core/modes/mtk_brom_exploit_download_qt.h"      // makeQtExploitDownloader（生产下载器）
#include "ui/brom_exploit_ui.h"                           // 执行前判据（纯函数，逐条被用例钉住）

namespace {

// BROM 不是 CVE，也不能伪装成 CVE。这个串只用于内部路由（打开专属界面），
// 列表里显示的是 "MTK BROM"，不会出现任何 CVE 形态的编号。
const char kInternalBromId[] = "__brom_kamakiri__";

// 载荷指纹：**本机实算**的 sha256 前 8 位（spec §3.8 要求 UI 显示它；M5 落地）。
// 为什么强调"实算"：显示清单里那个值是回显，与手上这份字节没有因果关系；实算才有对表意义
// （用户拿它去和来源页/清单比）。**它不是安全属性** —— 见 I5 的口径（sha256 只证"与清单一致"）。
QString payloadFingerprint(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()).left(8);
}

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
    // var1 覆盖（W8 终审 I4）：只在面板判定"表里没有、用户填了"时才带上；表里有值时留空，
    // 让 runBromExploit 的"表优先"语义照旧（面板不替它做决定）。
    opt.var1 = req.var1;
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

QString vulnStateText(VulnEntry::State state)
{
    switch (state) {
    case VulnEntry::VULNERABLE:     return QStringLiteral("存在漏洞");
    case VulnEntry::NOT_VULNERABLE: return QStringLiteral("不受影响");
    case VulnEntry::EXPLOITED:      return QStringLiteral("已利用");
    case VulnEntry::FAILED:         return QStringLiteral("失败");
    default:                        return QStringLiteral("待检测");
    }
}

QString vulnAffectedText(const VulnEntry &entry)
{
    QStringList parts;
    if (!entry.affected.androidVersionMin.isEmpty()
        || !entry.affected.androidVersionMax.isEmpty()) {
        parts << QStringLiteral("Android %1-%2")
                     .arg(entry.affected.androidVersionMin.isEmpty()
                              ? QStringLiteral("?")
                              : entry.affected.androidVersionMin)
                     .arg(entry.affected.androidVersionMax.isEmpty()
                              ? QStringLiteral("?")
                              : entry.affected.androidVersionMax);
    }
    if (!entry.affected.patchLevelBefore.isEmpty())
        parts << QStringLiteral("补丁 < %1").arg(entry.affected.patchLevelBefore);
    if (!entry.affected.platforms.isEmpty())
        parts << entry.affected.platforms.join(QStringLiteral(", "));
    return parts.join(QStringLiteral(" · "));
}

} // namespace

// 单个漏洞的专属界面。
//   - 有 uiLua：沙箱执行脚本，按 ui.* 声明渲染 HTML / 表单 / 状态 / 按钮；
//   - 否则：默认模板（描述 + 执行利用）。
// BROM 是第一个完全走 Lua 的条目；外部漏洞库也可以在自己的 JSON 里带 ui。
class VulnDetailDialog : public QDialog
{
public:
    VulnDetailDialog(const VulnEntry &entry, QWidget *parent,
                     const LuaSandbox::DeviceSnapshot &device,
                     const LuaSandbox::BromApi &bromApi,
                     std::function<void(const QString &)> logger,
                     std::function<void()> onExploit)
        : QDialog(parent)
        , m_cveId(entry.id)
        , m_entry(entry)
        , m_logger(std::move(logger))
        , m_onExploit(std::move(onExploit))
    {
        setWindowTitle(QStringLiteral("%1 — 漏洞详情").arg(entry.id));
        setMinimumSize(720, 560);

        m_rootLayout = new QVBoxLayout(this);

        m_title = new QLabel(this);
        m_title->setWordWrap(true);
        m_title->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: bold;"));
        m_rootLayout->addWidget(m_title);

        m_meta = new QLabel(this);
        m_meta->setWordWrap(true);
        m_meta->setStyleSheet(QStringLiteral("color: #aaa;"));
        m_rootLayout->addWidget(m_meta);

        if (!entry.uiLua.isEmpty()) {
            m_sandbox = new LuaSandbox();
            m_sandbox->setDevice(device);
            m_sandbox->setBromApi(bromApi);
            m_sandbox->setLogger(m_logger);

            QString error;
            if (!m_sandbox->run(entry.uiLua, &error)) {
                auto *err = new QLabel(
                    QStringLiteral("自定义界面加载失败：%1").arg(error), this);
                err->setWordWrap(true);
                err->setStyleSheet(QStringLiteral("color:#c0392b;"));
                m_rootLayout->addWidget(err);
                buildDefaultBody();
            } else {
                buildLuaBody();
            }
        } else {
            buildDefaultBody();
        }

        updateEntry(entry);
    }

    ~VulnDetailDialog() override { delete m_sandbox; }

    QString cveId() const { return m_cveId; }

    void updateEntry(const VulnEntry &entry)
    {
        m_entry = entry;
        m_title->setText(QStringLiteral("%1　%2").arg(entry.id, entry.summary));

        const QString affected = vulnAffectedText(entry);
        m_meta->setText(QStringLiteral("严重度：%1　CVSS：%2　状态：%3\n影响范围：%4")
                            .arg(entry.severity)
                            .arg(entry.cvssScore, 0, 'f', 1)
                            .arg(vulnStateText(entry.state))
                            .arg(affected.isEmpty() ? QStringLiteral("未声明") : affected));

        if (m_exploitBtn) {
            m_exploitBtn->setEnabled(entry.state == VulnEntry::VULNERABLE);
            m_exploitBtn->setToolTip(
                entry.state == VulnEntry::VULNERABLE
                    ? QStringLiteral("对该设备执行此漏洞的利用脚本")
                    : QStringLiteral("只有检测结果为「存在漏洞」时才可执行利用"));
        }
    }

private:
    void buildDefaultBody()
    {
        m_htmlView = new QTextBrowser(this);
        m_htmlView->setReadOnly(true);
        m_htmlView->setOpenExternalLinks(false);
        m_rootLayout->addWidget(m_htmlView, 1);

        if (!m_entry.uiHtml.isEmpty()) {
            m_htmlView->setHtml(m_entry.uiHtml);
        } else {
            QString desc = m_entry.description;
            if (desc.isEmpty())
                desc = m_entry.summary;
            if (!m_entry.cvssVector.isEmpty())
                desc += QStringLiteral("\n\nCVSS 向量：%1").arg(m_entry.cvssVector);
            if (!m_entry.references.isEmpty())
                desc += QStringLiteral("\n\n参考：\n")
                        + m_entry.references.join(QLatin1Char('\n'));
            m_htmlView->setPlainText(desc);
        }

        auto *buttons = new QHBoxLayout();
        if (m_onExploit) {
            m_exploitBtn = new QPushButton(QStringLiteral("执行利用"), this);
            connect(m_exploitBtn, &QPushButton::clicked, this, [this]() {
                if (m_onExploit)
                    m_onExploit();
            });
            buttons->addWidget(m_exploitBtn);
        }
        auto *closeBtn = new QPushButton(QStringLiteral("关闭"), this);
        connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
        buttons->addStretch();
        buttons->addWidget(closeBtn);
        m_rootLayout->addLayout(buttons);
    }

    void buildLuaBody()
    {
        m_htmlView = new QTextBrowser(this);
        m_htmlView->setReadOnly(true);
        m_htmlView->setOpenExternalLinks(false);
        m_rootLayout->addWidget(m_htmlView, 1);

        // 表单只建一次：动作执行后保留用户输入，不重建控件。
        m_formWidget = new QWidget(this);
        m_formLayout = new QFormLayout(m_formWidget);
        m_formLayout->setContentsMargins(0, 0, 0, 0);
        m_rootLayout->addWidget(m_formWidget);

        for (const LuaSandbox::InputSpec &input : m_sandbox->inputs()) {
            if (input.checkbox) {
                auto *check = new QCheckBox(input.label, m_formWidget);
                check->setChecked(input.defaultValue == QStringLiteral("1"));
                connect(check, &QCheckBox::toggled, this,
                        [this, id = input.id](bool on) {
                            m_sandbox->setValue(id, on ? QStringLiteral("1")
                                                       : QStringLiteral("0"));
                        });
                m_formLayout->addRow(QString(), check);
                m_inputWidgets.insert(input.id, check);
            } else {
                auto *edit = new QLineEdit(input.defaultValue, m_formWidget);
                edit->setPlaceholderText(input.label);
                connect(edit, &QLineEdit::textChanged, this,
                        [this, id = input.id](const QString &text) {
                            m_sandbox->setValue(id, text);
                        });
                m_formLayout->addRow(input.label, edit);
                m_inputWidgets.insert(input.id, edit);
            }
        }

        m_statusWidget = new QWidget(this);
        m_statusLayout = new QVBoxLayout(m_statusWidget);
        m_statusLayout->setContentsMargins(0, 0, 0, 0);
        m_rootLayout->addWidget(m_statusWidget);

        for (const LuaSandbox::StatusSpec &status : m_sandbox->statuses()) {
            auto *label = new QLabel(status.text, m_statusWidget);
            label->setWordWrap(true);
            label->setStyleSheet(QStringLiteral("font-size: 11px; color: #aaa;"));
            m_statusLayout->addWidget(label);
            m_statusLabels.insert(status.id, label);
        }

        m_actionLayout = new QHBoxLayout();
        m_rootLayout->addLayout(m_actionLayout);

        auto *bottom = new QHBoxLayout();
        auto *closeBtn = new QPushButton(QStringLiteral("关闭"), this);
        connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);
        bottom->addStretch();
        bottom->addWidget(closeBtn);
        m_rootLayout->addLayout(bottom);

        refreshLuaUi();
    }

    void refreshLuaUi()
    {
        const QString html = m_sandbox->html();
        m_htmlView->setHtml(
            html.isEmpty()
                ? QStringLiteral("<p>%1</p>").arg(m_entry.summary.toHtmlEscaped())
                : html);

        for (const LuaSandbox::StatusSpec &status : m_sandbox->statuses()) {
            if (QLabel *label = m_statusLabels.value(status.id))
                label->setText(status.text);
        }

        // 动作按钮重建：脚本可能在动作后改变数量或 enabled。
        while (QLayoutItem *item = m_actionLayout->takeAt(0)) {
            if (QWidget *widget = item->widget())
                widget->deleteLater();
            delete item;
        }
        for (const LuaSandbox::Action &action : m_sandbox->actions()) {
            auto *button = new QPushButton(action.label, this);
            button->setEnabled(action.enabled);
            connect(button, &QPushButton::clicked, this, [this, id = action.id]() {
                runLuaAction(id);
            });
            m_actionLayout->addWidget(button);
        }
        m_actionLayout->addStretch();
    }

    void runLuaAction(const QString &id)
    {
        QString error;
        if (!m_sandbox->invokeAction(id, &error)) {
            if (m_logger)
                m_logger(QStringLiteral("界面动作 %1 失败：%2").arg(id, error));
        }
        refreshLuaUi();
    }

    QString m_cveId;
    VulnEntry m_entry;
    LuaSandbox *m_sandbox = nullptr;
    QVBoxLayout *m_rootLayout = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_meta = nullptr;
    QTextBrowser *m_htmlView = nullptr;
    QWidget *m_formWidget = nullptr;
    QFormLayout *m_formLayout = nullptr;
    QWidget *m_statusWidget = nullptr;
    QVBoxLayout *m_statusLayout = nullptr;
    QHBoxLayout *m_actionLayout = nullptr;
    QHash<QString, QWidget *> m_inputWidgets;
    QHash<QString, QLabel *> m_statusLabels;
    QPushButton *m_exploitBtn = nullptr;
    std::function<void(const QString &)> m_logger;
    std::function<void()> m_onExploit;
};

VulnPanel::VulnPanel(QWidget *parent)
    : QWidget(parent)
    , m_db(new VulnDb(this))
    , m_engine(new ExploitEngine()) // 无 parent：稍后 moveToThread
{
    // 漏洞脚本全部是同步 QProcess（单条最长可到 timeoutSeconds），留在主线程
    // 会把界面卡死。引擎整体搬到工作线程，UI 只通过 queued 回调更新。
    qRegisterMetaType<ExploitResult>("ExploitResult");
    m_engineThread = new QThread(this);
    m_engineThread->setObjectName(QStringLiteral("VulnEngine"));
    m_engine->moveToThread(m_engineThread);
    m_engineThread->start();
    // W8-T6：载荷解析的**生产默认**——来源清单（用户配置优先，否则内置副本）+ 不落盘缓存。
    // `cacheDir` 有意留空（只留内存）：与 FlashPanel 的 preloader 获取同款口径 —— 载荷二进制
    // 只在这一步的内存里，不往用户盘上落一份（本仓不分发载荷，也不额外留下一份副本）。
    m_bromPayloadOptions.sourceListPath = bromui::defaultSourceListPath();
    m_bromPayloadOptions.cacheDir.clear();

    setupUI();

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

VulnPanel::~VulnPanel()
{
    if (!m_engineThread)
        return;

    // 先请求取消：正在跑的脚本会在约 100ms 内被 kill，
    // 不必等它自然超时（漏洞脚本单条可能几分钟）。
    m_engine->requestCancel();

    if (m_engineThread->isRunning()) {
        QThread *const mainThread = QThread::currentThread();
        // 把 engine 移回主线程再 delete；直接跨线程销毁 QObject 是未定义行为。
        QMetaObject::invokeMethod(m_engine, [this, mainThread]() {
            m_engine->moveToThread(mainThread);
        }, Qt::BlockingQueuedConnection);
        m_engineThread->quit();
        m_engineThread->wait();
    }
    delete m_engine;
    m_engine = nullptr;
}

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
        QStringLiteral("CVE ID / 标识"), // 1
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

    // 内置列表只有 BROM 这一条：它不是 CVE，走芯片级路由；
    // 普通漏洞全部来自用户加载的外部库（可叠加多个）。
    updateBromRow(0);

    // ── Connections ──
    connect(m_backBtn, &QPushButton::clicked, this, &VulnPanel::switchToDeviceInfo);
    connect(m_loadDbBtn, &QPushButton::clicked, this, &VulnPanel::onLoadDatabase);
    connect(m_scanBtn, &QPushButton::clicked, this, &VulnPanel::onScan);
    connect(m_exploitAllBtn, &QPushButton::clicked, this, &VulnPanel::onExploitAll);
    connect(m_vulnTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int) {
        if (auto *item = m_vulnTable->item(row, 1))
            openVulnDetail(item->text());
    });
}

void VulnPanel::openVulnDetail(const QString &cveId)
{
    // BROM 是列表里的固定条目，但有自己完全独立的界面。
    if (cveId == QString::fromLatin1(kInternalBromId)) {
        openBromDetail();
        return;
    }

    VulnEntry entry;
    bool found = false;
    for (const auto &e : m_currentEntries) {
        if (e.id == cveId) {
            entry = e;
            found = true;
            break;
        }
    }
    if (!found)
        return;

    if (m_detailDialog) {
        m_detailDialog->close(); // WA_DeleteOnClose + destroyed 回调会把指针置空
        m_detailDialog = nullptr;
    }

    auto *dialog = new VulnDetailDialog(
        entry, this, deviceSnapshot(), LuaSandbox::BromApi(),
        [this](const QString &line) { appendLog(line); },
        [this, cveId]() { onExploitSingle(cveId); });
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QObject::destroyed, this, [this]() {
        m_detailDialog = nullptr;
    });
    m_detailDialog = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

void VulnPanel::openBromDetail()
{
    QFile script(QStringLiteral(":/ui/brom_exploit.lua"));
    if (!script.open(QIODevice::ReadOnly)) {
        appendLog(QStringLiteral("内置 BROM 界面脚本缺失（:/ui/brom_exploit.lua）"), true);
        return;
    }

    VulnEntry entry;
    entry.id = QStringLiteral("MTK BROM（非 CVE）");
    entry.summary = QStringLiteral("MTK BROM 漏洞利用（kamakiri 路径）");
    entry.description = QStringLiteral(
        "芯片级漏洞利用。按芯片 hw_code 查硬件地址表；载荷需自备。");
    entry.uiLua = QString::fromUtf8(script.readAll());

    if (m_detailDialog) {
        m_detailDialog->close();
        m_detailDialog = nullptr;
    }

    auto *dialog = new VulnDetailDialog(
        entry, this, deviceSnapshot(), makeBromApi(),
        [this](const QString &line) { appendLog(line); },
        nullptr);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &QObject::destroyed, this, [this]() {
        m_detailDialog = nullptr;
    });
    m_detailDialog = dialog;
    dialog->show();
    dialog->raise();
    dialog->activateWindow();
}

LuaSandbox::DeviceSnapshot VulnPanel::deviceSnapshot() const
{
    LuaSandbox::DeviceSnapshot snapshot;
    snapshot.serial = m_deviceInfo.serialNumber;
    snapshot.model = m_deviceInfo.model;
    snapshot.manufacturer = m_deviceInfo.manufacturer;
    snapshot.androidVersion = m_deviceInfo.androidVersion;
    snapshot.sdk = m_deviceInfo.sdkVersion;
    snapshot.securityPatch = m_deviceInfo.securityPatch;
    return snapshot;
}

// BROM 的宿主能力面：只给底层操作，界面和流程全在 resources/brom_exploit.lua。
LuaSandbox::BromApi VulnPanel::makeBromApi()
{
    LuaSandbox::BromApi api;

    // 复用设备检测层的结果（DeviceDetector 每 2s 轮询并识别 MODE_MTK_BROM），
    // 不再在 BROM 界面里另开一次 libusb 枚举 —— 否则同一件事有两套检测。
    api.scan = [this]() -> QString {
        QStringList bromDevices;
        for (auto it = m_devices.constBegin(); it != m_devices.constEnd(); ++it) {
            const DeviceInfo &info = it.value();
            if (info.mode != DeviceDetector::MODE_MTK_BROM)
                continue;
            bromDevices << QStringLiteral("%1（%2）").arg(it.key(), info.model);
        }
        if (bromDevices.isEmpty()) {
            return QStringLiteral(
                "设备列表中没有 BROM/preloader 设备。请让设备进入 BROM 模式，"
                "等上方设备列表识别后再打开本界面。");
        }
        return QStringLiteral("设备检测到 %1 台：%2")
            .arg(bromDevices.size())
            .arg(bromDevices.join(QStringLiteral("；")));
    };

    api.chipStatus = [](const QString &hwText, const QString &var1Text) -> QString {
        bromui::GateInputs in;
        in.deviceSelected = true;
        in.hwCodeText = hwText;
        in.var1Text = var1Text;
        in.payload = bromui::PayloadState::Verified; // 只取芯片摘要，不参与执行判定
        return bromui::evaluateRunGate(in).chipSummary;
    };

    api.gate = [this](const QString &hwText, const QString &var1Text,
                      bool payloadReady) -> LuaSandbox::BromGate {
        bromui::GateInputs in;
        in.deviceSelected = !m_deviceInfo.serialNumber.isEmpty();
        in.hwCodeText = hwText;
        in.var1Text = var1Text;
        in.payload = payloadReady ? bromui::PayloadState::Verified : m_bromPayloadState;
        in.payloadError = m_bromPayloadError;
        const bromui::RunGate gate = bromui::evaluateRunGate(in);
        return LuaSandbox::BromGate{gate.canRun, gate.chipSummary, gate.reason};
    };

    api.resolve = [this](const QString &hwText, const QString &payloadPath,
                         bool allowNetwork, QString *message) {
        return resolveBromPayload(hwText, payloadPath, allowNetwork, message);
    };

    api.run = [this](const QString &hwText, const QString &var1Text,
                     bool allowPatched, QString *message) {
        return runBromExploit(hwText, var1Text, allowPatched, message);
    };

    return api;
}

bool VulnPanel::resolveBromPayload(const QString &hwText, const QString &payloadPath,
                                   bool allowNetwork, QString *message)
{
    mtkbrom::ExploitPayloadOptions opt = m_bromPayloadOptions;
    opt.explicitPath = payloadPath.trimmed();
    opt.allowNetwork = allowNetwork;
    if (opt.sourceListPath.isEmpty())
        opt.sourceListPath = bromui::defaultSourceListPath();

    quint16 hwCode = 0;
    const QString hint = bromui::parseHwCodeText(hwText, &hwCode, nullptr)
                             ? bromui::formatHwCodeHint(hwCode)
                             : QString();

    appendLog(QStringLiteral("解析 MTK BROM 载荷（芯片提示 %1；来源 %2；网络 %3）")
                  .arg(hint.isEmpty() ? QStringLiteral("（无）") : hint,
                       opt.explicitPath.isEmpty() ? QStringLiteral("来源清单")
                                                  : QStringLiteral("显式文件"),
                       opt.allowNetwork ? QStringLiteral("开") : QStringLiteral("关（默认）")));

    QByteArray payload;
    QStringList log;
    QString error;
    const mtkbrom::ExploitDownloader downloader =
        m_bromDownloader ? m_bromDownloader : mtkbrom::makeQtExploitDownloader();
    const bool ok =
        mtkbrom::resolveExploitPayload(opt, hint, payload, &log, &error, downloader);

    for (const QString &line : log)
        appendLog(QStringLiteral("  %1").arg(line));

    if (!ok) {
        m_bromPayload.clear();
        m_bromPayloadState = bromui::PayloadState::Rejected;
        m_bromPayloadError = error;
        if (message)
            *message = error;
        return false;
    }

    m_bromPayload = payload;
    m_bromPayloadState = bromui::PayloadState::Verified;
    m_bromPayloadError.clear();
    if (message) {
        *message = QStringLiteral("✔ 已就绪（%1 字节 · sha256 前 8 位 %2）")
                       .arg(payload.size())
                       .arg(payloadFingerprint(payload));
    }
    return true;
}

bool VulnPanel::runBromExploit(const QString &hwText, const QString &var1Text,
                               bool allowPatched, QString *message)
{
    if (!riskui::ensureAccepted(this,
                                riskui::riskTierForFeature(riskui::RiskFeature::VulnExploit),
                                m_consentStore)) {
        if (message)
            *message = QStringLiteral("未接受风险确认，已取消");
        return false;
    }

    quint16 hwCode = 0;
    QString perr;
    if (!bromui::parseHwCodeText(hwText, &hwCode, &perr)) {
        if (message)
            *message = QStringLiteral("芯片码不可用：%1").arg(perr);
        return false;
    }

    bromui::GateInputs in;
    in.deviceSelected = !m_deviceInfo.serialNumber.isEmpty();
    in.hwCodeText = hwText;
    in.var1Text = var1Text;
    in.payload = m_bromPayloadState;
    in.payloadError = m_bromPayloadError;
    const bromui::RunGate gate = bromui::evaluateRunGate(in);
    if (!gate.canRun) {
        if (message)
            *message = QStringLiteral("执行条件未满足：%1").arg(gate.reason);
        return false;
    }

    appendLog(QStringLiteral("开始 MTK BROM 漏洞利用（kamakiri 路径） —— 目标芯片 0x%1；"
                             "作用于当前 USB 上枚举到的 BROM 设备（不是设备列表里选中的那台；"
                             "芯片码在握手后现场核对）")
                  .arg(hwCode, 4, 16, QLatin1Char('0')));

    BromRunRequest req;
    req.hwCode = hwCode;
    req.payload = m_bromPayload;
    req.allowPatchedGeneration = allowPatched;
    if (const mtkbrom::ChipHwAddrs *addrs = mtkbrom::lookupChipHwAddrs(hwCode)) {
        const bromui::Var1Resolution var1 = bromui::resolveVar1(*addrs, var1Text);
        if (var1.source == bromui::Var1Source::FromInput)
            req.var1 = var1.value;
    }

    QElapsedTimer timer;
    timer.start();
    QStringList log;
    QString error;
    const BromRunner runner = m_bromRunner ? m_bromRunner : BromRunner(runBromExploitOnDevice);
    const bool ok = runner(req, &log, &error);

    for (const QString &line : log)
        appendLog(QStringLiteral("  %1").arg(line));

    if (ok) {
        appendLog(QStringLiteral("✔ 触发序列完成（耗时 %1ms）。判据只有 ack：载荷已回 ack、"
                                 "触发控制传输已发出 —— 设备是否真的进入可利用状态，本仓无真机验证。")
                      .arg(timer.elapsed()));
        if (message)
            *message = QStringLiteral("✔ 触发序列完成（判据只有 ack，未真机验证）");
        return true;
    }

    appendLog(QStringLiteral("✘ 执行失败（耗时 %1ms）：%2").arg(timer.elapsed()).arg(error),
              true);
    if (message)
        *message = QStringLiteral("✘ 执行失败：%1").arg(error);
    return false;
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

void VulnPanel::setDeviceInfo(const DeviceInfo &info)
{
    m_deviceInfo = info;
    m_deviceLabel->setText(QStringLiteral("%1 · Android %2 · 补丁 %3")
        .arg(info.model.isEmpty() ? info.serialNumber : info.model)
        .arg(info.androidVersion.isEmpty() ? "?" : info.androidVersion)
        .arg(info.securityPatch.isEmpty() ? "未知" : info.securityPatch));

    m_rootLabel->setText(info.hasRoot()
        ? QStringLiteral("已Root")
        : QStringLiteral("未Root"));
    m_rootLabel->setStyleSheet(info.hasRoot()
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

void VulnPanel::setDevices(const QMap<QString, DeviceInfo> &devices)
{
    m_devices = devices;
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

    // Column 4: 打开该漏洞的专属界面（利用按钮移到详情窗口里）
    auto *btnWidget = new QWidget();
    auto *btnLayout = new QHBoxLayout(btnWidget);
    btnLayout->setContentsMargins(2, 2, 2, 2);
    auto *detailBtn = new QPushButton(QStringLiteral("详情"), btnWidget);
    detailBtn->setMaximumWidth(60);
    const QString cveId = entry.id;
    connect(detailBtn, &QPushButton::clicked, this, [this, cveId]() {
        openVulnDetail(cveId);
    });
    btnLayout->addWidget(detailBtn);
    btnLayout->addStretch();
    m_vulnTable->setCellWidget(row, 4, btnWidget);
}

// BROM 的固定条目：它不来自漏洞库、不参与 CVE 扫描，但必须和普通漏洞一样
// 出现在列表里，用户才能用同一套"选中 → 详情"的交互进入它。
void VulnPanel::updateBromRow(int row)
{
    if (row < 0)
        return;
    if (m_vulnTable->rowCount() <= row)
        m_vulnTable->setRowCount(row + 1);

    auto *iconItem = new QTableWidgetItem(QStringLiteral("⚙"));
    iconItem->setTextAlignment(Qt::AlignCenter);
    iconItem->setForeground(QColor(QStringLiteral("#8e44ad")));
    m_vulnTable->setItem(row, 0, iconItem);

    // 明确标注"非 CVE"：BROM 利用按芯片 hw_code 路由，不对应任何 CVE 编号。
    m_vulnTable->setItem(row, 1,
                         new QTableWidgetItem(QStringLiteral("MTK BROM（非 CVE）")));

    auto *descItem = new QTableWidgetItem(
        QStringLiteral("MTK BROM 漏洞利用（kamakiri）\n"
                       "芯片级利用，不参与 CVE 扫描；点「详情」进入专属界面"));
    descItem->setToolTip(QStringLiteral(
        "与 CVE 列表不同：BROM 利用按芯片 hw_code 查硬件地址表，"
        "需要用户自备载荷（本仓不分发载荷二进制）。"));
    m_vulnTable->setItem(row, 2, descItem);

    auto *stateItem = new QTableWidgetItem(QStringLiteral("独立入口"));
    stateItem->setForeground(QColor(QStringLiteral("#8e44ad")));
    m_vulnTable->setItem(row, 3, stateItem);

    auto *btnWidget = new QWidget();
    auto *btnLayout = new QHBoxLayout(btnWidget);
    btnLayout->setContentsMargins(2, 2, 2, 2);
    auto *detailBtn = new QPushButton(QStringLiteral("详情"), btnWidget);
    detailBtn->setMaximumWidth(60);
    connect(detailBtn, &QPushButton::clicked, this, [this]() {
        openVulnDetail(QString::fromLatin1(kInternalBromId));
    });
    btnLayout->addWidget(detailBtn);
    btnLayout->addStretch();
    m_vulnTable->setCellWidget(row, 4, btnWidget);
}

// ── Slots ──

void VulnPanel::onLoadDatabase()
{
    // 支持一次选多个库；每次都**合并**到现有库（按 id 去重），不是替换。
    const QStringList paths = QFileDialog::getOpenFileNames(this,
        QStringLiteral("选择漏洞数据库文件（可多选，会与已加载的库合并）"),
        QString(),
        QStringLiteral("漏洞数据库 (*.json);;所有文件 (*)"));
    if (paths.isEmpty())
        return;

    const int before = m_db->entryCount();
    int loadedFiles = 0;
    QStringList failedFiles;

    for (const QString &path : paths) {
        const QFileInfo fi(path);
        if (m_db->loadFromFile(path)) {
            ++loadedFiles;
            appendLog(QStringLiteral("加载漏洞数据库: %1").arg(fi.fileName()));
        } else {
            failedFiles << fi.fileName();
            appendLog(QStringLiteral("加载失败: %1（格式错误或无法读取）").arg(fi.fileName()),
                      true);
        }
    }

    rebuildTable();

    const int after = m_db->entryCount();
    appendLog(QStringLiteral("合并完成：成功 %1 个文件，新增 %2 条，当前共 %3 条")
                  .arg(loadedFiles)
                  .arg(after - before)
                  .arg(after));

    int c = 0, h = 0, m = 0, l = 0;
    for (const auto &e : m_currentEntries) {
        const QString s = e.severity.toUpper();
        if (s == "CRITICAL") ++c;
        else if (s == "HIGH") ++h;
        else if (s == "MEDIUM") ++m;
        else if (s == "LOW") ++l;
    }
    appendLog(QStringLiteral("当前库 | ★ CRITICAL: %1  ▲ HIGH: %2  ● MEDIUM: %3  ○ LOW: %4")
                  .arg(c).arg(h).arg(m).arg(l));

    if (!failedFiles.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("部分文件加载失败"),
            QStringLiteral("以下文件未能加载：\n%1")
                .arg(failedFiles.join(QLatin1Char('\n'))));
    }
}

void VulnPanel::rebuildTable()
{
    m_vulnTable->setRowCount(0);
    m_currentEntries = m_db->getAllEntries();

    for (const auto &e : m_currentEntries) {
        const int row = m_vulnTable->rowCount();
        m_vulnTable->insertRow(row);
        updateEntryInTable(row, e);
    }

    // BROM 固定条目：普通漏洞之后，永远在列表里。
    updateBromRow(m_vulnTable->rowCount());

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
        .arg(m_deviceInfo.hasRoot() ? " · 已Root" : ""));

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

    // 3. Refresh table（普通条目之后保持 BROM 固定条目在列表末尾）
    for (int row = 0; row < m_currentEntries.size(); ++row) {
        updateEntryInTable(row, m_currentEntries[row]);
    }
    updateBromRow(m_currentEntries.size());

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
    for (const auto &e : vulnEntries) {
        appendLog(QStringLiteral("  ── %1 [%2] CVSS %3 ──")
            .arg(e.id, e.severity).arg(e.cvssScore, 0, 'f', 1));
    }

    const qint64 startedAt = QDateTime::currentMSecsSinceEpoch();
    m_engine->resetCancel();

    // 整批提交到引擎线程执行。runFull 内部是同步 QProcess（单条最长可达
    // timeoutSeconds），放在 UI 线程会把窗口卡死。结果逐条 queued 回主线程。
    QMetaObject::invokeMethod(m_engine, [this, vulnEntries, startedAt]() {
        int successCount = 0;
        int failCount = 0;

        for (const auto &entry : vulnEntries) {
            const ExploitResult result = m_engine->runFull(entry);
            if (result.state == VulnEntry::EXPLOITED)
                ++successCount;
            else
                ++failCount;

            QMetaObject::invokeMethod(this, [this, entry, result]() {
                int idx = -1;
                for (int i = 0; i < m_currentEntries.size(); ++i) {
                    if (m_currentEntries[i].id == entry.id) {
                        idx = i;
                        break;
                    }
                }
                if (idx >= 0) {
                    m_currentEntries[idx].state = result.state;
                    updateEntryInTable(idx, m_currentEntries[idx]);
                }

                if (result.state == VulnEntry::EXPLOITED)
                    appendLog(QStringLiteral("  ✔ %1: 利用成功").arg(entry.id));
                else
                    appendLog(QStringLiteral("  ✘ %1: %2").arg(entry.id, result.error), true);
                appendLog(QString()); // separator

                if (m_detailDialog
                    && static_cast<VulnDetailDialog *>(m_detailDialog)->cveId() == entry.id) {
                    for (const auto &e : m_currentEntries) {
                        if (e.id == entry.id) {
                            static_cast<VulnDetailDialog *>(m_detailDialog)->updateEntry(e);
                            break;
                        }
                    }
                }
            }, Qt::QueuedConnection);
        }

        const qint64 elapsed = QDateTime::currentMSecsSinceEpoch() - startedAt;
        QMetaObject::invokeMethod(this, [this, successCount, failCount, elapsed]() {
            appendLog(QStringLiteral("全部完成 · %1 成功, %2 失败 · 耗时 %3ms")
                .arg(successCount).arg(failCount).arg(elapsed));
            m_isExploiting = false;
            setButtonsEnabled(true);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void VulnPanel::onExploitSingle(const QString &cveId)
{
    // W2-T5：单条利用 —— Tier3 诊断与安全研究（与批量利用同一档：闸门一次即两处都放行）
    if (!riskui::ensureAccepted(this, riskui::riskTierForFeature(riskui::RiskFeature::VulnExploit), m_consentStore)) return;
    if (m_deviceInfo.serialNumber.isEmpty()) {
        appendLog(QStringLiteral("请先连接设备"), true);
        return;
    }

    if (findRowByCveId(cveId) < 0)
        return;

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

    if (m_isExploiting)
        return;
    m_isExploiting = true;
    setButtonsEnabled(false);

    appendLog(QStringLiteral("  利用 %1  [%2] CVSS %3")
        .arg(cveId, entry.severity).arg(entry.cvssScore, 0, 'f', 1));

    m_engine->resetCancel();

    // 单条同样放到引擎线程：一条脚本跑满 timeoutSeconds 也不能冻结界面。
    QMetaObject::invokeMethod(m_engine, [this, entry, cveId]() {
        const ExploitResult result = m_engine->runFull(entry);

        QMetaObject::invokeMethod(this, [this, cveId, result]() {
            for (auto &e : m_currentEntries) {
                if (e.id == cveId) {
                    e.state = result.state;
                    break;
                }
            }

            // 异步期间表格可能被重排，回调里按 CVE 重新定位。
            const int row = findRowByCveId(cveId);
            if (row >= 0) {
                for (const auto &e : m_currentEntries) {
                    if (e.id == cveId) {
                        updateEntryInTable(row, e);
                        break;
                    }
                }
            }

            if (result.state == VulnEntry::EXPLOITED)
                appendLog(QStringLiteral("  ✔ %1: 利用成功").arg(cveId));
            else
                appendLog(QStringLiteral("  ✘ %1: %2").arg(cveId, result.error), true);

            // 详情窗口正开着同一个漏洞时，同步刷新它的状态与按钮。
            if (m_detailDialog
                && static_cast<VulnDetailDialog *>(m_detailDialog)->cveId() == cveId) {
                for (const auto &e : m_currentEntries) {
                    if (e.id == cveId) {
                        static_cast<VulnDetailDialog *>(m_detailDialog)->updateEntry(e);
                        break;
                    }
                }
            }

            m_isExploiting = false;
            setButtonsEnabled(true);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);
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
