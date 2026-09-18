# 用户协议与风险分级门控 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development 逐任务执行。
> Steps 用 `- [ ]` 勾选跟踪。

**Goal:** 启动首屏一次使用须知 + 高风险功能二次确认，同意状态落 `QSettings` 留痕；协议正文与
`功能清单.txt` / README 的口径一致。

**Architecture:** 三个纯/半纯层 + 两个对话框 + 五处接线。映射层（`riskui`）是**无 Widgets 的纯函数**，
与 `flashui::flashButtonLabelsFor` 同款分层，可离线单测；留痕层（`legal::ConsentStore`）只依赖 QSettings；
UI 层两个对话框各带 offscreen 用例；接线用一个**唯一闸门函数** `riskui::ensureAccepted(...)`，
所有生产调用点都用全限定名，便于计数钉住。

**Tech Stack:** Qt6 Core + Widgets + Test；Qt 资源系统（`resources.qrc`）；QSettings（本仓首次引入）。

**Spec:** `docs/superpowers/specs/2026-09-18-consent-and-risk-gate-design.md`（已批准）。正文文本以该
spec §2 的代码块为**唯一副本**，逐字搬入资源文件，不得改写。

## Global Constraints

沿用本仓相位约束：**无真机不得写"已验证"**（本特性全部可离线验证，无此风险）；`/tmp` 满 → 临时目录
用 `QTemporaryDir`（它落在系统临时目录，用例**不得**依赖它可写大文件）；**"0 警告"须显式
`-Wall -Wextra`**；只 `git add` 明确路径；**恒真断言要加固并给变异证据（逐条留日志）**；文案类断言
用**三路比对**（spec 正文 ↔ 资源文件 ↔ 用例期望表）。

**本计划的三处已知风险（实现者必须照做）**：
1. `src/` 用 `file(GLOB_RECURSE ...)`（`CMakeLists.txt:107-111`）——**新增源文件后必须重跑
   `cmake -B build`**，否则新文件不进构建（症状：构造/符号找不到）。
2. 测试目标由 `tests/*.cpp` 通配生成，**特殊配置要写进 `CMakeLists.txt` 的 `_test_stem` 分支**
   （Widgets/offscreen/资源/额外源文件），照抄既有 `test_eub_recovery_dialog` 的写法。
3. `-Wswitch` 只能**提醒**不能红。本计划用 `#pragma GCC diagnostic error "-Wswitch"` 把"漏登记枚举"
   变成**编译错误**（下 §Task 1）；这是唯一让它真红的办法。

---

### Task 1: `riskui` 分级映射（纯函数）+ 全枚举覆盖用例

**Files:**
- Create: `src/ui/risk_ui.h`、`src/ui/risk_ui.cpp`
- Test: `tests/test_risk_tiers.cpp`
- Modify: `CMakeLists.txt`（`_test_extra_sources` 分支：本 stem → `src/ui/risk_ui.cpp`）

**Interfaces（后续任务按此调用，不得改名）：**
```cpp
// src/ui/risk_ui.h
#pragma once
#include <QString>
#include <QStringList>
#include "device_detector.h"          // src/core 已在 include 路径上

namespace riskui {

enum class RiskTier { None = 0, Tier2Boot = 2, Tier3Diagnostic = 3 };

enum class RiskFeature {
    DieRepair, FrpClear, PartitionFlash, Unlock, Sideload, AdbRoot, MtkUnlock,   // → Tier2Boot
    VulnExploit, EngineerMode, SafetyNet, SignatureSpoof, ImeiTool              // → Tier3Diagnostic
};

RiskTier   riskTierForMode(DeviceDetector::DeviceMode mode);
RiskTier   riskTierForFeature(RiskFeature f);
QString    tierTitle(RiskTier t);        // Tier2Boot="底层引导与写操作" Tier3Diagnostic="诊断与安全研究" None=""
QStringList tierWarnings(RiskTier t);    // 进二次确认对话框的风险要点；None → 空表

} // namespace riskui
```

