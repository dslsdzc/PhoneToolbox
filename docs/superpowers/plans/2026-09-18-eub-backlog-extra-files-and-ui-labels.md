# EUB 两条 backlog 实施计划（extraFiles 发送 + FlashPanel 文案纯函数化）

> **状态：已执行完毕**（2026-09-18）—— Task 1 / 1b / 2 / 3 全部落地，各自通过独立审查 + 修复波，整批终审通过（无 Critical/Important）。
> 本文件是**计划当时**的文本；其中"真样本"相关的表述以文首"事实依据"段的现行口径与 `exynos-eub-facts.md` §H 为准。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development 逐任务执行。
> Steps 用 `- [ ]` 勾选跟踪。

**Goal:** ① 让 Exynos9830 的救援流程完整（实现参照流程要求的段后 `extraFiles` 发送，解除本相位的临时禁用）；
② 把 FlashPanel 的"模式 → 按钮文案/tooltip"抽成**纯函数**并逐模式钉住（本相位同一处出过两次"文案与动作不符"缺陷，
且两次都只能靠读码发现）。

**事实依据：** `reference/hubble/hubble.py:152-185`（extract+解压）、`:310-341`（分段后按序发送 extraFiles）、
`:102-110`（同一 `send_part_to_device` = 与分段相同的帧风格）、`ExynosData/Exynos9830.json:3`（仅 9830 有 `files_to_send`）。
证据等级：**单源**（hubble）；"9830 需要这两个文件"是**参照流程的要求**。**真机仍未验证**；真样本已就位
（`reference/eub-samples/` 的 5 个官方 BL 包，`exynos-eub-facts.md` §H）并由本计划的 Task 1b 实跑核对：
已证实 9830 包内确有 `ldfw.img.lz4`/`tzsw.img.lz4`、7580 的 sha1 逐字符一致、41 段偏移落在与表无关的结构地标上。

**架构：** 复用既有分层 —— `eub_payload`（按名取条目 + lz4）、`eub_session`（把 extraFiles 当作段之后的新阶段）、
`eub_recovery_dialog`（源校验与文案）；FlashPanel 侧新增一个**不依赖 Widgets/libusb** 的纯函数层。

## Global Constraints

沿用 `2026-09-17-exynos-eub-recovery.md` 的全局约束（不做清单、无真机不得写"已验证"、注释带出处、
`/tmp` 满故全新目录放 `/home`、**"0 警告"必须显式 `-Wall -Wextra`**、只 `git add` 明确路径、
**恒真断言要加固并给变异证据且逐条留日志**）。补充：

0. **真样本（Task 1b）**：`reference/eub-samples/` 是 gitignored 的**真样本**（用户 2026-09-18 授权下载）；
   只把**路径**编进用例、内容不进仓库；`EUB_SAMPLES_REQUIRED=ON` 时缺失即 FAIL（不许 QSKIP 静默通过）。
1. **9830 的源约束（需用户确认的取舍）**：`extraFiles` 只能从 **BL tar** 内取（`ldfw.img` / `ldfw.img.lz4`）。
   用户若只给了裸 `sboot.bin`，对话框必须**明确拒绝**并指出"需要 BL_*.tar.md5"——不新增第二个文件选择器（YAGNI）。
2. **发送序列的记账**：extraFiles 是**段之后的新阶段**，与段同款"重开设备 → 发送 →（可选）读回显"。
   参照实现（hubble）在分段后**不重开**设备（同一句柄连续写）——本仓选重开（facts §B8 记录的两种做法之一），
   **必须在注释里写明这是与参照的有意分歧**。
3. **帧风格**：extraFiles 用**该表的 `style`**（与分段相同；hubble 用同一函数，事实依据如上）。

---

### Task 1: extraFiles 的载荷与发送（eub_payload + eub_session）

**Files:**
- Modify: `src/core/eub/eub_payload.{h,cpp}`
- Modify: `src/core/eub/eub_session.{h,cpp}`
- Test: `tests/test_eub_payload.cpp`、`tests/test_eub_session.cpp`
- Modify: `CMakeLists.txt`（若需给 extra 目标补源——预计**不需要**，两个目标都已含所需源）

