#ifndef IMAGE_WORKER_H
#define IMAGE_WORKER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include <memory>
#include "image_engine/registry.h"
#include "image_engine/fs/fs_image.h"
#include "root_patcher/root_patcher.h"

// 跨线程（queued）信号连接要求参数类型可被元类型系统识别：
// Qt 6 中 Q_DECLARE_METATYPE 生成的 qt_metatype_id() 首次使用即自动注册，
// 构造里再显式 qRegisterMetaType（string 形式连接也依赖运行时注册）。
Q_DECLARE_METATYPE(imgreg::Detected)
Q_DECLARE_METATYPE(patcher::PatchConfig)
// D5 文件系统浏览：FsEntry（listTree 不填充 data，跨线程投递开销小）+ 整树列表
Q_DECLARE_METATYPE(imgfs::FsEntry)
Q_DECLARE_METATYPE(QList<imgfs::FsEntry>)

// ImageWorker：镜像处理后台执行封装（QThread + moveToThread 工作对象模式）。
//
// 线程模型（对照 Qt 官方文档推荐）：需要槽在工作线程执行的场景，官方明确
// 要求使用 worker 对象 + moveToThread()（"a developer who wishes to invoke
// slots in the new thread must use the worker-object approach"）；QtConcurrent
// 适用于一次性函数调用，无法天然保证多操作串行排队。本类采用 worker 对象
// 迁移到专用 QThread：runXxx 公开入口经 QMetaObject::invokeMethod 以
// Qt::QueuedConnection 把操作投递到工作线程事件队列 —— 所有操作自动排队
// 串行执行，绝不阻塞 UI 线程。
//
// 结果/进度经信号回传（跨线程 queued 投递到 UI 线程，接收方需处于事件循环）。
// 全局契约：任何失败都不崩溃，以 ok=false + error 上报（识别失败编码在
// Detected 内：format==Unknown 且 detail 以"读取失败"开头）。
// 析构：quit()+wait() 等待工作线程退出后释放，防悬挂/泄漏。
class ImageWorker : public QObject
{
    Q_OBJECT

public:
    explicit ImageWorker(QObject *parent = nullptr);
    ~ImageWorker() override;

    // ---- 公开入口（任意线程调用安全；实际工作在工作线程串行执行）----

    // 识别镜像格式（读文件头魔数 + 扩展名兜底，见 imgreg::detect）。
    // 完成时发 detectFinished；读取失败时 detail 以"读取失败"开头。
    void runDetect(const QString &path);

    // 解包镜像到 outDir（按 detected 格式分派到对应引擎；D3 实现）
    void runUnpack(const QString &path, const QString &outDir,
                   const imgreg::Detected &detected);

    // 打包（D6 实现：sparse→raw / .img 集合→tar / payload 全量等）
    void runPack(const QString &outPath, const QStringList &inputs,
                 const imgreg::Detected &detected);

    // 格式转换（D3 实现：sparse↔raw 等）
    void runConvert(const QString &path, const QString &outPath,
                    const imgreg::Detected &detected);

    // Boot 镜像 Root 修补（D4 实现：调 patcher::patchFile）
    void runPatch(const QString &path, const patcher::PatchConfig &config);

    // ---- 文件系统浏览（D5）：会话型操作 ----
    // FsImage 为全内存会话（openFsImage 持镜像副本），生命周期绑定本 worker：
    // runFsOpen 打开新会话（自动替换上一会话），runFsClose 关闭；list/extract/
    // replace/repack 均以打开时的 path 为会话 key，不匹配即失败（防跨镜像串用）。
    // FsImage 接口无 error 参数（list/extract/replace 仅 bool，见 fs_image.h）：
    // 失败原因由本层按已知后端语义拼装通用文案（后端扩展待办：错误上抛）。
    // 大镜像（>1GiB）由 UI 层先提示（FsImage 全内存操作），本层按契约继续。
    void runFsOpen(const QString &path);
    // 提取镜像内文件：outPath 为空 → 写系统临时目录（供外部打开），否则写指定路径
    void runFsExtractTo(const QString &path, const QString &innerPath,
                        const QString &outPath);
    void runFsReplace(const QString &path, const QString &innerPath,
                      const QString &hostFile);
    void runFsRepack(const QString &path, const QString &outPath);
    void runFsClose(const QString &path);

signals:
    void detectFinished(const QString &path, imgreg::Detected detected);
    void unpackFinished(bool ok, const QStringList &outputs, const QString &error);
    void packFinished(bool ok, const QString &output, const QString &error);
    void convertFinished(bool ok, const QString &output, const QString &error);
    void patchFinished(bool ok, const QString &output, const QString &error);
    // D5 文件系统浏览结果（fsOpened 携带整树，FsEntry.data 不填充）
    void fsOpened(const QString &path, bool ok,
                  const QList<imgfs::FsEntry> &entries, const QString &error);
    void fsExtracted(const QString &path, const QString &innerPath,
                     const QString &outFile, bool ok, const QString &error);
    void fsReplaced(const QString &path, const QString &innerPath,
                    const QString &hostFile, bool ok, const QString &error);
    void fsRepacked(const QString &path, const QString &outPath,
                    bool ok, const QString &error);
    // percent: 0=开始, 100=完成；stage: 阶段名（识别/解包/打包/转换/修补）。
    // 引擎暂不支持逐块回调，先按"开始/结束"两档发射。
    void progress(int percent, const QString &stage);

private:
    // 实际执行体（运行于工作线程，经 QueuedConnection 投递，串行排队）
    void doDetect(const QString &path);
    void doUnpack(const QString &path, const QString &outDir,
                  const imgreg::Detected &detected);
    void doPack(const QString &outPath, const QStringList &inputs,
                const imgreg::Detected &detected);
    void doConvert(const QString &path, const QString &outPath,
                   const imgreg::Detected &detected);
    void doPatch(const QString &path, const patcher::PatchConfig &config);

    // ---- D5 文件系统浏览会话 ----
    void doFsOpen(const QString &path);
    void doFsExtractTo(const QString &path, const QString &innerPath,
                       const QString &outPath);
    void doFsReplace(const QString &path, const QString &innerPath,
                     const QString &hostFile);
    void doFsRepack(const QString &path, const QString &outPath);
    void doFsClose(const QString &path);
    imgfs::FsImage *session(const QString &path) const; // 会话校验（失效返回 nullptr）
    void closeSession();

    QThread m_thread; // 专用工作线程（本对象已 moveToThread，线程事件循环驱动串行执行）

    // 当前文件系统会话（仅工作线程访问：run 方法串行排队，无数据竞争）
    std::unique_ptr<imgfs::FsImage> m_fsSession;
    QString m_fsSessionPath;
};

#endif // IMAGE_WORKER_H
