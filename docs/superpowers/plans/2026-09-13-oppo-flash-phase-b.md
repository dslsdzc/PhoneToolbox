# OPPO/一加 EDL 刷写链 (Phase B) 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 从解包产物（或 `.ofp`/`.ops` 整包）构建刷写计划，经 Sahara/Firehose 走完一次真实刷写；协议与传输分层，全部逻辑可用 mock 在无真机下验证。

**Architecture:** 新增 `src/core/edl/`：`IEdlTransport`（唯一设备依赖点）+ `sahara`/`firehose`（纯协议）+ `flash_plan`（纯函数：计划构建与校验）+ `edl_session`（编排）。`EDLHandler` 退化为薄封装（公开 API 不变）并顺带修复 5 处既有缺陷。UI 新增计划预览对话框，刷写经 `FlashTool` 新通道 `oppo-edl`。

**Tech Stack:** C++17 / Qt6 Core（QXmlStreamReader、QJsonDocument）、libusb（仅传输实现）、Qt6 Test。

## Global Constraints

- 设计 spec：`docs/superpowers/specs/2026-09-13-oppo-flash-phase-b-design.md`（**先读它**，尤其 §3.5 校验规则、§4 错误矩阵、§6 测试策略）
- 协议事实速查（**所有 XML 属性集、命令语义、参照行号的权威出处**）：`docs/superpowers/specs/oppo-flash-protocol-facts.md`
- 命名空间 `edl`；新文件放 `src/core/edl/`（`CMakeLists.txt:107-111` 的 `GLOB_RECURSE` 会自动收进主程序，但**新增文件后必须重跑 `cmake -B build -G Ninja` 配置**——模式串没有 `CONFIGURE_DEPENDS`）
- 测试放 `tests/test_edl_*.cpp` / `tests/test_flash_plan.cpp`，用 `QTEST_APPLESS_MAIN`（需 Widgets 的那个例外见 Task 8），**必须**加进根 `CMakeLists.txt` 的 `IMAGE_TEST_SOURCES` 列表；多源编入同一测试目标的现成范式见 `CMakeLists.txt:318-321`（mtk_brom+mtk_emmc）与 `:340-353`（test_pipeline 九源）
- **除 `edl_libusb_transport` 与 `edl_handler` 外，模块不得依赖 libusb、不得含 QObject**（这是"无真机可验证"的物理前提）
- 错误一律 `bool + QString *error` + 明确中文文案；失败禁止静默
- 所有协议常量/属性集必须带参照出处注释（`reference/qdl/src/firehose.c:行号` 或 `edl/edlclient/Library/firehose.py:行号`）
- 提交只 `git add` 具体文件路径（`build/` 下有历史遗留被跟踪文件，**禁止** `git add -A` / `-u` / `commit -a`）
- 构建：`cmake -B build -G Ninja && cmake --build build`；测试：`ctest --test-dir build --output-on-failure` 或 `./build/<target>`
- **真机部分不做**：不要求真机、不写"真机验证"步骤；离线证明不了的项在报告里如实标注

---

### Task 0: 预检与接口摸底（5 分钟）

**Files:**
- Read: `src/core/modes/edl_handler.{h,cpp}`（要搬迁的 Sahara/libusb 代码与公开 API）
- Read: `docs/superpowers/specs/oppo-flash-protocol-facts.md`（协议事实）
- Read: `CMakeLists.txt:107-123`（GLOB）、`:266-300`（测试注册）、`:318-321`、`:340-353`（多源测试目标范式）

- [ ] **Step 1: 读既有 EDL 实现**

Run: `grep -n "SAHARA_\|sahara\|0x01\|READ_DATA" src/core/modes/edl_handler.h | head -30`
Expected: 看到 Sahara 命令枚举与公开方法签名（`connectSahara`/`disconnect`/`firehoseConnect`/`listPartitions`/`readPartition`/`writePartition`/`writeRaw`/`isConnected`/`lastError` + `outputMessage`/`progress` 两信号）。

- [ ] **Step 2: 确认 transport 需要的 libusb 细节位置**

Run: `sed -n '45,129p' src/core/modes/edl_handler.cpp`
Expected: 看到 libusb 初始化/枚举/打开/端点配置（0x01/0x82、0x02/0x83）、VID/PID 表 —— Task 7 要把这段整体搬进 `edl_libusb_transport.cpp`。

- [ ] **Step 3: 确认测试目标命名规则**

Run: `sed -n '303,312p' CMakeLists.txt`
Expected: 首源（`test_compression.cpp`）→ 目标名 `image_engine_tests`，其余 → `image_engine_tests_<stem>`。本计划新增的测试目标名依次为 `image_engine_tests_test_flash_plan` / `_test_edl_sahara` / `_test_edl_firehose` / `_test_edl_session` / `_test_flash_plan_dialog`。

---

### Task 1: 计划层 — 数据模型 + `rawprogram*.xml` / `patch*.xml` 解析

**Files:**
- Create: `src/core/edl/flash_plan.h`
- Create: `src/core/edl/flash_plan.cpp`
- Test: `tests/test_flash_plan.cpp`
- Modify: `CMakeLists.txt`（`IMAGE_TEST_SOURCES` 加 `tests/test_flash_plan.cpp`）

**Interfaces:**
- Consumes: 无
- Produces: 本计划**全局共用**的模型与解析接口（后续 Task 2/3/4/5/6/7/8 都按此逐字引用）：

```cpp
// src/core/edl/flash_plan.h
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace edl {

struct PlanEntry {
    enum class Action { Program, Erase, Patch };
    Action  action = Action::Program;
    QString partitionName;      // rawprogram: label；patch: filename（供日志）
    QString imageFile;          // 绝对路径；Patch 为 "DISK" 时表示下发设备
    quint32 lun = 0;            // physical_partition_number
    quint64 startSector = 0;    // 纯十进制时填此值
    QString startSectorExpr;    // start_sector 非纯十进制时**原样保留**（firehose 表达式，如 "NUM_DISK_SECTORS-5."）；
                                // 非空时 startSector==0，且发送方必须原样透传该串
                                // （reference/qdl/src/firehose.c:874-879 明确"解析会写错地址"）
    quint64 numSectors = 0;     // Program/Erase：sparse 展开后的 raw 扇区数
    quint32 sectorSize = 4096;  // 逐条目 SECTOR_SIZE_IN_BYTES
    bool    sparse = false;
    quint64 rawBytes = 0;
    QString sha256;             // 可选（rawprogram 无此属性；OPS 元数据有）
    quint64 byteOffset = 0;     // Patch 专用
    quint32 sizeInBytes = 0;    // Patch 专用
    QString value;              // Patch：原样透传（表达式不解释）
    QString what;               // Patch：只进日志，不进 XML
};

struct FlashPlan {
    QString source;             // 计划来源描述
    QString storageType;        // "ufs"/"emmc" → configure 的 MemoryName
    QList<PlanEntry> entries;   // 已排序：先 Erase，再 Program（按 lun、start_sector），最后 Patch
    QStringList warnings;
    quint64 totalBytes = 0;     // Program 条目 rawBytes 之和（进度分母）
};

struct StorageInfo { quint32 lun = 0; quint64 totalBlocks = 0; quint32 blockSize = 4096; };
struct PlanCheck { bool ok = false; QStringList errors; QStringList warnings; };

// 来源①：解析单个 rawprogram{N}.xml / patch{N}.xml（lun 由调用方从文件名序号得出，见 Task 3）
bool parseRawprogramXml(const QString &xmlPath, quint32 lun,
                        QList<PlanEntry> &out, QStringList &warnings, QString *error);
bool parsePatchXml(const QString &xmlPath, quint32 lun,
                   QList<PlanEntry> &out, QStringList &warnings, QString *error);

} // namespace edl
```

**解析规则（照协议速查 §1/§2，实现时逐条对照）**
- rawprogram `<program>` 必需属性：`SECTOR_SIZE_IN_BYTES`/`filename`/`label`/`num_partition_sectors`/`physical_partition_number`/`start_sector`/`file_sector_offset`；缺一 → **跳过该条目并记 warning**（参照 qdl `src/program.c:254-275` 的计数丢弃语义）。`sparse` 属性缺失不算错（`src/util.c:108-125`）。
- `imageFile` = XML 所在目录 + `filename`（绝对化）。`num_partition_sectors` 直接取 XML 值（sparse 的换算在 Task 2 的校验步骤做，见下）。
- `rawprogram` 里的 `<erase>` 标签 → `PlanEntry{Action::Erase}`（`src/program.c:343-344`）。
- patch `<patch>` 必需属性：`start_sector`/`byte_offset`/`physical_partition_number`/`size_in_bytes`/`value`/`filename`/`SECTOR_SIZE_IN_BYTES`/`what`（`src/patch.c:41-48`）。`filename != "DISK"` → **跳过 + warning**（参照两实现都跳过，协议速查 §2）。`value` **原样保留**（不解释 `NUM_DISK_SECTORS-6.`/`CRC32(...)`）。`what` 存进模型但代码里注明"只进日志"。
- **`start_sector` 允许表达式**（`program` 与 `patch` 都有）：先按纯十进制解析，成功 → `startSector=N`；**失败不算错** → 原样存入 `startSectorExpr` 且 `startSector=0`。**绝不因此丢弃条目**（参照 qdl 把它当字符串读并原样下发：`program.c:261`/`patch.c:46`/`firehose.c:874-879`）。qdl 真实样本 `reference/qdl/tests/data/patch0.xml` 的 Backup-GPT 条目、`rawprogram0.xml` 的 `label=BackupGPT` 就是这一类 —— 丢弃它们等于漏掉备份 GPT 头修补。

- [ ] **Step 1: 写失败测试**

`tests/test_flash_plan.cpp`：

