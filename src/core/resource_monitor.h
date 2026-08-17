#ifndef RESOURCE_MONITOR_H
#define RESOURCE_MONITOR_H

#include <QObject>
#include <QTimer>
#include <QByteArray>

// 任务 H1：程序整体 CPU 使用率监控与自动降级
// （用户约束：程序整体 CPU 使用率超过 80% 必须自动降级，恢复后自动回升）。
//
// 单例：监听模块（DeviceDetector / SystemToolPanel / ImageWorker / 自身）通过
//   connect(&ResourceMonitor::instance(), &ResourceMonitor::cpuHigh, ...)
// 订阅降级信号，连接以监听对象为 context（QObject 父子/context 自动断开）。
// 监听模块须在 instance().start() 之前完成构造（MainWindow 构造函数末尾启动，
// 此时各面板已就绪）；start() 幂等。
//
// 线程模型：仅主线程使用（QTimer + 直接连接），跨线程使用需自行加锁（无此需求）。
//
// 采样：QTimer 周期（正常 1s）读 /proc/stat，两次采样计算整体 CPU 使用率
//   busy% = (Δtotal - Δidle) / Δtotal × 100
// 滞回阈值：>80% 触发 high（降级），<70% 恢复（回升），70~80 保持原状态防抖动。
// 降级时自身采样放慢为 2s（减少高负载下的自身开销），恢复后回到 1s。
class ResourceMonitor : public QObject
{
    Q_OBJECT

public:
    // 整体 CPU 使用率高负载阈值（%）：采样值 > kHighThreshold 才触发降级
    static constexpr int kHighThreshold = 80;
    // 恢复阈值（%）：采样值 < kLowThreshold 才恢复（滞回带 70~80 保持原状态）
    static constexpr int kLowThreshold = 70;
    // 采样周期（ms）：正常 1s；高负载降级为 2s（自身也参与降级）
    static constexpr int kSampleIntervalMs = 1000;
    static constexpr int kHighSampleIntervalMs = 2000;

    static ResourceMonitor &instance();
    void start();            // 幂等；启动采样（应在全部监听模块构造完成后调用）
    int cpuPercent() const;  // 最近一次成功采样的整体 CPU 使用率（0-100）

    // 解析 /proc/stat 的聚合 cpu 行。字段顺序对照 Linux 内核文档
    // Documentation/filesystems/proc.rst §1.7（2026-08-05 核实，非记忆）：
    //   cpu  user nice system idle iowait irq softirq steal [guest guest_nice]
    //   - user/nice/system/idle/iowait/irq/softirq/steal：用户/优先级用户/内核/
    //     idle/iowait/中断/软中断/steal 时间（USER_HZ）
    //   - guest / guest_nice：文档注明"already included in user/nice"，不重复累加
    // 返回 total = user+nice+system+idle+iowait+irq+softirq+steal，
    //      idle = idle + iowait（iowait 是 CPU 无事可做的等待时间，按惯例计入空闲）。
    // 失败（无聚合行 / 字段缺失 / 非数字 / 溢出）返回 false 且不改写输出。
    static bool parseProcStat(const QByteArray &data, quint64 &total, quint64 &idle);

    // 两次采样的整体 CPU 使用率（0-100）：(Δtotal-Δidle)/Δtotal×100。
    // 计数回绕（total2<total1 或 idle2<idle1）或零间隔返回 -1（调用方跳过本次）。
    static int percentBetween(quint64 total1, quint64 idle1,
                              quint64 total2, quint64 idle2);

    // 滞回状态机（纯函数，供测试注入采样值）：>80 触发 high，<70 恢复，中间保持。
    static bool nextHighState(bool currentHigh, int percent);

signals:
    // 高负载状态翻转时发射（仅状态变化时发射一次，非每个采样周期）：
    // high=true → 整体 CPU 使用率已 >80%（触发降级）；false → 已 <70%（恢复）。
    void cpuHigh(bool high, int percent);

private:
    explicit ResourceMonitor(QObject *parent = nullptr);
    void sample(); // 一次采样：读 /proc/stat → 解析 → 计算 → 滞回判定 → 发射

    QTimer m_timer;
    bool m_started = false;
    bool m_hasPrev = false;   // 是否已有上一次采样（首个采样只建立基准）
    quint64 m_lastTotal = 0;  // 上次采样的 total / idle
    quint64 m_lastIdle = 0;
    bool m_high = false;      // 当前高负载状态
    int m_percent = 0;        // 最近一次成功采样的使用率
};

#endif // RESOURCE_MONITOR_H
