# Android 设备性能监控引擎设计

- 状态：草案，待评审，未实现
- 日期：2026-11-19
- 基线：`dslsdzc/PhoneToolbox` 的 `main` 分支，提交 `ce8b819`
- 目标读者：实现者、性能监控维护者、UI 维护者
- 关联：`docs/superpowers/specs/2026-11-18-root-agent-design.md`、`docs/api.md`

## 0. 基线说明

本设计只以当前可见的 `main` 为基线。仓库没有 `develop` 分支，本文不假设存在未检查的分支或未合并的实现。

当前实现的事实：

1. `src/ui/system_tool_panel.cpp` 约 2080 行，设备探测、命令执行、数据解析、指标计算、图表更新都在 `SystemToolPanel` 内。
2. `monitorExec()` 使用 `QProcess::waitForFinished()`，`onPerformancePoll()` 在 UI 线程同步调用。
3. 性能采样周期为 500ms；`ResourceMonitor` 高负载时把周期放慢到 2s。
4. `ResourceMonitor` 读取的是桌面端 PhoneToolbox 自身的 `/proc/self/stat`（Windows 使用 `GetProcessTimes`），只用于保护桌面端，不是被测设备指标。
5. CPU 图表显示的是 CPU 频率，不是 CPU 使用率；GPU 图表显示的是频率，不是 GPU 利用率。
6. `LiveChartWidget` 只保存数值，不保存时间戳；截图导出按 `点数 × 0.5s` 计算时长。
7. GPU 候选路径混合了当前频率、最大频率、可用频率列表和 OPP 表；解析时没有严格区分语义。
8. `RootShell::run()` 是同步接口，内部用 `QEventLoop` 等待 Root Agent 结果。
9. Root Agent 已经支持 `exec`、`push`、`pull`、命令超时、输出上限、取消和审计。

## 1. 意图与为什么

PhoneToolbox 已经能通过 ADB 和 Root 代理读取一部分设备性能数据，但这些数据是“页面内的临时采样”，还不是一个可扩展的监控引擎。继续在 `SystemToolPanel` 里增加指标，会同时放大四个问题：

- UI 阻塞；
- 指标语义混杂；
- 机型/厂商分支失控；
- 历史数据无法可信地回放和导出。

本设计要把现有能力升级为一个独立的设备性能监控引擎，满足：

1. 按设备实际具备的节点和驱动能力动态构建采集方案，不按手机型号写死。
2. 采集、解析、计算、存储、渲染分层；UI 不直接执行设备命令。
3. 所有指标都有明确的有效性状态，不用 `0` 表示“读不到”。
4. 历史数据以真实时间戳存储，采样间隔变化、暂停、掉线都能正确表达。
5. 先保留现有可用的性能调用，再逐步迁移；不推倒重来。

## 2. 目标、非目标、反目标

### 2.1 目标

1. 支持整机、逐核、按 cpufreq policy 的 CPU 频率与使用率。
2. 支持 GPU 频率，并在驱动确实暴露利用率/忙闲计数时支持 GPU 使用率。
3. 支持多 thermal zone 温度，按 CPU/GPU/电池/外壳等分类持续监控。
4. 支持 RAM、Swap、内存压力。
5. 支持电池电流、电压、功率、电量与充电状态。
6. 支持前台应用 FPS、帧时间、卡顿统计，并与显示刷新率区分。
7. 支持网络上下行速率、包速率、错误/丢包。
8. 支持存储吞吐、IOPS、平均延迟。
9. 支持进程级 CPU、RSS/PSS（权限允许时），默认只采集 Top N。
10. 所有采集异步执行，UI 线程不做设备 I/O 等待。
11. 采集方案可诊断：用户能看到每个指标来自哪个节点、为什么不可用。
12. 历史、图表和导出都使用真实时间戳。

### 2.2 非目标

1. 不做通用采样 profiler、方法追踪、perfetto 级 trace 分析。
2. 不做云端遥测、远程监控或跨设备聚合。
3. 不在没有 OS 支持的情况下伪造“每应用功耗”或“CPU/GPU 单独功耗”。
4. 不承诺所有设备都能提供 GPU 利用率和 FPS；没有可信来源时明确显示不可用。
5. 不为了兼容极旧设备而牺牲现代设备的数据质量。
6. 不把桌面端自身资源监控和被测设备监控混在一起。

### 2.3 反目标

以下情况即使“功能看起来能用”，也视为失败：

- UI 因设备探测或采样卡住超过一帧。
- 把频率表第一个数字当作当前频率。
- 把 GPU 频率当作 GPU 利用率。
- 读不到数据时画 0 MHz、0%、0°C。
- 采样间隔被降级后，截图仍按 0.5s 计算时长。
- 新增一个机型就新增一组 `if (model == ...)`。
- 监控本身导致设备明显发热、耗电或产生海量 Root Agent 审计日志。
- 一个采集器失败导致整页停止刷新。

## 3. 核心原则

### 3.1 能力驱动，而不是型号驱动

采集方案由 `CapabilitySet` 决定：节点是否存在、是否可读、驱动签名、SDK、可用传输通道。机型名称只用于展示和诊断，不参与核心分支。

允许的厂商差异表达方式：

- 节点路径匹配；
- 驱动标识/GLES 渲染器；
- 节点内容格式；
- 可读权限和传输通道；
- 解析器优先级。

不允许：

```cpp
if (model == "24090RA29C") { ... }
```

### 3.2 语义先行

每个节点在注册时必须声明语义：

- 当前值；
- 最大值/最小值；
- 可用值列表；
- 累计计数；
- 忙/总时间；
- 温度；
- 电流/电压/功率；
- 帧时间。

解析器与语义绑定，禁止用一个 `firstNumber()` 处理所有节点。

### 3.3 无效值不是零

指标值使用显式状态。`0 MHz` 只有在设备确实报告 0 时才成立。缺失、权限不足、超时、格式错误、未支持、预热中、过期都必须单独表达。

### 3.4 时间是数据的一部分

每条样本保存：

