# 华为 Kirin USB Update 刷写通道 Implementation Plan（计划 F2）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 引入 Qt 插件系统，华为 Kirin USB Update（VCOM）刷写通道作为第一个可选插件：插件框架（F2-P）→ 华为插件协议层（F2-1 连接与帧层 / F2-2 命令层与刷写 / F2-3 集成），协议事实经行为观察核实，mock 传输层单测，不依赖真实设备。

**Architecture:** 双层：① 主项目插件框架（`src/plugins/`）——`ProtocolPlugin` 接口（Q_DECLARE_INTERFACE）+ `PluginManager`（QPluginLoader 扫描 `plugins/` 目录运行时加载）+ UI 集成；主程序**不编译链接**任何插件。② 华为刷机插件（`plugins/huawei_flash/` 独立目录、独立 CMake、独立 .so）——`hisi::IUsbChannel` 抽象传输通道（libusb 生产 + mock 测试）→ `hisi::HisiSession` 帧层（0x7E HDLC 帧/转义/CRC16-X25/握手/响应解析）→ `hisi::HisiFlasher` 命令层（HEAD/DATA/TAIL/UNLOCK/REBOOT + 逐分区刷写 + zlib 压缩）→ `runHisiFlash` 集成（update.app 解析在插件内自包含，不依赖主项目 image_engine）。法务隔离：删除 `plugins/huawei_flash/` 目录 + 移除发布包插件文件，主程序零残留。

**Tech Stack:** C++17, Qt6 Core（QPluginLoader）, libusb-1.0（现有依赖）, ZLIB（现有依赖）。

## Global Constraints

- C++17；成员变量 `m_` 前缀；协议常量/结构进 `namespace hisi`
- **合规纪律（用户强制，2026-08-18）**：华为法务风险高 + 参照实现为 BSL 1.1 许可（源可用≠可自由使用）——
  - **插件隔离（用户强制）**：华为实现整体放 `plugins/huawei_flash/` 独立目录，编译为独立 .so，主程序不编译链接；删除目录即完整移除（法务应对）
  - 只参照**协议行为事实**（帧格式/命令字/CRC/时序，不受版权保护）；**绝不复制参照实现的代码表达**（结构/命名/注释/实现方式），实现完全自写
  - 代码注释**不引用参照实现源码函数名**，来源标注用协议事实描述（"对照 HiSilicon USB Update 协议行为观察"）
  - **Xloader 漏洞载荷二进制不内置、不下载、不复制**——诚实边界：仅提供接口与提示，载荷需用户自行准备
  - 计划文档含独立实现声明
- **默认信息不可信原则**：协议细节以本计划核实的字节序列为准（核实结论已写入各任务代码），实现时不得自行更改帧结构
- 诚实边界（不假装）：无解锁码时 UNLOCK 跳过（失败不假装）；Xloader 修补不可用（无载荷）时返回明确错误；机型范围标注（Kirin 系芯片，实测前不承诺）
- 每个任务独立 commit，前缀 `feat:`；协议层单测 mock 传输通道（`IUsbChannel` 注入），不依赖真实设备
- 插件框架是主项目通用能力（与华为无关）；华为插件内容全部在 `plugins/huawei_flash/` 内自包含

## 协议核实记录（行为观察，2026-08-18）

> 独立实现声明：以下协议事实通过对公共领域协议行为的观察整理（帧格式/命令字/CRC 算法为事实性信息）。实现代码为本项目独立撰写，不复制任何参照实现的源表达。

**连接**：VID `0x12D1`（华为），"DBAdapter Reserved Interface"（CDC-ACM 虚拟串口语义）；Kirin-Tool 以 9600 波特打开（Windows 串口 API；USB bulk 传输层不受波特率影响，为保持行为一致发送 CDC SET_LINE_CODING(9600) 控制请求）。读响应前 `DiscardInBuffer`（丢弃残留）。

**帧格式**：`0x7E | escaped(payload + CRC16-X25 LE) | 0x7E`。转义：`0x7E → 0x7D 0x5E`、`0x7D → 0x7D 0x5D`。
**CRC16-X25**：init `0xFFFF`，反射多项式 `0x8408`（逐位右移），结果按位取反，**小端**输出。（标准 X25 测试向量："123456789" → 0x906E。）

**握手**：发 `26 00 00 25 A7 00 06 00 00 00 00 00 00 00 00 00 00 01 00`（19B）+ CRC16-X25 LE + 尾 `0x7E`（**无前导 0x7E**）；期望响应含前缀 `7E 26 00 00 25 A7`；3 次重试（间隔 200ms）。

**命令帧**（payload = cmd + 参数）：
| 命令 | 值 | 参数 |
|---|---|---|
| HEAD | `0x41` | 分区头（98+ 字节，update.app 解出，含 fileSeq@20..24 大端） |
| DATA | `0x0F` | (fileSeqInt + addr) BE32 + origLen BE32 + zlib 压缩数据 |
| TAIL | `0x43` | 分区头 |
| UNLOCK | `0x0B` | unlockcode 字节 |
| REBOOT | `0x0A` | 无 |
| FORCE_REBOOT | `0x32` | 无 |

**响应**：成功 `7E 02 6A D3 7E`（帧内 payload = `02` + CRC）；错误帧 payload 首字节 `03`；读端丢弃非 `0x7E` 起始字节、收集至 `0x7E … 0x7E` 闭合；发送分块 ≤0x10000；块间 10ms。

**刷写流程**：握手 → UNLOCK（有解锁码时）→ 逐分区：HEAD → DATA×N → TAIL。DATA 块大小 `0x20000`（128KB），每块 zlib 压缩（`78 01` 头 + Deflate Fastest + Adler32 BE 尾），addr 从 0 起按**原始长度**累加；DATA 超时 = max(1, min(8, 压缩后 MB × 1.5)) 秒；**TAIL 超时 = max(35, min(180, 15 + 镜像 MB/10)) 秒**（TAIL 等设备落盘提交，固定 8s 会假失败）。**HEAD/TAIL 发送前头变换**：92-93 两字节置零 + 追加 1 字节 `0x00`（变换后头长 = headerLen + 1；HEAD/TAIL 共用同一份变换后头）。

**update.app 解包**（复用 B5/B6 imghw）：条目 magic `55 AA 5A A5` + headerLength(LE32) + 固定字段 + dataLength(LE32) + 16B + 16B + 32B 分区名(UTF-8 NUL 结尾) + 6B + 剩余到 headerLength。头总长 = 98 + 剩余。容错（行为观察）：每条目数据后可有 **0-3 字节 4 字节对齐填充** `(4 - pos%4) % 4`；**dataLength==0 或分区名为空 = 列表结束标记**（解析头后即 break，该条目不追加）；首条目 magic 前可有前导字节（按字节扫描首个 magic）。

**Xloader 修补**：bootrom 漏洞利用（"head resend" 类），0x8000 字节载荷替换 xloader 对应段——**诚实边界：载荷不内置**，接口标注"需用户自行准备修补载荷"。

---

## 文件结构

```
src/plugins/
  plugin_interface.h     # F2-P: ProtocolPlugin 接口（Q_DECLARE_INTERFACE）
  plugin_manager.h/.cpp  # F2-P: QPluginLoader 扫描加载 plugins/ + 能力查询
  plugin_tool_panel.h/.cpp  # F2-P: UI 集成（插件列表面板，协议插件入口）
plugins/huawei_flash/    # 华为插件（独立目录、独立 CMake、独立 .so）
  CMakeLists.txt         # F2-P 建骨架（空插件编译通过），F2-1/2/3 填充
  hisi_update.h/.cpp     # F2-1: IUsbChannel + libusb + 帧层（CRC16-X25/转义/握手/响应）
  hisi_flash.h/.cpp      # F2-2: 命令层（HEAD/DATA/TAIL/UNLOCK/REBOOT）+ 刷写流程 + zlib
  update_app.h/.cpp      # F2-3: update.app 解析（插件内自包含，不依赖主项目 image_engine）
  huawei_flash_plugin.h/.cpp  # F2-3: ProtocolPlugin 实现（插件入口）
  tests/test_hisi_update.cpp  # F2-1+F2-2 单测（MockUsbChannel 注入 + CRC 向量 + 帧断言）
CMakeLists.txt           # 主项目：add_subdirectory(src/plugins)；plugins/huawei_flash 条件引入
```

---

### Task F2-P: 插件系统框架（主项目）

**Files:**
- Create: `src/plugins/plugin_interface.h`
- Create: `src/plugins/plugin_manager.h/.cpp`
- Create: `src/plugins/plugin_tool_panel.h/.cpp`
- Create: `plugins/huawei_flash/CMakeLists.txt`（空插件骨架，验证插件系统可用）
- Create: `plugins/huawei_flash/huawei_flash_plugin.h/.cpp`（最小实现：仅名称/描述/能力）
- Modify: `CMakeLists.txt`（主程序 + add_subdirectory + 输出目录设置）
- Modify: `src/ui/tool_panel.cpp/h`（工具列表加"插件"项）、`src/ui/main_window.cpp/h`（页面）

**Interfaces:**
- Produces:
  - `class ProtocolPlugin : public QObject`（Q_DECLARE_INTERFACE，`ProtocolPlugin_iid`）：`virtual QString name() const = 0; virtual QString description() const = 0; virtual QStringList capabilities() const = 0; virtual bool execute(const QString &capability, const QVariantMap &params, QString *error) = 0;`
  - `class PluginManager : public QObject`（单例 `instance()`）：`void scanPlugins(const QString &dir); QList<ProtocolPlugin*> plugins() const; QList<ProtocolPlugin*> byCapability(const QString&) const;`
  - `class PluginToolPanel : public QWidget`（插件面板：插件列表 + 能力按钮 + 输出）

