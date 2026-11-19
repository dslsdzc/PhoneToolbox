# Android 性能监控引擎 M0 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 建立能力驱动、全异步、带真实时间戳的 Android 性能采集地基，并把现有 CPU 频率、GPU 频率、温度、RAM/Swap 从 `SystemToolPanel` 迁移到新引擎，UI 不再同步等待设备命令。

**Architecture:** 新增 `perf_core` 静态库承载指标模型、时间序列、异步 CommandBus、能力探测、采集计划、采集器和引擎；`PerformancePage` 只订阅引擎并渲染。GPU 频率通过适配器按节点语义选择，历史保存真实时间戳与有效性状态。M1-M5 的 CPU 使用率、GPU 利用率、FPS、网络、存储、进程监控各自另出计划。

**Tech Stack:** C++17、Qt6 Core/Widgets/Network/Test、CMake、现有 ADB/Root Agent 基础设施。

**Spec:** `docs/superpowers/specs/2026-11-19-android-performance-engine-design.md`

## 范围

本计划只覆盖设计文档的 **M0（阶段 0：地基 + 现有指标等价迁移）**。M1-M5 是后续独立计划；本计划的数据模型、CommandBus、采集器接口必须为它们留出扩展点，但不在本计划实现 CPU 使用率、GPU 利用率、FPS、网络、存储、进程监控。

## Global Constraints

- 基线是 `main`；不要假设存在 `develop`。
- C++17；Qt6；不新增第三方库。
- UI 线程不得调用 `waitForFinished()`、`QEventLoop` 等待设备命令或 `RootShell::run()`。
- 只有 `MetricState::Valid` 的数值可以参与计算和绘图；其他状态必须以缺口/状态文本呈现，不能写 0。
- 旧设备（Android 1.0-4.4）的批量 shell 脚本不得使用 `$()`、`||`、`&&`、`awk`、`head`；使用 `echo __MARKER__` + `cat`。
- 不按手机型号写死分支；选择依据是节点、驱动签名和权限。
- 核心指标（CPU 频率、温度、内存、Swap）必须纯 ADB 可用；GPU 频率在 root-only 时允许经 Root Agent，缺失时显示 `PermissionDenied`。
- 每个任务结束时运行对应测试并提交；提交前 `git diff --check` 必须通过。
- 现有 80 个桌面测试必须保持通过。
- 新测试使用 CTest；测试目标命名 `perf_tests_<stem>`。

## Review Focus

这些是设计文档暗示、但普通任务测试容易漏掉的失败模式：

1. **超时后的迟到结果被当作下一条命令**：CommandBus 和 RootCommandService 都必须按请求 id 丢弃迟到结果；Task 2、Task 7 各自测试。
2. **取消/断开后仍回调旧请求**：取消必须保证回调不再触发；Task 2、Task 7 测试。
3. **无效样本被画成 0 或时间轴按点数压缩**：Task 1、Task 9 测试断线与真实时间映射。
4. **设备切换时在途结果写入新设备的历史**：Task 8 测试旧 sequence 被丢弃。
5. **旧设备批处理使用了不支持的 shell 语法**：Task 3/4 用 legacy 命令夹具断言批次只包含 echo/cat。

---

## Task 1: 指标模型与时间序列存储

**Files:**
- Create: `src/core/perf/metric.h`
- Create: `src/core/perf/metric.cpp`
- Create: `src/core/perf/metric_store.h`
- Create: `src/core/perf/metric_store.cpp`
- Create: `tests/test_perf_metrics.cpp`
- Modify: `CMakeLists.txt`（新增 `perf_core` 库、`ENABLE_PERF_TESTS` 测试循环）

**Interfaces:**
- Produces: `perf::MetricKind`、`perf::MetricSemantics`、`perf::MetricState`、`perf::MetricDescriptor`、`perf::MetricSample`、`perf::MetricBatch`
- Produces: `perf::SeriesStore::append(const MetricDescriptor&, const MetricSample&)`、`clear()`、`samples(const QString&) const`、`window(const QString&, qint64, qint64) const`、`minIntervalMs(const QString&) const`、`maxIntervalMs(const QString&) const`、`averageIntervalMs(const QString&) const`
- Produces: `Q_DECLARE_METATYPE(perf::MetricBatch)`

- [ ] **Step 1: 写失败的模型/存储测试**

在 `tests/test_perf_metrics.cpp` 中定义 `class TestPerfMetrics : public QObject`，至少包含：

```cpp
void metricState_onlyValidHasValue();
void sample_invalidKeepsReason();
void store_keepsRealTimestamps();
void store_invalidSampleCreatesGap();
void store_intervalStatsUseActualSpacing();
void store_windowFiltersByTime();
void store_clearRemovesEverything();
```

断言要点：

```cpp
perf::MetricSample sample;
sample.metricId = "cpu.policy0.cur_freq_mhz";
sample.state = perf::MetricState::PermissionDenied;
sample.value = 999.0;
QVERIFY(!sample.hasValue());

store.append(desc, sampleAt(1000, 800.0));
store.append(desc, sampleAt(1600, 1200.0));
QCOMPARE(store.minIntervalMs(id), qint64(600));
QCOMPARE(store.maxIntervalMs(id), qint64(600));
```

无效样本测试：

```cpp
store.append(desc, sampleAt(1000, 800.0));
perf::MetricSample gap;
gap.metricId = id;
gap.hostTimeMs = 1500;
gap.state = perf::MetricState::Timeout;
store.append(desc, gap);
store.append(desc, sampleAt(2000, 900.0));
QCOMPARE(store.samples(id).size(), 3);
QVERIFY(!store.samples(id).at(1).hasValue());
```

- [ ] **Step 2: 建立最小声明和 CMake 目标，运行测试确认失败**

先只创建 `metric.h` 的类型声明、`metric.cpp` 的空实现、`metric_store.h` 的接口声明和 `metric_store.cpp` 的空实现（方法返回空/0），并按 Step 4 接入 `perf_core` 与 `perf_tests_test_perf_metrics`。此时测试应能编译，但断言失败或链接失败；不允许“0 个测试通过”。

Run: `cmake -B build -G Ninja && cmake --build build -j$(nproc) && ctest --test-dir build -R perf_tests_test_perf_metrics --output-on-failure`
Expected: FAIL，失败原因是 `SeriesStore` 行为未实现，而不是找不到测试目标。

- [ ] **Step 3: 实现模型与 `SeriesStore`**

`metric.h` 定义：

```cpp
namespace perf {
enum class MetricKind { Gauge, Counter, State, Text };
enum class MetricSemantics { Instant, DeltaRate, DeltaRatio, Derived };
enum class MetricState { Valid, WarmingUp, Offline, Unavailable, PermissionDenied,
                         Timeout, ParseError, NotSupported, Stale, ResetDetected, Error };

struct MetricDescriptor {
    QString id;
    QString displayName;
    QString unit;
    MetricKind kind = MetricKind::Gauge;
    MetricSemantics semantics = MetricSemantics::Instant;
    QMap<QString, QString> tags;
    QString source;
    int precision = 1;
};

struct MetricSample {
    QString metricId;
    qint64 hostTimeMs = 0;
    qint64 wallTimeMs = 0;
    qint64 deviceTimeNs = -1;
    MetricState state = MetricState::Unavailable;
    double value = 0.0;
    QString text;
    QString detail;
    bool hasValue() const { return state == MetricState::Valid; }
};

struct MetricBatch {
    qint64 hostTimeMs = 0;
    quint64 sequence = 0;
    QVector<MetricSample> samples;
    QStringList collectorErrors;
};
} // namespace perf
Q_DECLARE_METATYPE(perf::MetricBatch)
```