- 主机单调时钟时间（用于排序、窗口、间隔）；
- 墙钟时间（用于导出和用户阅读）；
- 可选的设备时间（用于 FPS 帧时间戳）。

禁止用“点数 × 固定间隔”推断时间。

### 3.5 采集与渲染解耦

采集器只产生 `MetricBatch`；存储层只保存时间序列；UI 只订阅自己需要的指标。UI 不执行 `cat`，不解析 `/proc`。

## 4. 总体架构

```
┌──────────────────────────────────────────────────────────────┐
│ UI: PerformancePage                                          │
│  - 概览卡片 / 分类页 / 图表 / 诊断 / 导出                    │
└───────────────────────────┬──────────────────────────────────┘
                            │ 订阅指标、控制会话
┌───────────────────────────▼──────────────────────────────────┐
│ PerformanceEngine                                            │
│  - 会话生命周期、采样计划、采集器注册、失败退避、状态机       │
│  - 产生 MetricBatch，写入 MetricStore                         │
└───────────┬───────────────────────────────┬──────────────────┘
            │                               │
┌───────────▼──────────────┐   ┌────────────▼─────────────────┐
│ CapabilityProbe          │   │ SampleScheduler              │
│  - 节点/驱动/权限探测    │   │  - 分频、对齐、跳过重叠      │
│  - 生成 CollectionPlan   │   │  - 主机负载预算（可选）      │
└───────────┬──────────────┘   └────────────┬─────────────────┘
            │                               │
┌───────────▼───────────────────────────────▼──────────────────┐
│ CommandBus（异步）                                           │
│  - AdbTransport（QProcess 异步）                             │
│  - RootAgentTransport（现有 RootAgentClient）                │
│  - 批处理、超时、取消、并发上限、结果关联                    │
└───────────┬───────────────────────────────┬──────────────────┘
            │                               │
┌───────────▼──────────────┐   ┌────────────▼─────────────────┐
│ Collectors               │   │ Parsers                      │
│  CPU/GPU/Thermal/Memory  │   │  纯函数，按节点语义选择      │
│  Power/FPS/Network/Disk  │   │  可单元测试，不做 I/O        │
│  Process                 │   │                              │
└──────────────────────────┘   └──────────────────────────────┘
```

### 4.1 组件职责

| 组件 | 职责 | 不负责 |
|---|---|---|
| `CapabilityProbe` | 探测节点、权限、驱动、SDK、可用通道 | 持续采样 |
| `CollectionPlan` | 把能力映射成采集项、命令批次、周期 | 执行命令 |
| `SampleScheduler` | 按周期触发采集组、跳过重叠、退避 | 解析 |
| `CommandBus` | 异步执行、批处理、超时、取消、路由 Root/ADB | 解释业务语义 |
| `Collector` | 声明需要哪些读项，把原始结果交给解析器 | 直接画 UI |
| `Parser` | 把字节/状态转为带单位的指标样本 | 访问设备 |
| `MetricStore` | 时间序列、环形缓冲、间隔统计、缺口 | 设备 I/O |
| `PerformanceEngine` | 会话、状态、失败策略、对外 API | 控件布局 |
| `PerformancePage` | 展示、选择、导出、诊断 | 执行 shell |

## 5. 数据模型

### 5.1 指标描述

```cpp
enum class MetricKind {
    Gauge,       // 瞬时值：频率、温度、使用率
    Counter,     // 累计值：rx_bytes、utime、sectors_read
    State,       // 枚举：online/offline、charging/discharging
    Text,        // 型号、驱动名
};

enum class MetricSemantics {
    Instant,
    DeltaRate,       // 由累计值差分得到速率
    DeltaRatio,      // busy/total 差分
    Derived,         // 由多个指标计算
};

struct MetricDescriptor {
    QString id;                 // "cpu.policy0.cur_freq_mhz"
    QString displayName;
    QString unit;               // "MHz", "%", "C", "B/s", "ms"
    MetricKind kind;
    MetricSemantics semantics;
    QMap<QString, QString> tags; // vendor, cluster, zone, interface...
    QString source;              // 节点路径或 "gfxinfo"
    int precision = 1;
};
```

### 5.2 样本状态

```cpp
enum class MetricState {
    Valid,
    WarmingUp,        // 累计值第一次采样，尚不能算速率
    Offline,          // CPU 核心/设备确实处于离线状态，值不适用
    Unavailable,      // 预期存在但当前读不到
    PermissionDenied, // EACCES/EPERM 或需要 Root
    Timeout,
    ParseError,
    NotSupported,     // 能力探测确认设备没有该来源
    Stale,            // 超过 3 个周期没有新样本
    ResetDetected,    // 计数器回绕/设备重启；本条是缺口
    Error,
};

struct MetricSample {
    QString metricId;
    qint64 hostTimeMs;      // CLOCK_MONOTONIC
    qint64 wallTimeMs;      // 导出/展示
    qint64 deviceTimeNs;    // 可选，FPS 用
    MetricState state;
    double value;           // 仅 state == Valid 时有效
    QString text;           // State/Text 指标
    QString detail;         // "Permission denied: /sys/..."
};
```

`MetricSample` 不带 `0` 默认值参与计算。访问数值必须检查 `state == Valid`。

状态约定：

- `Offline` 表示“设备明确报告离线”，例如 `cpuN/online == 0`。它不是错误，也不是 0 值。
- `ResetDetected` 只作为一条缺口样本写入历史；下一条同类累计值样本为 `WarmingUp`，建立新基线后再进入 `Valid`。
- `Stale` 由存储/调度层生成，不覆盖最后一条有效值；图表在 `Stale` 之后断线。
- `NotSupported` 由能力探测得出；`Unavailable`/`PermissionDenied` 允许后续重试并重新规划。

### 5.3 批次

```cpp
struct MetricBatch {
    qint64 hostTimeMs;
    quint64 sequence;
    QVector<MetricSample> samples;
    QStringList collectorErrors;
};
```

同一批次的指标共享采集时间，便于关联 CPU/GPU/温度。

## 6. 能力探测与采集计划

