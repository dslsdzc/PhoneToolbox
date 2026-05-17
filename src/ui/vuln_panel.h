#ifndef VULN_PANEL_H
#define VULN_PANEL_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTextEdit>
#include <QList>
#include "core/device_info.h"
#include "vuln_db/vuln_db.h"
#include "vuln_db/exploit_engine.h"

class VulnPanel : public QWidget
{
    Q_OBJECT

public:
    explicit VulnPanel(QWidget *parent = nullptr);
    ~VulnPanel();

    void setDeviceInfo(const DeviceInfo &info);
    void clearDeviceInfo();

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

private:
    void setupUI();
    void updateEntryInTable(int row, const VulnEntry &entry);
    int  findRowByCveId(const QString &cveId) const;
    void appendLog(const QString &msg, bool isError = false);
    void setButtonsEnabled(bool enabled);
    void updateButtonStates();
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

    // Data
    VulnDb *m_db;
    ExploitEngine *m_engine;
    DeviceInfo m_deviceInfo;
    QList<VulnEntry> m_currentEntries;
    bool m_isScanning = false;
    bool m_isExploiting = false;
};

#endif // VULN_PANEL_H
