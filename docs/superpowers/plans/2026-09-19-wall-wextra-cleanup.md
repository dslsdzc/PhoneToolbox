# 全库 `-Wall -Wextra` 开启 + 存量警告清零 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development 逐任务执行。

**Goal:** 把 `-Wall -Wextra` 变成**构建的默认**（不是"某次手动加过的开关"），并把由此暴露的**存量警告全部清零** ——
让"0 警告"从此是**有开关约束的、可复核的事实**，而不是每次都要另开目录另跑一遍才成立的弱证据。

**背景（用户 2026-09-18 已裁决）：** "全库开 + 修完既有警告"。

**基线（控制方实测，2026-09-19，两个独立来源吻合）：** 全新目录 `-DCMAKE_CXX_FLAGS="-Wall -Wextra"` 从零构建 =
**111 条 warning / 14 个 TU / 0 error**。逐文件分布：

| 文件 | 条数 | | 文件 | 条数 |
|---|---|---|---|---|
| `tests/test_oppo_ofp.cpp` | 30 | | `src/image_engine/fs/fs_image.cpp` | 2 |
| `tests/test_oppo_extract.cpp` | 29 | | `src/image_engine/bspatch_image.cpp` | 2 |
| `tests/test_oppo_ops.cpp` | 18 | | `src/ui/vuln_panel.cpp` | 1 |
| `tests/test_image_worker.cpp` | 12 | | `tests/test_hisi_update.cpp` | 1 |
| `src/image_engine/fs/ext4_reader.cpp` | 4 | | `tests/test_fs_ext4.cpp` | 1 |
| `src/core/flash_tool.cpp` | 4 | | | |
| `src/core/modes/mtk_payload.cpp` | 3 | | | |
| `src/core/restart_tool.cpp` | 2 | | | |
| `src/vuln_db/importers/local_importer.cpp` | 2 | | | |

（**开工时以当时实测为准**：W4/W2 批次会改动其中若干文件，且 W4-T3 已发现 `flash_tool.cpp` 那 4 条里含 2 条
`-Wunused-variable`（`kPlatformScript`/`kOtherScript`）——**先跑一遍记账，别照抄上表**。）

## Global Constraints

- **无真机不得写"已验证"**；本波全是编译期/用例层，无真机依赖。
- **"0 警告"的取证格式（本仓已三次栽在假绿灯上，硬要求）**：
  1. 全新构建目录放 `/home`（**`/tmp` 已满**）。**开关已进 `CMakeLists.txt` 默认（W5-T1 `74fb0ca`）**——
     配置时**不要**再传 `-DCMAKE_CXX_FLAGS`（那是"手动加开关"时代的取证法，现在验的是更强的命题"默认就有"）；
     要核开关真的生效，从 `build.ninja` 提一条真实编译命令看有没有 `-Wall -Wextra`；
  2. 日志先自检：`grep -c 'Building CXX object' <日志>` **> 0**（否则就是 no-op，见"未编译 ≠ 无警告"）；
  3. 构建退出码**别丢在管道里**（`--target` 名字写错时 ninja 报错而管道退出码仍可能是 0）；
  4. 报"0 警告"时必须同时报**构建步数**与**退出码**。
- 只 `git add` 明确路径；逐条小提交（提交信息写文件名或警告类型，如 `fix(warn): oppo_ofp 30 条 -W…`）。
- 修警告**不许改行为**：能加 `Q_UNUSED`/`static_cast`/`[[maybe_unused]]` 就加；**不许**为了让警告消失而
  删掉仍有用途的代码、改公开签名、或把 `-Werror` 塞进来。任何一处**不得不动行为**的，单列进报告让控制方裁决。
- 用例里的警告（`test_oppo_*` 占 77 条）同样要修 —— 测试代码也是代码。

## 口径（W5-T1 实测更正，**后续任务按此数**）

- **构建图内只有 C++**：`src/`、`tests/` 下 **0 个 `.c` 文件**，`project(... LANGUAGES CXX)`；`build.ninja` 里 C_COMPILER 规则 0 条。
  （仓库树里另有 **77 个 `.c`**，但都在 `reference/`（gitignored）与 `mtkclient/` 子模块里，**不在构建图内** —— 说"全仓 0 个 `.c`"不准确。）
  开关仍用不区分语言的形态落（`add_compile_options`），对两条规则都生效（W5-T1 用仓外合成工程实测）。
- ⚠️ **"条数 → 处数"有两类折减，且"处"本身有三档口径**（W5-T5 实测 + W5-T3 复审细化）：
  ① **多目标重复**（一个文件编进 N 个目标 → 每处报 N 次）。乘数可从 `build.ninja` 的对象 build edge 数核：
  `flash_tool.cpp` → 2 个 `.o`、`mtk_payload.cpp` → 3 个 `.o`；**静态库路径不重复**（`ext4_reader.cpp` 只 1 个 `.o`）。
  ② **同位置/同行多诊断**，细分两小类（**别在去重时把后者当 1 条**）：
  - **同位置重复**：同一 `(file,line,col)` 报多条（T3 的三个站点各重复 4 条 → 12 条只落 3 个位置）；
  - **同行不同列**：`bspatch_image.cpp:134:28` + `:134:49`（2 条 = 1 行）。
  实测：`restart_tool` 2 条 = 1 处（同一 switch 两个缺失枚举值）、`local_importer` 2 条 = 1 处（同一行两个未用参数）
  → W5-T5 那 12 条实为 **6 处**；T3 那 12 条按"3 个诊断位置 / 3 个源行 / **1 个真实修改点**（结构体 2 行 DMI）"三档都说得通。
  **核账口径**：总数对"条数"，折减后对"处数"，**并写明用的是哪一档**。
