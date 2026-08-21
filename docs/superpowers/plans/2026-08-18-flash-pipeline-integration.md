# 刷机管线自动集成 Implementation Plan（计划 F5）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将已交付的三个协议通道（MTK BROM / 华为 Kirin USB Update / 展锐 ResearchDownload）**自动接入现有刷机管线**：DeviceDetector 检测新模式 → FlashTool 按模式分派 → FlashPanel 自动启用对应通道。用户指令（2026-08-18）："华为的插件直接接入刷机管线，自动集成，其他的也是"——不是孤立插件面板手动执行，而是像 fastboot/EDL 一样在管线中自动可用。

**Architecture:** 三处接线：① `DeviceDetector` 扩展 3 个新模式（MODE_MTK_BROM=7 / MODE_HUAWEI_USB_UPDATE=8 / MODE_SPD=9）——`detectConnectedDevices` 追加 libusb VID/PID 快查（0x0E8D:0x0003 / 0x12D1 / 0x1782）；② `FlashTool` 分派——`flashPartition` 按模式路由：MTK BROM → `mtkbrom::runBromFlash`、华为 → `PluginManager` 查能力 `huawei-usb-update.flash` → `execute`（**插件经运行时加载接入管线**，主程序零编译链接不变）、展锐 → `spd::runSpdFlash`；③ `FlashPanel` UI——设备列表显示新模式，刷写按钮按模式启用（整包刷写走文件对话框传参：update.app / pac+FDL / DA 二进制+分区镜像）。

**Tech Stack:** C++17, Qt6, libusb（现有）, PluginManager（F2-P 交付）, mtkbrom（F1 交付）, spd（F4 交付）。

## Global Constraints

- C++17；成员变量 `m_` 前缀；沿用现有管线模式（DeviceDetector/FlashTool/FlashPanel 的信号槽约定）
- **插件隔离保持**：华为通道经 PluginManager 运行时加载（F2-P 设计），FlashTool 不编译链接插件；插件缺失时该通道优雅降级（明确提示"未找到华为刷写插件"）
- **诚实边界**：FDL/DA/update.app 等文件由用户在 UI 选择（不内置）；协议层真机待验证项沿用标注
- 每个任务独立 commit，前缀 `feat:`；UI 层无单测设施（项目约定），验证以构建 + 冒烟为主；协议层分派逻辑（纯函数）可单测
- 不改变既有模式（ADB/Fastboot/EDL/MTK DA/Recovery）的行为

## 现有管线结构（实现前确认）

- `DeviceDetector::DeviceMode`（device_detector.h:18-26）：MODE_UNKNOWN=0 / ADB=1 / FASTBOOT=2 / FASTBOOTD=3 / EDL_9008=4 / MTK_DA=5 / RECOVERY=6
- `detectConnectedDevices`（device_detector.cpp:57-142）：fastboot → EDL（detectEDLDevices，libusb）→ ADB 顺序；m_currentDevices（QMap<QString, DeviceInfo>）维护连接/断开/模式变更
- `FlashTool::flashPartition(deviceId, partition, imagePath)`（flash_tool.h:35）+ EDLHandler 成员 + MtkHandler 成员（JSON-RPC）
- `FlashPanel`：设备列表 + 分区列表 + 刷写按钮（调 flashTool->flashPartition）
- 设备 ID：字符串（fastboot 序列号 / ADB 序列号 / EDL 用 bus-addr 形式）

---

## 文件结构

```
src/core/
  device_detector.h/.cpp   # F5-1: DeviceMode 扩展 + 3 协议检测 + 模式名
  flash_tool.h/.cpp        # F5-2: 分派路由（MTK BROM/华为插件/展锐）+ 整包刷写 API
src/ui/
  flash_panel.h/.cpp       # F5-3: 模式感知按钮 + 整包文件参数对话框
tests/
  test_pipeline.cpp        # F5-2: 分派纯函数单测（模式→通道映射）
CMakeLists.txt             # 测试注册
```

---

### Task F5-1: DeviceDetector 新模式检测

**Files:**
- Modify: `src/core/device_detector.h`（DeviceMode 枚举 + 检测函数声明 + 模式名）
- Modify: `src/core/device_detector.cpp`（detectConnectedDevices 追加 + detectProtocolDevices 实现）