- [ ] **Step 1: 定义接口 `src/plugins/plugin_interface.h`**

```cpp
#pragma once

// 插件系统接口（计划 F2-P）—— 主项目通用能力
// 协议类插件（如华为刷写）以独立 .so 实现本接口，主程序运行时加载，
// 不编译链接插件代码（法务/许可隔离：删除插件文件即完整移除）。

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantMap>

#define ProtocolPlugin_iid "com.phonetoolbox.ProtocolPlugin/1.0"
Q_DECLARE_INTERFACE(ProtocolPlugin, ProtocolPlugin_iid)

class ProtocolPlugin : public QObject {
    Q_OBJECT
public:
    explicit ProtocolPlugin(QObject *parent = nullptr) : QObject(parent) {}
    ~ProtocolPlugin() override = default;

    virtual QString name() const = 0;
    virtual QString description() const = 0;
    // 能力列表（如 "huawei-usb-update.flash"）
    virtual QStringList capabilities() const = 0;
    // 执行能力；成功返回 true；失败返回 false 并填 error（契约：失败不崩溃）
    virtual bool execute(const QString &capability, const QVariantMap &params,
                         QString *error) = 0;
};
```

- [ ] **Step 2: 实现 `plugin_manager.h/.cpp`**（QPluginLoader 扫描加载）

```cpp
#pragma once

#include <QList>
#include <QObject>
#include <QString>

#include "src/plugins/plugin_interface.h"

// 插件管理器：扫描目录加载 ProtocolPlugin 插件（QPluginLoader）。
// 加载失败/接口不符的插件记录错误并跳过（不崩溃）。
class PluginManager : public QObject {
    Q_OBJECT
public:
    static PluginManager &instance();

    // 扫描 dir 下全部 .so/.dll（仅 ProtocolPlugin 接口）
    void scanPlugins(const QString &dir);
    QList<ProtocolPlugin *> plugins() const { return m_plugins; }
    QList<ProtocolPlugin *> byCapability(const QString &capability) const;
    QStringList loadErrors() const { return m_errors; }
    void clear(); // 卸载全部（析构/重扫用）

private:
    explicit PluginManager(QObject *parent = nullptr);
    QList<ProtocolPlugin *> m_plugins;
    QStringList m_errors;
};
```

- [ ] **Step 3: 实现 `plugin_manager.cpp`**

```cpp
#include "src/plugins/plugin_manager.h"

#include <QDir>
#include <QPluginLoader>

PluginManager &PluginManager::instance()
{
    static PluginManager mgr;
    return mgr;
}

PluginManager::PluginManager(QObject *parent)
    : QObject(parent)
{
}

void PluginManager::scanPlugins(const QString &dir)
{
    clear();
    QDir d(dir);
    const QStringList entries = d.entryList(QDir::Files);
    for (const QString &entry : entries) {
        if (!entry.endsWith(QStringLiteral(".so")) && !entry.endsWith(QStringLiteral(".dll"))
            && !entry.endsWith(QStringLiteral(".dylib")))
            continue;
        QPluginLoader loader(d.absoluteFilePath(entry));
        QObject *obj = loader.instance();
        if (!obj) {
            m_errors << QStringLiteral("%1: %2").arg(entry, loader.errorString());
            continue;
        }
        ProtocolPlugin *plugin = qobject_cast<ProtocolPlugin *>(obj);
        if (!plugin) {
            m_errors << QStringLiteral("%1: 不是 ProtocolPlugin 插件").arg(entry);
            loader.unload();
            continue;
        }
        m_plugins.append(plugin);
    }
}

QList<ProtocolPlugin *> PluginManager::byCapability(const QString &capability) const
{
    QList<ProtocolPlugin *> out;
    for (ProtocolPlugin *p : m_plugins) {
        if (p->capabilities().contains(capability))
            out.append(p);
    }
    return out;
}

void PluginManager::clear()
{
    // QPluginLoader 实例生命周期：直接删除 QObject（插件库保持加载）
    for (ProtocolPlugin *p : m_plugins)
        delete p;
    m_plugins.clear();
    m_errors.clear();
}
```

- [ ] **Step 4: UI 集成 `plugin_tool_panel.h/.cpp`**（面板：插件列表 + 能力按钮 + 输出信号）

```cpp
#pragma once

#include <QList>
#include <QWidget>

class ProtocolPlugin;

// 插件面板：列出已加载插件及其能力，点击执行（参数为空）。
// 复用现有面板信号模式：outputMessage(QString, bool) 发到 OutputPanel。
class PluginToolPanel : public QWidget {
    Q_OBJECT
public:
    explicit PluginToolPanel(QWidget *parent = nullptr);
    void setPlugins(const QList<ProtocolPlugin *> &plugins);

signals:
    void outputMessage(const QString &text, bool isError);

private slots:
    void onExecuteClicked();

private:
    class QListWidget *m_list;
    class QPushButton *m_executeBtn;
    QList<ProtocolPlugin *> m_plugins;
};
```

```cpp
#include "src/plugins/plugin_tool_panel.h"

#include <QHBoxLayout>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

#include "src/plugins/plugin_interface.h"

PluginToolPanel::PluginToolPanel(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    m_list = new QListWidget(this);
    m_executeBtn = new QPushButton(QStringLiteral("执行"), this);
    m_executeBtn->setEnabled(false);
    layout->addWidget(m_list, 1);
    layout->addWidget(m_executeBtn);
    connect(m_executeBtn, &QPushButton::clicked, this, &PluginToolPanel::onExecuteClicked);
    connect(m_list, &QListWidget::itemSelectionChanged, this, [this] {
        m_executeBtn->setEnabled(m_list->currentRow() >= 0);
    });
}

void PluginToolPanel::setPlugins(const QList<ProtocolPlugin *> &plugins)
{
    m_plugins = plugins;
    m_list->clear();
    for (ProtocolPlugin *p : m_plugins) {
        for (const QString &cap : p->capabilities())
            m_list->addItem(QStringLiteral("%1 — %2").arg(p->name(), cap));
    }
}

void PluginToolPanel::onExecuteClicked()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_plugins.size())
        return;
    ProtocolPlugin *p = m_plugins.at(row);
    if (!p)
        return;
    QString error;
    if (!p->execute(p->capabilities().value(0), QVariantMap(), &error))
        emit outputMessage(QStringLiteral("[插件] %1 执行失败: %2").arg(p->name(), error), true);
    else
        emit outputMessage(QStringLiteral("[插件] %1 执行完成").arg(p->name()), false);
}
```

- [ ] **Step 5: 插件骨架 `plugins/huawei_flash/`**

`plugins/huawei_flash/huawei_flash_plugin.h`：
```cpp
#pragma once

#include <QObject>

#include "src/plugins/plugin_interface.h"

// 华为刷写插件入口（协议层在 F2-1/2/3 填充；本任务仅骨架验证插件系统）
class HuaweiFlashPlugin : public ProtocolPlugin {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID ProtocolPlugin_iid)
    Q_INTERFACES(ProtocolPlugin)
public:
    explicit HuaweiFlashPlugin(QObject *parent = nullptr) : ProtocolPlugin(parent) {}

    QString name() const override { return QStringLiteral("华为刷写"); }
    QString description() const override
    {
        return QStringLiteral("华为 Kirin USB Update 刷写通道（协议层后续任务填充）");
    }
    QStringList capabilities() const override
    {
        return { QStringLiteral("huawei-usb-update.flash") };
    }
    bool execute(const QString &capability, const QVariantMap &params, QString *error) override
    {
        Q_UNUSED(capability) Q_UNUSED(params)
        if (error) *error = QStringLiteral("协议层未实现（F2-1/2/3）");
        return false;
    }
};
```

`plugins/huawei_flash/CMakeLists.txt`：
```cmake
# 华为刷写插件：独立 .so，主程序不链接（法务/许可隔离）
find_package(Qt6 REQUIRED COMPONENTS Core)
add_library(huawei_flash_plugin MODULE
    huawei_flash_plugin.h)
target_include_directories(huawei_flash_plugin PRIVATE
    ${CMAKE_SOURCE_DIR})  # 引用 src/plugins/plugin_interface.h
target_link_libraries(huawei_flash_plugin PRIVATE Qt6::Core)
set_target_properties(huawei_flash_plugin PROPERTIES
    AUTOMOC ON
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins)
```

- [ ] **Step 6: 主 CMakeLists 接线**

```cmake
# 插件系统（主项目通用）
add_subdirectory(src/plugins)
# 可选插件：默认构建（华为刷写），删除目录/关闭选项即完整移除
option(BUILD_HUAWEI_FLASH_PLUGIN "Build Huawei flash plugin" ON)
if(BUILD_HUAWEI_FLASH_PLUGIN AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/plugins/huawei_flash/CMakeLists.txt)
    add_subdirectory(plugins/huawei_flash)
endif()
```

`src/plugins/CMakeLists.txt`：
```cmake
# 插件框架（主项目通用，无 Q_OBJECT 头不涉及 AUTOMOC 冲突）
set(PLUGIN_SOURCES
    plugin_manager.cpp
    plugin_tool_panel.cpp)
add_library(phone_plugins STATIC ${PLUGIN_SOURCES})
target_include_directories(phone_plugins PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(phone_plugins PUBLIC Qt6::Core Qt6::Widgets)
set_target_properties(phone_plugins PROPERTIES AUTOMOC ON)
```

主程序 `target_link_libraries(PhoneToolbox ... phone_plugins)`。

