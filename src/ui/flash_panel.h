#ifndef FLASH_PANEL_H
#define FLASH_PANEL_H

#include <QWidget>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
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
    QLabel *m_edlStatusLabel;
    QString m_programmerPath;

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
};

#endif // FLASH_PANEL_H