- **"14" 与 "17" 是两个不同口径，别混**：
  - **111 那一侧 = 14 源文件 / 17 编译单元**：`flash_tool.cpp` 编进 **2** 个目标、`mtk_payload.cpp` 编进 **3** 个目标。
    表里的"条数"是**编译单元报告数**，不是"要改的处数" —— `flash_tool.cpp` 那 **4 条 = 2 处源码**（2 目标 × 2 处）、
    `mtk_payload.cpp` 那 **3 条 = 1 处源码**（3 目标 × 1 处）。**按"条数"排工作量会翻倍。**
  - **103 那一侧 = 11 源文件 / 14 编译单元**（同样是"文件 + 多目标重复"：`flash_tool` +1、`mtk_payload` +2）。
    它那个 14 与 111 侧的 14（源文件数）**数值相同纯属巧合**，不是"照抄"。
- **"103 条" 的结案**：那是**增量构建的下界**，不是"目标覆盖不同"。证据：那份 501 步日志里**整个 `image_engine.dir` 的编译行就是 0 条**
  （`libimage_engine.a` 也没重链 → 那三个 TU 的警告没被重放），111 − (4+2+2) = 103 正好对上 → 口径写
  **"103 条 / 11 源文件 / 14 编译单元（增量下界）"**。
- **术语**：`build.ninja` 里 `FLAGS = …-Wall -Wextra` 的那 **395** 条是**编译构建语句（build edge）**；
  目标级**规则** `CXX_COMPILER` 是 **71** 条（在 `CMakeFiles/rules.ninja`）。两者别混。
- **构建会一直"脏"到 T2～T5 完成**（有意为之，已写进 `CMakeLists.txt` 注释）；**ccache 在编译器路径上**：
  命中会重放已存的 stderr（警告真实），但"步数"≠"真调了一次编译器"。
- 权威基线（全新目录全量）：**111 条 / 14 源文件 / 0 error**，与开关进默认前逐行一致
  （唯一差异：`mtk_payload.cpp` 那条因 A-37 注释从 `:632` 挪到 `:649`）。

## 任务划分

| 任务 | 范围 | 备注 |
|---|---|---|
| **W5-T1** | 打开开关：`CMakeLists.txt` 顶层加 `-Wall -Wextra`（不区分语言的形态；**本仓只有 C++**，见「口径」） | ✅ 已完成（`74fb0ca`）：395/395 编译规则带开关、全新全量 111 条与基线一致。**先记账再修**已完成，逐文件分布见下 |
| **W5-T2** | OPPO 测试三件套（30 + 29 + 18 = **77 条**） | 单一最大块；多半是同一类模式（未使用参数 / 有符号比较），修法要**逐类核**而不是照抄 |
| **W5-T3** | 其余测试目标（`test_image_worker` 12 + `test_hisi_update` 1 + `test_fs_ext4` 1 = 14 条） | **根因已查明（W5-T2 逐条核过）**：`test_image_worker` 那 12 条与 OPPO 三测试同根因 —— 同一个 `QcFileSpec`（`tests/oppo_test_helpers.h:91`）。**走 DMI 路线**：给该结构体的 `sha256`/`md5` 各加 `= QString()`（2 行消 12 条，治根因），**不要**按站点逐个补 |
| **W5-T4** | `src/image_engine`（`ext4_reader` 4 + `fs_image` 2 + `bspatch_image` 2 = 8 条） | **仓内无 `.c`**（见「口径」）→ C 路径已由仓外合成工程验证过，**本任务不重复验证**，只管修这 8 条 |
| **W5-T5** | `src/core` + `src/ui` + `src/vuln_db`（`flash_tool` 4 + `mtk_payload` 3 + `restart_tool` 2 + `local_importer` 2 + `vuln_panel` 1 = 12 条） | 这些是**生产**代码，修法要更保守：优先 `[[maybe_unused]]`/`Q_UNUSED`，动到逻辑就单列 |
| **W5-T6** | 终验 + 文档 | 全新目录全量构建（**0 警告 / 0 error + 步数 + 退出码**）、全量 `ctest` 全绿、`功能清单.txt` 与 README 的"构建"段补一句"默认开启 `-Wall -Wextra`" |

**顺带项（控制方记账，做不做由各任务实现者判断并在报告写明）**
- `setErr` 全仓仍有 **13 份副本**（odin / eub / image_engine / edl），A-38 只在 MTK 收口 → 若要全仓收敛，**另起任务**，不要塞进本波。
- `flashPartition` 包装已无生产调用方（W4-T3 的 I1 已补注释）。

## 计划自查

- 覆盖：111 条全部落进某任务；开关本身（T1）与终验（T6）都有明确判据。
- 已知风险：① 全库开开关后"中途警告可见"会让别人的增量构建变吵（T1 已注明）；
  ② 修 warning 时**最容易顺手改行为** —— 本计划把"不许改行为"写成硬约束，并要求把**不得不动行为**的处单列。
- 不做：`-Werror`（会把任何新警告变成别人构建失败的硬墙，且本仓无 CI 约定）；其余额外警告等级（如 `-Wsign-conversion`）。