```cpp
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include "core/edl/flash_plan.h"

// 手写字节的合成 rawprogram（绝不调用被测解析代码）
static QString writeFile(const QString &dir, const QString &name, const QByteArray &bytes)
{
    const QString path = dir + "/" + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    f.write(bytes);
    f.close();
    return path;
}

class TestFlashPlan : public QObject
{
    Q_OBJECT
private slots:
    void parsesProgramEntries();
    void skipsMalformedEntryWithWarning();
    void parsesEraseTag();
    void parsesPatchEntriesAndSkipsNonDisk();
};

void TestFlashPlan::parsesProgramEntries()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString xml = writeFile(dir.path(), "rawprogram0.xml",
        "<?xml version=\"1.0\" ?>\n<data>\n"
        "  <program SECTOR_SIZE_IN_BYTES=\"4096\" filename=\"xbl.img\" label=\"xbl\"\n"
        "           num_partition_sectors=\"8192\" physical_partition_number=\"1\"\n"
        "           start_sector=\"1234\" file_sector_offset=\"0\" />\n"
        "</data>\n");
    QVERIFY(!xml.isEmpty());

    QList<edl::PlanEntry> out; QStringList warn; QString err;
    QVERIFY2(edl::parseRawprogramXml(xml, 1, out, warn, &err), qPrintable(err));
    QCOMPARE(out.size(), 1);
    QCOMPARE(warn.size(), 0);
    const edl::PlanEntry &e = out[0];
    QCOMPARE(e.action, edl::PlanEntry::Action::Program);
    QCOMPARE(e.partitionName, QStringLiteral("xbl"));
    QCOMPARE(e.lun, quint32(1));
    QCOMPARE(e.startSector, quint64(1234));
    QCOMPARE(e.numSectors, quint64(8192));
    QCOMPARE(e.sectorSize, quint32(4096));
    QCOMPARE(e.imageFile, dir.path() + "/xbl.img");
}

void TestFlashPlan::skipsMalformedEntryWithWarning()
{
    QTemporaryDir dir;
    const QString xml = writeFile(dir.path(), "rawprogram0.xml",
        "<data>\n"
        "  <program SECTOR_SIZE_IN_BYTES=\"4096\" filename=\"good.img\" label=\"good\"\n"
        "           num_partition_sectors=\"8\" physical_partition_number=\"0\"\n"
        "           start_sector=\"0\" file_sector_offset=\"0\" />\n"
        "  <program SECTOR_SIZE_IN_BYTES=\"4096\" label=\"nofile\"\n"       // 缺 filename
        "           num_partition_sectors=\"8\" physical_partition_number=\"0\"\n"
        "           start_sector=\"16\" file_sector_offset=\"0\" />\n"
        "</data>\n");
    QList<edl::PlanEntry> out; QStringList warn; QString err;
    QVERIFY(edl::parseRawprogramXml(xml, 0, out, warn, &err));
    QCOMPARE(out.size(), 1);          // 只留合法条目
    QCOMPARE(warn.size(), 1);         // 缺属性条目被记 warning
    QVERIFY(warn[0].contains(QStringLiteral("filename")));
}

void TestFlashPlan::parsesEraseTag()
{
    QTemporaryDir dir;
    const QString xml = writeFile(dir.path(), "rawprogram2.xml",
        "<data>\n"
        "  <erase SECTOR_SIZE_IN_BYTES=\"4096\" physical_partition_number=\"2\"\n"
        "         start_sector=\"0\" num_partition_sectors=\"4096\" />\n"
        "</data>\n");
    QList<edl::PlanEntry> out; QStringList warn; QString err;
    QVERIFY(edl::parseRawprogramXml(xml, 2, out, warn, &err));
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].action, edl::PlanEntry::Action::Erase);
    QCOMPARE(out[0].lun, quint32(2));
    QCOMPARE(out[0].numSectors, quint64(4096));
}

void TestFlashPlan::parsesPatchEntriesAndSkipsNonDisk()
{
    QTemporaryDir dir;
    const QString xml = writeFile(dir.path(), "patch0.xml",
        "<patches>\n"
        "  <patch SECTOR_SIZE_IN_BYTES=\"4096\" byte_offset=\"0\" filename=\"DISK\"\n"
        "         physical_partition_number=\"0\" size_in_bytes=\"4\"\n"
        "         start_sector=\"2\" value=\"NUM_DISK_SECTORS-6.\" what=\"Update LBA\" />\n"
        "  <patch SECTOR_SIZE_IN_BYTES=\"4096\" byte_offset=\"8\" filename=\"gpt_main0.bin\"\n"
        "         physical_partition_number=\"0\" size_in_bytes=\"4\"\n"
        "         start_sector=\"1\" value=\"DEADBEEF\" what=\"offline bin only\" />\n"
        "</patches>\n");
    QList<edl::PlanEntry> out; QStringList warn; QString err;
    QVERIFY(edl::parsePatchXml(xml, 0, out, warn, &err));
    QCOMPARE(out.size(), 1);                                    // 非 DISK 被跳过
    QCOMPARE(warn.size(), 1);
    QVERIFY(warn[0].contains(QStringLiteral("gpt_main0.bin")));
    QCOMPARE(out[0].action, edl::PlanEntry::Action::Patch);
    QCOMPARE(out[0].value, QStringLiteral("NUM_DISK_SECTORS-6."));   // 表达式原样保留
    QCOMPARE(out[0].sizeInBytes, quint32(4));
    QCOMPARE(out[0].byteOffset, quint64(0));
}

QTEST_APPLESS_MAIN(TestFlashPlan)
#include "test_flash_plan.moc"
```

- [ ] **Step 2: 注册测试并确认编译失败**

把 `tests/test_flash_plan.cpp` 加进 `CMakeLists.txt` 的 `IMAGE_TEST_SOURCES` 末尾。
Run: `cmake -B build -G Ninja && cmake --build build --target image_engine_tests_test_flash_plan 2>&1 | tail -5`
Expected: 编译失败 `core/edl/flash_plan.h: No such file or directory`

- [ ] **Step 3: 实现**

`src/core/edl/flash_plan.h` 按上面 Interfaces 块逐字创建。`src/core/edl/flash_plan.cpp`：
- 用 `QXmlStreamReader`；属性读取用一个辅助 `attr(reader, name, ok)`（缺失置 `ok=false`）。
- `parseRawprogramXml`：遍历元素；`program` → 校验必需属性 → 建条目（`action=Program`）；`erase` → 建条目（`action=Erase`，`num_partition_sectors`/`start_sector` 可缺省 = 整 LUN 擦，缺省时 `numSectors=0` 且代码注释注明"整 LUN"）；其它元素忽略。缺必需属性 → `warnings << QStringLiteral("rawprogram 条目被跳过：缺属性 %1（label=%2）")`。
- `parsePatchXml`：`filename != "DISK"` → warning + 跳过（注释标 `reference/qdl/src/firehose.c:1405-1406`、`edl/edlclient/Library/firehose_client.py:977-978`）。
- XML 格式错误（`reader.hasError()`）→ `*error` = 中文文案（含文件名与 `reader.errorString()`），返回 false。
- 每条属性读取处注释标参照行号（协议速查 §1/§2 已列全）。

- [ ] **Step 4: 跑测试**

Run: `cmake --build build && ./build/image_engine_tests_test_flash_plan`
Expected: `Totals: 4 passed, 0 failed`（Qt 另计 init/cleanup）

- [ ] **Step 5: 提交**

```bash
git add src/core/edl/flash_plan.h src/core/edl/flash_plan.cpp tests/test_flash_plan.cpp CMakeLists.txt
git commit -m "feat(edl): 计划层模型 + rawprogram/patch XML 解析（参照 qdl 属性语义）"
```

---

### Task 2: 计划层 — 排序/统计 + `validatePlan` + sparse 扇区换算

**Files:**
- Modify: `src/core/edl/flash_plan.h`（加声明）
- Modify: `src/core/edl/flash_plan.cpp`
- Modify: `src/image_engine/sparse_image.h` / `.cpp`（**新增**纯函数 `sparseRawSizeFromHeader`，不改既有接口）
- Test: `tests/test_flash_plan.cpp`（加用例）、`tests/test_sparse.cpp`（加用例）

**Interfaces:**
- Consumes: Task 1 的 `PlanEntry`/`FlashPlan`/`StorageInfo`/`PlanCheck`/`parseRawprogramXml`/`parsePatchXml`
- Produces:
  - `void finalizePlan(FlashPlan &plan)` —— 排序（Erase → Program 按 `lun`,`startSector`；Patch 最后）+ 填 `totalBytes`
  - `bool normalizePlan(FlashPlan &plan, QStringList &warnings, QString *error)` —— **就地**归一化：sparse 条目按文件头回填 `rawBytes`/修正 `numSectors`（warnings 去重汇总）。调用时机：解析完成后、校验之前（`buildPlanFromDir` 内部会调它）
  - `PlanCheck validatePlan(const FlashPlan &plan, const QList<StorageInfo> &device)` —— **纯校验、不改入参**（签名与 spec §3.3 一致）；"修正"归 `normalizePlan`
  - `imgsparse::sparseRawSizeFromHeader(const QByteArray &header, quint64 &rawBytes)`

**校验规则（spec §3.5，逐条实现）**
1. 逐条 `startSector + numSectors ≤ 该 LUN 的 totalBlocks`（缺该 LUN 的 StorageInfo → error）
2. **只在 Program 条目之间**做同 LUN 区间重叠检查（Erase/Patch 不参与）
3. 条目 `sectorSize` 与设备 `blockSize` 不一致 → **warning**（参照允许逐条目覆盖，以条目值为准）
4. 镜像文件存在且可读（`Action::Program` 且非 sparse 时直接查；Patch 的 `imageFile=="DISK"` 不查文件）
5. sparse 条目：读文件头 → `sparseRawSizeFromHeader` → 若 `rawBytes` 与 `numSectors × sectorSize` 不符 → **以头为准修正 `numSectors` 并记 warning**（参照按去 sparse 后大小算，协议速查 §1）
6. 有 `sha256` 的条目 → 该字段非空即记入 `warnings`（"刷前完整校验"由 Task 8 的可选项驱动；此处不读整个文件）
7. **`startSectorExpr` 非空的条目跳过规则 1 与规则 2**（表达式无法在主机侧求值，参照也不解释），并在 warnings 里记一条汇总：`"N 个条目的 start_sector 为表达式，未参与设备几何校验（按参照原样下发）"`
8. `errors` 非空 → `ok=false`

- [ ] **Step 1: 写失败测试**

加到 `tests/test_flash_plan.cpp`（新槽 `validateRejectsAndWarns`、`sparseNumSectorsFromHeader`），并在 `tests/test_sparse.cpp` 加 `sparseRawSizeFromHeaderCase`：