### 6.1 探测内容

`CapabilityProbe` 在以下时机运行：

- 监控会话开始；
- 设备重新连接；
- Root Agent 连接状态变化；
- 某采集项连续 3 次 `Unavailable`/`PermissionDenied`；
- 用户点击“重新识别”。

探测分三组，全部异步、批处理：

1. **系统组（ADB，无 root）**
   - `getprop`：SDK、ABI、hardware、厂商、构建类型；
   - CPU 目录：`/sys/devices/system/cpu/cpu*`、`cpufreq/policy*`；
   - 热区：`/sys/class/thermal/thermal_zone*` 的 `type`；
   - 内存、网络、磁盘、电源节点；
   - GPU 签名：kgsl、mali、devfreq 名称、`dumpsys SurfaceFlinger` 的 GLES 行。

2. **权限组（Root Agent 优先，其次 adb su）**
   - 对候选 root-only 节点做一次 `[ -r path ]`/读取探测；
   - 记录每个节点的实际通道：`adb`、`root-agent`、`adb-su`、`none`；
   - Root Agent 不可用时，不把节点标记为“设备不支持”，而是 `PermissionDenied` 并保留重试可能。

3. **前台应用组（ADB）**
   - 当前 resumed package；
   - SurfaceFlinger layer 列表；
   - 显示刷新率。

### 6.2 探测结果

```cpp
struct NodeCapability {
    QString path;
    QString semantic;       // cur_freq/max_freq/utilization/temp/...
    QString adapter;        // mali/adreno/generic...
    bool exists = false;
    bool readable = false;
    QString transport;      // adb/root-agent/adb-su
    QString detail;
};

struct CapabilitySet {
    QString serial;
    int sdk = 0;
    QString abi;
    QString hardware;
    QString gpuVendor;       // mali/adreno/powervr/unknown
    QString gpuRenderer;     // 来自 GLES
    QVector<CpuPolicy> policies;
    QVector<ThermalZone> thermalZones;
    QMap<QString, NodeCapability> nodes;
    bool rootAgent = false;
    bool adbRoot = false;
    QStringList flowErrors;
};
```

### 6.3 采集计划

`CollectionPlan` 是能力到读项的映射：

```cpp
struct ReadSpec {
    QString metricId;
    QString path;
    QString parserId;
    QString transport;     // adb/root-agent/adb-su
    int cadenceMs;
    int timeoutMs;
    int priority;
};

struct CollectionGroup {
    QString id;
    QString transport;
    int cadenceMs;
    QVector<ReadSpec> reads;
    bool legacySafeBatch;  // 不使用 $()、||、awk
};
```

构建规则：

1. 每个指标可以有多个候选来源，按优先级选择第一个可用来源。
2. 同一传输通道、同一周期的读项合并为一个 shell 批次，用 `__MARKER__` 分隔。
3. Root-only 读项单独成组，避免整个批次因权限失败。
4. 一个读项失败只影响它自己的指标，不影响同批其他指标。
5. 旧设备批次使用 echo + cat；现代设备可以使用更紧凑的命令，但为了行为一致，默认复用旧格式。
6. 计划生成后不按机型修改；只在能力变化时重建。

## 7. 异步命令总线

### 7.1 为什么必须异步

现有 `monitorExec()` 在 UI 线程 `waitForFinished()`。当 GPU 候选路径、24 个热区、Root 代理超时叠加时，界面会卡住数秒。新引擎不允许任何 UI 线程阻塞等待。

### 7.2 接口

```cpp
struct CommandRequest {
    QString groupId;
    QString transport;      // adb/root-agent/adb-su
    QString script;
    int timeoutMs;
    quint64 sequence;
};

struct CommandResult {
    quint64 sequence;
    QString groupId;
    int exitCode;
    bool timedOut;
    QString stdoutText;
    QString stderrText;
};
```

`CommandBus`：

- 每个设备一个实例；
- ADB 使用 `QProcess` 异步启动，`finished/errorOccurred` 回调；
- Root Agent 使用现有 `RootAgentClient::execShell()` 信号；
- 同一 Root Agent 连接同一时间只允许一个 exec，队列串行；
- 超时用 `QTimer`，取消时终止进程或发 `cancel`；
- 结果按 `sequence` 关联，不依赖命令文本匹配；
- 输出有上限，超过上限截断并标记。

### 7.3 批处理

一个 500ms 采样批次应尽量只产生一次 ADB/Root 调用：

```sh
echo __cpu_policy0_cur__
cat /sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq
echo __cpu_policy0_max__
cat /sys/devices/system/cpu/cpufreq/policy0/scaling_max_freq
...
```

解析器按 marker 取段。marker 名称来自计划，不来自用户输入。

### 7.4 Root 通道选择

优先级：

1. 已连接且已授权的 Root Agent（Noise）；
2. adb shell `su -c`（如果能力探测确认可用）；
3. 不可用，指标状态为 `PermissionDenied`。

Root Agent 通道必须避免高频 exec 造成的审计膨胀。第一阶段仍通过批量 exec 读取；后续阶段引入“性能采样会话”协议（见 12.6）。

### 7.5 共享 Root 命令服务（必须改现有模块）

当前 `RootAgentClient` 由 `RootAgentManagerPanel` 私有持有，`execShell()` 一次只允许一条命令，`execFinished` 不带请求 id，设置页会把每条结果写进日志。性能引擎不能直接复用这个形态，否则：

- 采样会占住 client，用户现有的 root 按钮会报“上一条命令还没结束”；
- 超时后的迟到结果可能被算到下一条命令；
- 2Hz 采样会刷爆设置页日志；
- `RootShell::run()` 是同步的，不能在 UI 线程调用。

本设计把 Root 接入改为共享服务，并纳入实现范围：

1. 新增 `RootCommandService`（按 serial 一个实例），拥有队列、请求 id、超时和 quiet 标记。
2. `RootAgentClient` 增加带 id 的异步接口：
   - `quint64 execShellAsync(command, timeoutMs, quiet, callback)`；
   - 新信号 `execFinishedFor(quint64 requestId, ...)`，保留旧 `execFinished` 给现有调用方；
   - 迟到结果按 id 丢弃；
   - `quiet=true` 的结果不进入设置页日志，只更新诊断计数。