**映射表（本计划的裁决，逐条落地，不要自行增删）：**

| DeviceMode | Tier |
|---|---|
| `MODE_UNKNOWN` | `None` |
| `MODE_ADB` / `MODE_FASTBOOT` / `MODE_FASTBOOTD` / `MODE_RECOVERY` | `Tier2Boot` |
| `MODE_EDL_9008` / `MODE_MTK_DA` / `MODE_MTK_BROM` | `Tier2Boot` |
| `MODE_HUAWEI_USB_UPDATE` / `MODE_SPD` / `MODE_SAMSUNG_ODIN` / `MODE_SAMSUNG_EUB` | `Tier2Boot` |

（除 `MODE_UNKNOWN` 外**全部** Tier2：FlashPanel 的每个协议通道都是底层写路径。`MODE_ADB`/`MODE_RECOVERY`
也是——面板在 ADB 模式下同样执行刷入/擦除，属写操作。）

`RiskFeature` 映射见上面枚举注释；`tierWarnings` 文本（**逐字**，用例钉住关键短语）：

- `Tier2Boot`：`{"这些操作直接写设备存储或底层引导，失败可能导致设备变砖、数据不可恢复。", "请先确认机型与固件匹配，并确认设备属于你或已获授权。", "部分通道未经真机验证（见项目文档「诚实边界」），实际行为可能与预期不同。"}`
- `Tier3Diagnostic`：`{"漏洞扫描与利用脚本会尝试绕过设备安全机制。", "未经设备所有者同意对他人设备使用，可能违法。", "脚本来自第三方数据库，本工具不对其效果与后果负责。"}`

- [ ] **Step 1: 失败用例**（`tests/test_risk_tiers.cpp`，纯 QtTest，无 Widgets）
  - 全枚举覆盖：把上表写成 `static const riskui::RiskTier kExpected[]`（**12 项**，顺序 = 枚举值 0..11），
    用 `static_assert(sizeof(kExpected)/sizeof(kExpected[0]) == DeviceDetector::MODE_SAMSUNG_EUB + 1, "DeviceMode 新增时同步更新本表")` 把表长钉在枚举上；
    `for (int m = 0; m <= DeviceDetector::MODE_SAMSUNG_EUB; ++m) QCOMPARE(riskTierForMode(static_cast<DeviceDetector::DeviceMode>(m)), kExpected[m]);`
  - `riskTierForFeature` 逐项断言（12 项，顺序 = 枚举声明顺序）
  - `tierTitle`：三档各一条（`None` → 空串）
  - `tierWarnings`：Tier2/Tier3 **非空**且分别含关键短语（`"变砖"` / `"绕过"`）；`None` → 空表
- [ ] **Step 2: RED**（`cmake -B build && cmake --build build --target image_engine_tests_test_risk_tiers`；
  预期：编译失败——头文件还不存在）
- [ ] **Step 3: 实现**。`riskTierForMode` **必须**用 `switch` 且**不带 `default:`**，外面包：
```cpp
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic error "-Wswitch"      // 新增 DeviceMode 而未在此登记 → 编译错误
#endif
    switch (mode) {
    case DeviceDetector::MODE_UNKNOWN:            return RiskTier::None;
    case DeviceDetector::MODE_ADB:                return RiskTier::Tier2Boot;
    /* … 12 个 case 全列（表见上）… */
    }
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
    return RiskTier::None;                        // 不可达（枚举已穷尽）；保留以免 UB
```
  `riskTierForFeature` 同款（12 项）。
