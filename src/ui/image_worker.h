#ifndef IMAGE_WORKER_H
#define IMAGE_WORKER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QThread>
#include "image_engine/registry.h"
#include "root_patcher/root_patcher.h"

// 跨线程（queued）信号连接要求参数类型可被元类型系统识别：
// Qt 6 中 Q_DECLARE_METATYPE 生成的 qt_metatype_id() 首次使用即自动注册，
// 构造里再显式 qRegisterMetaType（string 形式连接也依赖运行时注册）。
Q_DECLARE_METATYPE(imgreg::Detected)
Q_DECLARE_METATYPE(patcher::PatchConfig)

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

signals:
    void detectFinished(const QString &path, imgreg::Detected detected);
    void unpackFinished(bool ok, const QStringList &outputs, const QString &error);
    void packFinished(bool ok, const QString &output, const QString &error);
    void convertFinished(bool ok, const QString &output, const QString &error);
    void patchFinished(bool ok, const QString &output, const QString &error);
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

    QThread m_thread; // 专用工作线程（本对象已 moveToThread，线程事件循环驱动串行执行）
};

#endif // IMAGE_WORKER_H
