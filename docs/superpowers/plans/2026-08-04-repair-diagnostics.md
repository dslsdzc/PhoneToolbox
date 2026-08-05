# 维修诊断模块 Implementation Plan（2026-08-05 重写）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `system_tool_panel` 新增"维修诊断"分类，实现**「打开工程模式」按钮** —— 识别设备品牌/芯片 → 启动对应厂商工程模式（自检流程由厂商模块承担）。2026-08-05 用户决策：不做 12 项自研测试，厂商工程模式覆盖屏幕/触摸/传感器/音频/相机/按键/电池自检。

**Architecture:** 入口映射做成纯函数模块 `src/core/engineer_mode.h/.cpp`（品牌/芯片 → 拨号码/Activity，无 UI 依赖可 Qt Test），面板侧 `SystemToolPanel` 新增"维修诊断"分类页复用现有 ADB QProcess 执行模式。

**Tech Stack:** C++17, Qt6, 现有 `AdbEmbedded`/`FlashTool::executeAdb` 模式。

## Global Constraints

- C++17，成员变量 `m_` 前缀；映射模块纯函数（输入设备信息 → 入口描述）
- **默认信息不可信原则（用户要求）**：各品牌工程模式入口（拨号码/Activity 包名）**必须联网搜索验证**（WebSearch 官方/维修社区资料，交叉核对），不凭记忆；验证结果写入报告并注明来源
- **前端隔离约束（计划 D 延续，若本计划改 UI）**：`image_engine`/`root_patcher` 冻结；`src/core/` 的维修模块为新增文件不属后端库（engineer_mode 独立于 image_engine），可新增
- 纯 ADB 实现（拨号盘/am start），无需 Root
- 每个任务独立 commit，前缀 `feat:`；映射逻辑 Qt Test（`tests/test_engineer_mode.cpp`）

---

## 文件结构

```
src/core/
  engineer_mode.h/.cpp      # 品牌/芯片 → 工程模式入口（纯函数，可测试）
src/ui/
  system_tool_panel.cpp/h   # 新增"维修诊断"分类页（Modify，仅一个按钮）
tests/
  test_engineer_mode.cpp
```

---

### Task E1: engineer_mode — 入口映射模块

**Files:**
- Create: `src/core/engineer_mode.h`, `src/core/engineer_mode.cpp`
- Create: `tests/test_engineer_mode.cpp`
- Modify: `CMakeLists.txt`（IMAGE_TEST_SOURCES 追加 —— engineer_mode 编入主程序 src/core 已 GLOB，测试目标链接该编译产物）

**Interfaces:**
- Produces: `namespace engmode { struct Entry { QString name; QString dialCode; // 拨号码（如 *#*#6484#*#*） QString activity;   // am start -n 目标（如 com.mediatek.engineermode/.EngineerMode） QString note; }; QString detectBrand(const QString &roProductBrand); // 品牌归一（xiaomi→Xiaomi） Entry lookup(const QString &brand, const QString &hardware, const QString &model); // 品牌→入口；返回 note 标注来源/可靠性 bool isValid(const Entry &e); }`

**⚠️ 联网验证（强制）**：各品牌工程模式入口表必须搜索验证后写死：
- MTK 芯片（hardware 含 mtk/mediatek）：`*#*#3646633#*#*` 与 `com.mediatek.engineermode` Activity
- 高通（hardware 含 qcom/sm/sdm）：`*#*#4636#*#*`（系统自带 Testing）
- 三星：`*#0*#`（FactoryTest）
- 小米/红米：`*#*#6484#*#*`（CIT）
- 华为/荣耀：`*#*#2846579#*#*`
- OPPO/一加/realme、vivo、联想等：搜索验证后补充
- 每个入口标注验证来源；无法验证的入口不写入（返回 note "未验证" 或留空降级提示）

- [ ] **Step 1: 写失败测试**（断言品牌归一 + 已知品牌入口非空 + 未知品牌返回空 Entry + 入口字段合法性：拨号码以 *# 开头或 Activity 以包名/ 开头）

- [ ] **Step 2: 运行确认失败**（编译错误）

- [ ] **Step 3: 联网验证入口表 + 实现**（验证结果写入报告，含来源 URL）

- [ ] **Step 4: 运行测试确认通过**

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/core/engineer_mode.* tests/test_engineer_mode.cpp
git commit -m "feat: 工程模式入口映射模块 (TDD)"
```

---

### Task E2: SystemToolPanel 维修诊断页 + 打开工程模式接线

**Files:**
- Modify: `src/ui/system_tool_panel.cpp/h`（新增"维修诊断"分类页：一个「打开工程模式」按钮 + 说明文案）

**Interfaces:**
- Consumes: `engmode::detectBrand/lookup`（E1）、现有 `executeAdb`/`runShell` 槽模式
- Produces: 点击按钮 → 读设备品牌/芯片（`getprop ro.product.brand` / `ro.hardware` / `ro.product.model`）→ `engmode::lookup` → 有 Activity：`am start -n <activity>`；只有拨号码：`am start -a android.intent.action.DIAL -d "tel:<code>"`（打开拨号盘，不拨打）；无入口：明确错误 + 提示"该设备无已知工程模式入口"

- [ ] **Step 1-4: 实现 + 构建验证**

验证：构建通过；无设备时按钮禁用或点击提示"无设备连接"（遵循现有 panel 的 `m_connected` 状态管理）；有设备时点击按入口表执行。

- [ ] **Step 5: Commit**

```bash
git add src/ui/system_tool_panel.*
git commit -m "feat: 维修诊断页与工程模式打开 (UI)"
```

---

## Self-Review 记录

- **Spec 覆盖**：2026-08-05 简化决策（单按钮打开工程模式）→ E1（入口映射）+ E2（UI 接线）；EEPROM/字库写入仍为后续扩展。
- **不可信原则**：E1 强制联网验证入口表（品牌/芯片 → 拨号码/Activity），来源入报告。
- **依赖顺序**：E1→E2。
- **类型一致性**：`engmode::Entry/detectBrand/lookup/isValid` 命名跨任务一致。
