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
//
// H1 降级策略（构造里接 ResourceMonitor::cpuHigh）：整体 CPU >80% 时本工作线程降为
// **LowPriority**，恢复（<70%）回 NormalPriority。**不用 IdlePriority**：实测（Linux/Qt 6.11）
// 它映射到 SCHED_IDLE 调度类，持续高负载下工作线程被饿死（10 秒级停滞实测）—— 降级的
// 取舍是"让位而非停摆"，理由与实测数据见 image_worker.cpp 构造内的注释。
//
// 析构：quit()+wait() 等待工作线程退出后释放，防悬挂/泄漏。
class ImageWorker : public QObject
{
    Q_OBJECT

public:
    explicit ImageWorker(QObject *parent = nullptr);
    ~ImageWorker() override;

    // ---- 公开入口（任意线程调用安全；实际工作在工作线程串行执行）----

    // G5 流式路径判定（供 UI 日志与冒烟测试复用）：输入大小 > kStreamThreshold
    // 时走流式接口（内存 O(chunk)，字节级进度回调）；小文件保持旧内存接口。
    // 文件不存在/不可读返回 false（走旧接口 → 读取失败错误，不崩溃）。
    static bool useStreamPath(const QString &path);
    // 格式感知判定：大小达标且该格式存在流式接口才返回 true（Super/Boot/Dat/
    // Kdz/UpdateApp/Sin/Pac/DiskGpt/TwrpWin 等无流式接口 → 恒 false，worker
    // 内分派回退旧路径）。日志文案用（避免"流式处理"标注与实际路径不符）。
    static bool useStreamPath(const QString &path, imgreg::Format format);
    // 流式阈值：64MB。该量级下内存路径峰值 ≈ 2×镜像大小（读入 + 产物），
    // 流式路径几乎不占额外内存（G4 基准：~14MB）。
    static constexpr qint64 kStreamThreshold = 64LL * 1024 * 1024;

    // 识别镜像格式（读文件头魔数 + 扩展名兜底，见 imgreg::detect；OFP/OPS
    // 无头魔数 → 追加末 0x1000 尾页二次探测，顺序先 OPS 后 OFP，见 .cpp）。
    // 完成时发 detectFinished；读取失败时 detail 以"读取失败"开头。
    void runDetect(const QString &path);

    // 解包镜像到 outDir（按 detected 格式分派到对应引擎；D3 实现，
    // OFP/OPS 见 Phase A 接线）。
    void runUnpack(const QString &path, const QString &outDir,
                   const imgreg::Detected &detected);

    // 打包（D6 实现，按 detected.format 分派）：Sparse→Raw（simg2img 逆向）、
    // RawImage→Sparse（img2simg）、Tar/TarMd5→.img 集合打成 tar/tar.md5
    // （目标格式按 outPath 后缀 ".md5" 判定 → appendMd5Footer）。
    // 无打包接口的格式（Payload/UpdateApp 等）防御性返回错误（按钮已禁用）。
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
    // ok=false → error 为致命错误文案；ok=true 且 error 非空 → 部分成功警告
    //（A10：部分条目被跳过，调用方必须把文案落日志，不得丢弃）
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
    // percent: 0=开始, 100=完成；stage: 阶段名（识别中/解包中/打包中/转换中/
    // 修补中/文件浏览中）。
    // G5 细化：大文件（> kStreamThreshold）走流式接口，引擎逐 chunk/块回调
    // 字节进度 → 按"已处理/总量"百分比发射（任务中途封顶 99，100 仅在完成/
    // 失败路径发射，防面板提前隐藏进度条）；小文件保持"开始/结束"两档。
    void progress(int percent, const QString &stage);

private:
    // 实际执行体（运行于工作线程，经 QueuedConnection 投递，串行排队）
    void doDetect(const QString &path);
    void doUnpack(const QString &path, const QString &outDir,
                  const imgreg::Detected &detected);
    // doPack 分派（打包接口核实记录见 image_worker.cpp doPack 顶部注释）
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
