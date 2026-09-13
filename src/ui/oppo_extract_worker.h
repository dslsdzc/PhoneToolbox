// src/ui/oppo_extract_worker.h
//
// PB-B6：OPPO 整包（.ofp/.ops）解包的工作线程封装。把 imgopp::extractOFP/extractOPS 从
// GUI 线程搬到专用线程，进度经 queued 信号回 GUI 线程 —— 进度条真实推进、界面保持响应，
// 取代"GUI 线程同步跑 + 手动 processEvents 泵"（GB 级包那样会让界面全程不可交互）。
//
// 线程模型（与 ImageWorker 同款，理由同 `image_worker.h` 类注释）：worker 对象 +
// moveToThread()，操作用 QMetaObject::invokeMethod 以 QueuedConnection 投递到工作线程。
// 本仓既有的后台执行设施一律是这条路径（QThread 对象 + 工作对象），QtConcurrent 面向
// "一次性函数调用"、要额外引入 Qt6::Concurrent 依赖，且拿不到"操作串行排队"这一保证。
// 与 ImageWorker 的分工：本类只管**整包解包这一趟**（不做队列复用、不碰面板状态），
// 故不塞进 ImageWorker 的公开 API（那会牵动所有面板与 image_worker 的测试）。
//
// **取消语义（诚实口径）**：解包引擎没有"半途停"的钩子，取消是**条目（文件）边界**生效的
// 询问（见 imgopp::ExtractCancel）—— 点了取消之后，当前文件仍会写完并校验完，然后停止。
// 故 UI 的按钮文案必须写清"当前文件完成后生效"，不得暗示立即停止；已写产物保留
// （与"校验失败不回滚"同一契约），由调用方决定是否清理输出目录。
//
// 一次性使用：一次 start → 一次 finished（重复 start 无意义，未做排队）。所有结果字段
// 由工作线程写、调用方在 waitForFinished()（join）之后读，故无需加锁。
#ifndef OPPO_EXTRACT_WORKER_H
#define OPPO_EXTRACT_WORKER_H

#include <QObject>
#include <QString>
#include <QThread>

#include <atomic>

class OppoExtractWorker : public QObject
{
    Q_OBJECT

public:
    explicit OppoExtractWorker(QObject *parent = nullptr);
    ~OppoExtractWorker() override;

    // 启动解包（任意线程可调；.ops → extractOPS，其余按 .ofp → extractOFP，扩展名由调用方
    // 先行校验）。outDir 不存在时由引擎创建。
    void start(const QString &packagePath, const QString &outDir);

    // 请求取消（**线程安全**，任意线程可调）。请求只是"请求"：引擎在下一个条目边界读到它
    // 才停止，见类注释的取消语义。
    void requestCancel();

    // 阻塞等待工作线程结束（join）。**读 ok()/error()/cancelRequested() 之前必须调用**
    // （信号 finished 早于线程真正退出，此时结果字段的写入尚不保证对 GUI 线程可见）。
    void waitForFinished();

    // 工作线程已跑完（原子；用于"finished 信号可能早于事件循环 exec() 到达"的兜底判断）
    bool isFinished() const { return m_finished.load(); }
    // 引擎返回值：true = 全部可提取条目已写出；false = 失败**或**被取消
    //（ok()==false 时用 cancelRequested() 区分二者）
    bool ok() const { return m_ok; }
    // 用户是否请求过取消（请求过即视为"用户要停"：即使引擎恰好已跑完，调用方也不应再使用产物）
    bool cancelRequested() const { return m_cancelRequested.load(); }
    // 引擎文案：失败原因；成功但有条目被跳过时非空（= 部分成功警告，不得丢弃）；
    // 被取消时为 `用户取消：已完成 N/M 个文件（当前文件已写完并校验完，已写产物未回滚）`
    QString error() const { return m_error; }

signals:
    // 每完成一个文件发一次（name 为文件名，sparse 条目带"（sparse 镜像，原样输出）"标注）
    void progress(const QString &name, int percent);
    // 工作线程结束（成功/失败/取消都发一次）；经 queued 连接投递到接收者线程
    void finished();

private:
    // 实际执行体（运行于工作线程）
    void doExtract(const QString &packagePath, const QString &outDir);

    QThread m_thread;                             // 专用工作线程（本对象已 moveToThread）
    std::atomic_bool m_cancelRequested{false};    // 请求取消（GUI 线程写、工作线程读）
    std::atomic_bool m_finished{false};           // 结果已就绪（工作线程写、GUI 线程读）
    bool m_ok = false;                            // 仅工作线程写、join 后读
    QString m_error;                              // 同上
};

#endif // OPPO_EXTRACT_WORKER_H