- [ ] **Step 4: GREEN + 全量 ctest**（`ctest --test-dir build --output-on-failure`，现有目标应 100%）
- [ ] **Step 5: 变异证据（逐条留日志到 `.superpowers/sdd/w2-task-1-mutation-logs/`）**
  ① 把 `MODE_EDL_9008` 的 case 改成 `None` → 全枚举覆盖断言必红；
  ② 临时给 `DeviceMode` 加一个 `MODE_TMP = 12` → **编译必须失败**（`-Wswitch` 升级为 error；
     这条验的是"漏登记即红"的机制本身）；③ 把 `tierWarnings(Tier2)` 改成空表 → 非空断言必红。
  变异后**删净临时改动并重编译**再跑 ctest（否则跑的是变异二进制——本仓踩过）。
- [ ] **Step 6: 提交**（明确路径；DONE 前跑一次 `git status --porcelain` 对表）

---

### Task 2: `legal::ConsentStore`（同意留痕）+ 用例

**Files:**
- Create: `src/legal/consent_store.h`、`src/legal/consent_store.cpp`
- Test: `tests/test_consent_store.cpp`
- Modify: `CMakeLists.txt`（本 stem 的 `_test_extra_sources` = `src/legal/consent_store.cpp` + `src/ui/risk_ui.cpp`）

**Interfaces：**
```cpp
// src/legal/consent_store.h
#pragma once
#include <QString>
#include "risk_ui.h"                       // 只依赖 RiskTier 枚举；risk_ui 不反向依赖 legal

class QSettings;

namespace legal {
constexpr int kAgreementVersion = 1;       // 必须与 resources/legal/user-agreement.md 的「版本 N」一致（Task 3 用例钉住）

class ConsentStore {
public:
    explicit ConsentStore(QSettings *settings = nullptr);   // nullptr → 自建 IniFormat + UserScope
    ~ConsentStore();
    ConsentStore(const ConsentStore &) = delete;
    ConsentStore &operator=(const ConsentStore &) = delete;

    bool hasAcceptedAgreement() const;                   // 存储值 >= kAgreementVersion
    void acceptAgreement();                              // 写入当前版本
    bool hasAcceptedTier(riskui::RiskTier t) const;      // None → true（无需确认）
    void acceptTier(riskui::RiskTier t);
private:
    static QString keyForTier(riskui::RiskTier t);       // Tier2Boot→"legal/tier2Version" Tier3Diagnostic→"legal/tier3Version"
    QSettings *m_settings = nullptr;
    bool m_owned = false;
};
}
```
- 自建分支：`new QSettings(QSettings::IniFormat, QSettings::UserScope, QCoreApplication::organizationName(), QCoreApplication::applicationName())`
  （`src/main.cpp` 已设 organizationName=`"PhoneToolbox"`）；`m_owned=true`，析构 `delete`。
- 键：`legal/agreementVersion`、`legal/tier2Version`、`legal/tier3Version`。**不联网**。
- ⚠️ **spec §6 的一条要纠正**：spec 写"把 store 里写入 `kAgreementVersion+1` 的旧值 →
  `hasAcceptedAgreement()` 应为 false"。按 `>=` 语义这是**反的**：更高版本号意味着已接受更新版，
  应为 true。版本升级用例的正确形态是 **写入 `kAgreementVersion-1` → false**。实现按 `>=`，
  用例按本节写；在报告里注明与 spec §6 的这处出入。

- [ ] **Step 1: 失败用例**（用 `QTemporaryDir` + `QSettings(dir.filePath("t.ini"), QSettings::IniFormat)`
  注入，**不碰真实用户配置**）
  - 全新 ini：`hasAcceptedAgreement()` false；`acceptAgreement()` → true（同一 store 与新建 store 两次都查）
  - **版本升级语义**：直接 `settings.setValue("legal/agreementVersion", legal::kAgreementVersion - 1)` → false；
    写 `kAgreementVersion` → true；写 `kAgreementVersion + 1` → true（更高版本不回退）
  - tier 独立：`acceptTier(Tier2Boot)` → Tier2 true / Tier3 false；`hasAcceptedTier(None)` → true
  - tier 也受版本约束：把 `legal/tier2Version` 写成 `kAgreementVersion - 1` → false
