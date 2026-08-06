#include "resource_monitor.h"

#include <QFile>
#include <QDebug>
#include <limits>

// /proc/stat 聚合行字段核实（2026-08-05，Linux 内核 Documentation/filesystems/
// proc.rst §1.7 "Miscellaneous kernel statistics in /proc/stat"，非记忆）：
//   cpu  user nice system idle iowait irq softirq steal guest guest_nice
//   - user:    normal processes executing in user mode
//   - nice:    niced processes executing in user mode
//   - system:  processes executing in kernel mode
//   - idle:    twiddling thumbs
//   - iowait:  waiting for I/O to complete（内核文档注明该值不可靠）
//   - irq:     servicing interrupts
//   - softirq: servicing softirqs
//   - steal:   involuntary wait
//   - guest:   running a normal guest —— 文档注明已包含在 user 内
//   - guest_nice: running a niced guest —— 文档注明已包含在 nice 内
// 聚合行以 "cpu " 前缀区分 per-CPU 的 "cpuN " 行；时间单位为 USER_HZ。

ResourceMonitor &ResourceMonitor::instance()
{
    // Meyers 单例（函数局部静态，C++11 起初始化线程安全）。
    // 仅主线程使用（标注于头文件），跨线程访问需自行加锁。
    static ResourceMonitor monitor;
    return monitor;
}

ResourceMonitor::ResourceMonitor(QObject *parent)
    : QObject(parent)
{
    m_timer.setInterval(kSampleIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &ResourceMonitor::sample);
}

void ResourceMonitor::start()
{
    if (m_started)
        return;
    m_started = true;
    m_timer.start();
    qDebug() << "ResourceMonitor started (CPU sampling every"
             << m_timer.interval() << "ms)";
}

int ResourceMonitor::cpuPercent() const
{
    return m_percent;
}

void ResourceMonitor::sample()
{
    // 失败不崩溃：读文件失败 / 解析失败 → 本次采样跳过，保持上次状态。
    QFile f(QStringLiteral("/proc/stat"));
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QByteArray data = f.readAll();
    f.close();

    quint64 total = 0, idle = 0;
    if (!parseProcStat(data, total, idle))
        return; // 解析失败：保持上次状态

    if (!m_hasPrev) {
        // 首个采样仅建立基准（两次采样才能算差值）
        m_lastTotal = total;
        m_lastIdle = idle;
        m_hasPrev = true;
        return;
    }

    const int pct = percentBetween(m_lastTotal, m_lastIdle, total, idle);
    m_lastTotal = total;
    m_lastIdle = idle;
    if (pct < 0)
        return; // 计数回绕 / 零间隔：保持上次状态

    m_percent = pct;
    const bool newHigh = nextHighState(m_high, pct);
    if (newHigh != m_high) {
        // 状态翻转才发射（防抖动）；自身也参与降级：high 时采样放慢为 2s
        m_high = newHigh;
        m_timer.setInterval(m_high ? kHighSampleIntervalMs : kSampleIntervalMs);
        qDebug() << "ResourceMonitor CPU" << m_percent
                 << "% ->" << (m_high ? "HIGH (degrade)" : "normal (recover)");
        emit cpuHigh(m_high, m_percent);
    }
}

bool ResourceMonitor::parseProcStat(const QByteArray &data, quint64 &total,
                                    quint64 &idle)
{
    // 逐行扫描，匹配聚合行 "cpu "（"cpu" 后必须是空白分隔，排除 "cpuN" 行），
    // 依次解析 8 个无符号字段：user nice system idle iowait irq softirq steal。
    // 第 9/10 字段（guest/guest_nice）已含在 user/nice 内，忽略即可。
    int pos = 0;
    while (pos < data.size()) {
        int eol = data.indexOf('\n', pos);
        if (eol < 0)
            eol = data.size();
        const QByteArray line = data.mid(pos, eol - pos);
        pos = eol + 1;

        int i = 0;
        while (i < line.size() && (line.at(i) == ' ' || line.at(i) == '\t'))
            ++i; // 容忍行首空白
        if (line.size() - i < 3 || line.at(i) != 'c' || line.at(i + 1) != 'p'
            || line.at(i + 2) != 'u')
            continue; // 非 "cpu" 前缀
        i += 3;
        if (i >= line.size() || (line.at(i) != ' ' && line.at(i) != '\t'))
            continue; // 非 "cpu "（如 cpu0）：不是聚合行

        quint64 values[8] = {0};
        int v = 0;
        while (i < line.size() && v < 8) {
            while (i < line.size() && (line.at(i) == ' ' || line.at(i) == '\t'))
                ++i;
            if (i >= line.size())
                break;
            if (line.at(i) < '0' || line.at(i) > '9')
                return false; // 非数字字段 → 拒绝
            quint64 acc = 0;
            while (i < line.size() && line.at(i) >= '0' && line.at(i) <= '9') {
                const unsigned d = static_cast<unsigned>(line.at(i) - '0');
                if (acc > (std::numeric_limits<quint64>::max() - d) / 10)
                    return false; // 溢出 → 拒绝
                acc = acc * 10 + d;
                ++i;
            }
            values[v++] = acc;
        }
        if (v < 8)
            return false; // 字段缺失 → 拒绝

        total = values[0] + values[1] + values[2] + values[3]
              + values[4] + values[5] + values[6] + values[7];
        idle = values[3] + values[4]; // idle + iowait（iowait 按惯例计入空闲）
        return true;
    }
    return false; // 未找到聚合行
}

int ResourceMonitor::percentBetween(quint64 total1, quint64 idle1,
                                    quint64 total2, quint64 idle2)
{
    // 计数回绕或无效序列（计数器只增不减）：无法计算，调用方跳过本次
    if (total2 < total1 || idle2 < idle1)
        return -1;
    const quint64 dTotal = total2 - total1;
    if (dTotal == 0)
        return -1; // 两次采样零间隔
    const quint64 dIdle = idle2 - idle1;
    if (dIdle > dTotal)
        return -1; // idle 增量不可能超过 total 增量（防御）
    const quint64 busy = dTotal - dIdle;
    // 浮点计算避免 (busy*100) 溢出 quint64；结果钳到 0-100
    int pct = qRound(static_cast<double>(busy) * 100.0
                     / static_cast<double>(dTotal));
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    return pct;
}

bool ResourceMonitor::nextHighState(bool currentHigh, int percent)
{
    // 滞回：>80 触发 high，<70 恢复，70~80（含边界）保持当前状态防抖动。
    // 边界语义严格对照约束：触发条件是 ">80%"，恢复条件是 "<70%"。
    if (percent > kHighThreshold)
        return true;
    if (percent < kLowThreshold)
        return false;
    return currentHigh;
}
