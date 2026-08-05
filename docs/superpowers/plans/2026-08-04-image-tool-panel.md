# 镜像工具面板 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 左侧工具列表新增第 5 个面板「镜像工具」：整个面板是拖放目标，拖入即识别格式 → 解包/打包/转换/修补/文件系统浏览，后台线程执行避免 UI 卡顿。

**Architecture:** `src/ui/image_tool_panel.h/.cpp` 为独立面板，复用 `image_engine`（registry 识别 + 各格式引擎）+ `root_patcher`（修补）。CPU 密集操作用 `QThreadPool` + `QtConcurrent::run`（Qt6 Core 自带）后台执行，结果经信号回 UI 线程。主窗口的 `tool_panel` 加第 5 个按钮，`QStackedWidget` 加一页。

**Tech Stack:** C++17, Qt6 Widgets, image_engine, root_patcher。沿用现有面板风格（`flash_panel`/`system_tool_panel` 的布局与信号模式：`outputMessage(QString, bool)` 发到 OutputPanel）。

## Global Constraints

- C++17，成员变量 `m_` 前缀；面板类 `Q_OBJECT`（AUTOMOC 处理）
- 面板信号：`outputMessage(const QString &text, bool isError)`（现有约定）、`switchToDeviceInfo()` 不用
- 拖放：整个面板 `setAcceptDrops(true)` + `dragEnterEvent/dropEvent`；文件路径列表从 `event->mimeData()->urls()` 取
- 后台执行一律 worker 线程（D1 已定：QThread + moveToThread + invokeMethod），禁止在工作线程触碰 QWidget
- 修补/重打包操作前 UI 明确提示「仅适用于已解锁 Bootloader 的设备」
- 每个任务独立 commit，前缀 `feat:`
- **前端隔离硬约束（2026-08-05 用户要求）**：本计划**只允许改前端代码**（src/ui/ 及必要的构建接线）；后端 `src/image_engine/` 与 `src/root_patcher/` **冻结，禁止任何改动**（包括顺手的 bug 修复）。若实现发现后端接口缺失/缺陷：记录为"后端扩展待办"（独立于本计划），UI 侧以现有接口适配或优雅降级，不得改后端绕过

---

## 文件结构

```
src/ui/
  image_tool_panel.h/.cpp    # 第 5 面板（识别/操作编排）
src/core/ 或 src/ui/
  image_worker.h/.cpp        # QObject worker：跑 image_engine 调用，信号回传结果
tools/ 不变
```

---

### Task D1: image_worker — 后台执行封装

**Files:**
- Create: `src/ui/image_worker.h`, `src/ui/image_worker.cpp`
- Modify: `CMakeLists.txt`（源加入主程序 —— src/ui 已被 GLOB 收集，无需改 GLOB；确认）

**Interfaces:**
- Produces: `class ImageWorker : public QObject { Q_OBJECT public: explicit ImageWorker(QObject *parent=nullptr); void runDetect(const QString &path); void runUnpack(const QString &path, const QString &outDir, const imgreg::Detected &detected); void runPack(...); void runConvert(...); void runPatch(...); signals: void detectFinished(const QString &path, imgreg::Detected detected); void unpackFinished(bool ok, QStringList outputs, QString error); void progress(int percent, const QString &stage); };` —— 内部 QThread + moveToThread，操作排队串行

- [ ] **Step 1: 写失败测试**

无 Qt Test（UI 层无测试设施 —— 项目约定无单元测试）；本任务验证方式为编译 + 手动冒烟（`ImageWorker` 能被构造、信号槽连接不报错）。**验证步骤**：构建通过 + 一个临时 main 里 connect 全部信号并调用 runDetect 不崩溃。

- [ ] **Step 2-4: 实现 + 构建验证**

实现要点：worker 对象 moveToThread 到专用 QThread；槽内调 `imgreg::detect` 等；`progress` 信号在阻塞循环中按块数/字节数发射（各引擎暂不支持回调时，先按"开始/结束"两档进度）；析构时 `quit()+wait()`。

- [ ] **Step 5: Commit**

```bash
git add src/ui/image_worker.*
git commit -m "feat: 镜像处理后台 worker (线程封装)"
```

---

### Task D2: image_tool_panel — 骨架与拖放识别

**Files:**
- Create: `src/ui/image_tool_panel.h`, `src/ui/image_tool_panel.cpp`
- Modify: `src/ui/tool_panel.cpp/h`（加第 5 个工具按钮 + 信号），`src/ui/main_window.cpp/h`（QStackedWidget 加页）

**Interfaces:**
- Consumes: `ImageWorker`（D1）、`imgreg::detect`（计划 A Task 17）
- Produces: `class ImageToolPanel : public QWidget { Q_OBJECT ... signals: void outputMessage(const QString &text, bool isError); };` 面板结构：顶部信息卡（格式/详情 Label）、中部动作按钮组（解包/打包/转换/修补，按识别结果 enable）、底部进度条 + 日志框

- [ ] **Step 1: 写失败测试**

编译验证 + 手动冒烟（构建 + 启动程序截图不可行时：确认 `./build/PhoneToolbox` 能启动不崩溃，面板可通过按钮切换出现）。

- [ ] **Step 2-4: 实现 + 验证**

实现要点：`dragEnterEvent` 接受 `urls` 且至少 1 个本地文件；`dropEvent` 取第一个路径 → `worker.runDetect` → 信号回来更新信息卡 + enable 对应按钮。`tool_panel` 新增按钮文案「镜像工具」，`main_window` 的 `m_tools`（QStackedWidget）加页索引。遵循现有信号接线模式（参考 `flash_panel` 的 outputMessage 接线）。

- [ ] **Step 5: Commit**