- [ ] **Step 2: RED**（目标 `image_engine_tests_test_consent_store`）
- [ ] **Step 3-4: 实现 → GREEN + 全量 ctest**
- [ ] **Step 5: 变异证据**：① `hasAcceptedAgreement` 改成 `> 0` 判断 → 版本升级槽必红；
  ② `keyForTier` 两个 tier 返回同一个键 → tier 独立槽必红。（日志同上目录）
- [ ] **Step 6: 提交**

---

### Task 3: 协议资源 + `AgreementDialog`（首屏）+ 用例

**Files:**
- Create: `resources/legal/user-agreement.md`（**逐字**取自 spec §2 的 markdown 代码块，含首行「# PhoneToolbox 使用须知与免责声明（版本 1）」）
- Modify: `resources/resources.qrc`（登记 `legal/user-agreement.md`，前缀 `/legal`）
- Create: `src/ui/agreement_dialog.h`、`src/ui/agreement_dialog.cpp`
- Test: `tests/test_agreement_dialog.cpp`
- Modify: `CMakeLists.txt`（本 stem：`_test_extra_sources` = `src/ui/agreement_dialog.cpp`；
  加进 Widgets + offscreen 名单；**并把 `${QRC_FILES}` 编进本目标**才能读 `:/legal/...`）

**Interfaces：**
```cpp
// src/ui/agreement_dialog.h
#pragma once
#include <QDialog>

namespace agreementui {
// 展示使用须知（只读滚动 + 一个必勾确认框）。返回 true = 用户同意。
// 用户关闭/拒绝 → false（调用方负责退出程序）。
bool ask(QWidget *parent);
}
```
- 正文：`QFile(":/legal/user-agreement.md")` 读进 `QTextBrowser`（只读、可滚动）；读不到 → 显示
  一行错误并**禁用**同意按钮（不许"读不到也能同意"）。
- 控件：`QCheckBox("我已阅读并同意上述使用须知")`；按钮「同意并继续」(默认禁用) / 「退出」。
- 若窗口放不下：正文区最小高度 320px，不用 `resize` 写死尺寸。

- [ ] **Step 1: 失败用例**（offscreen；同既有对话框用例的 `QTEST_MAIN` 形态）
  - `:/legal/user-agreement.md` **存在且非空**（资源真的编进来了——这条会先红，直到 CMake 改对）
  - 正文含 `"版本 1"`，且解析出的数字 **== `legal::kAgreementVersion`**（三路一致钉）
  - 正文含三条关键句：`"不包含"`、`"违法"`、`"GPLv3"`（防正文被替换成占位文本）
  - 勾选前「同意并继续」禁用、勾选后可用；点「退出」→ `ask()` 返回 false
- [ ] **Step 2: RED → Step 3: 实现 → Step 4: GREEN + 全量 ctest**
- [ ] **Step 5: 变异证据**：① 把资源里的「版本 1」改成「版本 2」→ 版本一致槽必红（**改完改回**）；
  ② 把勾选框初始 `setChecked(true)` → "勾选前禁用"槽必红。
- [ ] **Step 6: 提交**

---

### Task 4: `RiskConfirmDialog` + 唯一闸门 `riskui::ensureAccepted`

**Files:**
- Create: `src/ui/risk_confirm_dialog.h`、`src/ui/risk_confirm_dialog.cpp`
- Test: `tests/test_risk_confirm_dialog.cpp`
- Modify: `CMakeLists.txt`（本 stem：`_test_extra_sources` = `risk_confirm_dialog.cpp` + `risk_ui.cpp` + `consent_store.cpp`；
  加进 Widgets + offscreen 名单）