**Interfaces:**
- Consumes: 无（libusb 现有）
- Produces:
  - `enum DeviceMode` 追加：`MODE_MTK_BROM = 7`、`MODE_HUAWEI_USB_UPDATE = 8`、`MODE_SPD = 9`
  - `void detectProtocolDevices(QMap<QString, DeviceInfo> &newDevices)`（libusb VID/PID 快查三个协议通道）
  - `getModeDisplayName` 追加三模式中文名（"MTK BROM" / "华为 USB Update" / "展锐"）

- [ ] **Step 1: 实现 `DeviceMode` 扩展 + `getModeDisplayName`**

device_detector.h 枚举追加：
```cpp
        MODE_MTK_BROM = 7,          // MTK BROM 直刷（F1）
        MODE_HUAWEI_USB_UPDATE = 8, // 华为 Kirin USB Update（F2 插件）
        MODE_SPD = 9                // 展锐 ResearchDownload（F4）
```

getModeDisplayName 追加分支（device_detector.cpp）：
```cpp
    case MODE_MTK_BROM:
        return QStringLiteral("MTK BROM");
    case MODE_HUAWEI_USB_UPDATE:
        return QStringLiteral("华为 USB Update");
    case MODE_SPD:
        return QStringLiteral("展锐");
```

- [ ] **Step 2: 实现 `detectProtocolDevices`（libusb VID/PID 快查）**

device_detector.cpp（参照 detectEDLDevices 的 libusb 用法）：
```cpp
void DeviceDetector::detectProtocolDevices(QMap<QString, DeviceInfo> &newDevices)
{
    // 协议通道设备检测（F5）：libusb VID/PID 快查三个自研协议通道。
    // 设备 ID 用 usb-<bus>-<addr>（与 EDL 一致）；模式映射：
    //   0x0E8D:0x0003 → MTK BROM；0x12D1（DBAdapter）→ 华为 USB Update；
    //   0x1782 → 展锐。
    struct ProtoId { int vid; int pid; DeviceMode mode; };
    const ProtoId ids[] = {
        { 0x0E8D, 0x0003, MODE_MTK_BROM },
        { 0x12D1, -1,     MODE_HUAWEI_USB_UPDATE },
        { 0x1782, -1,     MODE_SPD },
    };
    libusb_context *ctx = nullptr;
    if (libusb_init(&ctx) != LIBUSB_SUCCESS)
        return;
    libusb_set_option(ctx, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    libusb_device **list = nullptr;
    const ssize_t count = libusb_get_device_list(ctx, &list);
    if (count < 0) { libusb_exit(ctx); return; }
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != LIBUSB_SUCCESS)
            continue;
        for (const ProtoId &id : ids) {
            if (desc.idVendor != id.vid)
                continue;
            if (id.pid != -1 && desc.idProduct != id.pid)
                continue;
            const QString devId = QStringLiteral("usb-%1-%2")
                                      .arg(libusb_get_bus_number(list[i]))
                                      .arg(libusb_get_device_address(list[i]));
            DeviceInfo info;
            info.serial = devId;
            info.mode = id.mode;
            newDevices[devId] = info;
            break;
        }
    }
    libusb_free_device_list(list, 1);
    libusb_exit(ctx);
}
```

（DeviceInfo 结构字段以 device_info.h 实际为准——serial/mode 必有，其余字段按现有 EDL 设备构造模式适配。）

- [ ] **Step 3: detectConnectedDevices 接线**

在 detectEDLDevices 之后追加调用（device_detector.cpp）：
```cpp
    // 检测协议通道设备（F5）：MTK BROM / 华为 USB Update / 展锐
    detectProtocolDevices(newDevices);
```
注意：newDevices 已含协议设备后，连接/断开/模式变更的 emit 逻辑——现有代码只在 fastboot 分支处理 emit；协议设备走"新设备"分支即可（循环处理 m_currentDevices 对比——需要看 detectEDLDevices 怎么 emit，若 EDL 也是直接塞 newDevices 无 emit，则协议设备同样处理；若需要 emit deviceConnected，在 detectProtocolDevices 内或循环内补齐，实现时以 detectEDLDevices 的实际模式为准）。

- [ ] **Step 4: 构建验证 + Commit**

Run: `cmake --build build -j$(nproc)` → 零错误；`ctest --test-dir build` → 26/26。
（协议检测无设备环境返回空——冒烟：无设备时检测不崩溃。）