`SeriesStore` 用 `QHash<QString, QVector<MetricSample>>`；`append` 只按 `metricId` 存储，不修改样本；`window` 用二分或线性过滤；间隔统计只使用相邻两个 `Valid` 样本。

- [ ] **Step 4: 接入 CMake**

在 `CMakeLists.txt` 的主程序 GLOB 之后：

```cmake
file(GLOB_RECURSE PERF_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/perf/*.cpp")
file(GLOB_RECURSE PERF_HEADERS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/perf/*.h")
list(FILTER SOURCES EXCLUDE REGEX "/src/core/perf/")
list(FILTER HEADERS EXCLUDE REGEX "/src/core/perf/")
add_library(perf_core STATIC ${PERF_SOURCES} ${PERF_HEADERS})
target_include_directories(perf_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(perf_core PUBLIC Qt6::Core)
set_target_properties(perf_core PROPERTIES AUTOMOC ON)
```

在 `PhoneToolbox` 的平台链接块之后加：

```cmake
target_link_libraries(PhoneToolbox PRIVATE perf_core)
```

在 image 测试块之后新增：

```cmake
option(ENABLE_PERF_TESTS "Build performance engine tests" ON)
if(ENABLE_PERF_TESTS)
    find_package(Qt6 QUIET COMPONENTS Test)
    if(Qt6Test_FOUND)
        enable_testing()
        set(PERF_TEST_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_perf_metrics.cpp)
        foreach(_test_src IN LISTS PERF_TEST_SOURCES)
            get_filename_component(_stem ${_test_src} NAME_WE)
            add_executable(perf_tests_${_stem} ${_test_src})
            target_link_libraries(perf_tests_${_stem} PRIVATE perf_core Qt6::Test)
            set_target_properties(perf_tests_${_stem} PROPERTIES AUTOMOC ON)
            add_test(NAME perf_tests_${_stem} COMMAND perf_tests_${_stem})
        endforeach()
    endif()
endif()
```

后续任务的测试文件逐一加入 `PERF_TEST_SOURCES`。

- [ ] **Step 5: 运行测试**

Run: `cmake -B build -G Ninja && cmake --build build -j$(nproc) && ctest --test-dir build -R perf_tests_test_perf_metrics --output-on-failure`
Expected: PASS。

- [ ] **Step 6: 提交**

```bash
git add src/core/perf/metric.h src/core/perf/metric.cpp \
  src/core/perf/metric_store.h src/core/perf/metric_store.cpp \
  tests/test_perf_metrics.cpp CMakeLists.txt
git commit -m "feat(perf): add metric model and timestamped series store"
```

---

## Task 2: 异步 CommandBus

**Files:**
- Create: `src/core/perf/command.h`
- Create: `src/core/perf/command_bus.h`
- Create: `src/core/perf/command_bus.cpp`
- Create: `src/core/perf/adb_transport.h`
- Create: `src/core/perf/adb_transport.cpp`
- Create: `tests/test_command_bus.cpp`
- Modify: `CMakeLists.txt`（把测试加入 `PERF_TEST_SOURCES`）

**Interfaces:**
- Consumes: 无
- Produces: `perf::CommandRequest`、`perf::CommandResult`、`perf::ICommandTransport`
- Produces: `perf::CommandBus::registerTransport(const QString&, ICommandTransport*)`、`submit(const CommandRequest&) -> quint64`、`cancel(quint64)`、`cancelAll()`
- Produces: signal `perf::CommandBus::finished(const perf::CommandResult&)`
- Produces: `perf::AdbTransport(const QString &serial, const QString &adbPath, QObject *parent)`、`setSerial`、`setAdbPath`

- [ ] **Step 1: 写失败的 CommandBus 测试**

`tests/test_command_bus.cpp` 使用一个 `FakeTransport`：

```cpp
class FakeTransport : public perf::ICommandTransport {
public:
    QVector<perf::CommandRequest> requests;
    std::function<void(const perf::CommandResult&)> lastCallback;
    bool available() const override { return true; }
    void execute(const perf::CommandRequest &r,
                 std::function<void(const perf::CommandResult&)> done) override {
        requests.append(r);
        lastCallback = std::move(done);
    }
    void cancel(quint64) override { ++cancelled; }
    int cancelled = 0;
};
```

测试：

```cpp
void submitUsesSequenceAndEmitsFinished();
void submitUnknownTransportEmitsError();
void timeoutEmitsTimedOutOnce();
void lateResultAfterTimeoutIsIgnored();
void cancelPreventsCallback();
void cancelAllCoversPending();
```

断言：

```cpp
auto *bus = new perf::CommandBus;
auto *transport = new FakeTransport;
bus->registerTransport("adb", transport);
QSignalSpy spy(bus, &perf::CommandBus::finished);
const quint64 id = bus->submit({0, "cpu", "adb", "echo ok", 1000, false});
QCOMPARE(id, quint64(1));
transport->lastCallback(perf::CommandResult{id, "cpu", 0, false, "ok", ""});
QCOMPARE(spy.count(), 1);
```

超时测试用 `QTest::qWait(50)` 或把 timeout 设为 10ms。

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --target perf_tests_test_command_bus -j$(nproc)`
Expected: 编译失败，`perf/command_bus.h` 不存在。

- [ ] **Step 3: 实现接口与 CommandBus**

```cpp
struct CommandRequest {
    quint64 sequence = 0;
    QString groupId;
    QString transport;
    QString script;
    int timeoutMs = 5000;
    bool quiet = false;
};

struct CommandResult {
    quint64 sequence = 0;
    QString groupId;
    int exitCode = -1;
    bool timedOut = false;
    QString stdoutText;
    QString stderrText;
};

class ICommandTransport {
public:
    virtual ~ICommandTransport() = default;
    virtual bool available() const = 0;
    virtual void execute(const CommandRequest &request,
                         std::function<void(const CommandResult&)> done) = 0;
    virtual void cancel(quint64 sequence) = 0;
};
```

`CommandBus` 维护 `m_nextSequence`、`QHash<quint64, Pending>`；`submit` 分配 sequence、启动 `QTimer::singleShot(timeoutMs)`；结果到达时先查找 pending，找不到直接丢弃；超时先移除 pending 再 emit，迟到结果因此被丢弃。`cancel` 删除 pending 并调用 transport->cancel。

- [ ] **Step 4: 实现 ADB 异步传输**

`AdbTransport` 构造参数 `serial`、`adbPath`，并提供 `setSerial(const QString&)`、`setAdbPath(const QString&)`，便于设备切换时复用；`execute` 启动 `QProcess`：

```cpp
process->start(adbPath, {"-s", serial, "shell", request.script});
connect(process, &QProcess::finished, ...)  // 读取 stdout/stderr、exitCode
connect(process, &QProcess::errorOccurred, ...)
```

不用 `waitForFinished()`；输出超过 1 MiB 时截断并附 `[truncated]`。`cancel` 对当前 sequence 的进程调用 `kill()`。

- [ ] **Step 5: 运行测试**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R perf_tests_test_command_bus --output-on-failure`
Expected: PASS。