3. `RootAgentManagerPanel` 不再独占 client：连接、配对、断开由它管理，但执行入口交给 `RootCommandService`；设置页订阅非 quiet 结果用于日志。
4. `RootShell` 保留现有同步 `run()` 以兼容旧按钮，但新增 `runAsync()`。性能引擎只允许使用 `runAsync()`。
5. 同一 `RootAgentClient` 同一时间仍只执行一条命令；`RootCommandService` 负责串行化和优先级。高频采样使用低优先级，用户手动 root 操作插队。
6. 采样批次合并为一条 exec；500ms 组不要拆成多条命令。
7. 审计：阶段 0-4 每条批次仍产生一条 exec 审计；阶段 5 的“性能采样会话”把审计收敛为一次会话开始/结束。

这会让性能页与设置页共享同一条已配对 Root 通道，不会创建第二个 Noise 会话，也不会绕过现有授权。

### 7.6 Root/设备状态传播

现有 `MainWindow::onRootAgentStateChanged()` 只更新 `DeviceInfo`，不会可靠地把状态送到 `SystemToolPanel`。实现时必须新增/接通以下链路：

```text
SettingsPanel::rootAgentStateChanged
  -> MainWindow
  -> SystemToolPanel::setRootAgentState(serial, connected, rooted)
  -> PerformancePage/PerformanceEngine
```

同时：

- 设备连接/断开时，`MainWindow` 必须调用 `SystemToolPanel::setDeviceInfo/clearDeviceInfo`，而不是只在切换工具时调用；
- 引擎收到 Root 连接 -> 重新探测 root-only 节点并恢复 `PermissionDenied` 指标；
- 引擎收到 Root 断开 -> 取消在途 root 请求，相关指标转 `PermissionDenied`/`Stale`，不回退到未授权的 adb su；
- 设备重连 -> 重新探测，保留用户“正在监控”的意图。

## 8. 采集器

### 8.1 CPU 频率

数据源：

- `/sys/devices/system/cpu/cpufreq/policy*/scaling_cur_freq`；
- `cpuinfo_cur_freq`（可能更贵或不稳定）；
- `scaling_min_freq`、`scaling_max_freq`；
- `affected_cpus`、`related_cpus`；
- 每个 `cpuN` 的 `online`。

规则：

- 优先按 policy 采集，而不是按核心逐个采集。
- 一个 policy 覆盖多个核心时，图表显示“cluster/policy”，核心页显示映射关系。
- 离线核心不填 0，状态为 `offline`。
- 频率单位统一为 MHz；原始 Hz/kHz 在解析器内归一化。
- `cpuinfo_cur_freq` 读取失败时回退 `scaling_cur_freq`，并记录来源。

指标：

```text
cpu.policy0.cur_freq_mhz
cpu.policy0.min_freq_mhz
cpu.policy0.max_freq_mhz
cpu.core0.online
cpu.core0.policy
```

### 8.2 CPU 使用率

数据源：

- `/proc/stat` 的 `cpu` 和 `cpuN` 行；
- 差分计算，使用主机单调时钟；
- 逐核映射到 policy。

规则：

- 第一次采样 `WarmingUp`，不画点。
- 计数器回退/设备重启 -> `ResetDetected`，重置基线并留缺口。
- 核心热插拔：新增核心从 `WarmingUp` 开始；消失核心标记 `offline`。
- 整机使用率与逐核使用率分别计算，不用整机值填充离线核心。

指标：

```text
cpu.total.usage_pct
cpu.core0.usage_pct
cpu.policy0.usage_pct   // 可选：policy 内核心平均
```

### 8.3 进程级 CPU / 内存

数据源：

- `/proc/<pid>/stat`：utime、stime、starttime；
- `/proc/<pid>/cmdline` 或 `comm`；
- `/proc/<pid>/status`：VmRSS；
- `/proc/<pid>/smaps_rollup`：PSS（通常需要 root）。

规则：

- 默认 2s 采集一次，只在进程页可见时启用。
- 先枚举 `/proc`，再读取 Top N（按上次 CPU 或用户选择），避免全量解析。
- 无 root 时，其他应用可能不可读；状态为 `PermissionDenied`，不显示 0。
- CPU% 定义为“单核满载 = 100%”，与 `top` 口径一致；另提供“占整机”换算。
- 进程退出后序列结束，不补零。

### 8.4 GPU

GPU 必须拆成独立适配器。适配器选择依据节点和驱动签名，而不是机型。

```cpp
enum class GpuMetric {
    Model,
    CurrentFreq,
    MinFreq,
    MaxFreq,
    AvailableFreqs,
    Utilization,
    BusyTime,
    TotalTime,
    MemoryUsed,
};

struct GpuReadSpec {
    GpuMetric metric;
    QString path;
    QString parserId;
    QString transport;
    int priority;
};

class GpuAdapter {
public:
    virtual QString id() const = 0;
    virtual bool matches(const CapabilitySet &cap) const = 0;
    virtual QVector<GpuReadSpec> reads() const = 0;
    virtual GpuSnapshot parse(const RawReads &reads) const = 0;
};
```

适配器：

| 适配器 | 识别依据 | 频率 | 利用率 |
|---|---|---|---|
| Mali devfreq | `devfreq/*mali*`, `misc/mali0` | `cur_freq`, `max_freq`, `min_freq` | 仅当 `utilization`/`gpu_utilization` 节点存在 |
| Adreno KGSL | `kgsl-3d0` | `gpuclk`, `devfreq/cur_freq` | `gpubusy` 差分，或 `gpu_busy_percentage` |
| PowerVR | `devfreq/*rgx*`、GLES 含 PowerVR | `cur_freq` | 仅当驱动节点明确提供 |
| MTK gpufreq | `/proc/gpufreq/*` | `gpufreq_var_dump` 中的当前频率 | 仅当明确提供利用率 |
| 通用 devfreq | 名称含 gpu/mali/kgsl | `cur_freq` | 不推断 |
| 未知 | 以上都不匹配 | 无 | 无 |