```bash
git add src/core/device_detector.h src/core/device_detector.cpp
git commit -m "feat: DeviceDetector 扩展 — MTK BROM/华为 USB Update/展锐三协议模式检测 (F5-1)"
```

---

### Task F5-2: FlashTool 分派路由

**Files:**
- Modify: `src/core/flash_tool.h/.cpp`（分派 + 整包刷写 API + 插件接入）
- Create: `tests/test_pipeline.cpp`（分派纯函数单测）
- Modify: `CMakeLists.txt`（测试注册）

**Interfaces:**
- Consumes: `mtkbrom::runBromFlash`（F1）、`spd::runSpdFlash`（F4）、`PluginManager`（F2-P）、`DeviceDetector::DeviceMode`（F5-1）
- Produces:
  - `static QString flashChannelForMode(DeviceMode mode)`（纯函数：模式 → 通道名 "mtk-brom"/"huawei-usb-update"/"spd"/""——单测）
  - `bool FlashTool::flashFullPackage(const QString &deviceId, DeviceMode mode, const QVariantMap &params, QString *error)`（整包刷写入口：按模式分派——MTK BROM → runBromFlash(daPath, partitions)；华为 → PluginManager 查能力 execute；展锐 → runSpdFlash(pacPath, fdl1Path, fdl2Path)）

- [ ] **Step 1: 写失败测试 `tests/test_pipeline.cpp`**

```cpp
#include <QtTest>

#include "core/device_detector.h"
#include "core/flash_tool.h"

class TestPipeline : public QObject {
    Q_OBJECT
private slots:
    void channelMapping();
    void channelMappingUnknown();
};

void TestPipeline::channelMapping()
{
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_MTK_BROM),
             QStringLiteral("mtk-brom"));
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_HUAWEI_USB_UPDATE),
             QStringLiteral("huawei-usb-update"));
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_SPD),
             QStringLiteral("spd"));
}

void TestPipeline::channelMappingUnknown()
{
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_ADB), QString());
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_FASTBOOT), QString());
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_UNKNOWN), QString());
}

QTEST_APPLESS_MAIN(TestPipeline)
#include "test_pipeline.moc"
```

（flashChannelForMode 为静态纯函数——测试不依赖 UI/设备。）

- [ ] **Step 2: 跑测试验证失败**

Run: `cmake --build build` → Expected: 编译失败（flashChannelForMode 不存在）。先实现再注册 CMake（F1-1 模式）。

- [ ] **Step 3: 实现分派（flash_tool.h/.cpp）**

flash_tool.h 追加：
```cpp
    // F5: 整包刷写入口——按设备模式分派到协议通道（MTK BROM/华为插件/展锐）。
    // params 承载通道所需文件参数（键名见各通道实现）：
    //   mtk-brom:    daPath(DA 二进制) + partitions(分区名→镜像路径列表)
    //   huawei-usb-update: updateApp(update.app 路径)
    //   spd:         pacPath + fdl1Path + fdl2Path
    // 失败返回 false 并填 error；插件缺失时明确提示。
    bool flashFullPackage(const QString &deviceId, DeviceDetector::DeviceMode mode,
                          const QVariantMap &params, QString *error);

    // 纯函数（单测）：设备模式 → 协议通道名；非协议模式返回空串
    static QString flashChannelForMode(DeviceDetector::DeviceMode mode);
```