- [ ] **Step 6: 提交**

```bash
git add src/core/perf/command.h src/core/perf/command_bus.h \
  src/core/perf/command_bus.cpp src/core/perf/adb_transport.h \
  src/core/perf/adb_transport.cpp tests/test_command_bus.cpp CMakeLists.txt
git commit -m "feat(perf): add async command bus and adb transport"
```

---

## Task 3: 能力模型、探测结果与采集计划

**Files:**
- Create: `src/core/perf/capability.h`
- Create: `src/core/perf/capability.cpp`
- Create: `src/core/perf/capability_probe.h`
- Create: `src/core/perf/capability_probe.cpp`
- Create: `src/core/perf/collection_plan.h`
- Create: `src/core/perf/collection_plan.cpp`
- Create: `src/core/perf/root_command_executor.h`
- Create: `tests/test_perf_plan.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `perf::CommandResult`、`perf::CommandBus`
- Produces: `perf::NodeCapability`、`perf::CpuPolicy`、`perf::ThermalZone`、`perf::CapabilitySet`
- Produces: `perf::ReadSpec`、`perf::CollectionGroup`、`perf::CollectionPlan`
- Produces: `perf::PlanBuilder::parseCapabilities(const QHash<QString, perf::CommandResult>&) -> CapabilitySet`
- Produces: `perf::PlanBuilder::build(const CapabilitySet&) -> CollectionPlan`
- Produces: `perf::PlanBuilder::renderGroupScript(const CollectionGroup&) -> QString`
- Produces: `perf::PlanBuilder::splitBatch(const CollectionGroup&, const CommandResult&) -> QHash<QString, CommandResult>`
- Produces: `perf::RootExecResult`、`perf::IRootCommandExecutor`
- Produces: `perf::CapabilityProbe::probe(const QString &serial, std::function<void(CapabilitySet)>)`、`setRootExecutor(IRootCommandExecutor*)`

- [ ] **Step 1: 写失败的能力/计划测试**

`tests/test_perf_plan.cpp` 至少包含：

```cpp
void parseRecognizesMaliDevfreqName();
void parseRecognizesPolicyClusters();
void parseMarksRootOnlyNodeTransport();
void buildUsesPolicyInsteadOfPerCore();
void buildDoesNotTreatAvailableFreqsAsCurrent();
void buildGroupsByTransportAndCadence();
void legacyScriptUsesOnlyEchoAndCat();
void probeSubmitsOneBatchAndParses();
void rootProbeMarksRootOnlyGpuNode();
void splitBatchMapsMarkersWithoutMixingValues();
```

`probeSubmitsOneBatchAndParses` 用 FakeTransport 断言：只提交一个 `transport=="adb"` 的批次，批次包含 `__cpu_dirs__`、`__thermal_list__`、`__gpu_signature__` 等 marker；完成后回调的 `CapabilitySet.serial` 正确，且 `flowErrors` 为空。

关键断言：

```cpp
QCOMPARE(cap.gpuVendor, QStringLiteral("mali"));
QCOMPARE(cap.policies.size(), 2);
QCOMPARE(cap.policies.at(0).cpus, QStringList({"0","1","2","3"}));
QCOMPARE(cap.thermalZones.size(), 3);

const auto plan = perf::PlanBuilder::build(cap);
QVERIFY(plan.groups.size() >= 2);
QVERIFY(plan.groups.at(0).cadenceMs == 500);
QVERIFY(!plan.groups.at(0).reads.isEmpty());
```

legacy 测试：

```cpp
const QString script = perf::PlanBuilder::renderGroupScript(group);
QVERIFY(!script.contains("$("));
QVERIFY(!script.contains("||"));
QVERIFY(!script.contains("awk"));
QVERIFY(script.contains("__"));
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --target perf_tests_test_perf_plan -j$(nproc)`
Expected: 编译失败。

- [ ] **Step 3: 实现能力模型**

```cpp
struct NodeCapability {
    QString path;
    QString semantic;
    QString adapter;
    bool exists = false;
    bool readable = false;
    QString transport; // adb/root-agent/adb-su/none
    QString detail;
};

struct CpuPolicy {
    int index = 0;
    QStringList cpus;
    QString curFreqPath, minFreqPath, maxFreqPath;
};

struct ThermalZone {
    int index = 0;
    QString type;
    QString tempPath;
};

struct CapabilitySet {
    QString serial;
    int sdk = 0;
    QString abi, hardware, gpuVendor, gpuRenderer;
    QVector<CpuPolicy> policies;
    QVector<ThermalZone> thermalZones;
    QMap<QString, NodeCapability> nodes;
    bool rootAgent = false;
    bool adbRoot = false;
    QStringList flowErrors;
};
```

`root_command_executor.h`：

```cpp
struct RootExecResult {
    bool ok = false;
    int exitCode = -1;
    bool timedOut = false;
    QString stdoutText;
    QString stderrText;
    QString error;
};

class IRootCommandExecutor {
public:
    virtual ~IRootCommandExecutor() = default;
    virtual bool available(const QString &serial) const = 0;
    virtual quint64 exec(const QString &serial, const QString &command,
                         int timeoutMs, bool quiet,
                         std::function<void(const RootExecResult&)> callback) = 0;
    virtual void cancel(quint64 requestId) = 0;
};
```

`parseCapabilities` 输入是按 key 索引的探测命令结果，key 在 Task 4-6 中固定为：`getprop`、`cpu_dirs`、`policy_dirs`、`thermal_list`、`thermal_types`、`gpu_signature`、`devfreq_list`、`node:<path>`。M0 只需要这些 key。解析时：

- `policy*/affected_cpus` 映射到 `CpuPolicy`；
- `devfreq` 名称包含 `mali`/`gpu`/`kgsl` 识别 GPU；
- `node:<path>` 的 `exitCode==0` 且输出非错误 => readable=true；输出含 `Permission denied` => readable=false, transport=`root-agent`（若 rootAgent）或 `adb-su`（若 adbRoot），否则 `none`。

- [ ] **Step 4: 实现 CapabilityProbe 的批处理探测**

`CapabilityProbe` 构造参数为 `CommandBus*`。`probe(serial, done)`：

1. 组装一个 ADB 批处理脚本，使用 `echo __key__` + `cat`/`ls`/`getprop`；
2. 通过 `CommandBus::submit` 提交一次；
3. 结果到达后按 marker 拆分，调用 `PlanBuilder::parseCapabilities`；
4. 超时/失败时仍然回调，`flowErrors` 包含原因，并保留已成功的 key；
5. 如果 `setRootExecutor()` 设置了可用服务，再提交一个 root 批次，对 GPU 候选路径执行 `cat`/`[ -r ]`，把结果写入 `node:<path>`，`transport="root-agent"`；
6. Root 服务不可用时不提交 root 批次，相关节点保留 `adb` 探测结果，并在 detail 中记录未验证原因。

固定 marker：

```text
__getprop__
__cpu_dirs__
__policy_dirs__
__thermal_list__
__thermal_types__
__gpu_signature__
__devfreq_list__
```

`CapabilityProbe` 不解析业务指标，只产出 `CapabilitySet`。

- [ ] **Step 5: 实现计划构建与脚本渲染**

```cpp
struct ReadSpec {
    QString key;
    QString metricId;
    QString path;
    QString parserId;
    QString transport;
};

