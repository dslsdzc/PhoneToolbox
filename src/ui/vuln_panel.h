#ifndef VULN_PANEL_H
#define VULN_PANEL_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include <QList>
#include <functional>

#include "core/device_info.h"
#include "core/modes/mtk_brom_exploit.h"   // W8-T6：载荷来源选项 / 下载器注入点
#include "ui/brom_exploit_ui.h"            // PayloadState（载荷就绪三态）
#include "vuln_db/vuln_db.h"
#include "vuln_db/exploit_engine.h"

class QCheckBox;
class QLineEdit;

namespace legal { class ConsentStore; }

class VulnPanel : public QWidget
{
    Q_OBJECT

public:
    explicit VulnPanel(QWidget *parent = nullptr);
    ~VulnPanel();

    void setDeviceInfo(const DeviceInfo &info);
    void clearDeviceInfo();

    // ==================== W8-T6：MTK BROM 漏洞利用区 ====================
    //
    // 本区的三个注入点（样式同 `ExploitDownloader` / `PreloaderDownloader`：生产实现在别处，
    // 用例注入假的）：用例全程**不碰 USB、不联网、不写用户配置**。
    //
    //   * 执行：生产 = 枚举 USB → 打开 BROM 会话 → **校设备自报芯片码** → `fixExploitPayload`
    //     → `runBromExploit`；用例 = 记录调用参数的 lambda。
    //   * 载荷选项：生产默认在 .cpp（清单路径 = 用户配置优先/否则内置副本；**缓存目录留空** ——
    //     载荷只留在内存，与本仓的 preloader 获取同款口径）；注入时**整体替换**（不合并 ——
    //     避免"用例的临时清单 + 生产缓存目录"这种混合态把字节写进用户缓存）。
    //     ⚠️ 其中 `explicitPath` / `allowNetwork` 两项每次解析都由**面板控件**覆盖（用户当场的选择
    //     比注入的默认值优先），注入它们没有意义。
    //   * 下载器：生产 = `makeQtExploitDownloader()`（Qt Network，单独文件）；用例 = 假下载器。
    struct BromRunRequest {
        quint16    hwCode = 0;                   // 面板输入的芯片码（执行前会与设备自报值核对）
        QByteArray payload;                      // 已解析（含 sha256 结论）的载荷字节
        bool       allowPatchedGeneration = false; // 已修补代际（V6）硬闯（默认关 = fail-closed）
    };
    using BromRunner = std::function<bool(const BromRunRequest &req, QStringList *log, QString *error)>;

    void setBromRunner(const BromRunner &runner);
    void setBromDownloader(const mtkbrom::ExploitDownloader &downloader);
    void setBromPayloadOptions(const mtkbrom::ExploitPayloadOptions &opt);
    // 留痕 store 注入（四个闸门共用）：生产留 nullptr（`ensureAccepted` 自己构造默认 store，
    // 落用户配置）；用例注入临时 ini 的 store —— 既不碰用户配置，也能让"已接受该档"的路径
    // 不需要驱动模态窗就走通（否则执行路径在测试里根本到不了）。
    void setConsentStore(legal::ConsentStore *store);

signals:
    void outputMessage(const QString &msg, bool isError);
    void switchToDeviceInfo();

private slots:
    void onLoadDatabase();
    void onScan();
    void onExploitAll();
    void onExploitSingle(const QString &cveId);
    void onEngineOutput(const QString &line);
    void onProgressChanged(const QString &entryId, ExploitPhase phase);
    // W8-T6：MTK BROM 漏洞利用区的三个入口（解析载荷**不过闸门**：它只读文件/下载，
    // 不碰设备；「执行利用」过 Tier 3 —— 见 .cpp 的说明）
    void onBromResolvePayload();
    void onBromExploitRun();
    void onBromChipCodeChanged();

private:
    void setupUI();
    QWidget *buildBromExploitGroup();      // W8-T6：本区控件的唯一构建处（setupUI 调它）
    void updateEntryInTable(int row, const VulnEntry &entry);
    int  findRowByCveId(const QString &cveId) const;
    void appendLog(const QString &msg, bool isError = false);
    void setButtonsEnabled(bool enabled);
    void updateButtonStates();
    // W8-T6：按 bromui::evaluateRunGate 重算「执行利用」的启用态与原因文案。
    // 只要影响判据的东西变了（设备/芯片码/载荷状态）就必须调它 —— 三条禁用原因由
    // tests/test_brom_exploit_ui.cpp 逐条钉住。
    void refreshBromGate();
    void setBromPayloadState(bromui::PayloadState state, const QString &errorText);
    QString stateIcon(VulnEntry::State state) const;
    QString severityIcon(const QString &severity) const;

    // Header
    QLabel *m_deviceLabel;
    QLabel *m_rootLabel;
    QLabel *m_riskLabel;
    QPushButton *m_backBtn;

    // Actions
    QPushButton *m_loadDbBtn;
    QPushButton *m_scanBtn;
    QPushButton *m_exploitAllBtn;

    // Vulnerability table
    QTableWidget *m_vulnTable;

    // Log
    QTextEdit *m_logOutput;

    // ── W8-T6：MTK BROM 漏洞利用区 ──
    QLineEdit   *m_bromHwCodeEdit = nullptr;
    QLabel      *m_bromChipStatus = nullptr;
    QLineEdit   *m_bromPayloadPathEdit = nullptr;
    QCheckBox   *m_bromAllowNetworkCheck = nullptr;
    QCheckBox   *m_bromAllowPatchedCheck = nullptr;
    QPushButton *m_bromResolveBtn = nullptr;
    QLabel      *m_bromPayloadStatus = nullptr;
    QPushButton *m_bromRunBtn = nullptr;
    QLabel      *m_bromRunReason = nullptr;
    QByteArray   m_bromPayload;                          // 已解析的载荷（仅 Verified 态有值）
    bromui::PayloadState m_bromPayloadState = bromui::PayloadState::None;
    QString      m_bromPayloadError;                     // Rejected 态的原因
    mtkbrom::ExploitPayloadOptions m_bromPayloadOptions; // 默认值在 .cpp（含清单/缓存路径）
    mtkbrom::ExploitDownloader m_bromDownloader;         // 空 → 用 makeQtExploitDownloader()
    BromRunner   m_bromRunner;                           // 空 → 用内置的生产执行器
    legal::ConsentStore *m_consentStore = nullptr;       // 空 → 闸门用生产默认 store（用户配置）

    // Data
    VulnDb *m_db;
    ExploitEngine *m_engine;
    DeviceInfo m_deviceInfo;
    QList<VulnEntry> m_currentEntries;
    bool m_isScanning = false;
    bool m_isExploiting = false;
};

#endif // VULN_PANEL_H
