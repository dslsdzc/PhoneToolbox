// src/ui/da_export_dialog.h
//
// DA 切分另存的 UI 入口（W8-T6）—— T4 交付的 `writeExtractedDAs` 在此第一次有了调用点。
//
// 为什么是**独立对话框 + 显式输入**（控制方 2026-09-21 二次订正的口径）：
//   * DA 的解析发生在刷写链**内部**（`bromFlashOnSession`），FlashPanel 那条流程**拿不到 dacode**；
//   * 而 `selectDaEntry` 需要 dacode（= 芯片表里 hw_code 对应的那一列）。设备信息面板能显示
//     `hw_code`（设备报的），于是本功能**不依赖连接设备**：由用户显式给 DA 文件 + 芯片码两个输入即可；
//   * **不提供"按序号挑条目"**那种入口：`DaSelection` 的注释已警告按 `entryRegionIndex` 取会把
//     region[0]（EMI/env）当 DA1 —— 入口形态上就不给这个把手。
//
// 门控（W2 的界线）：本对话框是**本地只读**操作 —— 读一个已解析的 DA 文件、写两个文件，
// **不碰设备、不削弱任何安全机制** ⇒ **不纳入 tier3 门控**（那里弹的是"改设备状态"的动作）。
// 同样地，它不写任何设备状态，故也不进 FlashPanel 的复位表（不需要设备就能用）。
//
// 控件用 objectName 暴露给用例（仓内既有对话框同款）：daFilePathEdit / daHwCodeEdit /
// daOutDirEdit / daExportButton / daStatusLabel。
#pragma once

#include <QDialog>
#include <QString>

class QLabel;
class QLineEdit;
class QPushButton;

class DaExportDialog : public QDialog
{
    Q_OBJECT

public:
    explicit DaExportDialog(QWidget *parent = nullptr);

    // —— 用例注入（不经任何文件对话框/模态）——
    void setDaFilePath(const QString &path);
    void setHwCodeText(const QString &text);
    void setOutputDir(const QString &dir);

    // 等价于点「导出」：读 DA → 按芯片码查 dacode → selectDaEntry → writeExtractedDAs。
    // 成功 true（两个 da1.bin/da2.bin 已落盘，字节数写进状态行）；失败 false + *error（中文）。
    // 两种入口（按钮 / 用例）走的是**同一条**代码路径 —— 否则用例覆盖的不是用户点的那条。
    bool exportNow(QString *error = nullptr);

private slots:
    void onBrowseDaFile();
    void onBrowseOutDir();
    void onExportClicked();

private:
    void setStatus(const QString &text, bool isError);

    QLineEdit   *m_daPathEdit  = nullptr;
    QLineEdit   *m_hwCodeEdit  = nullptr;
    QLineEdit   *m_outDirEdit  = nullptr;
    QPushButton *m_exportBtn   = nullptr;
    QLabel      *m_statusLabel = nullptr;
};