硬规则：

- 频率表/OPP 表只能用于“可用频率”或“最大频率”，不能当当前频率。
- 没有利用率/忙闲计数时，`gpu.utilization_pct` 为 `NotSupported`，而不是用频率除以最大频率。
- 同一设备如果有多个来源，按优先级选择，不混合单位。
- GPU 名称优先取驱动节点；不可读时用 `dumpsys SurfaceFlinger` 的 GLES renderer 作为展示名，但不据此推断指标可用性。

指标：

```text
gpu.model
gpu.cur_freq_mhz
gpu.min_freq_mhz
gpu.max_freq_mhz
gpu.utilization_pct
gpu.mem_used_bytes
```

### 8.5 温度

数据源：

- `/sys/class/thermal/thermal_zone*/type`；
- `temp`；
- 可选 `trip_point_*_temp`；
- 可选 `/sys/class/hwmon` 适配器（后续）。

规则：

- 一次批次读取所有 zone，不逐个发命令。
- 毫摄氏度归一化为摄氏度；异常值（<-40 或 >200）标记 `ParseError`。
- 按 type 分类：CPU、GPU、电池、外壳、充电、其他。
- 默认概览显示 CPU/GPU/电池/外壳最高值；分类页显示全部 zone。
- 单个 zone 失败不影响其他 zone。

指标：

```text
thermal.zone12.temp_c
thermal.cpu.max_temp_c
thermal.gpu.max_temp_c
thermal.battery.temp_c
```

### 8.6 内存与 Swap

数据源：

- `/proc/meminfo`；
- `/proc/pressure/memory`（存在时）；
- `/proc/vmstat`（可选：pgmajfault、pswpin/pswpout）。

规则：

- “已用”优先用 `MemTotal - MemAvailable`；旧内核没有 `MemAvailable` 时回退 `MemFree + Buffers + Cached`，并标记来源。
- Swap 使用量用 `SwapTotal - SwapFree`；`SwapCached` 单列。
- PSI 的 `some avg10/avg60/avg300` 和 `full` 分别展示；不存在则 `NotSupported`。
- 内存百分比不使用四舍五入后的字节数反推。

指标：

```text
mem.total_bytes
mem.available_bytes
mem.used_pct
mem.cached_bytes
mem.dirty_bytes
swap.used_pct
mem.pressure.some_avg10
mem.pressure.full_avg10
```

### 8.7 功耗

数据源：

- `/sys/class/power_supply/battery/current_now`；
- `voltage_now`；
- `power_now`；
- `status`、`capacity`、`charge_counter`、`energy_now`；
- 其他 `power_supply` 节点（可选）。

规则：

- 单位探测：`current_now` 可能是 µA 或 mA；`voltage_now` 可能是 µV 或 mV；按量级和节点 `uevent` 判断，不能盲除。
- 有 `power_now` 时直接使用；没有时用 `voltage × current` 计算，并标记 `Derived`。
- 充电/放电符号以 `status` 解释；原始值保留，避免厂商符号差异。
- 只承诺“电池侧功率”，不承诺 CPU/GPU 分项功率；没有分路节点就不显示分项。
- 能耗用 `charge_counter`/`energy_now` 差分，采样间隔变化时仍按真实时间计算。

指标：

```text
power.battery.voltage_v
power.battery.current_a
power.battery.power_w
power.battery.soc_pct
power.battery.status
power.battery.energy_wh   // 可选
```

### 8.8 FPS / 帧时间

FPS 不能从 GPU 频率推算，也不能把屏幕刷新率当应用帧率。

数据源按优先级：

1. **前台应用 framestats**
   - `dumpsys gfxinfo <package> framestats`；
   - 解析 `Flags,IntendedVsync,Vsync,OldestInputEvent,NewestInputEvent,HandleInputStart,...,FrameCompleted`；
   - 用窗口内 `FrameCompleted` 或 `IntendedVsync` 时间戳计算 FPS、帧时间 P50/P90/P99、卡顿数。

2. **SurfaceFlinger latency**
   - `dumpsys SurfaceFlinger --latency "<layer>"`；
   - 通过 `dumpsys SurfaceFlinger --list` 找当前前台 layer；
   - 解析 desired/present/actual present 时间戳；
   - 计算实际呈现 FPS 和帧间隔。

3. **显示刷新率（仅上下文）**
   - `dumpsys display` 或 SurfaceFlinger 输出；
   - 只用于判断“60/90/120Hz”，不作为应用 FPS。

规则：

- 帧时间戳来自设备时钟（ns），窗口计算用设备时间；展示时间用主机时间。
- 没有前台应用、没有权限、layer 不可识别 -> `NotSupported`/`Unavailable`。
- `dumpsys gfxinfo` 有开销，默认 2s，不与 500ms 批次混在一起。
- 卡顿阈值基于该显示模式的实际刷新间隔，而不是固定 16.67ms。
- **严格只读**：不调用 `dumpsys gfxinfo reset`，不修改设备统计。gfxinfo 和 SurfaceFlinger latency 都是滚动缓冲，解析器必须按帧时间戳去重，只统计上次已见时间戳之后的新帧。
- 每次结果同时报告 `window_ms`、`frames_used`、`buffer_coverage_ms`。120Hz 下滚动缓冲可能覆盖不足 2s，此时显示“最近 N 帧 / 实际覆盖 X ms”，不把缓冲窗口伪装成固定 2s。
- 如果两次采样之间没有新帧，状态为 `Stale` 或保留上一窗口的 `window_ms`，不重复累计卡顿数。

指标：

```text
fps.foreground.fps
fps.foreground.frame_time_p50_ms
fps.foreground.frame_time_p90_ms
fps.foreground.frame_time_p99_ms
fps.foreground.jank_count
display.refresh_hz
```

### 8.9 网络

数据源：

