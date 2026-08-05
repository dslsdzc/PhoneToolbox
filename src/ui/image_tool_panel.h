#ifndef IMAGE_TOOL_PANEL_H
#define IMAGE_TOOL_PANEL_H

#include <QWidget>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QTextEdit>
#include "image_engine/registry.h"
#include "image_worker.h"

// ImageToolPanel：镜像工具面板（左侧第 5 个工具，QStackedWidget index 4）。
//
// D2 范围：面板骨架 + 拖放识别。整个面板 setAcceptDrops(true)：拖入文件即
// 调 ImageWorker::runDetect（工作线程执行，UI 线程不阻塞）；detectFinished
// 信号回来更新顶部信息卡（格式/详情）并按 detected.format 动态启用动作按钮。
// D3：解包/转换已接线（解包弹目录选择框、转换弹目标类型菜单 + 输出文件框）。
// D4：修补已接线（修补按钮 → RootPatchDialog 方案选择对话框 → worker.runPatch
// → patcher::patchFile：自动备份 <源>.orig.bak + 产物 <基名>_patched.img）。
// D6：打包已接线（打包按钮 → 目标格式菜单 →（tar 系）输入集合多选 → 输出文件
// 对话框 → worker.runPack；按钮按后端可用打包接口 enable，Payload 无打包接口不
// enable，详见 image_worker.cpp doPack 顶部核实记录）。
class ImageToolPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ImageToolPanel(QWidget *parent = nullptr);
    ~ImageToolPanel() override;

signals:
    void outputMessage(const QString &msg, bool isError);
    void switchToDeviceInfo();

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private slots:
    void onBrowseClicked();
    void onDetectFinished(const QString &path, const imgreg::Detected &detected);
    void onWorkerProgress(int percent, const QString &stage);
    void onUnpackClicked();
    void onPackClicked();
    void onConvertClicked();
    void onPatchClicked();
    void onFsBrowseClicked();
    void onUnpackFinished(bool ok, const QStringList &outputs, const QString &error);
    void onPackFinished(bool ok, const QString &output, const QString &error);
    void onConvertFinished(bool ok, const QString &output, const QString &error);
    void onPatchFinished(bool ok, const QString &output, const QString &error);

private:
    void setupUI();
    void setupConnections();
    void startDetect(const QString &path);
    void updateButtonsFor(const imgreg::Detected &detected);
    void appendLog(const QString &msg, bool isError = false);

    // 顶部信息卡
    QLabel *m_fileLabel;
    QLabel *m_formatLabel;
    QLabel *m_detailLabel;
    QPushButton *m_backBtn;

    // 动作按钮组
    QPushButton *m_unpackBtn;
    QPushButton *m_packBtn;
    QPushButton *m_convertBtn;
    QPushButton *m_patchBtn;
    QPushButton *m_fsBrowseBtn;  // D5 文件系统浏览（EROFS/ext4 才 enable）
    QPushButton *m_browseBtn;    // 文件选择（始终可用）

    // 进度条 + 日志
    QProgressBar *m_progressBar;
    QTextEdit *m_logOutput;

    // 数据（值成员：ImageWorker 自带工作线程，构造即启动，见 image_worker.h）
    ImageWorker m_worker;
    QString m_currentFile;           // 当前镜像路径
    imgreg::Detected m_detected;     // 最近一次识别结果
};

#endif // IMAGE_TOOL_PANEL_H
