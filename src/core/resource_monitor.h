#ifndef RESOURCE_MONITOR_H
#define RESOURCE_MONITOR_H

#include <QObject>
#include <QTimer>
#include <QByteArray>
#include <QElapsedTimer>

// 任务 H1：**本进程** CPU 使用率监控与自动降级
// （用户约束：程序整体 CPU 使用率超过 80% 必须自动降级，恢复后自动回升）。
//
// 指标口径（重要）：这里量的是 PhoneToolbox 自身，不是整机。
//   Linux   = /proc/self/stat 的 utime+stime 增量 / 实际墙钟时间
//   Windows = GetProcessTimes 的 user+kernel 增量 / 实际墙钟时间
//   单核满载 = 100%；多线程可超过 100%（不钳到 100，否则多核场景会被误判成"没超"）。
//
// 旧实现读的是 /proc/stat（整机）：16 核机器上本程序占满一个核只有约 6%，
// 永远不会触发降级；反过来别的程序跑满也会让本程序无谓降级。两头判断都是错的。
//
// 单例：监听模块（DeviceDetector / SystemToolPanel / ImageWorker / 自身）通过
//   connect(&ResourceMonitor::instance(), &ResourceMonitor::cpuHigh, ...)
//   订阅降级信号，连接以监听对象为 context（QObject 父子/context 自动断开）。
//   监听模块须在 instance().start() 之前完成构造（MainWindow 构造函数末尾启动，
//   此时各面板已就绪）；start() 幂等。
//
// 线程模型：仅主线程使用（QTimer + 直接连接），跨线程使用需自行加锁（无此需求）。
//
// 滞回阈值：>80% 触发 high（降级），<70% 恢复（回升），70~80 保持原状态防抖动。
// 降级时自身采样放慢为 2s（减少高负载下的自身开销），恢复后回到 1s。
// 平台不支持读取进程 CPU 时（非 Linux/Windows）静默跳过，不误触发降级。
class ResourceMonitor : public QObject
{
    Q_OBJECT

public:
    // 进程 CPU 使用率高负载阈值（%）：采样值 > kHighThreshold 才触发降级
    static constexpr int kHighThreshold = 80;
    // 恢复阈值（%）：采样值 < kLowThreshold 才恢复（滞回带 70~80 保持原状态）
    static constexpr int kLowThreshold = 70;
    // 采样周期（ms）：正常 1s；高负载降级为 2s（自身也参与降级）
    static constexpr int kSampleIntervalMs = 1000;
    static constexpr int kHighSampleIntervalMs = 2000;

    static ResourceMonitor &instance();
    void start();            // 幂等；启动采样（应在全部监听模块构造完成后调用）
    int cpuPercent() const;  // 最近一次成功采样的**本进程** CPU 使用率（%）

    // 解析 /proc/self/stat，取 utime + stime（USER_HZ）。
    // comm 字段可能包含空格和括号，所以从最后一个 ')' 之后开始数字段。
    // 失败（格式不符 / 字段缺失 / 非数字）返回 false 且不改写输出。
    static bool parseProcSelfStat(const QByteArray &data, quint64 &cpuTicks);

    // 两次采样的进程 CPU%（单核满载 = 100%，多线程可 >100）。
    // 计数回绕（ticks2 < ticks1）、零间隔或非法频率返回 -1（调用方跳过本次）。
    static int processPercent(quint64 ticks1, quint64 ticks2, qint64 elapsedMs,
                              long ticksPerSecond);

    // 以下两个纯函数是旧实现（整机 /proc/stat）的解析工具，保留供既有用例；
    // sample() 已不再使用。新代码请用 parseProcSelfStat / processPercent。
    static bool parseProcStat(const QByteArray &data, quint64 &total, quint64 &idle);
    static int percentBetween(quint64 total1, quint64 idle1,
                              quint64 total2, quint64 idle2);

    // 滞回状态机（纯函数，供测试注入采样值）：>80 触发 high，<70 恢复，中间保持。
    static bool nextHighState(bool currentHigh, int percent);

signals:
    // 高负载状态翻转时发射（仅状态变化时发射一次，非每个采样周期）：
    // high=true → 本进程 CPU 使用率已 >80%（触发降级）；false → 已 <70%（恢复）。
    void cpuHigh(bool high, int percent);

private:
    explicit ResourceMonitor(QObject *parent = nullptr);
    void sample(); // 一次采样：读进程 CPU 时间 → 与上次差分 → 滞回判定 → 发射

    QTimer m_timer;
    QElapsedTimer m_elapsed;
    bool m_started = false;
    bool m_hasPrev = false;     // 是否已有上一次采样（首个采样只建立基准）
    quint64 m_lastTicks = 0;    // 上次采样的进程 CPU ticks
    qint64 m_lastElapsedMs = 0; // 上次采样时的墙钟毫秒
    bool m_high = false;        // 当前高负载状态
    int m_percent = 0;          // 最近一次成功采样的使用率
};

#endif // RESOURCE_MONITOR_H