- `/proc/net/dev`；
- 可选 `/proc/net/snmp`（错误/丢包）。

规则：

- 排除 `lo`；按接口分类 wlan/rmnet/eth/其他。
- 累计字节差分 -> B/s；包差分 -> pkt/s。
- 计数器回退 -> `ResetDetected`。
- 首个样本 `WarmingUp`。
- 同时提供总量和分接口；默认概览显示活跃接口合计。

指标：

```text
net.total.rx_bytes_per_s
net.total.tx_bytes_per_s
net.wlan0.rx_bytes_per_s
net.wlan0.tx_bytes_per_s
net.total.rx_drops
```

### 8.10 存储 I/O

数据源：

- `/proc/diskstats` 或 `/sys/block/<dev>/stat`；
- `/proc/mounts` 找到 `/data` 对应设备。

规则：

- 过滤 loop、ram、zram；优先 userdata/整盘设备。
- 字段：读完成数、读扇区、读耗时、写完成数、写扇区、写耗时、进行中 I/O、I/O 耗时、加权耗时。
- 扇区按 512B 计算；若节点明确给出 4096B 逻辑扇区，按节点说明处理。
- 差分 -> 吞吐、IOPS、平均延迟（`delta_ms / delta_ios`）。
- 设备重启/计数器回退 -> `ResetDetected`。
- 无 `/data` 映射或权限不足 -> `Unavailable`/`PermissionDenied`。

指标：

```text
disk.userdata.read_bytes_per_s
disk.userdata.write_bytes_per_s
disk.userdata.read_iops
disk.userdata.write_iops
disk.userdata.avg_latency_ms
```

## 9. 采样调度

### 9.1 周期

| 组 | 默认周期 | 说明 |
|---|---:|---|
| CPU 频率/使用率 | 500ms | 主要实时曲线 |
| GPU 频率/利用率 | 500ms | 有忙闲计数时需要两次采样 |
| 内存/Swap | 1000ms | 变化较慢 |
| 温度 | 1000ms | 多 zone 批量 |
| 网络 | 1000ms | 累计值差分 |
| 存储 I/O | 1000ms | 累计值差分 |
| 功耗 | 1000ms | 电池节点 |
| FPS | 2000ms | dumpsys 开销大 |
| 进程 Top N | 2000ms | 仅在进程页可见时 |
| 能力探测 | 会话开始/重连/失败重试 | 不参与周期 |

### 9.2 规则

- 使用单调时钟对齐；同一 tick 内按组触发。
- 若上一轮同组采集未完成，跳过本轮并标记 `Stale`，不排队堆积。
- 单组超时不影响其他组。
- 连续失败按指数退避（1x、2x、4x，上限 30s），成功后恢复。
- 主机负载高时，`ResourceMonitor` 只调整“主机预算”，把 500ms 组降到 1s/2s；界面明确显示当前实际间隔。它不是设备 CPU 指标。
- 采样间隔变化写入历史元数据，导出时使用真实时间戳。

### 9.3 会话生命周期与页面可见性

- 引擎按“当前选中设备”唯一存在。切换设备时取消在途命令、清空图表和历史，不跨设备复用序列。多设备并行监控不是首版目标。
- 用户点击“开始监控”后会话生效；点击“暂停”停止调度，但保留已采集历史，时间轴不压缩。
- 切到其他顶层工具页或隐藏性能页：保留会话，但把核心组（CPU/GPU/内存/温度）降到 1s；FPS、进程组停止；回到性能页后恢复原周期并重新探测前台应用。
- 窗口最小化：核心组降到 2s；不采集 FPS/进程。
- 设备断开：取消在途命令，所有活动指标转 `Stale`/`Unavailable`；设备重连后重新探测，并在用户原本处于监控状态时自动恢复。
- Root Agent 断开：只取消 root 通道请求，不停止整个会话；非 root 指标继续。
- 页面销毁：引擎停止、取消命令、保存必要的会话摘要，不保留后台轮询。

### 9.4 时间戳

- `hostTimeMs`：`QElapsedTimer`/`clock_gettime(CLOCK_MONOTONIC)`，用于排序和窗口。
- `wallTimeMs`：`QDateTime::currentMSecsSinceEpoch()`，用于导出。
- 设备时间：FPS 帧时间戳保留原始 ns；如需与主机对齐，记录一次 `(host, device)` 对作为偏移，不修改原始数据。
- 每个 `MetricBatch` 有一个批次时间，组内样本共享；解析过程中不重新取当前时间。

## 10. 历史与存储

### 10.1 SeriesStore

```cpp
struct SeriesSample {
    qint64 hostTimeMs;
    MetricState state;
    double value;
};

class SeriesStore {
    QVector<SeriesSample> ring; // 固定容量，按时间递增
    qint64 capacityMs;          // 例如 10 分钟/1 小时
    qint64 minIntervalMs;
    qint64 maxIntervalMs;
};
```

规则：

- 每个指标独立序列，支持不同周期。
- 无效样本保留状态和时间，图表遇到无效值断线，不画 0。
- 容量按时间或点数限制，取先到者；淘汰最旧数据。
- 记录实际间隔统计（最小/平均/最大）和缺口数。
- 导出使用序列中的真实时间戳，不假设固定间隔。
- 默认保留：实时窗口 10 分钟；会话历史 1 小时；可配置。
- 内存预算：50 条序列 × 1 小时 × 2Hz × 24B ≈ 8.6MB，可接受；超过预算时降采样或缩短历史。

### 10.2 导出

保留 PNG 截图，并新增：

- CSV：`timestamp,metric,value,unit,state,source`；
- JSON：包含设备信息、能力计划、采样间隔统计、缺口、错误原因。

截图头部改为：

```text
设备 | Root 状态 | 开始/结束时间 | 实际间隔 min/avg/max | 采样点 | 缺口
```

不再写死 `0.5s`。

## 11. UI 设计

### 11.1 页面结构

`SystemToolPanel` 保留 7 个工具分类和分类 0 的现有操作按钮（CPU Governor、内存优化、I/O 调度、保存截图）。性能页作为分类 0 的内嵌内容页 `PerformancePage` 替换现有图表区，不新增顶层工具项，也不改动 `MainWindow` 的工具索引。