```cpp
void TestFlashPlan::validateRejectsAndWarns()
{
    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    edl::PlanEntry a; a.action = edl::PlanEntry::Action::Program;
    a.partitionName = QStringLiteral("a"); a.imageFile = QStringLiteral("/tmp/a.img");
    a.lun = 0; a.startSector = 100; a.numSectors = 100; a.sectorSize = 4096;
    edl::PlanEntry b = a; b.partitionName = QStringLiteral("b"); b.startSector = 150;  // 与 a 重叠
    edl::PlanEntry c = a; c.partitionName = QStringLiteral("c"); c.lun = 1;
    c.startSector = 900000000; c.numSectors = 100;                                     // 越界
    edl::PlanEntry d = a; d.partitionName = QStringLiteral("d"); d.startSector = 400;
    d.sectorSize = 512;                                                                // 与设备 blockSize 不符
    plan.entries = {a, b, c, d};

    QList<edl::StorageInfo> dev;
    dev.append({0, 1000000, 4096});
    dev.append({1, 1000, 4096});

    const edl::PlanCheck chk = edl::validatePlan(plan, dev);
    QVERIFY(!chk.ok);                                   // 有 error（重叠 + 越界）
    QVERIFY(chk.errors.size() >= 2);
    QVERIFY(chk.warnings.size() >= 1);                  // sectorSize 不符是 warning 不是 error
    bool hasOverlap = false, hasOob = false;
    for (const QString &e : chk.errors) {
        if (e.contains(QStringLiteral("重叠"))) hasOverlap = true;
        if (e.contains(QStringLiteral("越界"))) hasOob = true;
    }
    QVERIFY(hasOverlap);
    QVERIFY(hasOob);
}

void TestFlashPlan::sparseNumSectorsFromHeader()
{
    // 合成 sparse 头 —— **AOSP 真实布局**（reference/qdl/src/sparse.h:11-33、本仓既有 aospHeaderU16Layout）：
    //   u32 magic@0; u16 major@4; u16 minor@6; u16 file_hdr_sz@8; u16 chunk_hdr_sz@10;
    //   u32 blk_sz@12; u32 total_blks@16; u32 total_chunks@20; u32 image_checksum@24   （共 28B）
    QByteArray h(28, '\0');
    auto put32 = [&h](int off, quint32 v) {
        h[off] = char(v & 0xFF); h[off+1] = char((v >> 8) & 0xFF);
        h[off+2] = char((v >> 16) & 0xFF); h[off+3] = char((v >> 24) & 0xFF);
    };
    auto put16 = [&h](int off, quint16 v) {
        h[off] = char(v & 0xFF); h[off+1] = char((v >> 8) & 0xFF);
    };
    put32(0, 0xED26FF3A); put16(4, 1); put16(6, 0); put16(8, 28); put16(10, 12);
    put32(12, 4096); put32(16, 3); put32(20, 1); put32(24, 0);
    quint64 raw = 0;
    QVERIFY(imgsparse::sparseRawSizeFromHeader(h, raw));
    QCOMPARE(raw, quint64(3 * 4096));
    QVERIFY(!imgsparse::sparseRawSizeFromHeader(QByteArray(8, '\0'), raw));  // 头太短
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build 2>&1 | tail -5`
Expected: 编译失败（`validatePlan`/`sparseRawSizeFromHeader` 未声明）

- [ ] **Step 3: 实现**

- `sparse_image.h/.cpp`：`bool sparseRawSizeFromHeader(const QByteArray &header, quint64 &rawBytes);` —— 校验 `header.size() >= 28`、magic `0xED26FF3A`、`blk_sz > 0`、`total_blks > 0`，输出 `rawBytes = quint64(total_blks) * blk_sz`；失败返回 false（不写 error —— 纯函数，调用方给文案）。注释标 sparse 格式来源（与既有 `isSparse` 同源）。
- `finalizePlan`：`std::stable_sort`（Erase 优先，其次按 `lun`、`startSector`，Patch 一律最后）；`totalBytes` = 所有 `Action::Program` 条目 `rawBytes`（为 0 时用 `numSectors × sectorSize`）之和。
- `validatePlan`：按上面 7 条规则；错误文案形如
  `"条目 a 越界：LUN 0 需要扇区 900000000..900000100，设备仅 1000000"`
  `"条目 b 与 a 重叠：LUN 0 区间 [150,250) 与 [100,200)"`
  `"LUN 1 无设备几何信息（getstorageinfo 未返回）"`
  警告文案形如 `"条目 d 的 sectorSize 512 与设备 blockSize 4096 不一致（以条目值为准）"`。

- [ ] **Step 4: 跑测试**

Run: `cmake --build build && ./build/image_engine_tests_test_flash_plan && ./build/image_engine_tests_test_sparse`
Expected: 两个目标全 PASS（`test_sparse` 既有用例不回归）

- [ ] **Step 5: 提交**

```bash
git add src/core/edl/flash_plan.h src/core/edl/flash_plan.cpp src/image_engine/sparse_image.h src/image_engine/sparse_image.cpp tests/test_flash_plan.cpp tests/test_sparse.cpp
git commit -m "feat(edl): 计划排序/统计 + validatePlan（越界/重叠/扇区/sparse 换算）"
```

---

### Task 3: 计划层 — `buildPlanFromDir` + OPS `settings.xml` 来源 + GPT 回填对账

**Files:**
- Modify: `src/core/edl/flash_plan.h` / `.cpp`
- Test: `tests/test_flash_plan.cpp`（加用例）
- Create: `tests/flash_plan_helpers.h`（合成 OPS 元数据与 GPT 的夹具，Task 6 复用）

**Interfaces:**
- Consumes: Task 1/2 全部；`src/image_engine/disk_image.h` 的 GPT 解析（若其接口不适配，见 Step 3 的处理）
- Produces:
  - `bool buildPlanFromDir(const QString &dir, FlashPlan &plan, QString *error);`
  - `bool parseOpsSettingsXml(const QString &settingsXmlPath, const QString &packageDir, QList<PlanEntry> &out, QStringList &warnings, QString *error);`

**来源探测顺序（spec §3.4）**
1. 目录里存在 `rawprogram*.xml` → 用它们（文件名序号 = `lun`，见 `reference/qdl/tests/data/rawprogram1.xml:5-8`）+ `patch*.xml` → `finalizePlan`
2. 否则若存在 `settings.xml` → `parseOpsSettingsXml`（OPS 回退）
3. 否则 → `*error` = `"未找到 rawprogram*.xml 或 settings.xml，无法构建刷写计划。目录内 XML：<逗号分隔的 .xml 文件名列表>"`（列举帮助诊断）
- `storageType`：目录含 `prog_ufs_firehose_*.elf` → `"ufs"`；含 `prog_emmc_firehose_*` → `"emmc"`；都无 → 默认 `"ufs"` 并记 warning。

**`parseOpsSettingsXml` 规则（协议速查 §5）**
- 遍历根 `<Firehose>` 子节点，tag 名含 `Program` → `lun` = tag 末尾数字（`Program0` → 0，与 `rawprogramN` 同约定）；`UFS_PROVISION` → `lun = 0`。
- 其下每个条目（`<program>` 或内嵌 `<Image>`）：`filename` → `imageFile`（**包内偏移字段 `FileOffsetInSrc`/`SizeInByteInSrc` 一律忽略**，它们只描述包内位置，注释标 `opscrypto.py:503-513` 与 `OppEntry.cs:15-19`）。
- **几何**：`start_sector`/`num_partition_sectors`/`physical_partition_number` 若存在则取用（注释注明"强推断存在，依据 `edl/edlclient/Library/firehose_client.py:950-962` 的硬读 + chayleaf 原样导出实测"）；随后用包内 `gpt_main{N}.bin`（N=lun）的 LBA 表**对账**：分区名匹配（GPT 分区名 vs `label`/文件名主干）→ 不一致记 warning 并以 **GPT 为准**修正 `startSector`/`numSectors`；GPT 里查不到该分区 → 保留元数据值 + warning。
- `size_in_bytes`/`sha256`：OPS 元数据里有的字段原样带走（`sha256` → `PlanEntry::sha256`）。
- `<Patch{N}>` 组同理产出 `Action::Patch`（`filename != "DISK"` 跳过）。
- 元数据里没有 patch 组 → `warnings << "元数据中未找到 patch 分组：GPT 头定点修补缺失，刷后可能无法引导"`（spec §3.4 要求，不静默）。

- [ ] **Step 1: 写失败测试**

`tests/flash_plan_helpers.h`（手写字节，不调被测代码）：

```cpp
#pragma once
#include <QByteArray>
#include <QFile>
#include <QString>

// 合成 GPT 夹具。
// ⚠️ 不要照抄本段之外的任何"示例偏移"：请**先读 `src/image_engine/disk_image.{h,cpp}` 的 GPT 解析代码**，
//    按它实际接受的条件构造（签名 "EFI PART"、header 里的分区表 LBA/表项数/表项大小字段、
//    表项内 type GUID 非零、StartingLBA@32、EndingLBA@40、分区名 UTF-16LE@56、以及它是否校验 CRC32）。
//    夹具与我们的解析器自洽即可 —— 本任务要测的是 `buildPlanFromDir` 的**对账逻辑**，不是 GPT 解析器本身
//    （后者已有自己的测试）。请在函数注释里写明这一点。
QByteArray buildGptWithPartition(const QByteArray &name, quint64 firstLba, quint64 lastLba);

inline bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const qint64 n = f.write(bytes);
    f.close();
    return n == bytes.size();
}
```
（`buildGptWithPartition` 的实现放 `tests/flash_plan_helpers.cpp`？**不需要** —— 它是 inline 可放头文件，但既然要按解析器实际要求迭代，放头文件里 `inline` 实现即可；若你发现需要非 inline 状态，就落地为 `inline` 函数 + 注释说明。）

`tests/test_flash_plan.cpp` 新槽（`opsSourceUsesMetadataAndGpt`、`dirWithoutPlanReportsXmlList`）：