flash_tool.cpp 实现：
```cpp
QString FlashTool::flashChannelForMode(DeviceDetector::DeviceMode mode)
{
    switch (mode) {
    case DeviceDetector::MODE_MTK_BROM:
        return QStringLiteral("mtk-brom");
    case DeviceDetector::MODE_HUAWEI_USB_UPDATE:
        return QStringLiteral("huawei-usb-update");
    case DeviceDetector::MODE_SPD:
        return QStringLiteral("spd");
    default:
        return QString();
    }
}

bool FlashTool::flashFullPackage(const QString &deviceId, DeviceDetector::DeviceMode mode,
                                 const QVariantMap &params, QString *error)
{
    const QString channel = flashChannelForMode(mode);
    if (channel.isEmpty()) {
        if (error) *error = QStringLiteral("模式 %1 无协议通道").arg(int(mode));
        return false;
    }
    if (channel == QStringLiteral("mtk-brom")) {
        // F1 通道：DA 二进制用户提供（诚实边界）；分区数据由 params 提供
        const QString daPath = params.value(QStringLiteral("daPath")).toString();
        if (daPath.isEmpty()) {
            if (error) *error = QStringLiteral("缺少 DA 二进制路径（mtk-brom 通道）");
            return false;
        }
        // 分区列表：params["partitions"] 为 QVariantList<QString 镜像路径>
        QList<QPair<QString, QByteArray>> partitions;
        // （分区名与镜像的映射由 UI 层构建——实现时按 FlashPanel 传入结构适配）
        emit outputMessage(QStringLiteral("MTK BROM 刷写通道：%1").arg(deviceId), false);
        return mtkbrom::runBromFlash(daPath, partitions, error);
    }
    if (channel == QStringLiteral("huawei-usb-update")) {
        // F2 插件通道：经 PluginManager 运行时加载（法务隔离保持）
        const QString updateApp = params.value(QStringLiteral("updateApp")).toString();
        if (updateApp.isEmpty()) {
            if (error) *error = QStringLiteral("缺少 update.app 路径（huawei-usb-update 通道）");
            return false;
        }
        auto plugins = PluginManager::instance()
                           .byCapability(QStringLiteral("huawei-usb-update.flash"));
        if (plugins.isEmpty()) {
            if (error) *error = QStringLiteral("未找到华为刷写插件（plugins/ 目录缺失或未加载）");
            return false;
        }
        QVariantMap capParams;
        capParams.insert(QStringLiteral("updateApp"), updateApp);
        emit outputMessage(QStringLiteral("华为 USB Update 刷写通道：%1").arg(deviceId), false);
        return plugins.first()->execute(
            QStringLiteral("huawei-usb-update.flash"), capParams, error);
    }
    if (channel == QStringLiteral("spd")) {
        // F4 通道：FDL 二进制用户提供（诚实边界）
        const QString pacPath = params.value(QStringLiteral("pacPath")).toString();
        const QString fdl1 = params.value(QStringLiteral("fdl1Path")).toString();
        const QString fdl2 = params.value(QStringLiteral("fdl2Path")).toString();
        if (pacPath.isEmpty()) {
            if (error) *error = QStringLiteral("缺少 pac 路径（spd 通道）");
            return false;
        }
        emit outputMessage(QStringLiteral("展锐刷写通道：%1").arg(deviceId), false);
        return spd::runSpdFlash(pacPath, fdl1, fdl2, nullptr, error);
    }
    if (error) *error = QStringLiteral("未知通道: %1").arg(channel);
    return false;
}
```

（`#include "core/modes/mtk_brom.h"`、`"core/modes/spd_storage.h"`、`"src/plugins/plugin_manager.h"`、`<QPair>` 加到 flash_tool.cpp 头部。MTK BROM 分区列表的构建方式实现时按 F1 `runBromFlash` 签名适配——若 params 结构不适配，先实现为"空分区列表 + 待接线标注"，诚实边界。）

- [ ] **Step 4: CMake 注册 + 构建验证**

CMakeLists.txt：`IMAGE_TEST_SOURCES` 追加 `${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pipeline.cpp`；`_test_extra_sources` 分支（test_pipeline → 无额外源，但需链 flash_tool/device_detector 相关源——实现时确认：若 flashChannelForMode 为纯静态函数，测试可仅链 flash_tool.cpp + device_detector 依赖；若链复杂，将 flashChannelForMode 移入独立小文件或标注"测试仅编译验证"）。以实际构建为准适配。

Run: `cmake --build build -j$(nproc)` → 零错误；`ctest --test-dir build -R test_pipeline` → 通过；全量 26/26。

- [ ] **Step 5: Commit**

```bash
git add src/core/flash_tool.h src/core/flash_tool.cpp tests/test_pipeline.cpp CMakeLists.txt
git commit -m "feat: FlashTool 分派路由 — MTK BROM/华为插件/展锐接入刷写管线 (F5-2)

- flashChannelForMode 纯函数（模式→通道映射，单测）
- flashFullPackage：按模式分派——华为经 PluginManager 运行时加载（插件隔离保持），
  MTK BROM/展锐直接调协议层；文件参数由 UI 传入（诚实边界）
- 插件缺失/参数缺失时明确错误（不假装）"
```

---

### Task F5-3: FlashPanel UI 接线

**Files:**
- Modify: `src/ui/flash_panel.cpp/h`（模式感知按钮 + 整包文件参数对话框）