`PerformancePage` 的职责边界：

- 负责监控会话、概览、分类图表、诊断和导出；
- 不接管 CPU Governor/内存优化/I/O 调度等写操作；
- 现有操作按钮继续由 `SystemToolPanel` 外壳持有，放在页面顶部或底部操作区，避免功能丢失；
- “保存截图”按钮改为调用 `PerformancePage` 的导出接口，导出真实时间戳。

页面结构：

```text
┌ 设备 / Root 代理 / 采样状态 / 暂停 / 导出 ──────────────────┐
│ 概览卡片：CPU 使用率 · CPU 频率 · GPU 频率/利用率 · 温度     │
│           RAM · Swap · 功耗 · FPS · 网络 · 存储             │
├ 标签页 ────────────────────────────────────────────────────┤
│ 概览 | CPU | GPU | 内存 | 温度/功耗 | FPS | 网络 | 存储 | 进程│
├ 图表区 ────────────────────────────────────────────────────┤
│ 时间轴为真实时间；无效区间断线；悬停显示状态和来源           │
├ 诊断面板（可折叠）─────────────────────────────────────────┤
│ 采集计划 | 节点路径 | 传输通道 | 最近错误 | 重新识别         │
└────────────────────────────────────────────────────────────┘
```

### 11.2 图表

新增 `TimeSeriesChartWidget`，或扩展 `LiveChartWidget`：

- `addSample(qint64 timeMs, MetricState state, double value)`；
- 时间轴按真实时间映射；
- 无效状态断线，并可在悬停提示中显示原因；
- 支持多条序列和不同单位；
- 支持窗口缩放（最近 1/5/10 分钟）；
- 保留旧 `LiveChartWidget` 给其他页面，避免一次性大改。

### 11.3 状态展示

- 顶部状态：`采样 500ms | 实际 512ms | 丢弃 0 | 权限 2 | Root 代理已连接`。
- 卡片无数据时显示 `需要 Root`、`设备不支持`、`读取超时`，不显示 0。
- 诊断页显示每个指标的 `source`、`transport`、`state`、`detail`。
- 用户可手动“重新识别能力”，不必重启应用。

### 11.4 无阻塞

- 页面打开时显示骨架和“正在识别设备能力”，探测异步填充。
- 任何按钮触发的设备命令都不能在 UI 线程 `waitForFinished()`。
- UI 更新合并到 10Hz 上限；图表只重绘可见序列。

## 12. 迁移与实施顺序

### 12.1 阶段 0：地基

- 新增 `src/core/perf/`：`PerformanceEngine`、`MetricStore`、`CommandBus`、`CapabilityProbe`、`CollectionPlan`。
- 新增 `TimeSeriesChartWidget`。
- 把现有 CPU 频率、GPU 频率、温度、RAM/Swap 迁移到引擎。
- 旧 `SystemToolPanel` 的性能采集代码只保留 UI 入口，不再直接 `monitorExec()`。
- 验收：功能与当前版本等价，但 UI 不再阻塞，历史带时间戳。

### 12.2 阶段 1：CPU

- `/proc/stat` 整机/逐核使用率；
- cpufreq policy、cluster、online 状态；
- 频率上下限；
- 计数器重置、热插拔、`WarmingUp` 状态。

### 12.3 阶段 2：内存、温度、功耗

- 多 thermal zone 分类；
- PSI 内存压力；
- 电池电流/电压/功率/能耗；
- 诊断页展示不可用原因。

### 12.4 阶段 3：GPU

- Mali/Adreno/PowerVR/MTK/通用适配器；
- 频率语义分离；
- 仅在有可靠忙闲计数时提供利用率；
- GPU 适配器矩阵测试。

### 12.5 阶段 4：FPS、网络、存储

- 前台应用识别；
- gfxinfo/SurfaceFlinger 帧数据；
- 网络与磁盘累计值差分；
- 真实时间轴和缺口。

### 12.6 阶段 5：进程级与增强采样

- Top N 进程 CPU/RSS/PSS；
- 可选 Root Agent “性能采样会话”：一次授权、按计划在设备侧采样并按批返回，减少 2Hz exec 的审计和 ADB 开销；
- 该阶段不改变桌面端采集器接口，只增加一种 `CommandTransport`/`SamplingBackend`。

## 13. 测试策略

### 13.1 纯解析器测试

每个解析器用固定文本夹具：

- `/proc/stat`（多核、离线核、计数器回退）；
- `/proc/meminfo`（有/无 MemAvailable）；
- `/proc/net/dev`（多接口、异常行）；
- `/proc/diskstats`（dm、mmcblk、zram、回绕）；
- thermal zone；
- power_supply（µA/mA、µV/mV、充电/放电）；
- Mali/Adreno/PowerVR/MTK GPU 节点；
- gfxinfo framestats、SurfaceFlinger latency。

夹具放在 `tests/fixtures/perf/`，包含设备签名和预期指标。真实设备抓取需脱敏。

### 13.2 能力计划测试

- 给定节点集合，断言选中的适配器、指标、传输通道、批次分组。
- 断言不会把 `gpu_freq_table` 当当前频率。
- 断言无利用率节点时 `gpu.utilization_pct == NotSupported`。
- 断言同一批次失败不会污染其他指标。

### 13.3 调度测试

- 假时钟：500ms/1s/2s 组对齐；
- 慢采集跳过重叠，标 `Stale`；
- 失败退避与恢复；
- Root 连接/断开触发重规划；
- 设备切换清空旧序列。

### 13.4 UI 测试

- 无效样本断线，不画 0；
- 时间轴使用真实时间戳；
- 导出头部显示实际间隔；
- 采样暂停/恢复后时间轴不压缩。

### 13.5 真机/模拟器

