#ifndef FLASH_PANEL_H
#define FLASH_PANEL_H

#include <QWidget>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
#include <QTemporaryDir>          // 整包入口（.ofp/.ops）的解包临时目录须活过刷写，按值持有
#include <functional>
#include <memory>
#include "core/device_detector.h"
#include "core/flash_tool.h"

class FlashPanel : public QWidget
{
    Q_OBJECT

public:
    explicit FlashPanel(QWidget *parent = nullptr);

    void setDeviceInfo(const DeviceInfo &info);
    void clearDeviceInfo();
    void updateFileInfo(const QString &filePath);

signals:
    void outputMessage(const QString &msg, bool isError);
    void switchToDeviceInfo();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void onSelectFile();
    void onFlashClicked();
    void onEraseClicked();
    void onDumpClicked();
    void onBootImageClicked();
    void onUpdateClicked();
    void onSideloadClicked();
    void onRunScriptClicked();
    void onPartitionSelectionChanged();
    void onRefreshPartitions();
    void onToolOutput(const QString &msg, bool isError);
    void onToolProgress(int percent);

    // EDL 模式
    void onEdlSelectProgrammer();
    void onEdlConnect();
    void onEdlDisconnect();
    // EDL 刷写计划（Phase B Task 8）：目录/整包 → 计划预览 → flashFullPackage("oppo-edl")
    void onEdlPlanFlash();

    // MTK 模式
    void onMtkConnect();
    void onMtkDisconnect();

    // Bootloader 解锁/回锁
    void onUnlockBootloader();
    void onLockBootloader();

    // FRP 清除
    void onFrpErase();

    // 死砖修复
    void onBrickRepairClicked();

    // 连接管理
    bool ensureEDLConnected();
    bool ensureMTKConnected();

    // W8-T5：preloader 安全修补的 **Tier 3 二次确认入口** —— **闸门即本函数全部内容**。
    // 提问留在 `onFlashClicked` 的 MTK 分支里（与同分支其它提问同款；用户答"是"才调本函数）。
    // 之所以单独成一个函数：接线钉按「每个入口一个闸门 + **闸门必须是函数体第一件事**」判
    // （`tests/test_risk_gate_wiring.cpp`）—— 把闸门内联进 onFlashClicked 会让那个入口出现两个闸门，
    // 而"多一处"正是那条钉要抓的信号。
    // **不把提问也放进本函数**：那样闸门就跑到提问之后了（第 17 行），违反"闸门第一件事"——
    // 而现在需要它的理由只是"提问不是动作"，不值得为迁就规则去把 UX 顺序倒过来
    //（先让用户确认一件他还没选择要做的事）。
    bool confirmPreloaderPatch();

    // 固件验证
    QString validateFirmwareDirectory(const QString &dir);

    // 分区匹配
    QStringList matchPartitionFiles(const QString &dir, const QList<EDLPartition> &parts);
    QStringList matchPartitionFilesMtk(const QString &dir, const QList<MtkPartition> &parts);

    // 写入
    bool writePartitionWithRetry(const QString &partName,
                                 std::function<bool()> writeFn, int maxRetries = 2);

    // 备份
    QStringList backupCriticalPartitions(const QString &backupDir);

    // 断点续传
    void saveProgressFile(const QString &dir, int completedIndex);
    int loadProgressFile(const QString &dir);

private:
    void setupUI();
    bool checkBootloaderUnlock(const QString &deviceId);

    // 状态
    QLabel *m_deviceLabel;
    QLabel *m_modeLabel;

    // 文件
    QPushButton *m_selectFileBtn;
    QLabel *m_fileLabel;
    QLabel *m_romInfoLabel;
    QLabel *m_romSuggestionLabel;
    QString m_currentFile;

    // 分区
    QListWidget *m_partitionList;
    QPushButton *m_refreshPartitionsBtn;

    // EDL 模式
    QPushButton *m_edlSelectProgBtn;
    QPushButton *m_edlConnectBtn;
    QPushButton *m_edlDisconnectBtn;
    QPushButton *m_edlPlanBtn;
    QLabel *m_edlStatusLabel;
    QString m_programmerPath;
    // 整包入口（.ofp/.ops）的解包临时目录：解包产物必须活到本次刷写结束（否则刷写读不到镜像）
    std::unique_ptr<QTemporaryDir> m_edlPlanTempDir;

    // MTK 模式
    QPushButton *m_mtkConnectBtn;
    QPushButton *m_mtkDisconnectBtn;
    QLabel *m_mtkStatusLabel;

    // 操作
    QPushButton *m_flashBtn;
    QPushButton *m_eraseBtn;
    QPushButton *m_dumpBtn;
    QPushButton *m_updateBtn;
    QPushButton *m_sideloadBtn;
    QPushButton *m_runScriptBtn;
    QPushButton *m_bootImgBtn;
    QPushButton *m_unlockBtn;
    QPushButton *m_lockBtn;
    QPushButton *m_frpBtn;
    QPushButton *m_brickRepairBtn;
    QPushButton *m_backBtn;

    // 进度
    QProgressBar *m_progressBar;

    // 数据
    DeviceInfo m_deviceInfo;
    FlashTool *m_flashTool;
    QStringList m_partitions;
    int m_lastCompletedIndex = -1;
};

#endif // FLASH_PANEL_H