**Interfaces:**
- Produces:
  - `eub::loadNamedEntriesFromTar(const QString &tarPath, const QStringList &baseNames, QList<QByteArray> &out, QString *error)`
    —— 按 `baseNames` 顺序，每个名字先找**同名条目**、再找**同名 + `.lz4`**（与 `sboot.bin` 同款优先级，basename 大小写不敏感），
    取出并（必要时）解压；任一名字找不到 → false + 列出缺失名与包内条目摘要（复用既有 `listingOf`）。
  - `eub::EubSession::run(...)` 的新契约：段全部发完后，按 `lo.extraFiles` 顺序逐文件
    `open(带重试) → sendSegment(数据, lo.style) → 可选读回显 → close`；
    缺失任何一个 extra 文件 → **fail-closed**（不发任何 extra、也不再把"成功"报出去），文案含缺失文件名。
- Consumes: `imgtar::indexTarStream`、`imgcomp::lz4Decompress`（既有）；`splitSboot`、`sendSegment`（既有）。

- [ ] **Step 1: 失败用例**（payload 侧：tar 内按名取；缺名 → 失败并列出；`.lz4` 优先级的两个方向；会话侧：9830 表 + 两个 extra → 段后各一帧且顺序正确；缺 extra → 失败且**零 extra 发送**）
- [ ] **Step 2: RED**（`cmake --build build --target image_engine_tests_test_eub_payload image_engine_tests_test_eub_session`）
- [ ] **Step 3: 实现**（payload 的通用取条目函数 + session 的 extra 阶段；**变异证据逐条留日志**）
- [ ] **Step 4: GREEN + 全量 ctest**
- [ ] **Step 5: 提交**（明确路径）

### Task 1b: gated 真样本回归（EUB_SAMPLES_REQUIRED）

> **为什么加这条**：真样本验证（`docs/superpowers/specs/exynos-eub-facts.md` §H）已把 **5 个官方 BL 包 + 解压后的
> sboot.bin** 落在 `reference/eub-samples/`（gitignored）。此前"提取器/切段对不对"只能靠读码与合成夹具判断；
> 现在可以**用真数据实跑**。仿既有的 `MTK_SAMPLES_REQUIRED` / `ODIN_SAMPLES_REQUIRED` 模式。

**Files:** Modify `tests/test_eub_payload.cpp`、`tests/test_eub_loadout.cpp`、`CMakeLists.txt`（新 cache 变量 + 编译定义）

- [ ] **Step 1: 用例（样本缺失时 QSKIP，`EUB_SAMPLES_REQUIRED=ON` 时缺失即 FAIL）**
  - 用 **真 9830 BL tar** 跑 `loadNamedEntriesFromTar(tar, {"ldfw.img","tzsw.img"})` → 两个都取到；
    解压后大小 **0x600000 / 0x180000**（§H2）；条目名在真包里是 `ldfw.img.lz4`/`tzsw.img.lz4` → 覆盖 `.lz4` 回退。
  - 用真包跑 `loadSbootBytes` → 拿到 sboot.bin 且 **sha1 与 §H 记录一致**（9830 实测 `59ea267f01320dfea781668ecf229585e010a7d5`）。
  - **尺寸硬核对**：对每张有样本的表，断言 `splitSboot(真 sboot.bin)` **成功**（不越界）；
    并对 **8895** 断言其富余极小（真文件 1,847,568 − 表所需 1,847,296 = 272）——即"表能用但几乎没余量"这一边界被钉住。
    有样本的 SoC：9830 / 9610 / 7580 / 8895 / 8890（7885/9810/9820 无样本，跳过并计数）。
  - **7580 的 sha1 对拍**：真 `A510FXXS8CTI7` 的 sboot sha1 == `466852d13fa02d51729d21633f47708308579f58`（§H2 已证实）。