**Interfaces（Task 5 的接线全靠它）：**
```cpp
// src/ui/risk_confirm_dialog.h
#pragma once
#include <QWidget>
#include "risk_ui.h"

namespace legal { class ConsentStore; }

namespace riskui {
// 唯一闸门。已接受过该 tier（或 tier == None）→ true，不弹窗；
// 否则弹二次确认：勾选并继续 → 记入 store 并 true；取消/关窗 → false。
bool ensureAccepted(QWidget *parent, RiskTier tier, legal::ConsentStore *store = nullptr);
// store == nullptr → 用进程内默认 store（QSettings UserScope）。
}
```
- 对话框内容：标题 `tierTitle(tier)` + `tierWarnings(tier)` 逐条 + **协议入口**（一个可展开的
  `QTextBrowser` 里放同一份 `:/legal/user-agreement.md`）+ 必勾确认框「我已了解上述风险」。
- 取消语义：**静默返回 false**（与"用户取消"同口径），调用方不发起任何动作。

- [ ] **Step 1: 失败用例**
  - 对话框部分：未勾选「继续」禁用；勾选后可用；展示要点逐条来自 `tierWarnings()`（用
    `findChildren<QLabel*>`/文本包含断言，不要在用例里复制文案表——从函数取期望值）
  - `ensureAccepted` 部分（注入临时 store）：`tier=None` → true 且 store 无写入；
    已接受过的 tier → true 且**不再写**（用 `QSettings` 直读键值前后对比）；
    未接受 + 用户取消 → false 且 store 无写入。（"接受"分支需真人点按钮，**不驱动**；如实记录该分支
    只有对话框级用例覆盖——**不许造假绿用例**）
- [ ] **Step 2: RED → Step 3: 实现 → Step 4: GREEN + 全量 ctest**
- [ ] **Step 5: 变异证据**：① `ensureAccepted` 第一行改成恒 `return true;` → 已接受/取消两条槽必红；
  ② 对话框「继续」按钮去掉禁用初值 → 未勾选槽必红。
- [ ] **Step 6: 提交**

---

### Task 5: 接线（五处）+ 结构钉 + 文档

**Files:**
- Modify: `src/main.cpp`（首屏）、`src/ui/flash_panel.cpp`（14 个 handler）、
  `src/ui/eub_recovery_dialog.cpp`（1 处）、`src/ui/system_tool_panel.cpp`（4 处）、`src/ui/vuln_panel.cpp`（3 处）
- Test: `tests/test_risk_gate_wiring.cpp`（**结构钉**：读生产源文件计数）
- Modify: `CMakeLists.txt`（本 stem 仅需 `risk_ui.cpp` + `consent_store.cpp` 供链接；
  若只做字符串计数**不 include 头**，也要给 `_test_extra_sources` 以免链接期缺符号）
- Modify: `功能清单.txt`、`README.md`

**接线清单（逐条，缺一不可；发现与磁盘不符按磁盘为准并在报告里列出差异）：**

| 文件 | 位置 | 写法 |
|---|---|---|
| `src/main.cpp` | `MainWindow window;` **之前** | `legal::ConsentStore store; if (!store.hasAcceptedAgreement()) { if (!agreementui::ask(nullptr)) return 0; store.acceptAgreement(); }` |
| `src/ui/flash_panel.cpp` | 14 个 handler **函数体第一行** | `if (!riskui::ensureAccepted(this, riskui::riskTierForMode(m_currentMode))) return;` |

FlashPanel 的 14 个 handler（`src/ui/flash_panel.cpp`）：
`onFlashClicked`(:685) / `onEraseClicked` / `onUpdateClicked` / `onSideloadClicked` / `onRunScriptClicked` /
`onDumpClicked` / `onBootImageClicked` / `onUnlockBootloader` / `onLockBootloader` / `onFrpErase` /
`onBrickRepairClicked` / `onEdlConnect` / `onEdlPlanFlash` / `onMtkConnect`
（后三个是"底层引导"入口：连接即上传 programmer/DA 到设备 RAM，同属 Tier2。）