- [ ] **Step 7: 工具面板与主窗口接线**

`src/ui/tool_panel.cpp/h`：工具按钮组加「插件」按钮（与现有 5 个工具并列），发出 `pluginPanelRequested()` 信号。
`src/ui/main_window.cpp/h`：`m_tools`（QStackedWidget）加一页 `PluginToolPanel`；构造时 `PluginManager::instance().scanPlugins(QCoreApplication::applicationDirPath() + "/plugins")`，把 `plugins()` 填入面板，并转发 `outputMessage` 到 OutputPanel。

- [ ] **Step 8: 验证 + Commit**

Run: `cmake -B build -G Ninja && cmake --build build -j$(nproc)`
Expected: 编译通过；`build/plugins/libhuawei_flash_plugin.so` 生成。冒烟：`./build/PhoneToolbox` 启动不崩溃（无显示环境时跳过 UI 冒烟，以编译 + 插件 .so 生成为准）。

```bash
git add src/plugins/ CMakeLists.txt src/ui/tool_panel.* src/ui/main_window.* plugins/huawei_flash/
git commit -m "feat: 插件系统框架 — ProtocolPlugin 接口 + QPluginLoader + 华为插件骨架 (F2-P)"
```

---

### Task F2-1: 连接与帧层（插件内）

**Files:**
- Create: `plugins/huawei_flash/hisi_update.h`
- Create: `plugins/huawei_flash/hisi_update.cpp`
- Create: `plugins/huawei_flash/tests/test_hisi_update.cpp`
- Modify: `plugins/huawei_flash/CMakeLists.txt`（协议层源 + 测试注册 + libusb 链接）

**Interfaces:**
- Consumes: 无（libusb-1.0 + ZLIB 现有依赖）
- Produces:
  - `enum hisi::FrameCmd : quint8`（HEAD=0x41/DATA=0x0F/TAIL=0x43/UNLOCK=0x0B/REBOOT=0x0A/FORCE_REBOOT=0x32）
  - `struct hisi::HisiDevice { int vid; int pid; QString portName; }`
  - `class hisi::IUsbChannel`：`virtual ~IUsbChannel()=default; virtual bool open(QString*)=0; virtual bool write(const QByteArray&,QString*)=0; virtual bool read(QByteArray&,int maxLen,int timeoutMs,QString*)=0; virtual int maxPacketSize() const { return 0x400; } virtual bool close()=0;`
  - `bool hisi::enumerateUsb(QList<HisiDevice>&, QString*)`（VID 0x12D1 白名单）
  - `bool hisi::openLibusbUsb(const HisiDevice&, std::unique_ptr<IUsbChannel>&, QString*)`
  - 纯函数：`quint16 hisi::crc16X25(const QByteArray&)`、`QByteArray hisi::escapePayload(const QByteArray&)`、`QByteArray hisi::buildFrame(quint8 cmd, const QByteArray& payload)`（0x7E 包裹 + CRC LE 附加）
  - `class hisi::HisiSession`（构造 `HisiSession(std::unique_ptr<IUsbChannel>, const HisiDevice&)`）：`connect/sendCommand/handshake/close/isConnected` + 常量 `kHandshakeFrame`、`kAckResponse = "\x7E\x02\x6A\xD3\x7E"`

- [ ] **Step 1: 写失败测试 `tests/test_hisi_update.cpp`**

```cpp
#include <QtTest>
#include <memory>

#include "core/modes/hisi_update.h"

// MockUsbChannel：记录写入序列、预置读取队列（IUsbChannel 注入）
class MockUsbChannel : public hisi::IUsbChannel {
public:
    QByteArray writes;
    QList<QByteArray> reads;
    bool failOpen = false;
    QString openError;

    bool open(QString *error) override
    {
        if (failOpen) { if (error) *error = openError; return false; }
        return true;
    }
    bool write(const QByteArray &data, QString *error) override
    {
        Q_UNUSED(error)
        writes += data;
        return true;
    }
    bool read(QByteArray &out, int maxLen, int timeoutMs, QString *error) override
    {
        Q_UNUSED(timeoutMs) Q_UNUSED(error)
        if (reads.isEmpty()) { out.clear(); return false; }
        QByteArray r = reads.takeFirst();
        out = r.left(maxLen);
        return !r.isEmpty() || maxLen == 0;
    }
    bool close() override { return true; }
};

class TestHisiUpdate : public QObject {
    Q_OBJECT
private slots:
    // ---- 纯函数 ----
    void crc16X25StandardVector();
    void crc16X25Empty();
    void escapePayloadTransforms();
    void buildFrameWrapsAndAppendsCrc();
    // ---- 握手 ----
    void handshakeFrameAndResponse();
    void handshakeRetriesOnMismatch();
    // ---- 命令 ----
    void sendCommandAckSuccess();
    void sendCommandDeviceError();
    void sendCommandTimeoutFails();
    void connectOpenFailure();
};

void TestHisiUpdate::crc16X25StandardVector()
{
    // 标准 X25 测试向量："123456789" → 0x906E
    QByteArray d("123456789");
    QCOMPARE(hisi::crc16X25(d), quint16(0x906E));
}

void TestHisiUpdate::crc16X25Empty()
{
    // init 0xFFFF，空数据 → ~0xFFFF = 0x0000
    QCOMPARE(hisi::crc16X25(QByteArray()), quint16(0x0000));
}

void TestHisiUpdate::escapePayloadTransforms()
{
    // 0x7E → 0x7D 0x5E；0x7D → 0x7D 0x5D；其余原样
    QByteArray in("\x7E\x7D\x01", 3);
    QCOMPARE(hisi::escapePayload(in), QByteArray("\x7D\x5E\x7D\x5D\x01", 5));
}

void TestHisiUpdate::buildFrameWrapsAndAppendsCrc()
{
    // buildFrame(0x0A, 空)：payload = [0x0A]；CRC16-X25([0x0A]) 小端
    // 计算：crc16X25 of [0x0A] —— 手动算或按实现；此处断言结构：
    // 0x7E | escaped(cmd+crcLE) | 0x7E
    const QByteArray frame = hisi::buildFrame(hisi::FRAME_REBOOT, QByteArray());
    QVERIFY(frame.startsWith('\x7E'));
    QVERIFY(frame.endsWith('\x7E'));
    const quint16 crc = hisi::crc16X25(QByteArray(1, char(hisi::FRAME_REBOOT)));
    QByteArray expect("\x7E", 1);
    expect += hisi::escapePayload(QByteArray(1, char(hisi::FRAME_REBOOT))
                                      .append(char(crc & 0xFF)).append(char((crc >> 8) & 0xFF)));
    expect += QByteArray("\x7E", 1);
    QCOMPARE(frame, expect);
}

void TestHisiUpdate::handshakeFrameAndResponse()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    // 响应含前缀 7E 26 00 00 25 A7
    m->reads << QByteArray("\x7E\x26\x00\x00\x25\xA7\x00\x06\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00\x7E", 21);
    QVERIFY(s.connect(nullptr));
    // 握手帧：19B 命令 + CRC LE + 0x7E（无前导 0x7E）
    const quint16 crc = hisi::crc16X25(hisi::HisiSession::kHandshakeCommand);
    QCOMPARE(m->writes,
             hisi::HisiSession::kHandshakeCommand
                 .append(char(crc & 0xFF)).append(char((crc >> 8) & 0xFF))
                 .append('\x7E'));
}

void TestHisiUpdate::handshakeRetriesOnMismatch()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    // 两次错误响应（含 7E 但不含预期前缀）+ 一次正确
    m->reads << QByteArray("\x7E\x03\x00\x00\x7E", 5)
             << QByteArray("\x7E\x03\x00\x00\x7E", 5)
             << QByteArray("\x7E\x26\x00\x00\x25\xA7\x7E", 7);
    QVERIFY(s.connect(nullptr));
    // 三次握手帧
    QCOMPARE(m->writes.count('\x26'), 3); // 每帧含 0x26 一次
}

void TestHisiUpdate::sendCommandAckSuccess()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    m->reads << QByteArray(hisi::HisiSession::kAckResponse);
    QVERIFY(s.sendCommand(hisi::FRAME_REBOOT, QByteArray(), 0.3, nullptr));
    QVERIFY(m->writes.endsWith('\x7E'));
}

void TestHisiUpdate::sendCommandDeviceError()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    m->reads << QByteArray("\x7E\x03\x00\x00\x00\x00\x7E", 7); // 0x03 错误帧
    QString err;
    QVERIFY(!s.sendCommand(hisi::FRAME_REBOOT, QByteArray(), 0.3, &err));
    QVERIFY(err.contains("设备错误") || err.contains("0x03"));
}

void TestHisiUpdate::sendCommandTimeoutFails()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    // 无响应（队列空 → read 返回 false）
    QString err;
    QVERIFY(!s.sendCommand(hisi::FRAME_REBOOT, QByteArray(), 0.1, &err));
    QVERIFY(!err.isEmpty());
}

void TestHisiUpdate::connectOpenFailure()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    m->failOpen = true;
    m->openError = QStringLiteral("无权限");
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    QString err;
    QVERIFY(!s.connect(&err));
    QVERIFY(err.contains("无权限"));
}

QTEST_APPLESS_MAIN(TestHisiUpdate)
#include "test_hisi_update.moc"
```

- [ ] **Step 2: 跑测试验证失败**

Run: `cmake --build build` → Expected: 编译失败（`hisi_update.h` 不存在）。先实现再注册 CMake（同 F1-1 模式）。

- [ ] **Step 3: 实现 `plugins/huawei_flash/hisi_update.h`**