- API 10 legacy：批次命令不使用不支持的 shell 语法；
- API 21+：Root Agent/ADB 两条通道；
- 当前 MTK Mali 设备：`/sys/class/devfreq/13000000.mali/cur_freq` 通过 Root Agent 读取，GPU 利用率按实际节点决定是否可用；
- 至少一台 Adreno 设备验证 `gpubusy` 差分。

### 13.6 性能预算

- UI 线程单次回调不超过 16ms；
- 500ms 组单批命令目标 < 100ms（USB ADB，正常设备）；
- 单次输出上限、解析上限、序列内存上限均有测试；
- 连续运行 1 小时无内存增长、无采样队列堆积。

## 14. 风险与取舍

| 风险 | 取舍 |
|---|---|
| ADB 每次采样都有进程开销 | 先批处理；后续用 Root Agent 采样会话 |
| Root Agent 高频 exec 产生审计日志 | 批量读取；阶段 5 引入会话级审计 |
| GPU 利用率节点因厂商/内核差异大 | 适配器矩阵；没有可信来源就明确不可用 |
| FPS 的 dumpsys 输出随 Android 版本变化 | 多解析器 + 失败状态；不猜测 |
| 进程级数据无 root 时受限 | 明确 `PermissionDenied`，不伪造 |
| 旧设备 shell 能力弱 | 默认使用 echo+cat 批次；能力探测决定语法 |
| 高采样率导致设备发热 | 周期可调；主机负载降级；只采集当前页面需要的指标 |
| 历史内存增长 | 环形缓冲、时间/点数双上限、可见序列优先 |

## 15. 已定的实现者选择

以下由实现者决定，不需要用户逐项确认：

1. 使用 `src/core/perf/` 独立模块，不继续扩写 `SystemToolPanel`。
2. 保留 `LiveChartWidget`，新增时间序列图表，避免一次性重写所有图表。
3. `Policy` 优先于逐核频率；逐核只做映射展示。
4. GPU 利用率没有可信节点时为 `NotSupported`。
5. 历史默认 10 分钟实时窗口 + 1 小时会话历史，可配置。
6. 旧设备批次继续使用 echo+cat，不使用 `$()`、`||`、`awk`。
7. 第一阶段不引入设备侧常驻采样器；先用异步 ADB/Root 批处理达到可用，再在阶段 5 评估。
8. Root 接入采用共享 `RootCommandService`，允许修改 `RootAgentClient`/`RootAgentManagerPanel`，但保留现有同步 `RootShell::run()` 兼容旧按钮。
9. 引擎按当前选中设备唯一存在，切换设备清空历史。
10. 页面隐藏时核心指标降频继续，FPS/进程停止；最小化降到 2s。
11. FPS 严格只读、按时间戳去重，不调用 reset。
12. `PerformancePage` 嵌入分类 0，保留现有操作按钮。
13. 新增 `MetricState::Offline`；`ResetDetected` 作为缺口，下一条累计样本 `WarmingUp`。

## 16. 已确认的范围

用户在 2026-11-19 明确选择：

1. **首版验收范围：阶段 0-5 全量。**
   包含地基、现有指标迁移、CPU 使用率、内存压力、多温度、功耗、GPU 适配器与利用率、FPS、网络、存储 I/O、进程级监控。
   实现仍按阶段 0 → 5 的顺序交付，每个阶段独立可验收，但不把后续阶段排除在首版目标之外。

2. **纯 ADB 基线：核心指标必须无 Root、无 Agent 可用。**
   CPU、内存、Swap、温度、网络、存储、电池的电池侧指标，在纯 ADB 下必须工作。
   以下指标允许依赖 Root Agent，并在缺失时给出明确状态：
   - GPU 利用率（仅当驱动节点需要 root 或需要 root 才能读忙闲计数）；
   - 其他应用的 PSS / 进程级深度数据；
   - 厂商私有的分路功耗节点。

### 16.1 交付里程碑

| 里程碑 | 内容 | 验收重点 |
|---|---|---|
| M0 | 引擎地基、异步 CommandBus、能力探测、MetricStore、时间序列图表；迁移现有 CPU/GPU 频率、温度、RAM/Swap | UI 不阻塞；现有功能等价；历史带时间戳 |
| M1 | CPU 整机/逐核使用率、cpufreq policy、cluster、online、上下限 | 计数器/热插拔/离线状态正确 |
| M2 | 多 thermal zone、PSI、Swap、电池电压/电流/功率/能耗 | 纯 ADB 可用；单位与符号正确 |
| M3 | GPU 适配器（Mali/Adreno/PowerVR/MTK/通用）与利用率 | 频率/利用率语义分离；无来源时明确不支持 |
| M4 | FPS、帧时间、卡顿、网络、存储 I/O | FPS 只读去重；累计值差分与回绕正确 |
| M5 | 进程级 CPU/RSS/PSS、Top N、可选 Root Agent 采样会话 | 权限状态正确；高频采样不刷爆审计/日志 |

## 17. 验收标准

1. 打开性能页、开始监控、切换设备、Root 代理重连时，UI 线程无 `waitForFinished()`，界面不冻结。
2. CPU 频率、CPU 使用率、GPU 频率分别有独立指标，不互相替代。
3. GPU 利用率只在设备提供可信忙闲/利用率节点时出现；否则显示明确原因。
4. 任一指标读取失败都显示状态，不以 0 参与曲线和统计。
5. 采样间隔变化后，截图和导出使用真实时间戳与真实间隔。
6. 采集计划由能力探测生成；新增机型不需要新增型号分支。
7. 单元测试覆盖所有解析器、计划选择、状态机和调度；桌面端现有测试保持通过。
8. 核心指标（CPU、内存、Swap、温度、网络、存储、电池）在无 Root、无 Agent 的纯 ADB 模式下完成验证；GPU 利用率、其他应用 PSS、分路功耗允许依赖 Root Agent，并在缺失时显示明确状态。
9. 阶段 0-5 全部交付；每个阶段有独立的验收记录。至少在一台 Mali 设备、一台 Adreno 设备和一台 API 10 legacy 设备上完成对应范围的验证；没有设备的项目必须明确标注“未验证”，不能用测试夹具代替真机结论。