| 其他文件 | 位置 | tier 取法 |
|---|---|---|
| `src/ui/eub_recovery_dialog.cpp` | `onStart()`(:373) 首行 | `riskTierForMode(DeviceDetector::MODE_SAMSUNG_EUB)` |
| `src/ui/system_tool_panel.cpp` | `onSpoofDevice` / `onSafetynetBypass` / `onSignatureSpoof` / `onOpenEngineerMode`(:1466) 首行 | `riskTierForFeature(RiskFeature::ImeiTool / SafetyNet / SignatureSpoof / EngineerMode)` |
| `src/ui/vuln_panel.cpp` | `onScan`(:362) / `onExploitAll`(:492) / `onExploitSingle`(:558) 首行 | `riskTierForFeature(RiskFeature::VulnExploit)` |

- ⚠️ 若 FlashPanel 没有现成的 `m_currentMode` 成员，用面板已有的模式来源（实现者先查；
  不允许为此新增并行状态）。EUB 对话框若拿不到 `DeviceDetector` 头，直接传枚举常量。

- [ ] **Step 1: 接线**（上表逐条；每处都用**全限定名** `riskui::ensureAccepted(` —— 结构钉靠它计数）
- [ ] **Step 2: 结构钉用例**（`tests/test_risk_gate_wiring.cpp`，纯字符串计数，**不链 Widgets**）
  ```cpp
  static int countIn(const QString &rel, const QString &needle) {
      QFile f(QStringLiteral(QT_TESTCASE_SOURCEDIR "/").append(rel));
      if (!f.open(QIODevice::ReadOnly)) return -1;  // 打不开 → 用例红（不是静默 0）
      return QString::fromUtf8(f.readAll()).count(needle);
  }
  ```
  断言（**每条都给出计数与"为什么是这个数"**）：
  - `src/ui/flash_panel.cpp`：`"riskui::ensureAccepted("` == **14**；`"connect(m_"` + `"Btn, &QPushButton::clicked"` 的配对计数 == 记录值
    （**新增按钮 → 这条红 → 强迫作者回来核对闸门**；这是 tripwire，不是证明，报告里如实写）
  - `src/ui/system_tool_panel.cpp` == 4；`src/ui/vuln_panel.cpp` == 3；`src/ui/eub_recovery_dialog.cpp` == 1
  - `src/main.cpp` 含 `"hasAcceptedAgreement()"` 且含 `"agreementui::ask("`
  - **取材边界（写进报告）**：计数只数字面出现次数——包进 `if (false)` 仍绿；它的价值是"阻止无声漏接线"。
- [ ] **Step 3: 文档**：`功能清单.txt` 增一节「使用须知与风险分级」（启动首屏 + 二次确认 + 留痕在本机 +
  **门控是客户端提醒、可被绕过**），口径与协议正文一致；`README.md` 补 2-3 句。
- [ ] **Step 4: 全量 ctest + 显式 `-Wall -Wextra` 复核**（全新目录，放 `/home` 下）
- [ ] **Step 5: 提交**

---

## 计划自查

- **spec 覆盖**：§2 正文→T3；§3 映射→T1；§4 留痕→T2；§5 五处钩子→T5；§6 用例→T1-T5；
  §7 交付清单 7 项 → 全部有任务；§8 诚实边界 → T5 的文档节 + 报告如实写。
- **与 spec 的两处出入（已在上文标注）**：① §6 的版本升级断言方向写反（T2 按 `>=` 落地并说明）；
  ② §5 表里"SystemToolPanel 的 5 个入口"实际只有 4 个在 SystemToolPanel，其余（FRP/死砖/解锁/
  Sideload）在 FlashPanel —— T5 的接线清单按**磁盘实况**列，共 22 处调用点。
- **不做**：不改漏洞库/利用链本身；不给只读功能加门控；不做在线校验/DRM/遥测。