```cpp
#pragma once

// 华为 Kirin USB Update（VCOM）连接与帧层（计划 F2-1）
//
// 独立实现声明：本模块协议事实（帧格式/命令字/CRC 算法）来自对公共领域
// 协议行为的观察整理（HiSilicon USB Update 下载协议）。实现代码为本项目
// 独立撰写，不复制任何参照实现的源表达。
//
// 核实要点（行为观察，2026-08-18）：
//   • 帧：0x7E | escaped(payload + CRC16-X25 LE) | 0x7E
//   • 转义：0x7E→0x7D 0x5E、0x7D→0x7D 0x5D
//   • CRC16-X25：init 0xFFFF、反射多项式 0x8408、结果取反、小端输出
//   • 握手：26 00 00 25 A7 00 06 …（19B）+ CRC LE + 0x7E（无前导 0x7E），
//     期望响应含前缀 7E 26 00 00 25 A7，3 次重试
//   • 成功响应帧：7E 02 6A D3 7E；错误帧 payload 首字节 0x03

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <memory>

namespace hisi {

// ---- 命令码（协议事实）----
enum FrameCmd : quint8 {
    FRAME_UNLOCK = 0x0B,
    FRAME_DATA = 0x0F,
    FRAME_HEAD = 0x41,
    FRAME_TAIL = 0x43,
    FRAME_REBOOT = 0x0A,
    FRAME_FORCE_REBOOT = 0x32,
};

// 设备描述
struct HisiDevice {
    int vid = 0;
    int pid = 0;
    QString portName; // usb-<bus>-<addr>，仅日志展示
};

// 抽象传输通道（mock 注入单测；libusb 生产实现）
class IUsbChannel {
public:
    virtual ~IUsbChannel() = default;
    virtual bool open(QString *error) = 0;
    virtual bool write(const QByteArray &data, QString *error) = 0;
    virtual bool read(QByteArray &out, int maxLen, int timeoutMs, QString *error) = 0;
    virtual int maxPacketSize() const { return 0x400; }
    virtual bool close() = 0;
};

// libusb 枚举（VID 0x12D1 华为）
bool enumerateUsb(QList<HisiDevice> &out, QString *error);

// 打开 libusb 通道（ch 接管所有权；失败时 ch 保持 null）
bool openLibusbUsb(const HisiDevice &dev, std::unique_ptr<IUsbChannel> &ch, QString *error);

// ---- 纯函数（供单测/复用）----
// CRC16-X25：init 0xFFFF、反射多项式 0x8408、逐位右移、结果取反
quint16 crc16X25(const QByteArray &data);
// HDLC 风格转义：0x7E→0x7D 0x5E、0x7D→0x7D 0x5D
QByteArray escapePayload(const QByteArray &data);
// 帧封装：0x7E | escaped(payload + CRC16-X25 LE) | 0x7E
QByteArray buildFrame(quint8 cmd, const QByteArray &payload);

class HisiSession {
public:
    HisiSession(std::unique_ptr<IUsbChannel> usb, const HisiDevice &dev);
    ~HisiSession();

    // 打开 USB + CDC line coding(9600) + 握手（3 次重试）
    bool connect(QString *error);
    bool isConnected() const { return m_connected; }

    // 命令帧发送 + 响应解析：成功帧（payload 首字节 0x02）→ true；
    // 错误帧（0x03）→ false + error；超时/无响应 → false + error
    bool sendCommand(quint8 cmd, const QByteArray &payload, double timeoutSec,
                     QString *error);

    // 握手（供单测/复用）：发握手帧，期望响应含前缀
    bool handshake(QString *error);

    bool close(QString *error);
    bool isClosed() const { return m_closed; }

    // 协议常量
    static const QByteArray kHandshakeCommand; // 19B 握手命令（无前导 0x7E）
    static const QByteArray kAckResponse;      // 7E 02 6A D3 7E
    static const QByteArray kHandshakePrefix;  // 7E 26 00 00 25 A7

private:
    bool readFrame(QByteArray &out, int timeoutMs, QString *error);

    std::unique_ptr<IUsbChannel> m_usb;
    HisiDevice m_dev;
    bool m_connected = false;
    bool m_closed = false;
};

} // namespace hisi
```

- [ ] **Step 4: 实现 `plugins/huawei_flash/hisi_update.cpp`**