struct CollectionGroup {
    QString id;
    QString transport;
    int cadenceMs = 1000;
    QVector<ReadSpec> reads;
    bool legacySafeBatch = true;
};

struct CollectionPlan {
    QVector<CollectionGroup> groups;
    QStringList warnings;
};
```

`build` 规则：

- CPU 频率组：每个 policy 一条 `ReadSpec`，cadence 500；
- GPU 频率组：按适配器选中的 `cur_freq`，cadence 500；
- 温度组：所有 zone 一条组，cadence 1000；
- 内存组：`/proc/meminfo`，cadence 1000。

`renderGroupScript`：

```sh
echo __key__
cat /path
```

不拼接用户输入；路径在计划构建时已经白名单化。

`splitBatch` 按 marker 把一次批处理输出切成每个 `ReadSpec.key` 的 `CommandResult`：

- 每个子结果继承原批次的 `sequence`、`exitCode`、`timedOut`、`stderrText`；
- `stdoutText` 只包含该 marker 到下一个 marker 之间的内容；
- 缺失 marker 的 read 返回空 stdout，由 collector 标 `Unavailable`；
- 不把 `cat:` 错误行写进值区，collector 负责识别 `Permission denied`/`No such file`。

- [ ] **Step 6: 运行测试**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R perf_tests_test_perf_plan --output-on-failure`
Expected: PASS。

- [ ] **Step 7: 提交**

```bash
git add src/core/perf/capability.h src/core/perf/capability.cpp \
  src/core/perf/capability_probe.h src/core/perf/capability_probe.cpp \
  src/core/perf/collection_plan.h src/core/perf/collection_plan.cpp \
  src/core/perf/root_command_executor.h \
  tests/test_perf_plan.cpp CMakeLists.txt
git commit -m "feat(perf): add capability model and collection plan"
```

---

## Task 4: CPU 频率采集器与解析器

**Files:**
- Create: `src/core/perf/collectors/collector.h`
- Create: `src/core/perf/collectors/cpu_freq_collector.h`
- Create: `src/core/perf/collectors/cpu_freq_collector.cpp`
- Create: `src/core/perf/parsers/cpu_freq_parser.h`
- Create: `src/core/perf/parsers/cpu_freq_parser.cpp`
- Create: `tests/test_perf_cpu_freq.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `perf::CapabilitySet`、`perf::ReadSpec`、`perf::CommandResult`
- Produces: `perf::Collector` 接口
- Produces: `perf::CpuFreqCollector::reads(const CapabilitySet&) -> QVector<ReadSpec>`
- Produces: `perf::CpuFreqCollector::parse(const CapabilitySet&, const QHash<QString, CommandResult>&, qint64 hostTimeMs, qint64 wallTimeMs) -> QVector<MetricSample>`

- [ ] **Step 1: 写失败的 CPU 频率测试**

```cpp
void policyFreqUsesPolicyPath();
void coreFallbackUsesCpuPath();
void hzKhzMhzAreNormalized();
void permissionDeniedIsNotZero();
void offlineCoreIsOffline();
void missingFrequencyIsNotSupported();
```

断言：

```cpp
QCOMPARE(samples.at(0).metricId, QStringLiteral("cpu.policy0.cur_freq_mhz"));
QCOMPARE(samples.at(0).value, 1800.0);
QCOMPARE(samples.at(0).state, perf::MetricState::Valid);

const auto denied = parseWith("Permission denied");
QCOMPARE(denied.at(0).state, perf::MetricState::PermissionDenied);
QVERIFY(!denied.at(0).hasValue());
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --target perf_tests_test_perf_cpu_freq -j$(nproc)`
Expected: 编译失败。

- [ ] **Step 3: 定义 Collector 接口**

```cpp
class Collector {
public:
    virtual ~Collector() = default;
    virtual QString id() const = 0;
    virtual int cadenceMs() const = 0;
    virtual QVector<ReadSpec> reads(const CapabilitySet &cap) const = 0;
    virtual QVector<MetricSample> parse(const CapabilitySet &cap,
                                        const QHash<QString, CommandResult> &raw,
                                        qint64 hostTimeMs,
                                        qint64 wallTimeMs) const = 0;
};
```

- [ ] **Step 4: 实现 CPU 频率采集**

`reads` 优先为每个 `CpuPolicy` 生成 `cpu.policy<N>.cur_freq`、`min_freq`、`max_freq`；没有 policy 时按 `cpu0..N` 生成 fallback。`parse`：

- 取原始数值；`>= 10000000` 按 Hz，`>= 10000` 按 kHz，否则按 MHz；
- `cpuN/online == 0` 生成 `cpu.coreN.online` 状态 `Offline`，不生成频率值；
- stderr 含 `Permission denied` -> `PermissionDenied`；
- 空/无法解析 -> `ParseError`；
- 没有对应读项 -> `NotSupported`。

指标 id：

```text
cpu.policy0.cur_freq_mhz
cpu.policy0.min_freq_mhz
cpu.policy0.max_freq_mhz
cpu.core0.online
```

- [ ] **Step 5: 运行测试**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R perf_tests_test_perf_cpu_freq --output-on-failure`
Expected: PASS。

- [ ] **Step 6: 提交**

```bash
git add src/core/perf/collectors/collector.h \
  src/core/perf/collectors/cpu_freq_collector.* \
  src/core/perf/parsers/cpu_freq_parser.* \
  tests/test_perf_cpu_freq.cpp CMakeLists.txt
git commit -m "feat(perf): add cpu frequency collector and parser"
```

---

## Task 5: GPU 频率适配器与采集器

**Files:**
- Create: `src/core/perf/vendor/gpu_adapter.h`
- Create: `src/core/perf/vendor/mali_adapter.h`
- Create: `src/core/perf/vendor/mali_adapter.cpp`
- Create: `src/core/perf/vendor/generic_devfreq_adapter.h`
- Create: `src/core/perf/vendor/generic_devfreq_adapter.cpp`
- Create: `src/core/perf/collectors/gpu_collector.h`
- Create: `src/core/perf/collectors/gpu_collector.cpp`
- Create: `src/core/perf/parsers/gpu_freq_parser.h`
- Create: `src/core/perf/parsers/gpu_freq_parser.cpp`
- Create: `tests/test_perf_gpu.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Collector`、`CapabilitySet`、`ReadSpec`
- Produces: `perf::GpuMetric`、`perf::GpuReadSpec`、`perf::GpuAdapter`
- Produces: `MaliAdapter::matches`、`reads`、`parse`
- Produces: `GenericDevfreqAdapter::matches`、`reads`、`parse`
- Produces: `GpuCollector`

- [ ] **Step 1: 写失败的 GPU 测试**

```cpp
void maliMatchesDevfreqName();
void maliCurrentFreqIsNotMaxFreq();
void availableFreqsIsNotCurrentFreq();
void unknownGpuHasNoMetrics();
void permissionDeniedPathIsNotZero();
void noUtilizationNodeMeansNotSupported();
```

关键断言：

```cpp
const auto samples = collector.parse(cap, raw, 1000, 1000);
const auto freq = findByMetric(samples, "gpu.cur_freq_mhz");
QCOMPARE(freq.value, 850.0);
QCOMPARE(freq.state, perf::MetricState::Valid);

const auto util = findByMetric(samples, "gpu.utilization_pct");
QCOMPARE(util.state, perf::MetricState::NotSupported);
```