- [ ] **Step 2–4: RED → 实现（含 CMake 的 `EUB_SAMPLES_REQUIRED` 选项与样本目录变量）→ GREEN**
- [ ] **Step 5: 提交**（明确路径；**样本本身绝不进提交**）

### Task 2: 对话框解除 9830 禁用 + 源校验 + 文档口径

**Files:**
- Modify: `src/ui/eub_recovery_dialog.{h,cpp}`
- Test: `tests/test_eub_recovery_dialog.cpp`
- Modify: `功能清单.txt`、`README.md`、`docs/superpowers/specs/2026-09-17-exynos-eub-design.md`（§5.4/§7 的 9830 禁用口径改为"已实现"，并补 extraFiles 的设计与证据等级）

**Interfaces:**
- Consumes: Task 1 的 `loadNamedEntriesFromTar` 与新版 `run` 契约。
- Produces: `EubRecoveryDialog::prepare` 在 `lo.extraFiles` 非空时：源必须是 tar（否则报"需要 BL_*.tar.md5"）；
  源是 tar 时**立即验证**这些条目在包内存在（缺失 → 明说缺哪个），通过则**恢复开始按钮可用**（撤掉 I1 的预检禁用）。
  段表里 `extraFiles` 的文案从"本期不发送"改为"段后发送（各 N 字节，来自同一个 BL 包）"。

- [ ] **Step 1: 失败用例**（裸 sboot.bin + 9830 表 → prepare 失败且文案含"BL"；tar 缺 ldfw → 失败且含文件名；
  完整 tar → prepare 成功且**开始按钮可用**；段表文案不含"不发送"）
- [ ] **Step 2: RED** → **Step 3: 实现** → **Step 4: GREEN + 全量 ctest** → **Step 5: 提交**

### Task 3: FlashPanel 文案纯函数化 + 逐模式钉住

**Files:**
- Create: `src/ui/flash_button_labels.{h,cpp}`（**纯函数**：只依赖 `DeviceDetector::DeviceMode`，不碰 Widgets/libusb）
- Modify: `src/ui/flash_panel.cpp`（`:286-303` 复位段 + `:373-392` 协议块**两处调用点收口为一个**，消掉重复置位）
- Test: `tests/test_flash_button_labels.cpp`（新目标）
- Modify: `CMakeLists.txt`（测试源 + extra sources：`flash_button_labels.cpp`；**不**链 Widgets/libusb）

**Interfaces:**
- Produces:
  ```cpp
  namespace flashui {
  struct ButtonLabels { QString text; QString tooltip; };   // tooltip 为空 = 无提示（复位态）
  ButtonLabels flashButtonLabelsFor(DeviceDetector::DeviceMode mode);
  }
  ```
  规则：`MODE_SAMSUNG_EUB` → `{"EUB 救援…", <救援 tooltip>}`；`MODE_MTK_BROM` → `{"刷入", <MTK 三代 tooltip>}`；
  其余协议模式（华为/展锐/三星 Odin）→ `{"刷入", <通用协议 tooltip>}`；**其它一律** `{"刷入", ""}`（复位态）。

- [ ] **Step 1: 失败用例**（逐枚举：EUB 的文案与 tooltip；MTK 与通用协议各自的 tooltip；**非协议模式必须是 `{"刷入", ""}`** —— 正是 I3 缺陷的回归钉）
- [ ] **Step 2: RED** → **Step 3: 实现 + FlashPanel 收口**（注释写明"文案与 tooltip 必须一起复位；本函数是唯一来源"）
- [ ] **Step 4: GREEN + 全量 ctest + 显式 `-Wall -Wextra` 复核** → **Step 5: 提交**

---

## 计划自查

- **事实覆盖**：extraFiles 的来源/顺序/帧风格/证据等级均有 `reference/` 出处（上文），无推测项。
- **用户需确认的取舍**：① 9830 只接受 BL tar 源（不加第二个文件选择器）；② extraFiles 阶段**重开设备**（与参照的单句柄做法不同，注释写明）。
- **不做**：extraFiles 的 `.lz4` 之外的压缩格式、AP 包内查找、为 extraFiles 新增 UI 入口。