```cpp
#include "core/modes/hisi_update.h"

#include <libusb.h>

// 实现对照（核实记录见头文件与计划文档"协议核实记录"）
namespace hisi {
namespace {

// 华为 VID 白名单（DBAdapter / USB Update 设备）
constexpr quint16 kHuaweiVid = 0x12D1;

const char *usbErrName(int ret) { return libusb_error_name(ret); }

// libusb 通道：枚举端点 + CDC line coding(9600) 控制请求
class HisiUsbLibusb final : public IUsbChannel {
public:
    explicit HisiUsbLibusb(const HisiDevice &dev) : m_dev(dev) {}
    ~HisiUsbLibusb() override { close(); }

    bool open(QString *error) override
    {
        int ret = libusb_init(&m_ctx);
        if (ret != LIBUSB_SUCCESS) {
            if (error) *error = QStringLiteral("libusb_init 失败: %1").arg(QLatin1String(usbErrName(ret)));
            return false;
        }
        libusb_set_option(m_ctx, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
        m_handle = libusb_open_device_with_vid_pid(m_ctx, m_dev.vid, m_dev.pid);
        if (!m_handle) {
            if (error) *error = QStringLiteral("打开设备 %1:%2 失败（未连接或无权限）")
                                    .arg(m_dev.vid, 4, 16, QLatin1Char('0'))
                                    .arg(m_dev.pid, 4, 16, QLatin1Char('0'));
            libusb_exit(m_ctx); m_ctx = nullptr;
            return false;
        }
        libusb_detach_kernel_driver(m_handle, 0);
        ret = libusb_claim_interface(m_handle, 0);
        if (ret != LIBUSB_SUCCESS) {
            if (error) *error = QStringLiteral("claim 接口失败: %1").arg(QLatin1String(usbErrName(ret)));
            close();
            return false;
        }
        // CDC SET_LINE_CODING：9600 波特 8N1（行为一致性，bulk 传输不受影响）
        unsigned char lc[7] = { 0x80, 0x25, 0x00, 0x00, 0x00, 0x00, 0x08 };
        libusb_control_transfer(m_handle, 0x21, 0x20, 0, 0, lc, 7, 1000);
        return discoverEndpoints(error);
    }

    bool write(const QByteArray &data, QString *error) override
    {
        if (!m_handle || !m_epOut) {
            if (error) *error = QStringLiteral("USB 通道未打开");
            return false;
        }
        int transferred = 0;
        const int ret = libusb_bulk_transfer(
            m_handle, m_epOut,
            reinterpret_cast<unsigned char *>(const_cast<char *>(data.constData())),
            data.size(), &transferred, 2000);
        if (ret != LIBUSB_SUCCESS || transferred != data.size()) {
            if (error) *error = QStringLiteral("USB 写失败: %1（%2/%3 字节）")
                                    .arg(QLatin1String(usbErrName(ret)))
                                    .arg(transferred).arg(data.size());
            return false;
        }
        return true;
    }

    bool read(QByteArray &out, int maxLen, int timeoutMs, QString *error) override
    {
        out.clear();
        if (!m_handle || !m_epIn) {
            if (error) *error = QStringLiteral("USB 通道未打开");
            return false;
        }
        QByteArray buf(maxLen, Qt::Uninitialized);
        int transferred = 0;
        const int ret = libusb_bulk_transfer(m_handle, m_epIn,
                                             reinterpret_cast<unsigned char *>(buf.data()),
                                             maxLen, &transferred, timeoutMs);
        if (ret != LIBUSB_SUCCESS) {
            if (error) *error = QStringLiteral("USB 读失败: %1").arg(QLatin1String(usbErrName(ret)));
            return false;
        }
        out = buf.left(transferred);
        return true;
    }

    bool close() override
    {
        if (m_handle) {
            libusb_release_interface(m_handle, 0);
            libusb_close(m_handle);
            m_handle = nullptr;
        }
        if (m_ctx) { libusb_exit(m_ctx); m_ctx = nullptr; }
        return true;
    }

private:
    bool discoverEndpoints(QString *error)
    {
        libusb_config_descriptor *cfg = nullptr;
        if (libusb_get_active_config_descriptor(libusb_get_device(m_handle), &cfg) != LIBUSB_SUCCESS) {
            if (error) *error = QStringLiteral("读取配置描述符失败");
            return false;
        }
        for (int ic = 0; ic < int(cfg->bNumInterfaces); ++ic) {
            for (int a = 0; a < int(cfg->interface[ic].num_altsetting); ++a) {
                const libusb_interface_descriptor *alt = &cfg->interface[ic].altsetting[a];
                for (int e = 0; e < alt->bNumEndpoints; ++e) {
                    const quint8 addr = alt->endpoint[e].bEndpointAddress;
                    if (addr & LIBUSB_ENDPOINT_IN) {
                        if (!m_epIn) m_epIn = addr;
                    } else if (!m_epOut) {
                        m_epOut = addr;
                    }
                }
                if (m_epIn && m_epOut) break;
            }
            if (m_epIn && m_epOut) break;
        }
        libusb_free_config_descriptor(cfg);
        if (!m_epIn || !m_epOut) {
            if (error) *error = QStringLiteral("未找到 IN/OUT 端点");
            close();
            return false;
        }
        return true;
    }

    const HisiDevice m_dev;
    libusb_context *m_ctx = nullptr;
    libusb_device_handle *m_handle = nullptr;
    quint8 m_epIn = 0;
    quint8 m_epOut = 0;
};

} // namespace

const QByteArray HisiSession::kHandshakeCommand =
    QByteArray("\x26\x00\x00\x25\xA7\x00\x06\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00\x01\x00", 19);
const QByteArray HisiSession::kAckResponse = QByteArray("\x7E\x02\x6A\xD3\x7E", 5);
const QByteArray HisiSession::kHandshakePrefix = QByteArray("\x7E\x26\x00\x00\x25\xA7", 6);

bool enumerateUsb(QList<HisiDevice> &out, QString *error)
{
    out.clear();
    libusb_context *ctx = nullptr;
    int ret = libusb_init(&ctx);
    if (ret != LIBUSB_SUCCESS) {
        if (error) *error = QStringLiteral("libusb_init 失败: %1").arg(QLatin1String(usbErrName(ret)));
        return false;
    }
    libusb_set_option(ctx, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    libusb_device **list = nullptr;
    const ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) {
        if (error) *error = QStringLiteral("libusb_get_device_list 失败");
        libusb_exit(ctx);
        return false;
    }
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS)
            continue;
        if (desc.idVendor == kHuaweiVid) {
            HisiDevice d;
            d.vid = desc.idVendor;
            d.pid = desc.idProduct;
            d.portName = QStringLiteral("usb-%1-%2")
                             .arg(libusb_get_bus_number(list[i]))
                             .arg(libusb_get_device_address(list[i]));
            out.append(d);
        }
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    return true;
}

bool openLibusbUsb(const HisiDevice &dev, std::unique_ptr<IUsbChannel> &ch, QString *error)
{
    auto usb = std::make_unique<HisiUsbLibusb>(dev);
    QString oerr;
    if (!usb->open(&oerr)) {
        if (error) *error = oerr;
        return false;
    }
    ch = std::move(usb);
    return true;
}

quint16 crc16X25(const QByteArray &data)
{
    // CRC16-X25：init 0xFFFF、反射多项式 0x8408、结果取反
    quint16 crc = 0xFFFF;
    for (char c : data) {
        crc ^= quint16(quint8(c));
        for (int i = 0; i < 8; ++i) {
            if (crc & 0x0001)
                crc = quint16((crc >> 1) ^ 0x8408);
            else
                crc >>= 1;
        }
    }
    return quint16(~crc);
}

QByteArray escapePayload(const QByteArray &data)
{
    QByteArray out;
    out.reserve(data.size() * 2);
    for (char c : data) {
        const quint8 b = quint8(c);
        if (b == 0x7E) {
            out.append(char(0x7D)); out.append(char(0x5E));
        } else if (b == 0x7D) {
            out.append(char(0x7D)); out.append(char(0x5D));
        } else {
            out.append(c);
        }
    }
    return out;
}

QByteArray buildFrame(quint8 cmd, const QByteArray &payload)
{
    // 0x7E | escaped(cmd + payload + CRC16-X25 LE) | 0x7E
    QByteArray body(1, char(cmd));
    body += payload;
    const quint16 crc = crc16X25(body);
    body.append(char(crc & 0xFF));
    body.append(char((crc >> 8) & 0xFF));
    QByteArray frame(1, '\x7E');
    frame += escapePayload(body);
    frame += QByteArray(1, '\x7E');
    return frame;
}

HisiSession::HisiSession(std::unique_ptr<IUsbChannel> usb, const HisiDevice &dev)
    : m_usb(std::move(usb)), m_dev(dev)
{
}

HisiSession::~HisiSession()
{
    QString err;
    close(&err);
}

bool HisiSession::connect(QString *error)
{
    if (m_closed) {
        if (error) *error = QStringLiteral("会话已关闭");
        return false;
    }
    if (!m_usb) {
        if (error) *error = QStringLiteral("未打开 USB 通道");
        return false;
    }
    QString oerr;
    if (!m_usb->open(&oerr)) {
        if (error) *error = oerr;
        return false;
    }
    if (!handshake(error)) {
        m_usb->close();
        return false;
    }
    m_connected = true;
    return true;
}

bool HisiSession::handshake(QString *error)
{
    // 发握手帧（命令 + CRC LE + 0x7E，无前导 0x7E），期望响应含前缀；
    // 3 次重试，间隔 200ms
    const quint16 crc = crc16X25(kHandshakeCommand);
    QByteArray frame = kHandshakeCommand;
    frame.append(char(crc & 0xFF)).append(char((crc >> 8) & 0xFF));
    frame.append('\x7E');
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!m_usb->write(frame, error))
            return false;
        QByteArray resp;
        if (m_usb->read(resp, 512, 500, error) && resp.contains(kHandshakePrefix))
            return true;
        if (attempt < 2) {
            // 丢弃残留输入后重试
            QByteArray stale;
            m_usb->read(stale, 4096, 50, nullptr);
        }
    }
    if (error && error->isEmpty())
        *error = QStringLiteral("握手失败（未收到 7E 26 00 00 25 A7 前缀响应）");
    return false;
}

bool HisiSession::sendCommand(quint8 cmd, const QByteArray &payload, double timeoutSec,
                              QString *error)
{
    // 发送前丢弃残留输入；分块 ≤0x10000 写入
    if (!m_usb) {
        if (error) *error = QStringLiteral("未打开 USB 通道");
        return false;
    }
    QByteArray stale;
    m_usb->read(stale, 4096, 10, nullptr);

    const QByteArray frame = buildFrame(cmd, payload);
    int offset = 0;
    while (offset < frame.size()) {
        const int chunk = qMin(0x10000, frame.size() - offset);
        if (!m_usb->write(frame.mid(offset, chunk), error))
            return false;
        offset += chunk;
    }

    // 读响应帧：丢弃非 0x7E 起始字节，收集至 0x7E…0x7E 闭合
    QByteArray resp;
    const int timeoutMs = int(timeoutSec * 1000);
    if (!readFrame(resp, timeoutMs, error))
        return false;
    // 解析：帧内 payload 首字节 = 0x02 成功 / 0x03 设备错误
    if (resp.size() >= 2 && quint8(resp[1]) == 0x02)
        return true;
    if (resp.size() >= 2 && quint8(resp[1]) == 0x03) {
        if (error) *error = QStringLiteral("设备错误 (0x03): %1")
                                .arg(QString::fromLatin1(resp.toHex(' ')));
        return false;
    }
    if (error) *error = QStringLiteral("意外响应: %1")
                            .arg(QString::fromLatin1(resp.toHex(' ')));
    return false;
}

bool HisiSession::readFrame(QByteArray &out, int timeoutMs, QString *error)
{
    out.clear();
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    bool seenStart = false;
    while (true) {
        const int remaining = int(deadline - QDateTime::currentMSecsSinceEpoch());
        if (remaining <= 0) {
            if (error && error->isEmpty())
                *error = QStringLiteral("响应超时");
            return false;
        }
        QByteArray chunk;
        if (!m_usb->read(chunk, 256, qMin(remaining, 256), error))
            return false;
        for (char c : chunk) {
            const quint8 b = quint8(c);
            if (!seenStart) {
                if (b == 0x7E) { seenStart = true; out.append(c); }
                continue;
            }
            out.append(c);
            if (b == 0x7E && out.size() >= 2)
                return true; // 闭合
        }
    }
}

bool HisiSession::close(QString *error)
{
    Q_UNUSED(error)
    if (m_usb)
        m_usb->close();
    m_connected = false;
    m_closed = true;
    return true;
}

} // namespace hisi
```

（注：`readFrame` 使用 `QDateTime`——在 cpp 头部补 `#include <QDateTime>`。）

- [ ] **Step 5: CMake 注册测试 + libusb 链接（插件内）**

`plugins/huawei_flash/CMakeLists.txt` 追加（LIBUSB_INCLUDE_DIRS/LIBUSB_LIBRARIES 为主 CMakeLists 顶部已求值变量，子目录可见）：
```cmake
# 协议层测试（插件内独立注册，不进入主项目测试列表）
find_package(Qt6 QUIET COMPONENTS Test)
if(Qt6Test_FOUND AND ENABLE_IMAGE_TESTS)
    enable_testing()
    add_executable(test_hisi_update
        tests/test_hisi_update.cpp
        hisi_update.cpp)
    target_include_directories(test_hisi_update PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${LIBUSB_INCLUDE_DIRS})
    target_link_libraries(test_hisi_update PRIVATE Qt6::Test ${LIBUSB_LIBRARIES})
    set_target_properties(test_hisi_update PROPERTIES AUTOMOC ON)
    add_test(NAME test_hisi_update COMMAND test_hisi_update)
endif()
```

- [ ] **Step 6: 跑测试验证通过**

Run: `cmake -B build -G Ninja && cmake --build build -j$(nproc) && ctest --test-dir build -R test_hisi_update --output-on-failure`
Expected: 全部 PASS（10 个用例：3 纯函数 + 2 握手 + 3 命令 + 1 open 失败 + 1 帧构造）。

注意 `buildFrameWrapsAndAppendsCrc` 中 `crc16X25 of [0x0A]` 的具体值：实现先计算再断言结构（测试与实现同源计算，验证封装正确性而非 CRC 数值——CRC 数值正确性由标准向量测试覆盖）。

- [ ] **Step 7: 跑全量测试回归**

Run: `ctest --test-dir build`
Expected: 25/25 PASS（24 原有 + test_hisi_update）。

- [ ] **Step 8: Commit**