```bash
git add src/ui/image_tool_panel.* src/ui/tool_panel.* src/ui/main_window.*
git commit -m "feat: 镜像工具面板骨架与拖放识别 (UI)"
```

---

### Task D3: 解包/转换动作接线

**Files:**
- Modify: `src/ui/image_tool_panel.cpp`（解包按钮 → worker.runUnpack；转换按钮 → runConvert）
- Modify: `src/ui/image_worker.h/.cpp`（runUnpack/runConvert 实现）

**Interfaces:**
- Consumes: `imgsparse`/`imgpayload`/`imgtar`/`imgdat` 等引擎（计划 A）、`imgsuper`/`imgkdz`/`imghw`/`imgsin` 等（计划 B）
- Produces: `runUnpack` 按 detected 格式分派到对应引擎，产物写 `outDir/<格式名>/`；`runConvert` 支持 sparse↔raw（文件对话框选目标类型）

- [ ] **Step 1-4: 实现 + 验证**

验证：构建 + 手动冒烟（无真实样本时，用 Task A 测试样本生成器临时造一个 sparse 文件拖入面板，断言解包按钮 enable 且点按后日志出现路径）。进度：按产物个数发射 progress。

- [ ] **Step 5: Commit**

```bash
git add src/ui/image_tool_panel.cpp src/ui/image_worker.*
git commit -m "feat: 解包/转换流程接线 (UI)"
```

---

### Task D4: 修补动作接线（root_patcher 集成）

**Files:**
- Modify: `src/ui/image_tool_panel.cpp`（修补按钮 → 方案选择对话框 → worker.runPatch）
- Modify: `src/ui/image_worker.h/.cpp`（runPatch：调 `patcher::patchFile`）
- Modify: `CMakeLists.txt`（主程序链接 root_patcher —— image_engine 已链入，root_patcher 追加）

**Interfaces:**
- Consumes: `patcher::patchFile`（计划 C Task C6）
- Produces: 修补对话框（QDialog）：Root 方案单选（Magisk/KernelSU/APatch）、注入物文件选择（手动指定，优先于下载）、KMI 输入框（KernelSU 时显示）、提示文案「仅适用于已解锁 Bootloader 的设备」。下载注入物：`AssetsDownloader` + 进度条，失败可切手动指定。

- [ ] **Step 1-4: 实现 + 验证**

验证：构建 + 手动冒烟（fake APK 指定文件 → 修补失败路径显示错误信息；无注入物时提示下载/指定）。

- [ ] **Step 5: Commit**

```bash
git add src/ui/image_tool_panel.cpp src/ui/image_worker.* CMakeLists.txt
git commit -m "feat: Root 修补流程接线 (UI)"
```

---

### Task D5: 文件系统浏览（EROFS/ext4 查看器）

**Files:**
- Create: `src/ui/fs_browser_dialog.h/.cpp`（或面板内嵌 QTreeWidget）
- Modify: `src/ui/image_tool_panel.cpp`（识别到 EROFS/ext4 时 enable「浏览」按钮）

**Interfaces:**
- Consumes: `imgfs::FsImage` / `imgerofs` / `imgext4`（计划 B）
- Produces: 浏览对话框：左侧 QTreeWidget 目录树，双击文件 → 提取到临时文件 → QDesktopServices::openUrl（外部打开）或「提取到…」按钮另存；「替换文件」按钮（QFileDialog 选文件 → replaceFile → 标记镜像已修改 → 保存时 repack）

- [ ] **Step 1-4: 实现 + 验证**

验证：构建 + 手动冒烟（无真实 EROFS/ext4 样本时，浏览按钮对 RawImage 禁用 —— 确认禁用逻辑）。

- [ ] **Step 5: Commit**

```bash
git add src/ui/fs_browser_dialog.* src/ui/image_tool_panel.cpp
git commit -m "feat: EROFS/ext4 文件系统浏览对话框 (UI)"
```

---

### Task D6: 打包动作接线

**Files:**
- Modify: `src/ui/image_tool_panel.cpp`（打包按钮 → 组识别 → worker.runPack）
- Modify: `src/ui/image_worker.h/.cpp`（runPack：img2simg/tar 打包/buildUpdateAppWithData 等）

**Interfaces:**
- Consumes: 各引擎打包接口（imgsparse::img2simg、imgtar::buildTar、imghw::buildUpdateAppWithData、imgpayload::buildFullPayload —— 计划 B 标注为后续扩展的除外）
- Produces: 打包目标格式选择（与当前识别格式相关：sparse→raw 逆向、.img 集合→tar/tar.md5、payload 全量打包）

- [ ] **Step 1-4: 实现 + 验证**

- [ ] **Step 5: Commit**

```bash
git add src/ui/image_tool_panel.cpp src/ui/image_worker.*
git commit -m "feat: 打包流程接线 (UI)"
```

---

## Self-Review 记录

- **Spec 覆盖**：独立第 5 面板 ✅（D2）、全面板拖放 ✅（D2）、解包/打包/转换/修补四动作 ✅（D3/D4/D6）、文件系统浏览 ✅（D5）、后台执行防卡顿 ✅（D1）、解锁提示 ✅（D4）、"新增格式注册即用不改面板代码" ✅（动作按 detected.format 分派）。
- **依赖顺序**：D1→D2→D3/D4→D5/D6；全部依赖计划 A/B/C 的引擎接口。
- **类型一致性**：`ImageWorker`/`ImageToolPanel` 命名一致；`imgreg::Detected`、`patcher::PatchConfig` 与计划 A/C 的产出对齐。
- **测试说明**：UI 层遵循项目"无单元测试"约定，验证以构建 + 手动冒烟为主；引擎层的正确性已由计划 A/B/C 的 Qt Test 覆盖。
