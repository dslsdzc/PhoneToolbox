# 展锐 `.pac` 内 FDL 自动提取 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development 逐任务执行。
> Steps 用 `- [ ]` 勾选跟踪。

**Goal:** 用户只选一个 `.pac`，工具自动提取其中的 FDL1/FDL2 并用于刷写；手选保留为回退。

**Architecture:** 复用既有分层 —— `.pac` 解析已在 `imgpac::parsePac`（`PacPartition{name,fileName,offset,size}`）；
FDL 名判定已有纯函数 `spd::isFdlPartition`。本计划只加"按名切片"与 UI 接线，**不动协议层/刷写链**。

**事实与取舍：** 见 `docs/superpowers/specs/2026-09-18-spd-pac-fdl-auto-extract-design.md`（`PacPartition` 出处
`src/image_engine/pac_image.h:15-17`；名判定 `src/core/modes/spd_storage.h:78`；地址是常量
`spd_storage.cpp:249,258`；手选点 `src/ui/flash_panel.cpp:727-735`；测试侧构造器 `tests/test_pac.cpp:95,111`）。
**不做**：强制手选覆盖 / 解包内 XML 取地址 / 解压转换 FDL。

## Global Constraints

沿用 EUB 相位的全局约束：不做清单；无真机不得写"已验证"；`/tmp` 满 → 全新目录放 `/home`；
**"0 警告"须显式 `-Wall -Wextra`**；只 `git add` 明确路径（提交前用 `git status --porcelain` 对一遍清单）；
**恒真断言要加固并给变异证据（逐条留日志）**。

---

### Task 1: `spd::extractFdlsFromPac`（核心提取）

**Files:**
- Modify: `src/core/modes/spd_storage.{h,cpp}`（或新增 `spd_pac.{h,cpp}`——你判断；若新增需在 CMake 的
  `test_spd_*`/相关目标 extra sources 里登记，并**不要**把 libusb 拖进纯解析目标）
- Test: `tests/test_spd_pac.cpp`（新目标；用 `tests/test_pac.cpp` 的 `buildPacNew`/`buildPacLegacy` 造样本）

**Interfaces:**
```cpp
namespace spd {
struct ExtractedFdl { QString name; QString fileName; QByteArray data; };
// 从 pac 内提取 FDL1 与 FDL2（按 spd::isFdlPartition 判名，沿用其既有实现，不另立判据）。
// 两个都找到 → true；任一缺失 → false + 文案列出**缺哪个**与**包内有哪些条目**（可行动）。
bool extractFdlsFromPac(const QString &pacPath, ExtractedFdl &fdl1, ExtractedFdl &fdl2, QString *error);
}
```

- [ ] **Step 1: 失败用例**（用构造器造含 FDL1+FDL2 的新/老两代 pac → 提取字节与 `pacData.mid(offset,size)` 逐字节相等；
  缺 FDL2 → false + 文案含 `FDL2` 与包内条目；非 pac 文件 → false；条目顺序无关）
- [ ] **Step 2: RED**（`cmake --build build --target image_engine_tests_test_spd_pac`）
- [ ] **Step 3: 实现**（`parsePac` → 遍历找 FDL1/FDL2 → `mid`；失败清出参）
- [ ] **Step 4: GREEN + 全量 ctest**（现有 64 目标应 100%）
- [ ] **Step 5: 提交**（明确路径）

**变异证据（逐条留日志）**：① 只要找到一个就返回 true → "缺 FDL2"槽必红；② 取顺序反了（FDL2 填进 fdl1）→ 逐字节槽必红。

---

### Task 2: FlashPanel SPD 分支接线（自动优先 + 手选回退）+ 文档

**Files:**
- Modify: `src/ui/flash_panel.cpp`（SPD 分支 `~:727-735`）
- Test: 若该分支不可离线驱动 → **如实记录"UI 层无自动化覆盖"**（沿用既有做法，不许造假绿用例）
- Modify: `功能清单.txt`（队列项① → 已交付 + 边界）、`README.md`（展锐段落一句）

**行为契约：**
1. 选完 `.pac` 后**先尝试** `spd::extractFdlsFromPac`；
2. 成功 → **跳过两个手选对话框**，日志：`已从 pac 自动提取 FDL1（<条目名>，N 字节）与 FDL2（<条目名>，M 字节）`；
   提取出的两个 FDL 落盘到 `QStandardPaths::TempLocation`（文件名带条目名）供通道按路径读，刷写结束后 best-effort 删除；
   （⚠️ 本机 `/tmp` 满 → 用例**不得依赖真实临时目录**；落盘失败要报可行动错误而不是继续。）
3. 失败 → 日志给出原因（缺哪个 + 包内条目），**照旧弹两个手选**（用户无感降级）；
4. 文案仍如实：提取是纯解析；FDL 的真机可用性属既有 SPD 链的未验证范围。

- [ ] **Step 1: 实现 + 手动/可测的验证**（落盘逻辑用 `QTemporaryDir` 测；UI 分支若不可测则如实记录）
- [ ] **Step 2: 全量 ctest + 显式 `-Wall -Wextra` 复核**
- [ ] **Step 3: 文档口径**（功能清单/R/README）
- [ ] **Step 4: 提交**

---

## 计划自查

- 事实覆盖：全部来自本仓既有实现（无新协议事实）；**不做**项已列。
- 已知缺口：Task 2 的 UI 分支可能无自动化覆盖（如实记录，不造绿）；FDL 的真机可用性不在本计划范围。