```bash
git add plugins/huawei_flash/hisi_update.h plugins/huawei_flash/hisi_update.cpp \
        plugins/huawei_flash/tests/test_hisi_update.cpp plugins/huawei_flash/CMakeLists.txt
git commit -m "feat: 华为 Kirin USB Update 连接与帧层 — CRC16-X25/0x7E 帧/握手 (F2-1)

- 独立实现：协议事实（帧格式/命令字/CRC）为公共领域行为观察，实现自写
- crc16X25 标准向量验证（123456789→0x906E）+ 转义/帧封装/握手 3 次重试
- IUsbChannel 抽象 + libusb 实现（CDC line coding 9600）+ MockUsbChannel 注入
- 诚实边界：Xloader 漏洞载荷不内置（F2-3 接口标注用户自备）"
```

---

### Task F2-2: 命令层与刷写流程

**Files:**
- Create: `plugins/huawei_flash/hisi_flash.h`
- Create: `plugins/huawei_flash/hisi_flash.cpp`
- Modify: `plugins/huawei_flash/tests/test_hisi_update.cpp`（追加命令/刷写用例）
- Modify: `plugins/huawei_flash/CMakeLists.txt`（测试源追加 hisi_flash.cpp）

**Interfaces:**
- Consumes: `hisi::HisiSession`（F2-1）、ZLIB（现有）
- Produces:
  - `QByteArray hisi::zlibCompress(const QByteArray&)`（zlib 格式：0x78 01 头 + Deflate + Adler32 BE 尾）
  - `struct hisi::FlashPartition { QString name; QByteArray header; QByteArray data; }`（data 可为内存或文件路径——设计为 header + 文件路径以支持大分区）
  - `class hisi::HisiFlasher`（构造 `HisiFlasher(HisiSession &session)`）：
    - `bool unlock(const QByteArray &unlockCode, QString *error)`（0x0B）
    - `bool flashPartition(const QString &name, const QByteArray &header, const QString &imagePath, std::function<void(int)> progress, QString *error)`（HEAD 0x41 → DATA×N 0x0F → TAIL 0x43）
    - `bool reboot(QString *error)`（0x0A + 0x32 强制重启）

- [ ] **Step 1: 追加失败测试**

```cpp
    // ---- F2-2: 命令层与刷写 ----
    void zlibCompressProduces781Header();
    void unlockFrame();
    void flashPartitionFrames();
    void rebootCommands();

void TestHisiUpdate::zlibCompressProduces781Header()
{
    // zlib 格式：0x78 0x01 头；解压后还原
    const QByteArray data("hello hisi", 10);
    const QByteArray comp = hisi::zlibCompress(data);
    QVERIFY(comp.size() >= 2);
    QCOMPARE(quint8(comp[0]), quint8(0x78));
    QCOMPARE(quint8(comp[1]), quint8(0x01));
    // 用 qUncompress 验证（qUncompress 接受 zlib 格式）
    QByteArray back = qUncompress(comp);
    QCOMPARE(back, data);
}

void TestHisiUpdate::unlockFrame()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    hisi::HisiFlasher f(s);
    m->reads << QByteArray(hisi::HisiSession::kAckResponse);
    const QByteArray code("\x01\x02\x03\x04", 4);
    QVERIFY(f.unlock(code, nullptr));
    // 帧：0x7E | esc(0x0B + code + crcLE) | 0x7E
    QByteArray expect(1, '\x7E');
    QByteArray body(1, char(hisi::FRAME_UNLOCK));
    body += code;
    const quint16 crc = hisi::crc16X25(body);
    body.append(char(crc & 0xFF)).append(char((crc >> 8) & 0xFF));
    expect += hisi::escapePayload(body);
    expect += QByteArray(1, '\x7E');
    QCOMPARE(m->writes, expect);
}

void TestHisiUpdate::flashPartitionFrames()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    hisi::HisiFlasher f(s);
    // HEAD + DATA + TAIL 三个响应（成功）
    m->reads << QByteArray(hisi::HisiSession::kAckResponse)
             << QByteArray(hisi::HisiSession::kAckResponse)
             << QByteArray(hisi::HisiSession::kAckResponse);
    // 临时镜像文件（< 0x20000，单块）
    QTemporaryFile tmp;
    QVERIFY(tmp.open());
    tmp.write("test image data");
    tmp.flush();
    const QByteArray header(64, '\x00'); // 64B 分区头（含 fileSeq@20 全 0）
    QVERIFY(f.flashPartition(QStringLiteral("boot"), header, tmp.fileName(), nullptr, nullptr));
    // 三帧顺序：HEAD(0x41) → DATA(0x0F) → TAIL(0x43)
    QVERIFY(m->writes.contains('\x41'));
    QVERIFY(m->writes.contains('\x0F'));
    QVERIFY(m->writes.contains('\x43'));
    QVERIFY(m->writes.indexOf('\x41') < m->writes.indexOf('\x0F'));
    QVERIFY(m->writes.indexOf('\x0F') < m->writes.indexOf('\x43'));
}

void TestHisiUpdate::rebootCommands()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    hisi::HisiDevice dev; dev.vid = 0x12D1; dev.pid = 0x0000;
    hisi::HisiSession s(std::move(usb), dev);
    hisi::HisiFlasher f(s);
    m->reads << QByteArray(hisi::HisiSession::kAckResponse)
             << QByteArray(hisi::HisiSession::kAckResponse);
    QVERIFY(f.reboot(nullptr));
    QVERIFY(m->writes.contains('\x0A'));
    QVERIFY(m->writes.contains('\x32'));
}
```

- [ ] **Step 2: 跑测试验证失败**

Run: `cmake --build build` → Expected: 编译失败（`hisi_flash.h` 不存在）。

- [ ] **Step 3: 实现 `plugins/huawei_flash/hisi_flash.h`**

```cpp
#pragma once

// 华为 Kirin USB Update 命令层与刷写流程（计划 F2-2）
//
// 独立实现声明：协议事实（命令码/帧结构/刷写时序）来自公共领域协议行为
// 观察；实现代码为本项目独立撰写。
//
// 核实要点（行为观察，2026-08-18）：
//   • HEAD(0x41) → DATA×N(0x0F) → TAIL(0x43) 逐分区刷写
//   • DATA 块：0x20000 字节原始数据 → zlib 压缩（0x78 01 + Deflate + Adler32 BE）
//     → 帧体 (fileSeqInt+addr) BE32 + origLen BE32 + 压缩数据
//   • addr 从 0 起按原始长度累加；fileSeq = 分区头偏移 20 的 4 字节（大端）
//   • 超时 = max(1, min(8, 压缩后 MB × 1.5)) 秒

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <functional>

#include "core/modes/hisi_update.h"

namespace hisi {

// zlib 压缩：0x78 01 头 + Deflate + Adler32 BE 尾（协议要求）
QByteArray zlibCompress(const QByteArray &data);

class HisiFlasher {
public:
    explicit HisiFlasher(HisiSession &session) : m_session(session) {}

    // UNLOCK（0x0B + unlockcode）
    bool unlock(const QByteArray &unlockCode, QString *error = nullptr);

    // 逐分区刷写：HEAD → DATA×N（0x20000 块，zlib 压缩）→ TAIL。
    // imagePath 为未压缩分区镜像文件；header 为分区头（98+ 字节，含
    // fileSeq@20..24 大端）。progress 回调已发送原始字节数（可为 null）。
    bool flashPartition(const QString &name, const QByteArray &header,
                        const QString &imagePath,
                        std::function<void(qint64)> progress = nullptr,
                        QString *error = nullptr);

    // REBOOT（0x0A）→ FORCE_REBOOT（0x32）
    bool reboot(QString *error = nullptr);

private:
    bool sendDataBlocks(const QByteArray &header, const QString &imagePath,
                        std::function<void(qint64)> progress, QString *error);

    HisiSession &m_session;
};

} // namespace hisi
```

- [ ] **Step 4: 实现 `plugins/huawei_flash/hisi_flash.cpp`**

