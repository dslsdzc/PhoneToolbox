# 维修诊断模块 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `system_tool_panel` 新增"维修诊断"分类，实现 12 项手机维修场景诊断测试（纯 ADB 优先，Root 项标注）。

**Architecture:** 命令构造与输出解析做成纯函数模块 `src/core/repair_commands.h/.cpp`（无 UI 依赖，可 Qt Test），面板侧 `SystemToolPanel` 新增"维修诊断"页复用现有 ADB QProcess 执行模式（参考 `executeAdb` 与现有分类页 pattern）。

**Tech Stack:** C++17, Qt6, 现有 `AdbEmbedded`/`FlashTool::executeAdb` 模式。

## Global Constraints

- C++17，成员变量 `m_` 前缀；命令模块纯函数（输入 QStringList 命令 → 输出解析结构）
- 纯 ADB 项优先落地；需 Root 项 UI 标注"需 Root"
- 实时流（getevent）用 QProcess 持续读 + 信号更新 UI，不得阻塞 UI 线程
- 每个任务独立 commit，前缀 `feat:`；解析逻辑 Qt Test（新测试文件 `tests/test_repair_commands.cpp`）
- EEPROM/字库读写（EDL/MTK 通道）不在本计划（spec 已列后续扩展）

---

## 文件结构

```
src/core/
  repair_commands.h/.cpp    # 命令构造 + 输出解析（纯函数）
src/ui/
  system_tool_panel.cpp/h   # 新增"维修诊断"分类页（Modify）
tests/
  test_repair_commands.cpp
```

---

### Task E1: repair_commands — 命令构造模块

**Files:**
- Create: `src/core/repair_commands.h`, `src/core/repair_commands.cpp`
- Create: `tests/test_repair_commands.cpp`
- Modify: `CMakeLists.txt`（IMAGE_TEST_SOURCES 追加 —— 注意：repair_commands 编入主程序（src/core 已被 GLOB 收集），测试目标仅链接该模块编译产物）

**Interfaces:**
- Produces: `namespace repair { QStringList batteryDumpCmd(); QStringList powerSupplyReadCmd(const QString &attr); QStringList sensorDumpCmd(); QStringList telephonyDumpCmd(); QStringList imeiQueryCmd(); QStringList mmcLifeReadCmd(); QStringList audioToneCmd(int ms); QStringList geteventCmd(); QStringList cameraLaunchCmd(bool front); QStringList wifiStatusCmd(); QStringList btStatusCmd(); QStringList nfcStatusCmd(); }`（全部返回 `adb shell ...` 参数列表，由调用方拼 `adb` 前缀 + `-s <serial>`）

- [ ] **Step 1: 写失败测试**

`tests/test_repair_commands.cpp`（断言命令构造正确）：

```cpp
#include <QtTest>
#include "core/repair_commands.h"

class TestRepairCommands : public QObject
{
    Q_OBJECT
private slots:
    void batteryDump();
    void audioTone();
    void cameraFront();
};

void TestRepairCommands::batteryDump()
{
    QStringList cmd = repair::batteryDumpCmd();
    QCOMPARE(cmd, QStringList({"shell", "dumpsys", "battery"}));
}

void TestRepairCommands::audioTone()
{
    QStringList cmd = repair::audioToneCmd(3000);
    QVERIFY(cmd.contains("3000"));
    QVERIFY(cmd.first() == "shell");
}

void TestRepairCommands::cameraFront()
{
    QStringList cmd = repair::cameraLaunchCmd(true);
    // 前摄: am start -a android.media.action.IMAGE_CAPTURE 带前置参数
    QVERIFY(cmd.contains("android.media.action.IMAGE_CAPTURE"));
}

QTEST_APPLESS_MAIN(TestRepairCommands)
#include "test_repair_commands.moc"
```

- [ ] **Step 2: 运行确认失败**

Run: `cmake --build build --target image_engine_tests_test_repair_commands && ./build/image_engine_tests_test_repair_commands`
Expected: 编译错误（repair 未声明）。

- [ ] **Step 3: 实现**

`repair_commands.h`:

```cpp
#pragma once
#include <QStringList>

namespace repair {

// 全部返回 `adb shell ...` 的参数列表（不含 adb 与 -s 前缀，由调用方拼接）
QStringList batteryDumpCmd();                 // dumpsys battery
QStringList powerSupplyReadCmd(const QString &attr); // cat /sys/class/power_supply/battery/<attr>
QStringList sensorDumpCmd();                  // dumpsys sensorservice
QStringList telephonyDumpCmd();               // dumpsys telephony.registry
QStringList imeiQueryCmd();                   // service call iphonesubinfo 1 | grep -o ...
QStringList mmcLifeReadCmd();                 // cat /sys/class/mmc_host/mmc0/mmc0:*/life_time (root)
QStringList audioToneCmd(int ms);             // cmd audio play-tone <ms>
QStringList geteventCmd();                    // getevent -lt
QStringList cameraLaunchCmd(bool front);      // am start -a android.media.action.IMAGE_CAPTURE
QStringList wifiStatusCmd();                  // dumpsys wifi | grep -E "Wi-Fi is|mWifiInfo"
QStringList btStatusCmd();                    // dumpsys bluetooth_manager | grep mState
QStringList nfcStatusCmd();                   // dumpsys nfc | grep mState

} // namespace repair
```

`repair_commands.cpp` 按接口实现（命令串参考：`dumpsys battery`、`cat /sys/class/power_supply/battery/current_now`、`cmd audio play-tone 3000`、`getevent -lt`、`am start -a android.media.action.IMAGE_CAPTURE`、`service call iphonesubinfo 1` 等）。