即使 `max_freq` 是 1200、available list 第一项是 1200，当前频率仍必须是 `cur_freq` 的 850。

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --target perf_tests_test_perf_gpu -j$(nproc)`
Expected: 编译失败。

- [ ] **Step 3: 实现 GPU 适配器接口**

```cpp
enum class GpuMetric { Model, CurrentFreq, MinFreq, MaxFreq, AvailableFreqs,
                       Utilization, BusyTime, TotalTime, MemoryUsed };

struct GpuReadSpec {
    GpuMetric metric;
    QString path;
    QString key;
    QString transport;
};

class GpuAdapter {
public:
    virtual ~GpuAdapter() = default;
    virtual QString id() const = 0;
    virtual bool matches(const CapabilitySet &cap) const = 0;
    virtual QVector<GpuReadSpec> reads(const CapabilitySet &cap) const = 0;
};
```

M0 只实现 Mali 和通用 devfreq；Adreno/PowerVR/MTK 适配器在 M3 计划中加，但接口现在固定。

- [ ] **Step 4: 实现语义分离解析**

`MaliAdapter`：

- `cur_freq` -> `gpu.cur_freq_mhz`；
- `max_freq` -> `gpu.max_freq_mhz`；
- `min_freq` -> `gpu.min_freq_mhz`；
- `available_frequencies` -> `gpu.available_freqs_mhz`（Text/列表，不参与当前值）；
- `utilization`/`gpu_utilization` 节点存在才生成 `gpu.utilization_pct`；M0 解析器可以返回 `NotSupported`。

`GenericDevfreqAdapter` 匹配 `devfreq/*gpu*` 或 `*kgsl*`，只读 `cur_freq`/`max_freq`/`min_freq`，不推断利用率。

- [ ] **Step 5: 运行测试**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R perf_tests_test_perf_gpu --output-on-failure`
Expected: PASS。

- [ ] **Step 6: 提交**

```bash
git add src/core/perf/vendor src/core/perf/collectors/gpu_collector.* \
  src/core/perf/parsers/gpu_freq_parser.* tests/test_perf_gpu.cpp CMakeLists.txt
git commit -m "feat(perf): add semantic gpu frequency adapters"
```

---

## Task 6: 温度与内存采集器

**Files:**
- Create: `src/core/perf/collectors/thermal_collector.h`
- Create: `src/core/perf/collectors/thermal_collector.cpp`
- Create: `src/core/perf/collectors/memory_collector.h`
- Create: `src/core/perf/collectors/memory_collector.cpp`
- Create: `src/core/perf/parsers/thermal_parser.h`
- Create: `src/core/perf/parsers/thermal_parser.cpp`
- Create: `src/core/perf/parsers/meminfo_parser.h`
- Create: `src/core/perf/parsers/meminfo_parser.cpp`
- Create: `tests/test_perf_thermal_memory.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `Collector`
- Produces: `ThermalCollector`、`MemoryCollector`
- Produces: `ThermalParser::parseZone`、`MeminfoParser::parse`

- [ ] **Step 1: 写失败的测试**

```cpp
void thermalMillidegreeNormalized();
void thermalInvalidValueIsParseError();
void thermalOneZoneFailureDoesNotHideOthers();
void thermalTypesClassified();
void memAvailableUsedForUsedPercent();
void memWithoutAvailableFallsBack();
void swapPercentCorrect();
```

关键断言：

```cpp
QCOMPARE(zoneTemp("45000"), 45.0);
QCOMPARE(zoneTemp("-5000").state, perf::MetricState::ParseError);
QCOMPARE(mem.usedPct, 50.0); // 2GB total, 1GB available
QCOMPARE(swap.usedPct, 25.0);
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --target perf_tests_test_perf_thermal_memory -j$(nproc)`
Expected: 编译失败。

- [ ] **Step 3: 实现温度采集**

- `reads` 为每个 zone 生成 `thermal.zone<N>.temp`，同组 cadence 1000；
- `parse` 把毫摄氏度转摄氏度；`<-40` 或 `>200` -> `ParseError`；
- 类型分类：`cpu`/`tsens`/`ap`/`soc` -> CPU；`gpu`/`gpuss`/`kgsl` -> GPU；`bat`/`bms` -> 电池；`skin`/`board`/`case`/`back` -> 外壳；其他 Other；
- 生成 `thermal.zone<N>.temp_c`，以及概览用的 `thermal.cpu.max_temp_c` 等。

- [ ] **Step 4: 实现内存采集**

`MeminfoParser::parse(const QString&)` 返回：

```cpp
struct Meminfo {
    qint64 totalBytes = 0, availableBytes = 0, freeBytes = 0;
    qint64 buffersBytes = 0, cachedBytes = 0;
    qint64 swapTotalBytes = 0, swapFreeBytes = 0;
    bool hasAvailable = false;
};
```

- `MemTotal - MemAvailable` 计算已用；
- 没有 `MemAvailable` 用 `MemFree + Buffers + Cached`，并设置 `detail`；
- Swap 用 `SwapTotal - SwapFree`；
- 生成 `mem.total_bytes`、`mem.available_bytes`、`mem.used_pct`、`swap.used_pct`。

- [ ] **Step 5: 运行测试**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R perf_tests_test_perf_thermal_memory --output-on-failure`
Expected: PASS。

- [ ] **Step 6: 提交**

```bash
git add src/core/perf/collectors/thermal_collector.* \
  src/core/perf/collectors/memory_collector.* \
  src/core/perf/parsers/thermal_parser.* \
  src/core/perf/parsers/meminfo_parser.* \
  tests/test_perf_thermal_memory.cpp CMakeLists.txt
git commit -m "feat(perf): add thermal and memory collectors"
```

---

## Task 7: 共享 Root 命令服务

**Files:**
- Consume: `src/core/perf/root_command_executor.h`（Task 3 已创建）
- Create: `src/core/root_agent/root_command_service.h`
- Create: `src/core/root_agent/root_command_service.cpp`
- Create: `src/core/perf/root_bus_transport.h`
- Create: `src/core/perf/root_bus_transport.cpp`
- Modify: `src/core/root_agent/root_agent_client.h`
- Modify: `src/core/root_agent/root_agent_client.cpp`
- Modify: `src/core/root_agent/root_shell.h`
- Modify: `src/core/root_agent/root_shell.cpp`
- Modify: `src/ui/root_agent_manager_panel.cpp`
- Create: `tests/test_root_command_service.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `perf::RootExecResult`、`perf::IRootCommandExecutor`（Task 3）
- Produces: `RootCommandService`（实现上述接口）
- Produces: `RootBusTransport : ICommandTransport`（把 `IRootCommandExecutor` 适配给 CommandBus）
- Produces: `RootAgentClient::execShellAsync(quint64 requestId, const QString&, int, bool)`、signal `execFinishedFor(quint64, int, const QString&, const QString&, bool)`
- Produces: `RootShell::registerAsyncExecutor(serial, IRootCommandExecutor*)`、`asyncExecutor(serial)`、`unregisterAsyncExecutor(serial)`

- [ ] **Step 1: 写失败的 Root 服务测试**

`tests/test_root_command_service.cpp` 启动 `agent/build/phone_agent`（复用 `test_root_agent_client` 的启动方式），然后：

```cpp
void queuedCommandsRunInOrder();
void requestIdMatchesResult();
void timeoutLateResultIsIgnored();
void disconnectFailsPendingRequests();
void quietExecDoesNotEmitLog();
```

断言：

```cpp
QSignalSpy spy(&client, &RootAgentClient::execFinishedFor);
service.exec(serial, "echo one", 2000, true, [&](const perf::RootExecResult &r) {
    QCOMPARE(r.stdoutText.trimmed(), QStringLiteral("one"));
});
service.exec(serial, "echo two", 2000, true, [&](const perf::RootExecResult &r) {
    QCOMPARE(r.stdoutText.trimmed(), QStringLiteral("two"));
});
QTRY_COMPARE(doneCount, 2);
```

测试文件需要把 `root_agent_client.cpp`、`root_command_service.cpp` 编入目标，并链接 `Qt6::Network root_agent_crypto`。

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --target perf_tests_test_root_command_service -j$(nproc)`
Expected: 编译失败。

- [ ] **Step 3: 扩展 RootAgentClient**

在现有 `execShell` 旁边增加：

```cpp
void execShellAsync(quint64 requestId, const QString &command,
                    int timeoutMs, bool quiet);
bool isExecInProgress() const { return m_execInProgress; }
signals:
    void execFinishedFor(quint64 requestId, int exitCode,
                         const QString &stdoutText,
                         const QString &stderrText, bool timedOut);
```

实现要求：

- `execShell()` 调用 `execShellAsync(++m_nextExecId, ..., false)`；
- 保存 `m_execRequestId` 和 `m_execQuiet`；
- 在 `execFinished` 的 emit 位置同时 emit `execFinishedFor`；
- 超时、断开、错误也 emit `execFinishedFor`；
- `m_execInProgress` 仍然一次只允许一条；被拒绝时 emit 一个带 `requestId` 的错误结果，不要静默丢弃。

- [ ] **Step 4: 实现 RootCommandService**

```cpp
class RootCommandService : public QObject, public perf::IRootCommandExecutor {
    Q_OBJECT
public:
    explicit RootCommandService(QString serial, RootAgentClient *client,
                                QObject *parent = nullptr);
    bool available(const QString &serial) const override;
    quint64 exec(const QString &serial, const QString &command, int timeoutMs,
                 bool quiet,
                 std::function<void(const perf::RootExecResult&)> callback) override;
    void cancel(quint64 requestId) override;
private:
    void pump();
    struct Pending { quint64 id; QString command; int timeoutMs; bool quiet;
                     std::function<void(const perf::RootExecResult&)> callback; };
    QQueue<Pending> m_queue;
    quint64 m_nextId = 1;
    quint64 m_inFlight = 0;
    RootAgentClient *m_client = nullptr;
};
```

规则：

- 同一时间只提交一条给 client；
- `exec` 入队后 `pump()`；
- `execFinishedFor` 按 id 找 pending；找不到（已超时/取消）丢弃；
- `cancel` 在队列中移除；在途请求只标记丢弃，不强行杀进程；
- client `disconnected` 时把所有 pending 回调为 `RootExecResult{false, "root agent disconnected"}`；
- quiet 请求不调用 `RootAgentManagerPanel` 的日志连接。

- [ ] **Step 5: 实现 RootBusTransport**

```cpp
class RootBusTransport : public perf::ICommandTransport {
public:
    RootBusTransport(QString serial, perf::IRootCommandExecutor *executor);
    void setExecutor(perf::IRootCommandExecutor *executor);
    bool available() const override;
    void execute(const perf::CommandRequest &request,
                 std::function<void(const perf::CommandResult&)> done) override;
    void cancel(quint64 sequence) override;
};
```

`execute` 调 `executor->exec(serial, request.script, request.timeoutMs, request.quiet, ...)`，把 `RootExecResult` 转换为 `CommandResult`（`ok=false` 时映射为 `exitCode=-1` 和 `stderrText=error`）；`cancel` 调 `executor->cancel`。

- [ ] **Step 6: 接入 RootShell 与设置页**

`root_command_executor.h` 定义接口和 `RootExecResult`。`RootShell` 增加：

```cpp
void registerAsyncExecutor(const QString &serial, perf::IRootCommandExecutor *executor);
perf::IRootCommandExecutor *asyncExecutor(const QString &serial) const;
void unregisterAsyncExecutor(const QString &serial);
```

`RootAgentManagerPanel::clientFor()` 创建 client 后创建 `RootCommandService`，注册到 `RootShell`；析构/设备移除时注销。`RootShell::run()` 保持原样，不改现有同步调用方。

- [ ] **Step 7: 运行测试**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R "perf_tests_test_root_command_service|image_engine_tests_test_root_agent_client" --output-on-failure`
Expected: PASS；现有 root agent 测试不回归。

- [ ] **Step 8: 提交**

```bash
git add src/core/root_agent/root_command_service.* \
  src/core/perf/root_bus_transport.* \
  src/core/root_agent/root_agent_client.* \
  src/core/root_agent/root_shell.* \
  src/ui/root_agent_manager_panel.cpp \
  tests/test_root_command_service.cpp CMakeLists.txt
git commit -m "feat(perf): add shared async root command service"
```

---

## Task 8: 调度器与 PerformanceEngine

**Files:**
- Create: `src/core/perf/sample_scheduler.h`
- Create: `src/core/perf/sample_scheduler.cpp`
- Create: `src/core/perf/performance_engine.h`
- Create: `src/core/perf/performance_engine.cpp`
- Create: `tests/test_perf_engine.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `CommandBus`、`CollectionPlan`、`Collector`、`SeriesStore`、`IRootCommandExecutor`
- Produces: `SampleScheduler::setPlan`、`start`、`pause`、`stop`、`setVisible`、`setWindowMinimized`
- Produces: `PerformanceEngine::setDevice`、`setRootExecutor`、`refreshCapabilities`、`start`、`pause`、`clear`、`store`、signal `batchReady`
- Produces: `PerformanceEngine::addCollector(std::unique_ptr<Collector>)`
- Consumes: `CapabilityProbe`、`PlanBuilder`

- [ ] **Step 1: 写失败的调度器/引擎测试**

```cpp
void schedulerRunsGroupsAtCadence();
void schedulerSkipsOverlappingGroup();
void schedulerMarksStaleAfterMissedWindow();
void schedulerPausesWhenHidden();
void engineDiscardsResultsFromPreviousDevice();
void engineStoresOnlyValidSamples();
void engineBacksOffAfterFailures();
void engineReprobesWhenRootConnects();
```

`engineReprobesWhenRootConnects`：第一次 probe 时 GPU 节点是 `PermissionDenied`；调用 `setRootExecutor(fakeRoot)` 后，引擎重新 probe，假 Root 返回可读频率，最终 `gpu.cur_freq_mhz` 进入 `Valid`。

断言：

```cpp
scheduler.setNowForTest(0);
scheduler.start();
QCOMPARE(firedGroups().count("cpu"), 1);
advance(500);
QCOMPARE(firedGroups().count("cpu"), 2);

engine.setDevice(deviceA);
engine.start();
// 提交一个旧设备结果
engine.onCommandFinished(oldSequence, result);
QVERIFY(engine.store()->samples("cpu.policy0.cur_freq_mhz").isEmpty());
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --target perf_tests_test_perf_engine -j$(nproc)`
Expected: 编译失败。

- [ ] **Step 3: 实现 SampleScheduler**

用 `QElapsedTimer` 和 `QTimer`；每个组记录 `nextDueMs`、`inFlight`、`consecutiveFailures`。规则：

- 到点且不在途 -> 触发；
- 在途 -> 跳过，组状态 `Stale`；
- 失败 -> 退避 `1x, 2x, 4x`，上限 30s；
- `setVisible(false)`：核心组 1s，FPS/进程停止；
- `setWindowMinimized(true)`：核心组 2s；
- `pause` 停止定时器，不丢历史。

M0 不实现 FPS/进程组，但接口保留。

- [ ] **Step 4: 实现 PerformanceEngine**

```cpp
class PerformanceEngine : public QObject {
    Q_OBJECT
public:
    explicit PerformanceEngine(perf::CommandBus *bus, QObject *parent = nullptr);
    void addCollector(std::unique_ptr<perf::Collector> collector);
    void setDevice(const DeviceInfo &info);
    void setRootExecutor(perf::IRootCommandExecutor *executor);
    void setPlan(const perf::CollectionPlan &plan);
    void start();
    void pause();
    void clear();
    perf::SeriesStore *store();
signals:
    void batchReady(const perf::MetricBatch &batch);
    void stateChanged(bool running);
};
```

规则：

- 引擎拥有 `CapabilityProbe`；`setDevice()` 清空旧计划并异步 probe，完成后 `PlanBuilder::build()`，把计划交给 scheduler；
- `setRootExecutor()` 保存共享 Root 服务并触发一次新的 probe，用于恢复 root-only GPU 节点；
- `SystemToolPanel` 在构造引擎后注册 `CpuFreqCollector`、`GpuCollector`、`ThermalCollector`、`MemoryCollector`；
- 设备切换时 `clear()`：取消 bus 中本引擎提交的请求、清空 store、递增 `m_generation`；
- `CommandResult` 携带的 sequence 必须属于当前 generation，否则丢弃；
- scheduler 到期时 engine 对每个 `CollectionGroup` 调 `PlanBuilder::renderGroupScript()`，通过 `CommandBus::submit()` 提交，并记录 `sequence -> (group, generation)`；
- `CommandBus::finished` 到达时，若 sequence 不属于当前 generation 直接丢弃；否则 `PlanBuilder::splitBatch()` 得到每个 read 的 `CommandResult`，交给对应 collector 的 `parse()`；
- 每个 collector 的 `parse` 结果合并为 `MetricBatch`，写入 store，再 emit；
- 任何 collector 抛错/解析失败只记录 `collectorErrors`，不影响其他 collector；
- `RootShell::run()` 不在引擎中使用。

- [ ] **Step 5: 运行测试**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R perf_tests_test_perf_engine --output-on-failure`
Expected: PASS。

- [ ] **Step 6: 提交**

```bash
git add src/core/perf/sample_scheduler.* src/core/perf/performance_engine.* \
  tests/test_perf_engine.cpp CMakeLists.txt
git commit -m "feat(perf): add scheduler and performance engine"
```

---

## Task 9: 时间序列图表

**Files:**
- Create: `src/ui/performance/time_series_model.h`
- Create: `src/ui/performance/time_series_model.cpp`
- Create: `src/ui/performance/time_series_chart_widget.h`
- Create: `src/ui/performance/time_series_chart_widget.cpp`
- Create: `tests/test_time_series_chart.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `perf::MetricSample`、`perf::MetricDescriptor`
- Produces: `perf::TimeSeriesModel::append(const MetricDescriptor&, const MetricSample&)`、`segments(qint64 fromMs, qint64 toMs)`、`mapToRect(const MetricSample&, const QRectF&, qint64, qint64)`、`minValue`、`maxValue`、`lastValid`
- Produces: `perf::TimeSeriesChartWidget::setSeries`、`setWindowMs`、`setUnit`

- [ ] **Step 1: 写失败的图表模型测试**

```cpp
void invalidSampleBreaksLine();
void timestampsMapToXAxis();
void windowZoomDoesNotCompressTime();
void minMaxIgnoreInvalidSamples();
void emptySeriesPaintsNoLine();
```

断言：

```cpp
model.append(desc, sampleAt(1000, 800.0));
model.append(desc, gapAt(1500));
model.append(desc, sampleAt(2000, 900.0));
const auto segments = model.segments(1000, 2000);
QCOMPARE(segments.size(), 2);
QCOMPARE(segments.at(0).size(), 1);
QCOMPARE(segments.at(1).size(), 1);

const auto p = model.mapToRect(sampleAt(1500, 850.0), QRectF(0, 0, 1000, 100), 1000, 2000);
QCOMPARE(p.x(), 500.0, 0.1);
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --target perf_tests_test_time_series_chart -j$(nproc)`
Expected: 编译失败。

- [ ] **Step 3: 实现 TimeSeriesModel**

- 保存 `QVector<perf::MetricSample>`；
- `segments` 按时间排序，遇到非 `Valid` 样本切段；
- `mapToRect` 使用 `(time-from)/(to-from)`，不按点索引；
- `minValue`/`maxValue` 只统计 `Valid`；
- 空窗口返回空。

- [ ] **Step 4: 实现 TimeSeriesChartWidget**

`QWidget`，绘制网格、真实时间轴标签、分段折线和无效状态提示；`setWindowMs` 控制最近窗口；不依赖 `LiveChartWidget`，保留旧控件给其他页面。

- [ ] **Step 5: 接入 CMake**

该测试需要 `Qt6::Widgets`，并在 `set_tests_properties` 中设置 `QT_QPA_PLATFORM=offscreen`。

- [ ] **Step 6: 运行测试**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R perf_tests_test_time_series_chart --output-on-failure`
Expected: PASS。

- [ ] **Step 7: 提交**

```bash
git add src/ui/performance/time_series_model.* \
  src/ui/performance/time_series_chart_widget.* \
  tests/test_time_series_chart.cpp CMakeLists.txt
git commit -m "feat(perf): add timestamped time series chart"
```

---

## Task 10: PerformancePage 与 SystemToolPanel 迁移

**Files:**
- Create: `src/ui/performance/performance_page.h`
- Create: `src/ui/performance/performance_page.cpp`
- Create: `tests/test_performance_page.cpp`
- Modify: `src/ui/system_tool_panel.h`
- Modify: `src/ui/system_tool_panel.cpp`
- Modify: `src/ui/main_window.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `PerformanceEngine`、`TimeSeriesChartWidget`、`DeviceInfo`
- Produces: `PerformancePage(PerformanceEngine*, QWidget*)`、`setDeviceInfo`、`setRootAgentState`、`startMonitoring`、`pauseMonitoring`、`saveReport`
- Produces: signal `PerformancePage::outputMessage(QString,bool)`
- Produces: `SystemToolPanel::setRootAgentState(serial, connected, isRoot)`
- Consumes: `perf::PerformanceEngine`、`perf::CommandBus`、`perf::AdbTransport`、`RootShell::asyncExecutor`

- [ ] **Step 1: 写失败的页面测试**

```cpp
void startPauseUpdatesEngineState();
void deviceChangeForwardsToEngine();
void invalidMetricShowsReasonNotZero();
```

测试用 `CommandBus` + 空的 collectors，避免真实设备。

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --target perf_tests_test_performance_page -j$(nproc)`
Expected: 编译失败。

- [ ] **Step 3: 实现 PerformancePage**

构造函数：

```cpp
explicit PerformancePage(perf::PerformanceEngine *engine,
                         QWidget *parent = nullptr);
```

页面不拥有引擎；引擎由 `SystemToolPanel` 创建并保证生命周期长于页面。

布局：

- 顶部状态：设备、Root 代理、实际采样间隔、错误数；
- 开始/暂停按钮；
- CPU 频率、GPU 频率、温度、RAM/Swap 四个图表；
- 诊断折叠区：每个指标的来源、状态、detail；
- 导出按钮。

页面只通过引擎 API 操作，不直接执行 shell。

- [ ] **Step 4: 迁移 SystemToolPanel**

- `SystemToolPanel` 持有 `perf::CommandBus *m_perfBus`、`perf::PerformanceEngine *m_perfEngine` 和 `PerformancePage *m_performancePage`；
- 构造时注册 ADB transport（`AdbEmbedded::instance().getAdbPath()`）和 `RootBusTransport`；注册四个 M0 collector；
- `setDeviceInfo` 时更新 serial/adb 路径，调用 `m_perfEngine->setDevice(info)`；
- `setRootAgentState(serial, connected, isRoot)` 从 `RootShell::instance().asyncExecutor(serial)` 取共享 Root 服务，更新 `RootBusTransport`，并传给引擎；没有服务时传 `nullptr`；
- 在分类 0 的旧图表区位置放置 `PerformancePage`；
- 保留 CPU Governor、内存优化、I/O 调度按钮和 `m_perfStatus` 状态标签；
- 删除 `m_perfTimer`、`onPerfTimerToggle`、`onPerformancePoll`、`startMonitorProcess`、`stopMonitorProcess`、`monitorExec`、`probeGpuFrequencyPath`、`readGpuNode`、旧 GPU/温度/内存探测和 `m_cpuFullHistory` 等历史字段；
- `setDeviceInfo` / `clearDeviceInfo` 转发给页面；
- `onSaveChart` 改为调用 `m_performancePage->saveReport()`；
- `MainWindow::updateCurrentDeviceInfo()` 必须调用 `m_systemToolPanel->setDeviceInfo/clearDeviceInfo`（当前它只更新三个面板）；
- `MainWindow::onRootAgentStateChanged` 通过 `SystemToolPanel::setRootAgentState` 转发给页面。

验证旧符号已移除：

Run: `grep -n "onPerformancePoll\|monitorExec\|probeGpuFrequencyPath\|readGpuNode" src/ui/system_tool_panel.cpp`
Expected: 无输出。

- [ ] **Step 5: 接入 CMake**

`test_performance_page` 链接 `perf_core Qt6::Test Qt6::Widgets`，设置 offscreen 环境。

- [ ] **Step 6: 运行测试与构建**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R "perf_tests_test_performance_page|image_engine_tests_test_resource_monitor" --output-on-failure`
Expected: PASS。

- [ ] **Step 7: 提交**

```bash
git add src/ui/performance/performance_page.* \
  src/ui/system_tool_panel.* src/ui/main_window.cpp \
  tests/test_performance_page.cpp CMakeLists.txt
git commit -m "feat(perf): migrate performance page to engine"
```

---

## Task 11: 导出真实时间戳与 M0 验证

**Files:**
- Modify: `src/ui/performance/performance_page.cpp`
- Modify: `src/core/perf/metric_store.h/.cpp`（如导出辅助放在 store）
- Create: `tests/test_perf_export.cpp`
- Modify: `CMakeLists.txt`
- Modify: `README.md`（性能监控说明）

**Interfaces:**
- Consumes: `SeriesStore`、`MetricDescriptor`
- Produces: `perf::SeriesStore::exportCsv(const QString &path) -> bool`
- Produces: `perf::SeriesStore::exportSummary() -> perf::ExportSummary`（start/end、min/avg/max interval、gaps）
- Produces: `PerformancePage::saveReport(const QString &path)`

- [ ] **Step 1: 写失败的导出测试**

```cpp
void csvContainsRealTimestamps();
void summaryUsesActualInterval();
void invalidSamplesAreRowsWithState();
```

断言：

```cpp
store.append(desc, sampleAt(1000, 800.0));
store.append(desc, sampleAt(2500, 900.0));
const auto summary = store.exportSummary();
QCOMPARE(summary.minIntervalMs, qint64(1500));
QCOMPARE(summary.maxIntervalMs, qint64(1500));
QVERIFY(summary.durationMs >= 1500);
```

CSV 每行包含 `hostTimeMs,wallTimeMs,metricId,state,value,unit,detail`；无效样本 `value` 为空，`state` 非 `Valid`。

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --target perf_tests_test_perf_export -j$(nproc)`
Expected: 编译失败。

- [ ] **Step 3: 实现 CSV 与摘要**

按时间排序输出所有指标；文件头写设备信息由 `PerformancePage` 负责；存储层只输出数据。摘要统计相邻有效样本的间隔，缺口数为非 `Valid` 样本数。

- [ ] **Step 4: 实现 PNG 头部**

`PerformancePage::saveReport` 生成 PNG 时头部使用：

```text
设备 | Root 状态 | 开始/结束时间 | 实际间隔 min/avg/max | 采样点 | 缺口
```

删除 `总点数 * 0.5` 的推算。

- [ ] **Step 5: 全量验证**

Run:

```bash
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Expected: 现有 80 个测试 + 新增 `perf_tests_*` 全部通过。

- [ ] **Step 6: 真机/模拟器手工验证**

在至少一台 ADB 设备上：

1. 打开性能页，开始监控；
2. 确认界面不冻结，状态栏显示实际间隔；
3. 确认 CPU 频率、GPU 频率、温度、RAM/Swap 有数据或明确不可用原因；
4. 暂停 3 秒再恢复，确认时间轴没有按点数压缩；
5. 导出 PNG，确认头部时间/间隔不是固定 0.5s；
6. 切换设备，确认旧设备历史清空、在途结果不写入新设备。

- [ ] **Step 7: 提交**

```bash
git add src/ui/performance/performance_page.cpp \
  src/core/perf/metric_store.* tests/test_perf_export.cpp \
  CMakeLists.txt README.md
git commit -m "feat(perf): export timestamped performance reports"
```

---

## 自检记录

- **Spec coverage（M0）**：设计文档 §3 原则、§4 架构、§5 数据模型、§6 能力/计划、§7 CommandBus/Root 服务、§8.1/8.4/8.5/8.6 的 M0 部分、§9 调度、§10 历史、§11 UI、§12.1 迁移、§13 测试、§17 验收均有对应任务。
- **未覆盖（后续计划）**：M1 CPU 使用率/policy 完整模型、M2 PSI/功耗、M3 GPU 利用率/Adreno/PowerVR/MTK 适配器、M4 FPS/网络/存储、M5 进程级与设备侧采样会话。
- **类型一致性**：`MetricSample`/`MetricState`/`SeriesStore`/`CommandBus`/`CollectionPlan`/`Collector` 的命名在各任务中保持一致。
- **Review Focus 对应**：迟到结果 -> Task 2/7；取消回调 -> Task 2/7；无效样本断线 -> Task 1/9；设备切换在途结果 -> Task 8；legacy 语法 -> Task 3/4。