```cpp
void TestFlashPlan::opsSourceUsesMetadataAndGpt()
{
    QTemporaryDir dir;
    const QString settings =
        "<Firehose>\n"
        "  <Program0>\n"
        "    <program filename=\"xbl.img\" sparse=\"false\" ID=\"0\"\n"
        "             FileOffsetInSrc=\"2\" SizeInSectorInSrc=\"8\" SizeInByteInSrc=\"4096\"\n"
        "             Sha256=\"00\" physical_partition_number=\"0\"\n"
        "             start_sector=\"1\" num_partition_sectors=\"1\" />\n"   // 元数据故意写错几何
        "  </Program0>\n"
        "</Firehose>\n";
    QVERIFY(writeBytes(dir.path() + "/settings.xml", settings.toUtf8()));
    QVERIFY(writeBytes(dir.path() + "/gpt_main0.bin",
                       buildGptWithPartition("xbl", 4096, 12287)));       // GPT 才是真相

    edl::FlashPlan plan; QString err;
    QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
    QCOMPARE(plan.entries.size(), 1);
    QCOMPARE(plan.entries[0].lun, quint32(0));
    QCOMPARE(plan.entries[0].startSector, quint64(4096));                 // 以 GPT 为准
    QCOMPARE(plan.entries[0].numSectors, quint64(12287 - 4096 + 1));
    QVERIFY(plan.source.contains(QStringLiteral("OPS")));
    QVERIFY(!plan.warnings.isEmpty());                                    // 记录了与元数据不符
}

void TestFlashPlan::dirWithoutPlanReportsXmlList()
{
    QTemporaryDir dir;
    QVERIFY(writeBytes(dir.path() + "/notes.xml", QByteArray("<x/>")));
    edl::FlashPlan plan; QString err;
    QVERIFY(!edl::buildPlanFromDir(dir.path(), plan, &err));
    QVERIFY(err.contains(QStringLiteral("未找到")));
    QVERIFY(err.contains(QStringLiteral("notes.xml")));                    // 列举帮助诊断
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `cmake --build build 2>&1 | tail -5`
Expected: 编译失败（`buildPlanFromDir` 未声明）

- [ ] **Step 3: 实现**

- GPT 解析复用：先读 `src/image_engine/disk_image.h`。**若其现有接口不能按"给定文件 → 分区名/LBA 列表"给出结果**，则在本任务内新增一个**只读**函数到 `disk_image.{h,cpp}`：`bool parseGptPartitions(const QString &path, QList<GptPartition> &out, QString *error)`（`GptPartition{ QString name; quint64 firstLba; quint64 lastLba; }`）—— 复用既有 GPT 解析核心，不重写。实现前先在报告里写明你选了哪条路与理由。
- 其余按上面"规则"实现；每个字段的来源与参照行号写进注释。

- [ ] **Step 4: 跑测试**

Run: `cmake --build build && ./build/image_engine_tests_test_flash_plan`
Expected: 全 PASS（含 Task 1/2 既有用例）

- [ ] **Step 5: 提交**

```bash
git add src/core/edl/flash_plan.h src/core/edl/flash_plan.cpp tests/flash_plan_helpers.h tests/test_flash_plan.cpp src/image_engine/disk_image.h src/image_engine/disk_image.cpp
git commit -m "feat(edl): buildPlanFromDir + OPS 元数据来源 + GPT 回填对账"
```
（若 Step 3 判定无需改 `disk_image.*`，则提交里不含这两个文件。）

---

### Task 4: 传输接口 + Mock + Sahara 协议

**Files:**
- Create: `src/core/edl/edl_transport.h`
- Create: `src/core/edl/sahara.h` / `sahara.cpp`
- Create: `tests/mock_edl_transport.h`
- Create: `tests/edl_test_helpers.h`（**共享**的合成 Sahara 帧构造器，Task 6 复用 —— 不得在测试间各写一份）
- Test: `tests/test_edl_sahara.cpp`
- Modify: `CMakeLists.txt`（注册测试）

**Interfaces:**
- Consumes: 无（`IEdlTransport` 是本层根基）
- Produces:

```cpp
// src/core/edl/edl_transport.h
#pragma once
#include <QByteArray>
#include <QString>

namespace edl {

// EDL 传输抽象：唯一的设备依赖点（真机 = LibusbEdlTransport；测试 = MockEdlTransport）
class IEdlTransport
{
public:
    virtual ~IEdlTransport() = default;
    virtual bool       open(QString *error) = 0;                            // 打开 9008 设备
    virtual void       close() = 0;                                         // 幂等
    virtual bool       write(const QByteArray &data, QString *error) = 0;   // 裸写 OUT
    virtual QByteArray read(int maxBytes, int timeoutMs, QString *error) = 0; // 裸读 IN；超时→空+error
    virtual bool       resetDevice(QString *error) = 0;                     // 退出 EDL/复位
    virtual bool       waitReenumerate(int timeoutMs, QString *error) = 0;  // Sahara→Firehose 转折
    virtual int        maxPacketSize() const = 0;                           // OUT 端点最大包长（ZLP 判定）
};

} // namespace edl
```

```cpp
// src/core/edl/sahara.h
#pragma once
#include <QByteArray>
#include <QString>
#include "edl_transport.h"

namespace edl {

// Sahara 命令（照 src/core/modes/edl_handler.h 既有枚举搬迁）
enum SaharaCmd : quint32 {
    SAHARA_HELLO_REQ      = 0x01, SAHARA_HELLO_RSP   = 0x02,
    SAHARA_READ_DATA      = 0x03, SAHARA_END_OF_IMAGE = 0x04,
    SAHARA_DONE_REQ       = 0x05, SAHARA_DONE_RSP    = 0x06,
    SAHARA_READ_DATA_64   = 0x12
};

// 载入 programmer：等 HELLO_REQ → 回 HELLO_RSP（mode=Image transfer, ver 2, 兼容 1）
// → 循环服务 READ_DATA/READ_DATA_64（**用 quint64 偏移，不得截断**）→ END_OF_IMAGE → DONE_REQ/DONE_RSP
bool saharaLoadProgrammer(IEdlTransport &t, const QByteArray &programmer,
                          QString *error, int helloTimeoutMs = 30000);
}
```

`tests/mock_edl_transport.h`：

```cpp
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include "core/edl/edl_transport.h"

namespace edl {

// 脚本化 transport：reads 队列按序出队；writes 全量记录；可注入失败
class MockEdlTransport : public IEdlTransport
{
public:
    QList<QByteArray> reads;          // 每次 read() 出队一个（空队列 → 返回空 + error）
    QList<QByteArray> writes;         // 每次 write() 追加
    int  failWriteAt = -1;            // 第 N 次 write 失败（0 基），-1=不失败
    bool openResult = true;
    bool reenumerateResult = true;
    int  maxPacket = 1024;

    bool open(QString *error) override { Q_UNUSED(error); return openResult; }
    void close() override {}
    bool write(const QByteArray &data, QString *error) override {
        if (failWriteAt >= 0 && writes.size() == failWriteAt) {
            if (error) *error = QStringLiteral("注入的写失败");
            return false;
        }
        writes.append(data);
        return true;
    }
    QByteArray read(int maxBytes, int timeoutMs, QString *error) override {
        Q_UNUSED(maxBytes); Q_UNUSED(timeoutMs);
        if (reads.isEmpty()) { if (error) *error = QStringLiteral("读超时（mock 队列空）"); return {}; }
        return reads.takeFirst();
    }
    bool resetDevice(QString *error) override { Q_UNUSED(error); return true; }
    bool waitReenumerate(int timeoutMs, QString *error) override {
        Q_UNUSED(timeoutMs); Q_UNUSED(error); return reenumerateResult;
    }
    int maxPacketSize() const override { return maxPacket; }
};

} // namespace edl
```

- [ ] **Step 1: 写失败测试**

先建共享夹具 `tests/edl_test_helpers.h`（Task 6 也要 include 它）：

```cpp
// tests/edl_test_helpers.h
#pragma once
#include <QByteArray>
#include <QList>

// Sahara 帧：cmd(4B LE) + 总长(4B LE) + N×4B LE 参数（与 src/core/edl/sahara.cpp 的帧布局一致）
inline QByteArray saharaFrame(quint32 cmd, const QList<quint32> &words)
{
    QByteArray f(8 + words.size() * 4, '\0');
    auto put32 = [&f](int off, quint32 v) {
        f[off] = char(v & 0xFF); f[off+1] = char((v >> 8) & 0xFF);
        f[off+2] = char((v >> 16) & 0xFF); f[off+3] = char((v >> 24) & 0xFF);
    };
    put32(0, cmd); put32(4, 8 + words.size() * 4);
    for (int i = 0; i < words.size(); ++i) put32(8 + i * 4, words.at(i));
    return f;
}

// 64 位 READ_DATA 帧：参数 = image_id(u64) + offset(u64) + length(u64) = 6 个 u32 字（低位在前）。
// ⚠️ 别手写这串字：写 {0,12,0,0,4,0} 会编码成 image_id=0xC00000000 / offset=**0** / length=4
//    （本计划早期版本就是这么错的，实测回吐 "0123" 而非期望切片）—— 用本函数。
inline QByteArray saharaReadData64Frame(quint64 imageId, quint64 offset, quint64 length)
{
    QList<quint32> words;
    const quint64 vals[3] = {imageId, offset, length};
    for (quint64 v : vals) { words << quint32(v & 0xFFFFFFFFu) << quint32(v >> 32); }
    return saharaFrame(quint32(edl::SAHARA_READ_DATA_64), words);
}
```

`tests/test_edl_sahara.cpp`：用一个"假设备"驱动 —— 预置 reads 队列（HELLO_REQ 帧、两段 READ_DATA、END_OF_IMAGE），断言 writes 里的应答与数据切片。

```cpp
#include <QtTest>
#include "core/edl/sahara.h"
#include "mock_edl_transport.h"
#include "edl_test_helpers.h"

#include "edl_test_helpers.h"   // saharaFrame()（Task 6 的测试也用它，不许各写一份）

class TestEdlSahara : public QObject
{
    Q_OBJECT
private slots:
    void servesProgrammerInRequestedSlices();
    void failsWhenDeviceNeverRequests();
};

