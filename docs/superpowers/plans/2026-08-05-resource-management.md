# 资源管理与自动降级 Implementation Plan（计划 H）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 2026-08-05 用户约束：**程序整体 CPU 使用率不得超过 80%，超过则自动降级**（降低轮询频率/任务优先级/采样率），恢复后自动回升。

**Architecture:** 新增 `ResourceMonitor`（QObject 单例，QTimer 周期读 `/proc/stat` 计算整体 CPU 使用率，滞回阈值 80%/70%），发出 `cpuHigh(bool, int percent)` 信号；各轮询模块（DeviceDetector 2s、性能监控 500ms、ImageWorker 线程优先级、监控自身采样）订阅后按 high/low 切换频率/优先级。

**Tech Stack:** C++17, Qt6, Linux /proc/stat（Windows 用 GetSystemTimes，标注为后续）。

## Global Constraints

- C++17；`ResourceMonitor` 单例模式（`instance()`），信号槽跨模块
- 滞回阈值：>80% 触发 high，<70% 恢复（防抖动）
- 降级自动化（用户要求"自动降级"，不提示用户手动）
- 恢复自动回升（不永久降级）
- 每个任务独立 commit，前缀 `feat:`；本计划允许改 src/core 与 src/ui（资源管理为独立任务）
- 不可信原则：/proc/stat 字段解析对照 Linux 内核文档核实；Windows 路径标注待办

---

### Task H1: ResourceMonitor — CPU 采样与降级信号

**Files:**
- Create: `src/core/resource_monitor.h/.cpp`
- Create: `tests/test_resource_monitor.cpp`（/proc/stat 解析纯函数测试）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Produces: `class ResourceMonitor : public QObject { Q_OBJECT public: static ResourceMonitor &instance(); void start(); int cpuPercent() const; signals: void cpuHigh(bool high, int percent); };`；内部纯函数 `static bool parseProcStat(const QByteArray &data, quint64 &total, quint64 &idle)`（解析 `cpu ` 行 user/nice/system/idle/iowait/irq/softirq/steal 字段，对照内核文档核实）

**降级接入**：
- `DeviceDetector`：high → `m_monitorTimer->setInterval(5000)`，恢复 → 2000
- `SystemToolPanel` 性能监控：high → 2000ms，恢复 → 500ms
- `ImageWorker`：high → 工作线程 `setPriority(QThread::IdlePriority)`，恢复 → NormalPriority
- `ResourceMonitor` 自身：high → 采样 2s，恢复 → 1s
- 接入方式：各模块构造时 `connect(&ResourceMonitor::instance(), &ResourceMonitor::cpuHigh, ...)`；监听模块析构自动断开（QObject 父子）

**验证**：测试（解析函数 + 信号逻辑 mock）；构建 + ctest 21/21；手动验证（跑一个 CPU 密集操作观察降级触发 —— 若环境可测）

---

## Self-Review 记录

- **Spec 覆盖**：用户"CPU>80% 自动降级"约束 → H1 全量（采样 + 信号 + 四处接入）。
- **不可信原则**：/proc/stat 字段解析对照内核文档；Windows GetSystemTimes 标注待办。
- **依赖顺序**：单任务 H1。
- **类型一致性**：`ResourceMonitor::instance()` / `cpuHigh(bool,int)` 命名一致。
