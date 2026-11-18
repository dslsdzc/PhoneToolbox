#include "resource_monitor.h"

#include <QFile>
#include <QDebug>
#include <limits>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <unistd.h>   // sysconf(_SC_CLK_TCK)
#endif

namespace {

// 进程 CPU 时间的单位换算：Linux 的 USER_HZ（通常 100），Windows 统一成毫秒。
long cpuTicksPerSecond()
{
#ifdef Q_OS_WIN
    return 1000;
#else
    const long hz = ::sysconf(_SC_CLK_TCK);
    return hz > 0 ? hz : 100;
#endif
}

// 读取**本进程**累计 CPU 时间。失败返回 false，调用方跳过本次采样。
bool readProcessCpuTicks(quint64 &ticks)
{
#ifdef Q_OS_WIN
    FILETIME createTime, exitTime, kernelTime, userTime;
    if (!::GetProcessTimes(::GetCurrentProcess(), &createTime, &exitTime,
                           &kernelTime, &userTime))
        return false;

    ULARGE_INTEGER kernel;
    kernel.LowPart = kernelTime.dwLowDateTime;
    kernel.HighPart = kernelTime.dwHighDateTime;

    ULARGE_INTEGER user;
    user.LowPart = userTime.dwLowDateTime;
    user.HighPart = userTime.dwHighDateTime;

    // FILETIME 是 100ns；换成毫秒，与 cpuTicksPerSecond()==1000 对齐。
    ticks = (kernel.QuadPart + user.QuadPart) / 10000ULL;
    return true;
#else
    QFile f(QStringLiteral("/proc/self/stat"));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = f.readAll();
    f.close();
    return ResourceMonitor::parseProcSelfStat(data, ticks);
#endif
}

} // namespace

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
    m_elapsed.start(); // 墙钟基准：CPU% 的分母是实际经过时间，不是定时器标称间隔
    m_timer.start();
    qDebug() << "ResourceMonitor started (process CPU sampling every"
             << m_timer.interval() << "ms)";
}

int ResourceMonitor::cpuPercent() const
{
    return m_percent;
}

void ResourceMonitor::sample()
{
    quint64 ticks = 0;
    if (!readProcessCpuTicks(ticks))
        return; // 平台不支持 / 读取或解析失败：保持上次状态，不误触发

    const qint64 nowMs = m_elapsed.elapsed();
    if (!m_hasPrev) {
        // 首个采样仅建立基准（两次采样才能算差值）
        m_lastTicks = ticks;
        m_lastElapsedMs = nowMs;
        m_hasPrev = true;
        return;
    }

    const qint64 elapsedMs = nowMs - m_lastElapsedMs;
    const int pct = processPercent(m_lastTicks, ticks, elapsedMs, cpuTicksPerSecond());
    m_lastTicks = ticks;
    m_lastElapsedMs = nowMs;
    if (pct < 0)
        return; // 计数回绕 / 零间隔：保持上次状态

    m_percent = pct;
    const bool newHigh = nextHighState(m_high, pct);
    if (newHigh != m_high) {
        // 状态翻转才发射（防抖动）；自身也参与降级：high 时采样放慢为 2s
        m_high = newHigh;
        m_timer.setInterval(m_high ? kHighSampleIntervalMs : kSampleIntervalMs);
        qDebug() << "ResourceMonitor process CPU" << m_percent
                 << "% ->" << (m_high ? "HIGH (degrade)" : "normal (recover)");
        emit cpuHigh(m_high, m_percent);
    }
}

bool ResourceMonitor::parseProcSelfStat(const QByteArray &data, quint64 &cpuTicks)
{
    // /proc/self/stat 形状：
    //   pid (comm) state ppid ... utime(14) stime(15) ...
    // comm 里可能带空格甚至括号，所以不能按空格从头切；先跳到最后一个 ')'。
    const int closeParen = data.lastIndexOf(')');
    if (closeParen < 0)
        return false;

    QList<QByteArray> fields;
    for (const QByteArray &part : data.mid(closeParen + 1).split(' ')) {
        if (!part.isEmpty())
            fields.append(part);
    }

    // fields[0] 是 state（原字段 3）。utime=14 → fields[11]，stime=15 → fields[12]。
    if (fields.size() < 13)
        return false;

    bool okUtime = false;
    bool okStime = false;
    const quint64 utime = fields.at(11).toULongLong(&okUtime);
    const quint64 stime = fields.at(12).toULongLong(&okStime);
    if (!okUtime || !okStime)
        return false;

    cpuTicks = utime + stime;
    return true;
}

int ResourceMonitor::processPercent(quint64 ticks1, quint64 ticks2,
                                    qint64 elapsedMs, long ticksPerSecond)
{
    // 计数器只增不减；回绕 / 零间隔 / 非法频率 → 无法计算
    if (ticks2 < ticks1 || elapsedMs <= 0 || ticksPerSecond <= 0)
        return -1;

    const double seconds = static_cast<double>(elapsedMs) / 1000.0;
    const double cpuSeconds = static_cast<double>(ticks2 - ticks1)
                              / static_cast<double>(ticksPerSecond);
    const int pct = qRound(cpuSeconds / seconds * 100.0);
    // 不钳到 100：多线程程序可以合法超过单核 100%。
    return pct < 0 ? 0 : pct;
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