void TestEdlSahara::servesProgrammerInRequestedSlices()
{
    edl::MockEdlTransport t;
    const QByteArray prog = QByteArray("0123456789ABCDEF");   // 16 字节
    t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0})          // mode=2, ver=2
            << saharaFrame(edl::SAHARA_READ_DATA, {0, 0, 4, 0})          // image=0 off=0 len=4
            << saharaFrame(edl::SAHARA_READ_DATA, {0, 4, 8, 0})          // off=4 len=8
            << saharaReadData64Frame(0, 12, 4)               // 64 位：off=12 len=4（用共享夹具，别手写字串）
            << saharaFrame(edl::SAHARA_END_OF_IMAGE, {0, 0})
            << saharaFrame(edl::SAHARA_DONE_RSP, {});   // ← DONE 这对是 **host 主动**：host 发 DONE_REQ，
                                                        //   设备回 DONE_RSP（bkerler sahara.py:453-459 +
                                                        //   edl_handler.cpp:566-584 两源一致；与 HELLO_REQ/
                                                        //   READ_DATA/END_OF_IMAGE 的"设备主动"方向相反，最易记反）

    QString err;
    QVERIFY2(edl::saharaLoadProgrammer(t, prog, &err), qPrintable(err));
    QCOMPARE(t.writes.size(), 5);                                  // HELLO_RSP + 3 段数据 + DONE_REQ
    QCOMPARE(t.writes[1], QByteArray("0123"));                     // 第一段切片
    QCOMPARE(t.writes[2], QByteArray("456789AB"));                 // 第二段切片
    QCOMPARE(t.writes[3], QByteArray("CDEF"));                     // 64 位偏移段（未截断）
    const QByteArray helloRsp = t.writes[0];
    QCOMPARE(int(helloRsp.at(0)), 0x02);                           // cmd = HELLO_RSP
    QCOMPARE(int(t.writes[4].at(0)), 0x05);                        // 最后一帧 = DONE_REQ（host 主动）
    QCOMPARE(int(t.writes[4].at(4)), 8);                           // 长度字段 = 8（无 payload）
}

void TestEdlSahara::failsWhenDeviceNeverRequests()
{
    edl::MockEdlTransport t;                                       // 空队列 = 读超时
    QString err;
    QVERIFY(!edl::saharaLoadProgrammer(t, QByteArray("x"), &err, 10));
    QVERIFY(!err.isEmpty());                                        // 中文文案，含"未收到"
}
```

**另需补一条用例（控制器要求）**：`SAHARA_READ_DATA_64` 的**偏移 > 4GiB**（如 `offset = 0x1_0000_0000`）—— brief 自带那条只覆盖小偏移，**杀不掉截断缺陷**；断言切出的字节等于该 64 位偏移处的内容（夹具侧把 programmer 造得足够大或用"偏移越界即报错"的语义明确表达，由你选并说明）。

- [ ] **Step 2: 跑测试确认失败**

把 `tests/test_edl_sahara.cpp` 加进 `IMAGE_TEST_SOURCES`；把 `src/core/edl/sahara.cpp` 加入该测试目标的额外源（照 `CMakeLists.txt:318-321` 的范式：该测试目标需要 `src/core/edl/sahara.cpp`）。
Run: `cmake -B build -G Ninja && cmake --build build 2>&1 | tail -5`
Expected: 编译失败（`core/edl/sahara.h` 不存在）

- [ ] **Step 3: 实现**

按 `src/core/modes/edl_handler.cpp` 既有 Sahara 实现（**已工作**：HELLO/服务 programmer/DONE）搬迁到 `sahara.cpp`，差异只有三点：
1. 走 `IEdlTransport` 而非内联 libusb；
2. **`SAHARA_READ_DATA_64` 用 quint64 偏移**（修既有 `edl_handler.cpp:302-312` 的截断缺陷），地址/长度按 64 位小端解析（`{cmd, len, image_id(u64), offset(u64), length(u64)}`）；
3. 失败文案中文且带阶段名（`"Sahara 阶段未收到设备请求（HELLO）"` 等）。
帧布局注释标 `src/core/modes/edl_handler.cpp:201-254`（既有解析器）与 `edl/edlclient/Library/sahara.py` 对应处。

- [ ] **Step 4: 跑测试**

Run: `cmake --build build && ./build/image_engine_tests_test_edl_sahara`
Expected: `2 passed`

- [ ] **Step 5: 提交**

```bash
git add src/core/edl/edl_transport.h src/core/edl/sahara.h src/core/edl/sahara.cpp tests/mock_edl_transport.h tests/test_edl_sahara.cpp CMakeLists.txt
git commit -m "feat(edl): IEdlTransport 接口 + Mock + Sahara 协议（64 位偏移不截断）"
```

---

### Task 5: Firehose 命令构造与响应解析

**Files:**
- Create: `src/core/edl/firehose.h` / `firehose.cpp`
- Test: `tests/test_edl_firehose.cpp`
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: `IEdlTransport`（Task 4）、`PlanEntry`/`StorageInfo`（Task 1/2）
- Produces:

```cpp
// src/core/edl/firehose.h
#pragma once
#include <QByteArray>
#include <QString>
#include "edl_transport.h"
#include "flash_plan.h"

namespace edl {

// 命令 XML 构造（纯函数；属性集与参照出处见协议速查 §1-§3）
QByteArray xmlConfigure(const QString &memoryName, quint32 maxPayloadBytes = 0);
QByteArray xmlProgram(const PlanEntry &e);
QByteArray xmlPatch(const PlanEntry &e);          // 不发 what；filename 原样（"DISK"）
QByteArray xmlErase(const PlanEntry &e);          // numSectors==0 → 省略 start/count（整 LUN）
QByteArray xmlRead(quint32 lun, quint64 startSector, quint64 numSectors, quint32 sectorSize);
QByteArray xmlGetStorageInfo(quint32 lun);
QByteArray xmlSetBootableStorageDrive(quint32 lun);
QByteArray xmlReset();

// 响应解析（纯函数）
struct FirehoseResponse {
    bool    ack = false;
    bool    nak = false;
    QString raw;                 // 原始 XML（错误文案里回显）
    QString errorText;           // <log value="..."/> 里的文本
    QString storageInfoJson;     // getstorageinfo 时 <log value="{...}"/> 的 JSON 文本
};
FirehoseResponse parseFirehoseResponse(const QByteArray &xml);
bool parseStorageInfo(const FirehoseResponse &r, quint32 lun, StorageInfo &out, QString *error);

// 会话级小工具（需要传输）
bool firehoseSendCommand(IEdlTransport &t, const QByteArray &xml, FirehoseResponse &resp,
                         int timeoutMs, QString *error);
bool firehoseConfigure(IEdlTransport &t, QString &memoryName, quint32 &maxPayloadBytes, QString *error);

} // namespace edl
```

**要点（逐条照协议速查，注释标行号）**
- `xmlProgram`：属性顺序与集合 = `SECTOR_SIZE_IN_BYTES`/`num_partition_sectors`/`physical_partition_number`/`start_sector`，**`filename` 非空时追加**（`reference/qdl/src/firehose.c:1021-1030`）。
- **`start_sector` 的取值（program 与 patch 同款）**：`e.startSectorExpr` 非空 → **原样输出该串**；否则输出十进制 `e.startSector`（`reference/qdl/src/firehose.c:874-879` 明确主机侧解析表达式会写错地址）。
- `xmlPatch`：`SECTOR_SIZE_IN_BYTES`/`byte_offset`/`filename`/`physical_partition_number`/`size_in_bytes`/`start_sector`/`value`，**不发 `what`**（`firehose.c:1408,1410-1424`）。
- `xmlErase`：`numSectors==0` **且 `startSectorExpr` 为空** → `<erase SECTOR_SIZE_IN_BYTES=".." physical_partition_number=".." />`（整 LUN）；否则带 `num_partition_sectors`+`start_sector`（`firehose.c:611-628`）。**注意**：`startSectorExpr` 非空时 `numSectors==0` 并不表示"整 LUN"（Task 1 报告 §6 已标注该组合），此时仍须发 `start_sector="<expr>"` 且 `num_partition_sectors` 用 `numSectors`。
- `xmlConfigure`：`MemoryName`/`MaxPayloadSizeToTargetInBytes`/`Verbose="0"`/`ZlpAwareHost="1"`/`SkipStorageInit="0"`（`firehose.c:510-515`）；`maxPayloadBytes==0` 时省略该属性。
- `parseFirehoseResponse`：`<response value="ACK"/>` → `ack=true`；`value="NAK"` → `nak=true`（**严格判定，不得用 contains** —— 这正是既有 `edl_handler.cpp:494-510` 的缺陷）；`<log value="..."/>` 取文本（**先做 XML 转义反转义**，`&quot;` → `"` 等）。
- `firehoseConfigure`：发 `xmlConfigure` 等 ACK → 若响应含 `MaxPayloadSizeToTargetInBytesSupported` → 用它重发一次（`firehose.c:462-484,534-548`）；NAK 且 `errorText` 含 `Not support configure MemoryName` → 换另一存储类型重试一次（`firehose.py:936-940`）；NAK 且 `errorText` 含 `Only nop and sig tag can be` → **直接报错"设备要求 EDL 鉴权，本工具暂不支持"并返回 false，不重试**（`firehose.py:941-956`；spec §8 明确不做小米鉴权分支）；仍失败 → 中文 error 带设备返回原文。

- [ ] **Step 1: 写失败测试**

`tests/test_edl_firehose.cpp`：命令逐字段断言 + 响应解析 + configure 两轮协商（用 `MockEdlTransport` 预置响应）。

