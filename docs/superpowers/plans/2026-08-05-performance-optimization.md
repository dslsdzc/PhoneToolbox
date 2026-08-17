# 性能优化 Implementation Plan（计划 G）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 解决镜像处理慢（2026-08-05 用户反馈"太慢了"）。根因：`image_engine` 全内存设计 —— payload/sparse/tar/EROFS/ext4 等操作将整镜像（GB 级）读入 `QByteArray`，速度与内存双差。核心方案：**流式 I/O**（边处理边写盘，内存 O(1)）+ 增量目录加载 + 进度细化。

**Architecture:** image_engine 各格式引擎新增流式接口（保持旧接口兼容，UI 侧迁移到流式）；worker 层支持按字节进度回调。**本计划允许改动 image_engine**（性能优化为独立任务，不受计划 D 前端隔离约束）。

**Tech Stack:** C++17, Qt6, image_engine（payload/sparse/tar/fs 各引擎）。

## Global Constraints

- C++17；**流式接口与旧接口并存**（旧 QByteArray 接口保留供小文件/测试使用，大文件路径走流式）
- **不可信原则**：性能优化以基准数据驱动（先测后改，不凭感觉）；每个优化任务附带基准对比（优化前后耗时/内存）
- 流式实现不得改变输出字节（与旧接口结果逐字节一致 —— 既有测试即回归保障）
- 每个任务独立 commit，前缀 `feat:`（优化）或 `perf:`（基准/微优化）
- 基准测试可执行（tests/bench 或临时程序），记录基线

---

### Task G0: 性能基线基准

**Files:**
- Create: `tests/bench_image.cpp`（或临时基准程序：构造大样本测各操作耗时/峰值内存）

**内容**：
- 样本：构造 1GB 级 sparse / 256MB payload / 大 EROFS+ext4 镜像（mkfs 生成）
- 测：payload 解包、simg2img、img2simg、tar 打包、EROFS listTree/extract、ext4 extract 的耗时 + 峰值内存
- 输出基线表（写入报告，作为后续优化对比基准）

---

### Task G1: sparse 转换流式化

**Files:**
- Modify: `src/image_engine/sparse_image.h/.cpp`
- Modify: `tests/test_sparse.cpp`（流式接口测试）

**Interfaces:**
- Produces: `bool imgsparse::simg2imgStream(const QString &inPath, const QString &outPath, std::function<void(quint64)> progress, QString *error);`（按 chunk 流式读入写盘，内存 O(chunk)）；`bool imgsparse::img2simgStream(const QString &inPath, const QString &outPath, quint32 blockSize, ...)`（按块流式分析 FILL/RAW）

**验证**：流式输出与旧接口逐字节一致（既有测试回归）+ 1GB 样本峰值内存显著下降

---

### Task G2: payload 解包流式化

**Files:**
- Modify: `src/image_engine/payload_image.h/.cpp`
- Modify: `tests/test_payload.cpp`

**Interfaces:**
- Produces: `bool imgpayload::extractPartitionStream(const QString &payloadPath, const Partition &part, const QString &outPath, std::function<void(quint64)> progress, QString *error);`（manifest 已解析模式下按 op 流式解压写盘；diff 系仍需旧镜像片段 —— 流式读旧镜像对应 extent）

**验证**：与旧接口产物逐字节一致 + 256MB payload 基准对比

---

### Task G3: tar 打包/解包流式化

**Files:**
- Modify: `src/image_engine/tar_image.h/.cpp`
- Modify: `tests/test_tar.cpp`

**Interfaces:**
- Produces: `bool imgtar::buildTarStream(const QStringList &inputFiles, const QString &outPath, ...)`（按文件流式读入写 tar，避免整集装入内存）；`bool imgtar::extractTarStream(const QString &tarPath, const QString &outDir, ...)`（按条目流式解包）

**验证**：与旧接口产物逐字节一致 + 大集合基准

---

### Task G4: EROFS/ext4 提取流式化 + 目录增量加载

**Files:**
- Modify: `src/image_engine/fs/erofs_reader.h/.cpp`, `ext4_reader.h/.cpp`, `fs_image.h/.cpp`
- Modify: `tests/test_fs_erofs.cpp`, `test_fs_ext4.cpp`

**Interfaces:**
- Produces: `bool imgfs::extractFileStream(const QString &imagePath, const QString &inPath, const QString &outPath, ...)`（**镜像文件流式读** —— 当前 openFsImage 全镜像读入内存，改为按需 mmap/QFile 随机读）；`bool imgerofs::listTreeLazy(const QString &imagePath, const QString &dir, QList<FsEntry> &out, ...)`（按目录增量加载，不全树一次）

**验证**：与全内存结果一致 + 大镜像基准

---

### Task G5: worker 进度细化 + UI 适配

**Files:**
- Modify: `src/ui/image_worker.h/.cpp`, `src/ui/image_tool_panel.cpp`

**内容**：
- worker 的 runUnpack/runConvert/runPack 接入流式接口的 progress 回调 → 按字节/块百分比更新进度条（替代 0/100 两档）
- 大文件路径自动走流式接口（小文件保持旧路径）；进度条真实推进（用户感知"在做"）

**验证**：面板冒烟 + 进度条按样本真实推进

---

## Self-Review 记录

- **Spec 覆盖**：用户反馈"镜像处理慢" → G0 基线 + G1-G4 流式化（最大收益）+ G5 感知优化。
- **不可信原则**：G0 先测后改；每个任务基准对比（优化前后数字）。
- **兼容性**：流式接口与旧接口并存，输出逐字节一致（既有测试回归保障）；UI 大文件走流式。
- **依赖顺序**：G0→G1/G2/G3/G4（可并行）→G5。
- **类型一致性**：`*Stream` 接口命名一致；progress 回调 `std::function<void(quint64)>` 统一。
