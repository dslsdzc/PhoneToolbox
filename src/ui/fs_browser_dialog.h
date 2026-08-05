#ifndef FS_BROWSER_DIALOG_H
#define FS_BROWSER_DIALOG_H

#include <QDialog>
#include <QTreeWidget>
#include <QHash>
#include "image_engine/registry.h"
#include "image_worker.h"

class QLabel;
class QProgressBar;
class QPushButton;

// FsBrowserDialog：EROFS/ext4 文件系统浏览对话框（D5）。
//
// 线程模型：镜像数据与 FsImage 会话全部在 ImageWorker 工作线程 —— 本对话框
// 只做 UI：构造即 runFsOpen，fsOpened 回来填充目录树；打开/提取/替换/保存均
// 经 worker 投递，结果信号驱动 UI 更新，UI 线程不执行任何镜像引擎调用。
// 模态 exec() 期间 worker 信号由对话框事件循环正常派发（与 D4 RootPatchDialog
// 同款模式）。对话框析构时 runFsClose 收尾会话（worker 串行执行，在途操作
// 之后执行，安全）。
//
// 能力差异（后端核实见 task-D5-report.md，fs_image.cpp L34-94）：
//   • EROFS（ErofsImage）：只读 —— replace() 恒 false → 替换/保存按钮禁用；
//   • ext4（Ext4Image）：replace() 支持，但 metadata_csum 校验和镜像、legacy
//     block map、目录/符号链接由后端拒绝（返回 false；FsImage::replace 无
//     error 参数，无法上抛明细）→ UI 侧合并已知原因拼通用文案；
//   • 修改只在内存会话中生效，须「保存修改镜像」repack 后落盘。
//
// 对外信号 outputMessage 供面板日志复用（与各面板 appendLog 约定一致）。
class FsBrowserDialog : public QDialog
{
    Q_OBJECT

public:
    FsBrowserDialog(const QString &imagePath, imgreg::Format format,
                    ImageWorker *worker, QWidget *parent = nullptr);
    ~FsBrowserDialog() override;

    // 观察接口（冒烟/自动化用，不承担业务）：当前是否存在未保存的替换修改
    bool isModified() const { return m_modifiedCount > 0; }
    QTreeWidget *entryTree() const { return m_tree; }

signals:
    void outputMessage(const QString &msg, bool isError);

private slots:
    void onFsOpened(const QString &path, bool ok,
                    const QList<imgfs::FsEntry> &entries, const QString &error);
    void onFsExtracted(const QString &path, const QString &innerPath,
                       const QString &outFile, bool ok, const QString &error);
    void onFsReplaced(const QString &path, const QString &innerPath,
                      const QString &hostFile, bool ok, const QString &error);
    void onFsRepacked(const QString &path, const QString &outPath,
                      bool ok, const QString &error);

private:
    void setupUI();
    void buildTree(const QList<imgfs::FsEntry> &entries);
    QString currentInnerPath() const;
    void requestOpen(const QString &innerPath);
    void setBusy(bool busy);
    void setStatus(const QString &text, bool isError = false);
    void updateActionButtons();

    QString m_imagePath;
    bool m_canReplace = false;    // 仅 ext4（ErofsImage replace 恒 false）
    int m_modifiedCount = 0;      // 未保存的替换次数（>0 时启用保存按钮）
    bool m_opening = false;       // 本次提取为"外部打开"（成功即 openUrl）

    ImageWorker *m_worker = nullptr; // 面板的 worker（非本对象所有，不负责释放）

    QTreeWidget *m_tree = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_openBtn = nullptr;
    QPushButton *m_extractBtn = nullptr;
    QPushButton *m_replaceBtn = nullptr;
    QPushButton *m_saveBtn = nullptr;
    QHash<QString, QTreeWidgetItem *> m_items; // 文件 innerPath → 节点（刷新大小列）
};

#endif // FS_BROWSER_DIALOG_H