```cpp
#include <QtTest>
#include "core/edl/firehose.h"
#include "mock_edl_transport.h"

class TestEdlFirehose : public QObject
{
    Q_OBJECT
private slots:
    void programXmlHasFourRequiredAttrs();
    void patchXmlOmitsWhat();
    void eraseWholeLunOmitsRange();
    void responseAckAndNakAreStrict();
    void configureNegotiatesMaxPayload();
};

void TestEdlFirehose::programXmlHasFourRequiredAttrs()
{
    edl::PlanEntry e; e.action = edl::PlanEntry::Action::Program;
    e.imageFile = QStringLiteral("/tmp/xbl.img"); e.lun = 1;
    e.startSector = 1234; e.numSectors = 8192; e.sectorSize = 4096;
    const QByteArray xml = edl::xmlProgram(e);
    QVERIFY(xml.contains("SECTOR_SIZE_IN_BYTES=\"4096\""));
    QVERIFY(xml.contains("num_partition_sectors=\"8192\""));
    QVERIFY(xml.contains("physical_partition_number=\"1\""));
    QVERIFY(xml.contains("start_sector=\"1234\""));
    QVERIFY(xml.contains("filename=\"xbl.img\""));      // 仅文件名，不带路径
    QVERIFY(xml.startsWith("<program "));
}

void TestEdlFirehose::patchXmlOmitsWhat()
{
    edl::PlanEntry e; e.action = edl::PlanEntry::Action::Patch;
    e.lun = 0; e.startSector = 2; e.byteOffset = 0; e.sizeInBytes = 4;
    e.value = QStringLiteral("NUM_DISK_SECTORS-6."); e.sectorSize = 4096;
    e.imageFile = QStringLiteral("DISK"); e.what = QStringLiteral("Update LBA");
    const QByteArray xml = edl::xmlPatch(e);
    QVERIFY(xml.contains("value=\"NUM_DISK_SECTORS-6.\""));   // 原样透传
    QVERIFY(xml.contains("filename=\"DISK\""));
    QVERIFY(!xml.contains("what="));                          // what 不进 XML
}

void TestEdlFirehose::eraseWholeLunOmitsRange()
{
    edl::PlanEntry e; e.action = edl::PlanEntry::Action::Erase;
    e.lun = 3; e.numSectors = 0; e.sectorSize = 4096;
    const QByteArray xml = edl::xmlErase(e);
    QVERIFY(xml.contains("physical_partition_number=\"3\""));
    QVERIFY(!xml.contains("start_sector"));
    QVERIFY(!xml.contains("num_partition_sectors"));
}

void TestEdlFirehose::responseAckAndNakAreStrict()
{
    using edl::parseFirehoseResponse;
    QVERIFY(parseFirehoseResponse(QByteArray("<response value=\"ACK\" />")).ack);
    QVERIFY(parseFirehoseResponse(QByteArray("<response value=\"NAK\" />")).nak);
    // 既有实现正是被下面这类文本骗过：ACK 出现在 log 里但 response 是 NAK
    const auto r = parseFirehoseResponse(
        QByteArray("<response value=\"NAK\" /><log value=\"ACK expected but timeout\" />"));
    QVERIFY(r.nak && !r.ack);
    QCOMPARE(r.errorText, QStringLiteral("ACK expected but timeout"));
}

void TestEdlFirehose::configureNegotiatesMaxPayload()
{
    edl::MockEdlTransport t;
    t.reads << QByteArray("<response value=\"ACK\" MaxPayloadSizeToTargetInBytesSupported=\"1048576\" />")
            << QByteArray("<response value=\"ACK\" />");
    QString name = QStringLiteral("ufs"); quint32 maxPayload = 0; QString err;
    QVERIFY2(edl::firehoseConfigure(t, name, maxPayload, &err), qPrintable(err));
    QCOMPARE(t.writes.size(), 2);                       // 首轮 + 用协商值重发
    QCOMPARE(maxPayload, quint32(1048576));
    QVERIFY(t.writes[1].contains("MaxPayloadSizeToTargetInBytes=\"1048576\""));
}

QTEST_APPLESS_MAIN(TestEdlFirehose)
#include "test_edl_firehose.moc"
```

- [ ] **Step 2: 跑测试确认失败**

注册测试 + 把 `src/core/edl/firehose.cpp`、`src/core/edl/flash_plan.cpp` 加进该测试目标源。
Run: `cmake -B build -G Ninja && cmake --build build 2>&1 | tail -5`
Expected: 编译失败（`core/edl/firehose.h` 不存在）

- [ ] **Step 3: 实现**

按上面要点实现；XML 用 `QString` 拼接后 `toUtf8()`（属性值做 XML 转义：`&`→`&amp;`、`"`→`&quot;`、`<`→`&lt;`）。解析用 `QXmlStreamReader`。`parseStorageInfo`：解析 `storageInfoJson` 的 `total_blocks`/`block_size`（qdl 口径 `firehose.c:1874-1884`）；也接受 bkerler 的文本键（`SECTOR_SIZE_IN_BYTES`/`num_physical_partitions`，`firehose.py:1260-1275`）作为回退；都拿不到 → 中文 error。

- [ ] **Step 4: 跑测试**

Run: `cmake --build build && ./build/image_engine_tests_test_edl_firehose`
Expected: `5 passed`

- [ ] **Step 5: 提交**

```bash
git add src/core/edl/firehose.h src/core/edl/firehose.cpp tests/test_edl_firehose.cpp CMakeLists.txt
git commit -m "feat(edl): Firehose 命令构造与响应解析（严格 ACK 判定 + configure 两轮协商）"
```

---

### Task 6: `edl_session` 编排 + 数据面（含 sparse 展开）

**Files:**
- Create: `src/core/edl/edl_session.h` / `edl_session.cpp`
- Modify: `src/image_engine/sparse_image.h` / `.cpp`（**新增** `sparseWalk` + `SparseChunk`，不改既有接口）
- Test: `tests/test_edl_session.cpp`；`tests/test_sparse.cpp`（加 `sparseWalk` 用例）
- Modify: `CMakeLists.txt`

**Interfaces:**
- Consumes: Task 4/5 全部、Task 2 的 `validatePlan`、`imgsparse::sparseWalk`
- Produces:

```cpp
// src/core/edl/edl_session.h
#pragma once
#include <QByteArray>
#include <QString>
#include <QList>
#include <functional>
#include "edl_transport.h"
#include "flash_plan.h"

namespace edl {

struct FlashOptions {
    bool verifyAfterWrite = true;      // 边写边算 sha256，写完比对（不符 → 错误，不回滚）
    bool fullVerifyBeforeWrite = false;// 刷前完整校验（GB 级慢，默认关）
};

struct SessionProgress { QString stage; QString detail; int percent = 0; };
using ProgressFn = std::function<void(const SessionProgress &)>;

// 读回请求（备份/校验）
struct ReadRequest { quint32 lun = 0; quint64 startSector = 0; quint64 numSectors = 0;
                     quint32 sectorSize = 4096; QString outputPath; };

class EdlSession
{
public:
    explicit EdlSession(IEdlTransport &transport, ProgressFn progress = {});

    // 完整刷写：Sahara(载 programmer) → 等重枚举 → configure → getstorageinfo → validatePlan
    //           → 逐条目执行 → setbootablestoragedrive（若计划含 xbl/sbl1）→ reset
    bool run(const FlashPlan &plan, const QByteArray &programmer,
             const FlashOptions &opt, QString *error);

    // 只读回（不写入、不复位）
    bool readBack(const QList<ReadRequest> &reqs, const QList<StorageInfo> &device, QString *error);

private:
    bool writeEntry(const PlanEntry &e, const FlashOptions &opt, QString *error);   // 单条目：XML + 数据面 + 等 ACK
    IEdlTransport &m_t;
    ProgressFn     m_progress;
    quint32        m_maxPayload = 0;
    quint64        m_writtenBytes = 0;   // 进度分母累计
};

} // namespace edl
```

```cpp
// src/image_engine/sparse_image.h 追加
namespace imgsparse {
struct SparseChunk {
    quint64    rawOffsetBytes = 0;   // 该块在 raw 镜像中的起始字节
    quint64    rawBytes = 0;         // 覆盖的 raw 字节数
    bool       raw = false;          // 真数据块（data 有效）
    bool       fill = false;         // FILL 块（用 fillValue 填充，data 为空）
    bool       dontCare = false;     // DONT_CARE（不产出数据，调用方仍需推进偏移）
    quint32    fillValue = 0;
    QByteArray data;
};
// 流式逐块产出（不落盘）。cb 返回 false 表示调用方要求中止（此时函数返回 false 且 error 说明"被调用方中止"）。
bool sparseWalk(const QString &inPath, const std::function<bool(const SparseChunk &)> &cb,
                QString *error = nullptr);
}
```

**Task 5 审查的两条交接项（本任务必做，均已由审查员独立核实）**：
1. **drain 警告**：`firehoseSendCommand` 见 `<response>` 即停读，**残留字节留在 IN 端点**；qdl 的注释明确"不消费完，后续写会超时"（`reference/qdl/src/firehose.c:249-252`）。本任务**在每次写之前**（或每条命令之后）必须 drain 掉残留，否则下一笔写可能超时 —— 把这条写进代码注释，并加一条用例（预置"响应后还跟着一段字节"的读队列 → 断言下一命令仍成功）。
2. **`maxPayloadBytes == 0` 的语义**：设备未回 `MaxPayloadSizeToTargetInBytesSupported` 时 `firehoseConfigure` 会原样回传（**可能是 0**）。**不得把 0 当"无上限"**（否则数据面会试图一次发完整个镜像）—— 取一个保守默认并在注释里写明依据，且日志里说明用了默认值。

**getstorageinfo 的枚举口径（Task 5 实现者提出，控制方裁定）**：只查**计划里出现过的 LUN**（对 `plan.entries` 的 `lun` 去重后逐个查），**不**依赖设备的 LUN 总数 —— 这是 qdl 的口径（`reference/qdl/src/program.c:259` 逐条目取 `physical_partition_number`，不枚举 LUN）。`getstorageinfo` 响应里的 `num_physical_partitions`/`bNumberLu` 仅作日志信息，**不**用于决定查几个 LUN；`StorageInfo` 里没有 LUN 数字段是刻意的（Task 5 已在注释说明），不要为此扩结构体。

**Mock 扩展（Task 4 实现者提醒，本任务必做）**：`tests/mock_edl_transport.h` 目前只记录 writes，**不记录 `close`/`resetDevice`/`waitReenumerate` 的调用**（也不记录顺序）。本任务要断言"失败路径不发 reset""成功路径 reset 在最后"，因此先把 mock 扩成**记录调用序列**（如 `QStringList calls;` 追加 `"open"/"close"/"reset"/"waitReenumerate"`，并在 write 时追加 `"write"`），再据此断言。这是 §6 测试策略里"中止语义"的前置条件。

**执行要点（spec §4 + 协议速查）**
- 数据面分块：`chunkSectors = qMax(quint64(1), m_maxPayload / sectorSize)`；每块裸写 OUT（**无长度前缀**）；`len % t.maxPacketSize() == 0` 时补一次 0 字节写（ZLP，`reference/qdl/src/usb.c:548-553`）。
- 每块发完 `firehoseSendCommand` 等 ACK？**不** —— 参照只在整条 `program` 的数据发完后等一个 ACK（`firehose.c:1132-1137`）；块间不额外等待。
- 文件尾补零到扇区边界（`firehose.c:1096-1097`）。
- sparse：`sparseWalk` 的 `raw` 块直接发 `data`；`fill` 块按 `fillValue` 生成数据（**流式生成，不一次 materialize**）；`dontCare` 跳过但偏移推进。
- 进度：`percent` 按"已写字节 / plan.totalBytes"，条目切换时带 `detail = 条目名`。
- `verifyAfterWrite`：数据面推送时同步喂 `QCryptographicHash`（sha256），写完与该条目的 `sha256`（非空时）比对；不符 → 记 error（**不回滚**）。
- **中止语义**：任何一步 ACK 非 ACK / 写失败 → 立即返回 false，**不发 reset**；错误文案含"已写入 N 扇区、失败于偏移 M"。
- reset 仅在全部成功路径发一次。