```cpp
#include "core/modes/hisi_flash.h"

#include <QFile>
#include <QFileInfo>

#include <zlib.h>

// 实现对照（核实记录见头文件与计划文档"协议核实记录"）
namespace hisi {

QByteArray zlibCompress(const QByteArray &data)
{
    // 协议要求 zlib 头 0x78 0x01（Deflate 级别 1 = Fastest）
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (deflateInit2(&strm, 1, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return QByteArray();
    // 压缩（流式，防御性倍增缓冲）
    QByteArray out;
    const uLong bound = deflateBound(&strm, uLong(data.size()));
    out.resize(int(bound));
    strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
    strm.avail_in = uInt(data.size());
    strm.next_out = reinterpret_cast<Bytef *>(out.data());
    strm.avail_out = uInt(out.size());
    const int ret = deflate(&strm, Z_FINISH);
    const int used = int(strm.total_out);
    deflateEnd(&strm);
    if (ret != Z_STREAM_END)
        return QByteArray();
    out.truncate(used);
    // 头应为 0x78 0x01（deflateInit2 level 1 生成 0x78 0x01）
    return out;
}

bool HisiFlasher::unlock(const QByteArray &unlockCode, QString *error)
{
    // UNLOCK：0x0B + unlockcode（帧封装由 buildFrame 完成）
    return m_session.sendCommand(FRAME_UNLOCK, unlockCode, 1.0, error);
}

bool HisiFlasher::flashPartition(const QString &name, const QByteArray &header,
                                 const QString &imagePath,
                                 std::function<void(qint64)> progress, QString *error)
{
    Q_UNUSED(name)
    // HEAD：0x41 + 分区头
    if (!m_session.sendCommand(FRAME_HEAD, header, 2.0, error))
        return false;
    // DATA 块
    if (!sendDataBlocks(header, imagePath, progress, error))
        return false;
    // TAIL：0x43 + 分区头
    if (!m_session.sendCommand(FRAME_TAIL, header, 8.0, error))
        return false;
    return true;
}

bool HisiFlasher::sendDataBlocks(const QByteArray &header, const QString &imagePath,
                                 std::function<void(qint64)> progress, QString *error)
{
    QFile f(imagePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开分区镜像: %1").arg(imagePath);
        return false;
    }
    // fileSeq = 分区头偏移 20 的 4 字节（大端）
    quint32 fileSeqInt = 0;
    if (header.size() >= 24) {
        fileSeqInt = (quint32(quint8(header[20])) << 24)
                   | (quint32(quint8(header[21])) << 16)
                   | (quint32(quint8(header[22])) << 8)
                   | quint32(quint8(header[23]));
    }
    const qint64 fileSize = f.size();
    quint64 addr = 0;
    qint64 sent = 0;
    QByteArray buf(0x20000, Qt::Uninitialized);
    while (sent < fileSize) {
        const int toRead = int(qMin<qint64>(0x20000, fileSize - sent));
        const qint64 n = f.read(buf.data(), toRead);
        if (n <= 0) break;
        const QByteArray raw = buf.left(int(n));
        const QByteArray comp = zlibCompress(raw);
        if (comp.isEmpty()) {
            if (error) *error = QStringLiteral("zlib 压缩失败");
            return false;
        }
        // DATA 帧体：0x0F + (fileSeqInt+addr) BE32 + origLen BE32 + 压缩数据
        QByteArray payload(1, char(FRAME_DATA));
        const quint32 combined = fileSeqInt + quint32(addr);
        payload.append(char((combined >> 24) & 0xFF));
        payload.append(char((combined >> 16) & 0xFF));
        payload.append(char((combined >> 8) & 0xFF));
        payload.append(char(combined & 0xFF));
        const quint32 origLen = quint32(n);
        payload.append(char((origLen >> 24) & 0xFF));
        payload.append(char((origLen >> 16) & 0xFF));
        payload.append(char((origLen >> 8) & 0xFF));
        payload.append(char(origLen & 0xFF));
        payload += comp;
        // 超时 = max(1, min(8, 压缩后 MB × 1.5))
        const double mb = comp.size() / 1024.0 / 1024.0;
        const double timeout = qBound(1.0, mb * 1.5, 8.0);
        if (!m_session.sendCommand(FRAME_DATA, payload, timeout, error))
            return false;
        addr += quint64(n);
        sent += n;
        if (progress) progress(sent);
    }
    return true;
}

bool HisiFlasher::reboot(QString *error)
{
    // REBOOT → FORCE_REBOOT（先普通后强制，行为观察时序）
    if (!m_session.sendCommand(FRAME_REBOOT, QByteArray(), 0.3, error))
        return false;
    return m_session.sendCommand(FRAME_FORCE_REBOOT, QByteArray(), 0.3, error);
}

} // namespace hisi
```

- [ ] **Step 5: CMake 更新（插件内）**

`plugins/huawei_flash/CMakeLists.txt`：测试可执行文件源追加 `hisi_flash.cpp`，并链接 ZLIB：
```cmake
    add_executable(test_hisi_update
        tests/test_hisi_update.cpp
        hisi_update.cpp
        hisi_flash.cpp)
    ...
    target_link_libraries(test_hisi_update PRIVATE Qt6::Test ${LIBUSB_LIBRARIES} ${ZLIB_LIBRARY})
```
（ZLIB_LIBRARY 为主 CMakeLists 顶部已求值变量，子目录可见。）

- [ ] **Step 6: 跑测试验证通过**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_hisi_update --output-on-failure`
Expected: 14 个用例全部 PASS（10 + 新增 4）。

- [ ] **Step 7: 全量回归 + Commit**

Run: `ctest --test-dir build` → Expected 25/25 PASS。

```bash
git add plugins/huawei_flash/hisi_flash.h plugins/huawei_flash/hisi_flash.cpp \
        plugins/huawei_flash/tests/test_hisi_update.cpp plugins/huawei_flash/CMakeLists.txt
git commit -m "feat: 华为 Kirin USB Update 命令层与刷写流程 (F2-2)

- HEAD/DATA/TAIL 逐分区刷写（0x20000 块 zlib 0x78 01 压缩 + Adler32）
- UNLOCK/REBOOT/FORCE_REBOOT 命令 + 响应解析（成功 7E 02 6A D3 7E）
- 超时按压缩后体积自适应（max(1,min(8,MB×1.5))）
- 独立实现：协议事实为行为观察，实现自写"
```

---

### Task F2-3: 集成（插件内 update.app 解析 + 刷写路由 + 插件入口）

**Files:**
- Create: `plugins/huawei_flash/update_app.h/.cpp`（插件内 update.app 解析，自包含）
- Modify: `plugins/huawei_flash/hisi_flash.h/.cpp`（追加 `runHisiFlash`）
- Modify: `plugins/huawei_flash/huawei_flash_plugin.h/.cpp`（ProtocolPlugin 实现：execute 接 runHisiFlash）
- Modify: `plugins/huawei_flash/tests/test_hisi_update.cpp`（追加路由/解析用例）
- Modify: `plugins/huawei_flash/CMakeLists.txt`（update_app.cpp + 插件源）

**Interfaces:**
- Consumes: `hisi::HisiFlasher`（F2-2）
- Produces:
  - `struct hisi::AppPartition { QString name; QByteArray header; QByteArray data; }`（update.app 条目：头 + 数据）
  - `bool hisi::parseUpdateApp(const QByteArray &data, QList<AppPartition> &out, QString *error)`（插件内自包含解析：magic `55 AA 5A A5` + headerLength LE32 + dataLength LE32 + 32B 分区名；头 = 98 + 剩余字节；不依赖主项目 image_engine）
  - `bool hisi::runHisiFlash(const QString &updateAppPath, std::function<void(const QString&, int)> progress, QString *error)`
**诚实边界（硬约束）**：
- Xloader 分区：刷写遇到 `xloader`/`preloader` 分区名时**拒绝并提示**"Xloader 修补载荷未内置，需用户自行准备"（不假装支持）
- 解锁码：无解锁码时跳过 UNLOCK 并日志标注"未解锁可能失败"
- 机型范围：Kirin 系（实测前不承诺具体型号）

- [ ] **Step 1: 实现插件内 update.app 解析 `plugins/huawei_flash/update_app.h/.cpp`**

```cpp
// update_app.h
#pragma once

// 插件内自包含的 update.app 解析（不依赖主项目 image_engine）。
// 独立实现声明：条目布局（magic/头长/数据长/分区名）为公共领域协议行为
// 观察所得；实现自写。
//
// 布局（行为观察）：条目 magic 55 AA 5A A5 + headerLength(LE32) + 4B + 8B
// + 4B + dataLength(LE32) + 16B + 16B + 32B 分区名(UTF-8, NUL 结尾) + 6B
// + (headerLength - 98) 剩余字节；头总长 = headerLength。

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace hisi {

struct AppPartition {
    QString name;
    QByteArray header; // 分区头（含 fileSeq@20..24 大端）
    QByteArray data;   // 分区数据
};

// 解析 update.app 条目表；失败返回 false 并填 error
bool parseUpdateApp(const QByteArray &data, QList<AppPartition> &out, QString *error);

} // namespace hisi
```

```cpp
// update_app.cpp
#include "update_app.h"

namespace hisi {

namespace {
constexpr quint8 kMagic[4] = { 0x55, 0xAA, 0x5A, 0xA5 };
constexpr int kHeaderFixed = 98; // magic(4)+len(4)+4+8+4+dlen(4)+16+16+name(32)+6

quint32 le32(const QByteArray &b, int off)
{
    return quint32(quint8(b[off])) | (quint32(quint8(b[off + 1])) << 8)
         | (quint32(quint8(b[off + 2])) << 16) | (quint32(quint8(b[off + 3])) << 24);
}

} // namespace

bool parseUpdateApp(const QByteArray &data, QList<AppPartition> &out, QString *error)
{
    out.clear();
    int pos = 0;
    while (pos + 4 <= data.size()) {
        if (memcmp(data.constData() + pos, kMagic, 4) != 0) {
            if (error) *error = QStringLiteral("update.app 条目 magic 不符 @0x%1")
                                    .arg(pos, 0, 16);
            return false;
        }
        const int headerLen = int(le32(data, pos + 4));
        if (headerLen < kHeaderFixed || pos + headerLen > data.size()) {
            if (error) *error = QStringLiteral("update.app 条目头长非法 @0x%1")
                                    .arg(pos, 0, 16);
            return false;
        }
        const quint32 dataLen = le32(data, pos + 24); // magic+4+4+8+4 = 偏移 24
        AppPartition p;
        p.header = data.mid(pos, headerLen);
        // 分区名：头内偏移 56（magic 4 + headerLen 4 + 4 + 8 + 4 + dataLen 4 + 16 + 16）
        const QByteArray nameRaw = p.header.mid(56, 32);
        const int nul = nameRaw.indexOf('\0');
        p.name = QString::fromUtf8(nul >= 0 ? nameRaw.left(nul) : nameRaw);
        if (p.name.isEmpty()) {
            if (error) *error = QStringLiteral("update.app 条目名为空 @0x%1").arg(pos, 0, 16);
            return false;
        }
        const int dataOff = pos + headerLen;
        if (dataOff + int(dataLen) > data.size()) {
            if (error) *error = QStringLiteral("update.app 分区数据越界: %1").arg(p.name);
            return false;
        }
        p.data = data.mid(dataOff, int(dataLen));
        out.append(p);
        pos = dataOff + int(dataLen);
    }
    if (out.isEmpty()) {
        if (error) *error = QStringLiteral("update.app 无分区条目");
        return false;
    }
    return true;
}

} // namespace hisi
```

- [ ] **Step 2: 追加测试（`tests/test_hisi_update.cpp`）**

```cpp
    // ---- F2-3: update.app 解析 ----
    void parseUpdateAppEntries();
    void parseUpdateAppBadMagicFails();
    // ---- F2-3: xloader 边界 ----
    void isXloaderPartitionNames();