- [ ] **Step 4: 运行测试确认通过**

Run: `cmake --build build --target image_engine_tests_test_repair_commands && ./build/image_engine_tests_test_repair_commands`
Expected: 3 个测试全过。

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/core/repair_commands.* tests/test_repair_commands.cpp
git commit -m "feat: 维修诊断命令构造模块 (TDD)"
```

---

### Task E2: 输出解析 — 电池/电源/传感器

**Files:**
- Modify: `src/core/repair_commands.h/.cpp`（解析结构 + 解析函数）
- Modify: `tests/test_repair_commands.cpp`

**Interfaces:**
- Produces: `struct repair::BatteryInfo { int level; QString status; int tempC; int voltageMv; }; bool repair::parseBatteryDump(const QString &output, BatteryInfo &out);`（dumpsys battery 输出 `level: 85`/`status: 2`/`temperature: 300`/`voltage: 4100`）；`struct repair::PowerSupply { qint64 currentUa; qint64 voltageUv; qint64 temp; }; bool repair::parsePowerSupply(const QString &currentNow, const QString &voltageNow, const QString &temp, PowerSupply &out);`；`bool repair::parseSensors(const QString &output, QMap<QString, QString> &out);`（sensorservice 输出 `name|vendor|version|...|value1,value2,value3` 行）

- [ ] **Step 1-4: TDD 循环**（测试样本：真实 dumpsys battery 输出片段 + sensorservice 行格式）

解析要点：`level: 85` 正则 `^level:\s*(\d+)$`；temperature 单位 0.1°C（300 = 30.0°C）；voltage 单位 mV。

- [ ] **Step 5: Commit**

```bash
git add src/core/repair_commands.* tests/test_repair_commands.cpp
git commit -m "feat: 电池/电源/传感器输出解析 (TDD)"
```

---

### Task E3: 输出解析 — 通信/IMEI/存储寿命

**Files:**
- Modify: `src/core/repair_commands.h/.cpp`
- Modify: `tests/test_repair_commands.cpp`

**Interfaces:**
- Produces: `struct repair::TelephonyInfo { QString imei; QString signalDbm; QString networkType; }; bool repair::parseTelephony(const QString &registryDump, const QString &imeiOutput, TelephonyInfo &out);`；`int repair::parseMmcLife(const QString &lifeTime, bool *ok);`（life_time 文件 "0x01 0x02" → 取最大）

- [ ] **Step 1-4: TDD 循环**

- [ ] **Step 5: Commit**

```bash
git add src/core/repair_commands.* tests/test_repair_commands.cpp
git commit -m "feat: 通信/IMEI/存储寿命解析 (TDD)"
```

---

### Task E4: SystemToolPanel 维修诊断页 UI

**Files:**
- Modify: `src/ui/system_tool_panel.cpp/h`（新增"维修诊断"分类页，复用现有分类页创建模式 `addCategoryPage`/`addHint`/`addRow`）

**Interfaces:**
- Produces: 页内测试项（对应 12 项）：纯色屏幕（按钮组 5 色 + 亮度滑块）、触摸画线、电池诊断（实时刷新区）、循环次数(Root 标注)、音频测试（时长选择）、麦克风回路、传感器读数（表格刷新）、相机（前后摄 + 闪光灯）、getevent（实时流区域）、通信状态（WiFi/BT/NFC/SIM 按钮 + 结果区）、eMMC/UFS 寿命(Root 标注)、IMEI 查询
- 实现方式：复用面板现有 `executeAdb`/`runShell` 槽模式；getevent 用 QProcess 常驻 + `readyRead` 信号；实时刷新用 QTimer（500ms，参考性能监控现有 pattern）

- [ ] **Step 1-4: 实现 + 构建验证**

验证：构建通过；无设备时按钮禁用或点击提示"无设备连接"（遵循现有 panel 的 `m_connected` 状态管理）。

- [ ] **Step 5: Commit**

```bash
git add src/ui/system_tool_panel.*
git commit -m "feat: 维修诊断分类页 UI (12 项)"
```

---

### Task E5: 实时流与集成验证

**Files:**
- Modify: `src/ui/system_tool_panel.cpp`（getevent 实时流 + 传感器/电池定时刷新接线）

**Interfaces:**
- 无新接口；验证：连接真机或模拟器时各测试项输出正确；无设备时全部禁用

- [ ] **Step 1-4: 实现 + 手动冒烟**

- [ ] **Step 5: Commit**

```bash
git add src/ui/system_tool_panel.cpp
git commit -m "feat: getevent 实时流与诊断刷新 (UI)"
```

---

## Self-Review 记录

- **Spec 覆盖**：12 项清单 → E1（命令构造）+ E2/E3（解析）+ E4/E5（UI/实时）；Root 项标注（E4）；EEPROM 扩展不在范围（Global Constraints）。
- **诚实标注**：纯色屏幕/麦克风回路的设备端依赖（cmd display 亮度可控、录音需设备端 app）在 E4 实现时按实际 ADB 能力呈现（能做到什么显示什么，做不到的项标注"需设备端测试应用"）。
- **依赖顺序**：E1→E2→E3→E4→E5；E1-E3 为纯函数可独立测试。
- **类型一致性**：`repair::` 命名空间下 `BatteryInfo/PowerSupply/TelephonyInfo` 与 `parse*` 函数命名跨任务一致。