- [ ] **Step 1: 写失败测试**

`tests/test_edl_session.cpp`：合成 programmer + 含 1 个 Program 条目（小镜像 2 扇区）+ 1 个 DISK patch 的计划 → 用 `MockEdlTransport` 预置：HELLO、READ_DATA、END_OF_IMAGE、DONE、configure ACK、getstorageinfo JSON、program ACK、patch ACK、最终数据 ACK。

```cpp
#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include "core/edl/edl_session.h"
#include "mock_edl_transport.h"

#include "edl_test_helpers.h"   // saharaFrame()（与 test_edl_sahara.cpp 共用同一份，不重复实现）

// 组装一次完整会话的 mock 读队列：programmer 一次性读完 → 结束 → configure → getstorageinfo → program(声明+数据)
static void queueSuccessfulSession(edl::MockEdlTransport &t, const QByteArray &programmer, bool programAck)
{
    const QByteArray ack = QByteArray("<response value=\"ACK\" />");
    const QByteArray nak = QByteArray("<response value=\"NAK\" />");
    t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0})
            << saharaFrame(edl::SAHARA_READ_DATA, {0, 0, quint32(programmer.size()), 0})
            << saharaFrame(edl::SAHARA_END_OF_IMAGE, {0, 0})
            << saharaFrame(edl::SAHARA_DONE_RSP, {})   // DONE 对是 host 主动：host 发 DONE_REQ、设备回 DONE_RSP
            << ack                                                                    // configure
            << QByteArray("<log value=\"{&quot;storage_info&quot;:{&quot;total_blocks&quot;:100000,"
                          "&quot;block_size&quot;:4096}}\" /><response value=\"ACK\" />") // getstorageinfo
            << (programAck ? ack : nak)                                               // program 声明
            << (programAck ? ack : nak);                                              // 数据发完
}

class TestEdlSession : public QObject
{
    Q_OBJECT
private slots:
    void writesImageDataAndResetsOnSuccess();
    void stopsWithoutResetOnNak();
};

void TestEdlSession::writesImageDataAndResetsOnSuccess()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');                  // 2 扇区
    QFile f(dir.path() + "/boot.img");
    QVERIFY(f.open(QIODevice::WriteOnly)); f.write(image); f.close();

    edl::FlashPlan plan; plan.storageType = QStringLiteral("ufs");
    edl::PlanEntry e; e.action = edl::PlanEntry::Action::Program;
    e.partitionName = QStringLiteral("boot"); e.imageFile = dir.path() + "/boot.img";
    e.lun = 0; e.startSector = 100; e.numSectors = 2; e.sectorSize = 4096;
    e.rawBytes = 8192;
    plan.entries = {e}; plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    t.maxPacket = 512;
    const QByteArray prog = QByteArray(16, '\x11');
    queueSuccessfulSession(t, prog, true);

    QString err;
    edl::EdlSession s(t);
    QVERIFY2(s.run(plan, prog, edl::FlashOptions{}, &err), qPrintable(err));
    QVERIFY(t.writes.last().contains(QStringLiteral("reset")));   // reset 是最后一条
    QVERIFY(t.writes.contains(image));                            // 镜像字节原样推送
}

void TestEdlSession::stopsWithoutResetOnNak()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    QFile f(dir.path() + "/boot.img");
    QVERIFY(f.open(QIODevice::WriteOnly)); f.write(image); f.close();

    edl::FlashPlan plan; plan.storageType = QStringLiteral("ufs");
    edl::PlanEntry e; e.action = edl::PlanEntry::Action::Program;
    e.partitionName = QStringLiteral("boot"); e.imageFile = dir.path() + "/boot.img";
    e.lun = 0; e.startSector = 100; e.numSectors = 2; e.sectorSize = 4096; e.rawBytes = 8192;
    plan.entries = {e}; plan.totalBytes = 8192;

    edl::MockEdlTransport t; t.maxPacket = 512;
    const QByteArray prog = QByteArray(16, '\x11');
    queueSuccessfulSession(t, prog, false);                       // program 返回 NAK

    QString err;
    edl::EdlSession s(t);
    QVERIFY(!s.run(plan, prog, edl::FlashOptions{}, &err));
    QVERIFY(err.contains(QStringLiteral("boot")));                // 文案点名失败条目
    for (const QByteArray &w : t.writes)
        QVERIFY(!w.contains(QByteArray("reset")));                // 失败路径不得复位设备
}

QTEST_APPLESS_MAIN(TestEdlSession)
#include "test_edl_session.moc"
```

`tests/test_sparse.cpp` 加 `sparseWalkYieldsRawFillAndDontCare`：写一个含 RAW+FILL+DONT_CARE 三块的合成 sparse 文件 → 断言回调收到的三个 `SparseChunk`（`rawBytes`、`fillValue`、`dontCare` 与 `rawOffsetBytes` 递增）。

- [ ] **Step 2: 跑测试确认失败**

注册测试 + 把 `src/core/edl/{edl_session,firehose,flash_plan,sahara}.cpp` + `src/image_engine/sparse_image.cpp` 加进该测试目标源。
Run: `cmake -B build -G Ninja && cmake --build build 2>&1 | tail -5`
Expected: 编译失败（`core/edl/edl_session.h` 不存在）

- [ ] **Step 3: 实现**

按上面要点实现；`sparseWalk` 照既有 `simg2imgStream` 的块遍历逻辑改写（同一套 chunk 头解析），但产出结构化 `SparseChunk` 而非写文件。Session 内部按 spec §4 的 6 步顺序编排，每步失败文案带阶段名。

- [ ] **Step 4: 跑测试**

Run: `cmake --build build && ./build/image_engine_tests_test_edl_session && ./build/image_engine_tests_test_sparse`
Expected: 全 PASS（`test_sparse` 既有用例不回归）

- [ ] **Step 5: 提交**

```bash
git add src/core/edl/edl_session.h src/core/edl/edl_session.cpp src/image_engine/sparse_image.h src/image_engine/sparse_image.cpp tests/test_edl_session.cpp tests/test_sparse.cpp CMakeLists.txt
git commit -m "feat(edl): EdlSession 编排 + 数据面（分块/ZLP/补零/sparse 展开）+ 中止语义"
```

---

### Task 7: libusb 传输实现 + `EDLHandler` 退化 + 5 处既有缺陷修复

**Files:**
- Create: `src/core/edl/edl_libusb_transport.h` / `.cpp`
- Modify: `src/core/modes/edl_handler.h` / `.cpp`（退化为薄封装）
- Test: `tests/test_edl_libusb_transport.cpp`（**仅**可离线验证的部分：VID/PID 表与错误文案的纯函数部分）

**Interfaces:**
- Consumes: `IEdlTransport`（Task 4）、`EdlSession`（Task 6）
- Produces:
  - `class LibusbEdlTransport : public IEdlTransport`（实现 Task 4 的 7 个方法）
  - `EDLHandler` **公开 API 与两信号逐字不变**（`connectSahara`/`disconnect`/`firehoseConnect`/`listPartitions`/`readPartition`/`writePartition`/`writeRaw`/`isConnected`/`lastError` + `outputMessage(QString,bool)`/`progress(int)`），内部改为：枚举/打开设备 → `LibusbEdlTransport` → `EdlSession`

**要修的 5 处既有缺陷（协议速查 §6，逐条对照）**
1. 帧格式：**去掉 4 字节长度前缀**，裸发 XML；响应解析不丢前 4 字节（`edl_handler.cpp:456-461,489-491`）。
2. `waitFirehoseDone` 的 ACK 判定：改为 `parseFirehoseResponse` 的**严格** `value=="ACK"`（`:494-510`）。
3. `configure` 改为等响应（走 `firehoseConfigure`）（`:634-648`）。
4. `<program>` 补 `physical_partition_number` + 数据面走 `EdlSession` 的写入路径（`:791-814`）。
5. `<read>` 属性名改 `num_partition_sectors`、补 `physical_partition_number`、去掉 `filename`（`:716-725`）。

- [ ] **Step 1: 写失败测试**

`tests/test_edl_libusb_transport.cpp`：只测**不碰设备**的部分（例如 VID/PID 匹配表与错误文案构造）—— 把 VID/PID 表做成可测的纯函数：

```cpp
#include <QtTest>
#include "core/edl/edl_libusb_transport.h"

class TestEdlLibusbTransport : public QObject
{
    Q_OBJECT
private slots:
    void recognizesKnownEdlIds();
    void rejectsNonEdlIds();
};

void TestEdlLibusbTransport::recognizesKnownEdlIds()
{
    QVERIFY(edl::LibusbEdlTransport::isEdlId(0x05C6, 0x9008));
    QVERIFY(edl::LibusbEdlTransport::isEdlId(0x05C6, 0x900E));
    QVERIFY(!edl::LibusbEdlTransport::isEdlId(0x18D1, 0x4EE0));
}
```

- [ ] **Step 2: 跑测试确认失败**

注册测试 + 把 `src/core/edl/edl_libusb_transport.cpp`、`sahara.cpp`、`firehose.cpp`、`flash_plan.cpp`、`edl_session.cpp` 加进该目标源，并按名补 `libusb`（照 `CMakeLists.txt:367-371` 的范式）。
Run: `cmake -B build -G Ninja && cmake --build build 2>&1 | tail -5`
Expected: 编译失败

- [ ] **Step 3: 实现**

- `edl_libusb_transport.cpp`：把 `edl_handler.cpp:45-129` 的 libusb 逻辑整体搬来（初始化/枚举/打开/端点/超时），加上 `waitReenumerate`（沿用既有 3s×15 轮询，参数化超时）与 `maxPacketSize`（取 OUT 端点 `wMaxPacketSize`）。`isEdlId` 用既有 VID/PID 表（`0x05C6` + {`0x9008`,`0x900E`,`0x9025`}）。
- `edl_handler.cpp`：删掉被搬走的协议代码，改为持有 `LibusbEdlTransport` + `EdlSession`；`connectSahara` → `saharaLoadProgrammer`；`firehoseConnect` → `firehoseConfigure`；`listPartitions` → `getstorageinfo` + 计划条目的 label 列表；`readPartition` → `EdlSession::readBack`；`writeRaw` → 用 `PlanEntry` 走 session 的**单条目**写入；错误经 `lastError()` 与 `outputMessage` 原样透出。
- 保持公开头文件的注释与新语义一致（**若头注释描述了"4 字节前缀"之类已删行为，一并改**）。