**Interfaces:**
- Consumes: `FlashTool::flashFullPackage`（F5-2）
- Produces: 模式感知刷写流程——设备选中新模式（MTK BROM/华为 USB Update/展锐）时：
  - 华为：显示"选择 update.app"按钮 → QFileDialog → flashFullPackage(updateApp)
  - 展锐：显示"选择 pac + FDL1 + FDL2" → 三文件对话框 → flashFullPackage
  - MTK BROM：显示"选择 DA 二进制 + 分区镜像" → flashFullPackage（分区列表待接线标注）
  - 沿用现有 outputMessage 日志

- [ ] **Step 1: 实现模式感知分支**

flash_panel.cpp 在现有刷写按钮逻辑旁追加（实现时按 FlashPanel 现有结构适配——按钮 enable 逻辑按设备模式分派）：
```cpp
    // F5: 协议通道整包刷写（MTK BROM / 华为 USB Update / 展锐）
    const QString channel = FlashTool::flashChannelForMode(currentMode);
    if (!channel.isEmpty()) {
        QVariantMap params;
        if (channel == QStringLiteral("huawei-usb-update")) {
            const QString appPath = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择 update.app"), QString(),
                QStringLiteral("华为固件 (*.app)"));
            if (appPath.isEmpty()) return;
            params.insert(QStringLiteral("updateApp"), appPath);
        } else if (channel == QStringLiteral("spd")) {
            const QString pacPath = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择 pac 固件"), QString(),
                QStringLiteral("展锐固件 (*.pac)"));
            if (pacPath.isEmpty()) return;
            const QString fdl1 = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择 FDL1 二进制"));
            const QString fdl2 = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择 FDL2 二进制"));
            params.insert(QStringLiteral("pacPath"), pacPath);
            params.insert(QStringLiteral("fdl1Path"), fdl1);
            params.insert(QStringLiteral("fdl2Path"), fdl2);
        } else if (channel == QStringLiteral("mtk-brom")) {
            const QString daPath = QFileDialog::getOpenFileName(
                this, QStringLiteral("选择 DA 二进制"));
            if (daPath.isEmpty()) return;
            params.insert(QStringLiteral("daPath"), daPath);
            // 分区镜像选择（诚实边界：当前先整包/单分区待接线——按 F1
            // runBromFlash 分区结构适配，未适配前标注"分区选择待接线"）
        }
        QString error;
        if (!m_flashTool->flashFullPackage(m_currentDevice, currentMode, params, &error))
            emit outputMessage(QStringLiteral("刷写失败: %1").arg(error), true);
        else
            emit outputMessage(QStringLiteral("刷写完成"), false);
        return;
    }
```
（`#include <QFileDialog>` 与 `FlashTool::flashChannelForMode` 引用；currentMode/m_currentDevice 以 FlashPanel 实际成员为准。）

- [ ] **Step 2: 构建验证 + Commit**

Run: `cmake --build build -j$(nproc)` → 零错误；`ctest --test-dir build` → 26/26。冒烟：无设备/无插件时按钮路径不崩溃（单元不可测，人工冒烟路径标注）。

```bash
git add src/ui/flash_panel.h src/ui/flash_panel.cpp
git commit -m "feat: FlashPanel 模式感知刷写 — 协议通道文件参数对话框 (F5-3)

- 华为 update.app / 展锐 pac+FDL / MTK BROM DA 的文件选择流程
- 通道自动按设备模式启用（接入刷机管线，非手动插件面板）
- 诚实边界：MTK BROM 分区选择待接线标注"
```

---

## Self-Review 记录

- **Spec 覆盖**（用户指令"自动集成，其他的也是"）：DeviceDetector 3 新模式检测（F5-1）→ FlashTool 分派（F5-2）→ FlashPanel UI（F5-3）——三通道全部接入管线 ✓；华为插件经 PluginManager 运行时加载（插件隔离保持，法务纪律不破）✓
- **F2 缺口闭环**：插件面板参数无法传入的问题由 F5-3 的文件对话框解决（updateApp 从 UI 传入）✓
- **占位符扫描**：MTK BROM 分区列表"实现时按 F1 签名适配，未适配前标注待接线"——诚实边界非占位 ✓
- **类型一致性**：`flashChannelForMode`/`flashFullPackage` 跨任务签名一致；DeviceMode 枚举值跨文件一致 ✓
- **依赖顺序**：F5-1 → F5-2 → F5-3 串行；每任务结束是可独立测试的绿态（26/26 递增）✓