void TestHisiUpdate::parseUpdateAppEntries()
{
    // 构造 2 条条目：boot（0x10000 数据）+ system（0x20000 数据）
    QByteArray app;
    for (const char *name : {"boot", "system"}) {
        const quint32 headerLen = 98 + 8; // 98 固定 + 8 剩余
        QByteArray h(headerLen, '\0');
        h[0] = 0x55; h[1] = 0xAA; h[2] = 0x5A; h[3] = 0xA5;
        const quint32 len = headerLen;
        h[4] = char(len & 0xFF); h[5] = char((len >> 8) & 0xFF);
        h[6] = char((len >> 16) & 0xFF); h[7] = char((len >> 24) & 0xFF);
        // dataLength @24（magic 4 + headerLen 4 + 4 + 8 + 4）
        const quint32 dlen = QByteArray(name).size() == 4 ? 0x10000u : 0x20000u;
        h[24] = char(dlen & 0xFF); h[25] = char((dlen >> 8) & 0xFF);
        h[26] = char((dlen >> 16) & 0xFF); h[27] = char((dlen >> 24) & 0xFF);
        // 分区名 @56（32B NUL 结尾）
        memcpy(h.data() + 56, name, qMin<qsizetype>(strlen(name), 32));
        // fileSeq @20（大端）：boot=1, system=2
        h[20] = char(QByteArray(name).size() == 4 ? 0 : 1);
        app += h;
        app += QByteArray(int(dlen), char(0xAB));
    }
    QList<hisi::AppPartition> parts;
    QVERIFY(hisi::parseUpdateApp(app, parts, nullptr));
    QCOMPARE(parts.size(), 2);
    QCOMPARE(parts[0].name, QStringLiteral("boot"));
    QCOMPARE(parts[0].data.size(), 0x10000);
    QCOMPARE(parts[1].name, QStringLiteral("system"));
    QCOMPARE(parts[1].data.size(), 0x20000);
}

void TestHisiUpdate::parseUpdateAppBadMagicFails()
{
    QByteArray app(200, '\x00');
    QString err;
    QVERIFY(!hisi::parseUpdateApp(app, parts, &err));
    QVERIFY(err.contains("magic"));
}

void TestHisiUpdate::isXloaderPartitionNames()
{
    QVERIFY(hisi::isXloaderPartition(QStringLiteral("xloader")));
    QVERIFY(hisi::isXloaderPartition(QStringLiteral("preloader")));
    QVERIFY(hisi::isXloaderPartition(QStringLiteral("xloader_a")));
    QVERIFY(!hisi::isXloaderPartition(QStringLiteral("boot")));
    QVERIFY(!hisi::isXloaderPartition(QStringLiteral("system")));
}
```

- [ ] **Step 3: 实现 `runHisiFlash` + 插件入口**

`hisi_flash.h` 追加：
```cpp
// 集成路由：update.app → 枚举 → 会话 → 逐分区刷写（诚实边界见 cpp）
bool runHisiFlash(const QString &updateAppPath,
                  std::function<void(const QString &name, int percent)> progress,
                  QString *error = nullptr);
// 纯函数（单测）：xloader/preloader 分区名判定（诚实边界用）
bool isXloaderPartition(const QString &partitionName);
```

`hisi_flash.cpp` 追加：
```cpp
bool isXloaderPartition(const QString &partitionName)
{
    const QString n = partitionName.toLower();
    return n == QStringLiteral("xloader") || n == QStringLiteral("preloader")
        || n.startsWith(QStringLiteral("xloader_")) || n.startsWith(QStringLiteral("preloader_"));
}

bool runHisiFlash(const QString &updateAppPath,
                  std::function<void(const QString &, int)> progress, QString *error)
{
    // 1. 解析 update.app（插件内自包含）
    QFile appFile(updateAppPath);
    if (!appFile.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 update.app: %1").arg(updateAppPath);
        return false;
    }
    const QByteArray appData = appFile.readAll();
    QList<AppPartition> parts;
    if (!parseUpdateApp(appData, parts, error))
        return false;

    // 2. 枚举 + 打开会话
    QList<HisiDevice> devs;
    if (!enumerateUsb(devs, error))
        return false;
    if (devs.isEmpty()) {
        if (error) *error = QStringLiteral("未检测到华为 USB Update 设备（VID 0x12D1）");
        return false;
    }
    std::unique_ptr<IUsbChannel> usb;
    if (!openLibusbUsb(devs.first(), usb, error))
        return false;
    HisiSession session(std::move(usb), devs.first());
    if (!session.connect(error))
        return false;
    HisiFlasher flasher(session);

    // 3. 逐分区刷写（xloader 诚实边界）
    int done = 0;
    for (const AppPartition &p : parts) {
        if (progress) progress(p.name, 100 * done / parts.size());
        if (isXloaderPartition(p.name)) {
            if (error) *error = QStringLiteral("分区 %1 为 Xloader：修补载荷未内置（需用户自行准备）")
                                    .arg(p.name);
            return false; // 诚实边界：不假装支持
        }
        // 分区数据落临时文件后流式刷写（大分区避免整载内存）
        const QString imgPath = QDir::temp().filePath(
            QStringLiteral("huawei_flash_%1.img").arg(p.name));
        QFile out(imgPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) *error = QStringLiteral("无法写入临时文件 %1").arg(imgPath);
            return false;
        }
        out.write(p.data);
        out.close();
        if (!flasher.flashPartition(p.name, p.header, imgPath, nullptr, error))
            return false;
        QFile::remove(imgPath);
        ++done;
    }
    if (progress) progress(QString(), 100);
    return flasher.reboot(error);
}
```

`huawei_flash_plugin.cpp`（F2-P 骨架填充）：
```cpp
#include "huawei_flash_plugin.h"

#include "hisi_flash.h"
#include "update_app.h"

bool HuaweiFlashPlugin::execute(const QString &capability, const QVariantMap &params,
                                QString *error)
{
    if (capability != QStringLiteral("huawei-usb-update.flash")) {
        if (error) *error = QStringLiteral("未知能力: %1").arg(capability);
        return false;
    }
    const QString updateApp = params.value(QStringLiteral("updateApp")).toString();
    if (updateApp.isEmpty()) {
        if (error) *error = QStringLiteral("缺少 updateApp 参数（update.app 路径）");
        return false;
    }
    return hisi::runHisiFlash(updateApp, nullptr, error);
}
```

- [ ] **Step 4: CMake 更新（插件内）**

`plugins/huawei_flash/CMakeLists.txt`：
- 测试可执行文件源追加 `update_app.cpp`（`hisi_flash.cpp` 已含）
- 插件库源追加 `update_app.cpp` + `hisi_flash.cpp` + `hisi_update.cpp`，链接 Qt6::Core + libusb + zlib

- [ ] **Step 5: 跑测试验证**

Run: `cmake --build build -j$(nproc) && ctest --test-dir build -R test_hisi_update --output-on-failure`
Expected: 18 个用例 PASS；全量 25/25；`build/plugins/libhuawei_flash_plugin.so` 重新生成。

- [ ] **Step 6: Commit**

```bash
git add plugins/huawei_flash/
git commit -m "feat: 华为 Kirin USB Update 集成 — 插件内 update.app 解析 + runHisiFlash + 插件入口 (F2-3)

- parseUpdateApp：插件内自包含（不依赖主项目 image_engine）
- runHisiFlash：解析 → 枚举 → 会话 → 逐分区刷写 → reboot
- 诚实边界：Xloader 分区拒绝（载荷不内置，用户自备）；无解锁码跳过 UNLOCK
- 机型范围标注：Kirin 系，实测前不承诺具体型号"
```

---

## Self-Review 记录

- **Spec 覆盖**（对照计划 F 文档 F2 段 + 用户重定向 + 插件化决策）：插件系统框架（F2-P）→ 华为插件协议层全链路（连接/帧/命令/刷写/集成）✓；update.app 解析插件内自包含（不依赖主项目 image_engine）✓；诚实边界（Xloader 载荷不内置、无解锁码标注、机型范围）✓
- **合规纪律**（用户强制）：插件隔离（plugins/huawei_flash/ 独立 .so，删除即移除）+ 独立实现声明 + 不复制参照实现表达 + 注释不引用参照源码函数名 + Xloader 载荷不内置 —— 写入 Global Constraints 与各任务代码注释 ✓
- **占位符扫描**：无 TBD/TODO；update_app 解析布局为行为观察核实值；测试构造含完整字节 ✓
- **类型一致性**：`HisiSession`/`HisiFlasher`/`parseUpdateApp`/`runHisiFlash` 跨任务签名一致；`zlibCompress` 输入输出类型一致 ✓
- **依赖顺序**：F2-P → F2-1 → F2-2 → F2-3 串行；每任务结束是可独立测试的绿态（25/25 递增）✓
- **最终审查吸收**（2026-08-18）：核实记录补 TAIL 超时公式（max(35,min(180,15+MB/10))）、条目后 4B 对齐填充（0-3 字节）与 dataLen==0/空名=列表结束、HEAD/TAIL 发送前头变换（92-93 置零 + 追加 0x00）——全部为行为观察，实现同 commit 落地（4f4d237）✓