- [ ] **Step 4: 跑测试与回归**

Run: `cmake --build build && ./build/image_engine_tests_test_edl_libusb_transport && ctest --test-dir build --output-on-failure`
Expected: 新测试 PASS；**全量 ctest 不回归**（`test_pipeline` 里编入了 `edl_handler.cpp`，它必须仍能编译与链接）

- [ ] **Step 5: 提交**

```bash
git add src/core/edl/edl_libusb_transport.h src/core/edl/edl_libusb_transport.cpp src/core/modes/edl_handler.h src/core/modes/edl_handler.cpp tests/test_edl_libusb_transport.cpp CMakeLists.txt
git commit -m "feat(edl): libusb 传输实现 + EDLHandler 退化薄封装（顺带修 5 处既有缺陷）"
```

---

### Task 8: FlashTool 通道 + 计划预览对话框 + FlashPanel 入口

**Files:**
- Modify: `src/core/flash_tool.h` / `.cpp`（新增第 4 个通道 `oppo-edl`）
- Modify: `tests/test_pipeline.cpp`（`channelMapping` 加一条映射断言）
- Create: `src/ui/flash_plan_dialog.h` / `.cpp`
- Modify: `src/ui/flash_panel.cpp`（入口按钮）
- Test: `tests/test_flash_plan_dialog.cpp`（offscreen 平台）

**Interfaces:**
- Consumes: Task 3 `buildPlanFromDir`、Task 6 `EdlSession`、Task 7 `LibusbEdlTransport`/`EDLHandler`
- Produces:
  - `FlashTool::flashChannelForMode(DeviceDetector::MODE_EDL_9008)` → `QStringLiteral("oppo-edl")`
  - `flashFullPackage(..., params = {"planDir": <目录>, "programmerPath": <可选>}, ...)` —— `oppo-edl` 通道：`buildPlanFromDir` → 连接设备 → `EdlSession::run`
  - `class FlashPlanDialog : public QDialog` —— `explicit FlashPlanDialog(const edl::FlashPlan &plan, QWidget *parent = nullptr);`；`bool confirmed() const`（点过"开始刷写"才为 true）；信号 `void startRequested(const QString &planDir)`；静态 `bool buildAndShow(const QString &dir, QWidget *parent, QString *outDir, QString *error)` 供 FlashPanel 调用（内部 `buildPlanFromDir` + 显示对话框 + 用户确认后回填 `outDir`）
  - `flash_tool.h` 的 params 注释块（`:102-107`）**必须同步加一行**：`oppo-edl: planDir(解包产物目录) + programmerPath(可选，缺省在 planDir 内探测 prog_*firehose*.*)`

- [ ] **Step 1: 写失败测试**

`tests/test_flash_plan_dialog.cpp`（需要 Widgets：该测试目标额外链 `Qt6::Widgets`，并用 `QTEST_MAIN`；CMake 里给该测试设 `set_tests_properties(... PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")`）：

```cpp
#include <QtTest>
#include <QCheckBox>
#include <QPushButton>
#include "ui/flash_plan_dialog.h"

class TestFlashPlanDialog : public QObject
{
    Q_OBJECT
private slots:
    void startButtonGatedByCheckbox();
};

void TestFlashPlanDialog::startButtonGatedByCheckbox()
{
    edl::FlashPlan plan; plan.source = QStringLiteral("test"); plan.storageType = QStringLiteral("ufs");
    FlashPlanDialog dlg(plan);
    auto *start = dlg.findChild<QPushButton *>(QStringLiteral("startButton"));
    auto *ack = dlg.findChild<QCheckBox *>(QStringLiteral("ackCheck"));
    QVERIFY(start && ack);
    QVERIFY(!start->isEnabled());      // 未勾选 → 禁用
    ack->setChecked(true);
    QVERIFY(start->isEnabled());       // 勾选后可刷
    QVERIFY(!dlg.confirmed());
    dlg.accept();
    QVERIFY(dlg.confirmed());          // 仅"开始刷写"路径置位
}

QTEST_MAIN(TestFlashPlanDialog)
#include "test_flash_plan_dialog.moc"
```

`tests/test_pipeline.cpp` 的 `channelMapping` 加：
```cpp
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_EDL_9008),
             QStringLiteral("oppo-edl"));
```

- [ ] **Step 2: 跑测试确认失败**

注册 beiden 测试（`test_flash_plan_dialog` 需额外链 `phone_plugins`/Widgets 与 `src/core/edl/*.cpp`）。
Run: `cmake -B build -G Ninja && cmake --build build 2>&1 | tail -5`
Expected: 编译失败（`ui/flash_plan_dialog.h` 不存在）

- [ ] **Step 3: 实现**

- `flash_tool.cpp`：`flashChannelForMode` 加 `MODE_EDL_9008 → "oppo-edl"`；`flashFullPackage` 加第 4 分支（照既有三分支形状：多设备 VID 计数警告 + `params` 取值 + 进度/日志信号）。通道内：`buildPlanFromDir(planDir, plan, err)` → 失败直接返回；`programmerPath` 缺省时在 `planDir` 里探测 `prog_*firehose*.{elf,mbn,bin}`；`LibusbEdlTransport` + `EdlSession` 跑 `run(...)`，进度回调转发 `flashProgress`。
- `flash_plan_dialog.cpp`：`QTableView` 列 = 分区/LUN/起始扇区/扇区数/大小/文件/校验；顶部摘要（来源、存储类型、条目数、总字节）；warnings 列表；勾选框 objectName `ackCheck`（文案 `"我知晓此路径真机未验证"`）；按钮 objectName `startButton`（`"开始刷写"`），`ackCheck` 未勾选时禁用；`confirmed()` 仅在点 `startButton` 时置 true；整包入口（选 `.ofp`/`.ops`）用 `QTemporaryDir` + Phase A 的 `imgopp::extractOFP/extractOPS` 解包后走同一预览（解包进度用既有 `unpackProgress` 风格的模态进度条即可，具体控件自选）。
- `flash_panel.cpp`：加一个按钮 `"EDL 刷写计划…"`（放在既有 EDL 连接区附近），点击 → 选目录 → `FlashPlanDialog::buildAndShow` → 用户确认后调 `flashFullPackage(deviceId, MODE_EDL_9008, {{"planDir", dir}}, &err)`；失败文案落日志（照既有分支）。

- [ ] **Step 4: 跑测试**

Run: `cmake --build build && ./build/image_engine_tests_test_flash_plan_dialog && ./build/image_engine_tests_test_pipeline && ctest --test-dir build --output-on-failure`
Expected: 全绿；**GUI 冒烟无法自动完成（无 DISPLAY）→ 在报告里如实标注"面板渲染待用户手动验证"**

- [ ] **Step 5: 提交**

```bash
git add src/core/flash_tool.h src/core/flash_tool.cpp src/ui/flash_plan_dialog.h src/ui/flash_plan_dialog.cpp src/ui/flash_panel.cpp tests/test_flash_plan_dialog.cpp tests/test_pipeline.cpp CMakeLists.txt
git commit -m "feat(edl): FlashTool oppo-edl 通道 + 计划预览对话框 + 面板入口"
```

---

### Task 9: 文档收尾与验证

**Files:**
- Modify: `docs/superpowers/specs/oppo-format-notes.md`（更正 §53 的错误陈述）
- Modify: `功能清单.txt`（规划中段：Phase B 状态）
- Modify: `docs/superpowers/specs/oppo-flash-protocol-facts.md`（如有实现期修正，回填）

- [ ] **Step 1: 更正格式速查的错误**

`docs/superpowers/specs/oppo-format-notes.md:53` 现称"rawprogram/patch XML + 分区镜像在 Program/UFS_PROVISION 区域"。改为：
```
- 载荷: 仅 SAHARA 文件 + settings.xml 加密; Program/UFS_PROVISION 明文 —— 这也是镜像大文件能"半明文"存在的结构原因。
- **更正（Phase B 核查）**: 前句"rawprogram/patch XML 在 Program/UFS_PROVISION 区域"**不成立** —— `.ops` 解包产物只有镜像与 `settings.xml`，rawprogram/patch XML 需从 `<Program{N}>`/`<Patch{N}>` 块生成；`.ofp`-QC 的元数据分组里连 `Program` 都没有。证据见 `oppo-flash-protocol-facts.md` §5。
```

- [ ] **Step 2: 更新功能清单**

"规划中"里 Phase B 那行改为：
```
[√] OPPO/一加/realme EDL 刷写链 (Phase B 代码就绪: 计划层/协议层/会话/mock 验证; **真机验证待持机人**)
```
并在"--- 刷机 / 刷写 ---"段（若存在，先 grep 确认段名）加两行：
```
[√] EDL 刷写计划构建 (rawprogram/patch XML + OPS 元数据 + GPT 回填, 越界/重叠/扇区校验)
[√] Sahara/Firehose 协议栈 (可注入传输, 无真机 mock 验证; 真机待验)
```

- [ ] **Step 3: 提交**

```bash
git add docs/superpowers/specs/oppo-format-notes.md docs/superpowers/specs/oppo-flash-protocol-facts.md 功能清单.txt
git commit -m "docs: OPPO EDL 刷写链 Phase B 交付 — 格式速查更正 + 功能清单更新"
```

---

## 验证清单（全部完成后）

- [ ] `ctest --test-dir build --output-on-failure` 全绿（含 5 个新目标）
- [ ] 合成包 → `buildPlanFromDir` → mock 会话全流程测试通过（数据面逐字节相等断言）
- [ ] **真机验证：本阶段不做**（留给持机人）。交付说明必须保留：真机 USB 时序/ZLP/programmer 兼容性/真实 NAK/**刷完能否开机**均未验证；UI 有"我知晓真机未验证"勾选
- [ ] GUI 拖放/渲染未冒烟（无 DISPLAY）→ 待用户手动验证一次
- [ ] `git status` 无意外产物
