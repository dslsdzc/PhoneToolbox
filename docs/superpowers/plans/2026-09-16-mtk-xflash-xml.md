# MTK BROM Phase D2（XFlash）+ D3（XML）实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 MTK BROM 通道的**现代两代**补齐：XFlash（12B 帧 + 七步握手 + `INIT_EXT_RAM` EMI + `boot_to` 剥签名 + `WRITE_DATA` 逐分区写 + GPT 分区表 + `SHUTDOWN`）与 XML（文本协议 + `CMD:START` 握手 + `WRITE-FLASH`/`READ-FLASH`），并让三条链（LEGACY 已交付 / XFlash / XML）按设备代际**自动路由**，计划层/通道/UI 完全复用。

**Architecture:** 与 D1 同构的分层：纯函数（`mtk_gpt`、`mtk_preloader_emi` 扩展、`mtkplan` 扩展）→ 帧层（`mtk_xflash_session` / `mtk_xml_session`，跑在既有 `IBromUsb` 上）→ 载荷层（`mtk_xflash_payload` / `mtk_xml_payload`）→ 编排（`bromFlashOnSession` 三代路由，复用 D1 的计划层与逐分区写语义）→ 通道/UI（复用 D1，零新入口）。设备只经既有可注入 `IBromUsb` 触碰；GPT 用**读回调**注入，故可用文件字节直接驱动。

**Tech Stack:** C++17 / Qt6（Core + Widgets + Test）/ CMake + Ninja / 既有 `MockUsbChannel`（`tests/test_mtk_brom.cpp` 与 `tests/test_mtk_payload.cpp`）/ 真样本在 `reference/mtk-samples/`（**gitignored**）。

**Spec:** `docs/superpowers/specs/2026-09-16-mtk-xflash-xml-design.md`（已批准）
**事实依据:** `.superpowers/sdd/mtk-d2d3-facts-report.md`（736 行，逐条 `file:line`）+ D1 交付的解析层/计划层/通道（`main = 649873b`）
**样本:** `reference/mtk-samples/{PGPT.img,SGPT.img,sgdisk_print.txt,MT6789_Android_scatter.xml}`（来源与 sha256 见 spec §9/§12）+ D1 已有的 `MTK_DA_V5/V6`（在 `mtkclient/Loader/`）与 832 个 preloader

---

## Global Constraints

**跨代铁律（违反即真机错帧或静默读错数据；逐条出处见 spec §2）**
1. **三代握手是三套**：LEGACY 只有 `0xC0`（D1 已实现）；XFlash = `0xC0` → `SYNC` → `SETUP_ENVIRONMENT` → `SETUP_HW_INIT_PARAMS` → 读回 `SYNC`（`XFL:979-995`）；XML = `CMD:START` 文本消息（`XL:271-321`）。**不得把三者合并成一条路径**。
2. **端序按代分**：LEGACY 协议参数**大端**（D1 已实现）；**XFlash 与 XML 的 12B 帧头与所有参数全小端**（`XFL:112`、`XL:150`）。
3. **12B 帧头** = `pack("<III", 0xFEEEEEEF, datatype, length)`；`datatype`：`1`=协议流、`2`=DA 日志（`XFP:89-90`；**`XFP` = `mtkclient/Library/DA/xflash/xflash_param.py`**）。**头一次写、载荷第二次写**。
4. **`ack()` 的 0x6781 特例**：该 dacode **一次写 16 字节**（`pack("<IIII", MAGIC, DT_PROTOCOL_FLOW, 4, 0)`），其余芯片**两次写**（12B 头 + 4B 载荷）——**不存在"4 字节短帧"**（`XFL:85-100`）。
5. **`send_param` 分块 = 0x200**，每个参数一个独立帧，全部写完后**读一次 status**；`0xC0020053`/`0xC0020004` → 明确报错（上游 `sys.exit(1)`，我们返回 false + 中文）。
   ⚠️ **实施期更正（T2，上游实证）**：`0xC0040050`（EMI 版本不匹配）上游是 **"静默返回 False"**，**不是"容忍/当成功"** ——
   `xflash_lib.py:181-186` 只是**跳过错误打印与 `sys.exit`**，函数仍然 `return False`；而**显式 preloader** 的调用点
   `xflash_lib.py:1147-1149` 是 `if not self.send_emi(...): return False` → **EMI 送失败即整链中止**（自动搜索路径 `:1131-1136`
   才是"换个候选 preloader 继续"）。本仓照做：`checkStatus` 对 `0xC0040050` 返回 **false**（沿用 D1"更严但不改判据"的纪律，
   只是补一条可诊断的中文错误文案，上游此处不打印）。
6. **`status()` 的判据**：读 12B 头（magic 必须 `0xFEEEEEEF`）→ 读 `length` 字节 → `length==2` 取 `<H`、**为 0 才成功**；`length==4` 取 `<I`、**0 或 `0xFEEEEEEF` 都算成功**；其它长度取载荷首个 u32（`XFL:138-158`）。
7. **XFlash 的 EMI**：`INIT_EXT_RAM(0x01000A)` → status 0 → `sleep(10ms)` → `xsend(pack("<I", len(emi)))`（**长度单独一帧**）→ `send_param(emi)`。**无地址、无 emiver、无校验和、无回包链**（`XFL:251-270`）。
8. **EMI 切片两代不同**：XFlash = **整块**（真样本 912 B）；LEGACY = `MTK_BIN+0xC` 起（真样本 800 B）。**同一 preloader 两个函数**，互不替代。
9. **EMI 是否需要由 `GET_CONNECTION_AGENT(0x04000A)` 决定**：`b"preloader"` → **跳过**；`b"brom"` → 需要（缺 EMI **只告警不中止**）；其它值 → 失败（`XFL:1107-1151`）。
10. **DA2 提交三代不同**：LEGACY `boot_to` **保留尾部签名**（`m_len`，D1 已实现）；XFlash `BOOT_TO` **剥签名**（`m_len - m_sig_len`，`XFL:970-978`）；XML 走 `CMD:BOOT-TO`（`XC:90`）。
11. **GPT**：头部 `signature`（8B，偏移 0）、`revision`（0x08，必须 `0x10000`）、`header_size`（0x0C）、**`crc32`（0x10）**、`current_lba`（0x18）、`backup_lba`（0x20）、`first/last_usable_lba`（0x28/0x30）、`part_entry_start_lba`（0x48）、`num_part_entries`（0x50）、`part_entry_size`（0x54）；条目 = type GUID(16) + unique GUID(16) + first_lba(u64) + last_lba(u64) + flags(u64) + **name（UTF-16LE，`\x00\x00` 终止）**。**头部 CRC 必须"把 crc 字段清零后再算"**、条目表 CRC 直接整表算——**两者都校验，不符即 fail-closed**（上游从不校验）。空条目判据 = **type GUID 全 0**（UEFI 规范；上游用 unique GUID，**不复刻**）。
12. **LBA↔字节换算在调用方**（协议层只发字节地址）：`offsetBytes = firstLba * sectorSize`、`sizeBytes = (lastLba - firstLba + 1) * sectorSize`（`realtime.py:117-121`）。
13. **扇区大小**：eMMC=512、UFS=4096；**以 512→4096 探测兜底**（真样本 `PGPT.img` 就是 **4096**、LBA1@4096 才是 `EFI PART`）。上游 eMMC 恒 512 是缺陷，**不复刻**。
14. **XFlash 写数据流**（`XFL:852-899`）：`GET_PACKET_LENGTH` 拿 `write_packet_length`（**无回退，失败即报错**）→ `cmd_write_data(56B 参数)` → 每块：`dsize=min(packet,剩余)` → **补零到 512 的整数倍** → `checksum = sum(data) & 0xFFFF` → `send_param([<I 0>, <I checksum>, data])`
    → **循环结束后还要读一次 status**（为 0 才算写成功，`XFL:876-882`）→ 成功后发一次 `CC_OPTIONAL_DOWNLOAD_ACT`（`XFL:883`，返回值上游不检查）。
15. **56B 写参数** = `pack("<IIQQ", storage, parttype, addr, length)`（**24B**）+ `pack("<IIIIIIII", …)`（8×u32 = **32B**）→ **共 56B**。
    ⚠️ 计划期更正（T6 预核对）：本计划与事实报告原写"48B"是**算错**（把 8×u32 记成 24B）；上游 `NandExtension` 类有 9 个属性（`cellusage/addr_type/bin_type/region/operation_type/format_level/sys_slc_percent/usr_slc_percent/phy_max_size`），
    pack 里**跳过 `operation_type`** 只打包 8 个（`XFL:679-680`）。少 8 字节会让设备把后 8 字节读成垃圾。
16. **XML 不发 EMI**（`initialize_dram=YES` 交给 DA 自己）：代码事实是 XML 库**零 `emi` 引用**（`xml_cmd.py:100-128` 只有该参数、`XL:185` 传 True）——**D3 不实现 EMI 发送**。
17. **XML 信封**（`XC:18-27`，**单行、无空格**）：`<?xml version="1.0" encoding="utf-8"?><da><version>1.0</version><command>CMD:<NAME></command><arg>…</arg></da>`；`str` 载荷 `length = len+1` 且**带 NUL 结尾**（`XL:146-153`）。
18. **XML 应答**：`OK`；错误含 `ERR!`；带长度 `OK@0x<hexlen>\0`（⚠️ 实施期更正：上游 `ack()` = `xsend("OK\0")`，
    而 `xsend` 对 str 是 `length = len(data)+1` 且再追加一个 NUL → **载荷 4 字节 `OK\0\0`、头里 length=4**；
    `ack_value(n)` 同理 = `OK@0x<hex>\0\0`（**10 字节**）。**按上游逐字节发**（T8 实施者按原稿发了 3 字节并被钉在用例里，已按上游改）；**完成永远两步** `CMD:END` → `CMD:START`（`XL:195-209`）；写的数据流 = `CMD:DOWNLOAD-FILE` 回包里的 `packet_length`（`XL:419-424`），读 = `CMD:UPLOAD-FILE`（`XL:425-431`）。
    ⚠️ **两处上游缺陷不复刻**（计划期预核对补记）：① 上游 `send_command` 对 `"ERR!" in result` 是 `return result` —— 返回的是**非空字符串**，
    调用方 `if not res:` 会把它当**成功**（`xml_lib.py:218-219`）；我们**返回 false + 中文**（fail-closed）。② 上游 `DT_MESSAGE` 日志帧是**跳过并继续**的
    （`xread()` 内循环，`xml_lib.py:107-132`）—— 我们的 `getResponse` 同样跳过（有上限 `kMaxLogFramesToSkip`）而不是报错。
19. **代际判定 = 芯片表 `damode` + DA 文件 `v6`**（`v6` 强制 XML，`DC:216`）；**设备不上报 damode**；`plcap`/`blver` 那条"提升到 XFLASH"是**死代码**（`plcap` 全仓零调用），**不实现**。
20. **不做**：`DOWNLOAD`/`UPLOAD`/`FORMAT_PARTITION`（上游零调用）、`FORMAT`/格式化、SLA/DAA（明确报错）、UFS 专有命令族、RPMB、seccfg、DA 提取/签名绕过。

**工程约定（沿用 D1，违反会被审查打回）**
- 命名空间 `mtkbrom`（帧/载荷）与 `mtkgpt`（GPT 纯函数）；纯函数签名 `bool f(...) + QString *error`；错误文案中文且可诊断。
- `reference/` 全目录 **gitignored**：不进构建、不进提交；用例**只经编译期宏**取样本路径（`MTK_SAMPLES_DIR`），缺失时 `QSKIP`，**验证跑带 `-DMTK_SAMPLES_REQUIRED=ON` 且核对输出 `0 skipped`**。
- **提交只 `git add` 具体文件路径**（禁 `-A`/`-u`/`commit -a`）。
- 遍历 Qt 容器用 `std::as_const`（`<utility>`），不得 `qAsConst`。
- **QtTest 宏里不要内联含 `//`（URL）或括号的 raw string**（Qt 6.11.2 moc 会误词法化，报 `missing ')' in macro usage`）——JSON/XML 先落变量再进宏。
- **复核构建警告必须用全新构建目录**（`cmake -B /tmp/d2d3-fresh …`）；增量构建不重放警告。
- 新增用例**必须加进测试类的 `private slots:` 声明区**（漏声明 = 用例不跑而 ctest 照绿）。
- **派 subagent 时禁用任务工具**；进度写 `.superpowers/sdd/progress.md`。
- 上游 `mtkclient`（GPL-3.0）**只读参照**：只取事实与 `file:line`，代码文本不进仓库。

---

## 文件结构

```
新增
  src/core/modes/mtk_gpt.{h,cpp}               GPT 纯函数：探测/头部/条目/CRC/备份兜底（读回调注入）
  src/core/modes/mtk_xflash_session.{h,cpp}    12B 帧 + status + send_param + ack(0x6781 特例) + send_data + send_devctrl
  src/core/modes/mtk_xflash_payload.{h,cpp}    七步握手 / bring-up 四步 / EMI / boot_to / WRITE+READ_DATA / SHUTDOWN / 只读查询
  src/core/modes/mtk_xml_session.{h,cpp}       文本帧 + OK / OK@0x<len> / OK!EOT + get_command_result（DwnFile/UpFile/FileSysOp/END/START）
  src/core/modes/mtk_xml_payload.{h,cpp}       CMD:START 握手 / setup_env / setup_hw_init / SET-HOST-INFO / WRITE-FLASH / READ-FLASH / REBOOT
  tests/test_mtk_gpt.cpp
  tests/test_mtk_xflash_session.cpp
  tests/test_mtk_xflash_payload.cpp
  tests/test_mtk_xml_session.cpp
  tests/test_mtk_xml_payload.cpp
改
  src/core/modes/mtk_preloader_emi.{h,cpp}     +extractEmiXflash（整块切片；与 LEGACY 切片并存）
  src/core/mtk_flash_plan.{h,cpp}              +parseScatterXml（XML 方言）+ GPT→PartitionRef 适配
  src/core/modes/mtk_payload.{h,cpp}           bromFlashOnSession 三代路由（Legacy 既有 / XFlash / XML）
  src/core/flash_tool.{h,cpp}、src/ui/flash_panel.cpp  日志/文案说明走了哪条链（不新增入口）
  tests/test_mtk_preloader_emi.cpp             扩：XFlash 切片 + 832 两代对拍
  tests/test_mtk_flash_plan.cpp                扩：XML 方言 + GPT 适配
  tests/test_mtk_payload.cpp                   扩：三代路由与两条新链的失败边界
  CMakeLists.txt                               注册 5 个新测试目标 + 各目标依赖源
```

**任务依赖**：T1 独立；T2 独立；T3 独立；T4 依赖 T2；T5 依赖 T2+T4；T6 依赖 T2+T5；T7 依赖 T1；T8 独立；T9 依赖 T8；T10 依赖 T8+T9；T11 依赖 T1+T3+T4+T5+T6+T7；T12 依赖 T8+T9+T10+T11；T13 最后。**严格串行执行**（`mtk_payload`/`mtk_flash_plan`/CMakeLists 被多任务触碰）。

---


### Task 1: GPT 解析 `mtk_gpt`（纯函数 + 真 4096 样本 + 双 CRC fail-closed + 备份兜底）

**Files:**
- Create: `src/core/modes/mtk_gpt.{h,cpp}`
- Create: `tests/test_mtk_gpt.cpp`
- Modify: `CMakeLists.txt`（注册）

**Interfaces:**
- Consumes: 无（只依赖 Qt Core；设备数据经**读回调**注入）
- Produces:
  ```cpp
  namespace mtkgpt {
  struct Partition { QString name; quint64 firstLba, lastLba; QByteArray typeGuid, uniqueGuid; quint64 flags; };
  struct Table { quint32 sectorSize; QList<Partition> partitions; bool usedBackup; quint64 diskSectors; QByteArray diskGuid; };
  using ReadFn = std::function<bool(quint64 byteOffset, int length, QByteArray *out, QString *error)>;

  bool readTable(const ReadFn &read, quint64 diskSectors, Table &out,
                 QStringList *log = nullptr, QString *error = nullptr);
  bool parsePrimary(const QByteArray &raw, quint32 sectorSize, Table &out, QString *error = nullptr);
  // raw = **磁盘末尾若干扇区**的窗口；windowFirstLba = 该窗口第一扇区的 LBA（真样本 SGPT.img 的窗口 = 8 扇区）
  bool parseBackup(const QByteArray &raw, quint32 sectorSize, quint64 windowFirstLba, Table &out,
                   QString *error = nullptr);
  quint64 offsetBytes(const Partition &p, quint32 sectorSize);
  quint64 sizeBytes(const Partition &p, quint32 sectorSize);
  QByteArray testBuildSyntheticGpt(quint32 sectorSize, quint32 sectorCount);   // 用例辅助（见 Step 3 注）
  }
  ```

**真样本布局（本次实测钉死，写进代码注释）**
- `PGPT.img`（32768 B = 8 × 4096）：`EFI PART` @**4096**（LBA1）；`current_lba=1`、`backup_lba=124960767`、`part_entry_start_lba=2`、`n=61`、`esz=128`；**头部 CRC 与条目表 CRC 均 OK**；条目 @8192（=LBA2×4096）。
- `SGPT.img`（32768 B）：`EFI PART` @**28672**（窗口**最后一扇区**）；`current_lba=124960767`（= 磁盘末扇区）、`backup_lba=1`（指回主 GPT）、`part_entry_start_lba=124960760`（= 该窗口起始 LBA）→ **条目在窗口开头、头在末尾**（UEFI 备份布局；`entryOffsetInWindow = (entryLba - windowFirstLba) × sectorSize`）；头部 CRC OK。

- [ ] **Step 1: 写失败用例**（`tests/test_mtk_gpt.cpp`）

```cpp
#include <QtTest>
#include <QDir>
#include <QFile>

#include "core/modes/mtk_gpt.h"
#include "mtk_test_helpers.h"

using mtkgpt::Partition;
using mtkgpt::Table;

namespace {
// 文件偏移 → 读回调（拷进 shared_ptr 让 lambda 持有）
mtkgpt::ReadFn fileReader(const QString &path)
{
    auto shared = std::make_shared<QByteArray>();
    QFile f(path);
    if (f.open(QIODevice::ReadOnly))
        *shared = f.readAll();
    return [shared](quint64 off, int len, QByteArray *out, QString *err) {
        if (off + quint64(len) > quint64(shared->size())) {
            if (err) *err = QStringLiteral("越界读");
            return false;
        }
        *out = shared->mid(int(off), len);
        return true;
    };
}

bool anyName(const Table &t, const QString &n)
{
    for (const Partition &p : std::as_const(t.partitions))
        if (p.name == n) return true;
    return false;
}
} // namespace

class TestMtkGpt : public QObject
{
    Q_OBJECT
private slots:
    void parsesRealFourKSectorPrimaryGpt();
    void parsesRealBackupWindow();
    void fallsBackToBackupWhenPrimaryCrcBad();
    void rejectsGarbageSignatureAndBadRevision();
    void rejectsBadCrc();
    void parsesSyntheticGpt();

private:
    bool samplesReady(QString *why)
    {
        const QDir d(mtktest::samplesDir());
        if (!QFile::exists(d.filePath(QStringLiteral("PGPT.img")))
            || !QFile::exists(d.filePath(QStringLiteral("SGPT.img")))) {
            *why = QStringLiteral("真样本缺失（PGPT/SGPT.img）");
            return false;
        }
        return true;
    }
};

// —— 真样本：4096 字节扇区的主 GPT（LBA1@4096；512 处全零）——
void TestMtkGpt::parsesRealFourKSectorPrimaryGpt()
{
    QString why;
    if (!samplesReady(&why)) {
#if MTK_SAMPLES_REQUIRED
        QFAIL(qPrintable(why + QStringLiteral("（MTK_SAMPLES_REQUIRED=ON）")));
#else
        QSKIP(qPrintable(why));
#endif
    }
    const QString path = QDir(mtktest::samplesDir()).filePath(QStringLiteral("PGPT.img"));
    Table t;
    QStringList log;
    QString err;
    QVERIFY2(mtkgpt::readTable(fileReader(path), /*diskSectors=*/0, t, &log, &err), qPrintable(err));
    QCOMPARE(t.sectorSize, quint32(4096));
    QCOMPARE(t.partitions.size(), 61);
    QVERIFY(!t.usedBackup);
    QCOMPARE(t.partitions.at(0).name, QStringLiteral("misc"));
    QCOMPARE(t.partitions.at(0).firstLba, quint64(8));
    QCOMPARE(t.partitions.at(1).name, QStringLiteral("para"));
    QCOMPARE(t.partitions.at(2).name, QStringLiteral("expdb"));
    // ⚠️ 实施期更正（oracle 实证，sgdisk_print.txt:75）：PGPT.img 里**没有** preloader 分区
    //（preloader 在 scatter 的裸区，不在 GPT 表内）——末条是 flashinfo。用末条断言"名字解码正确 + 整表走完"。
    QVERIFY(anyName(t, QStringLiteral("flashinfo")));
    QCOMPARE(mtkgpt::offsetBytes(t.partitions.at(0), 4096), quint64(8 * 4096));
    QCOMPARE(mtkgpt::sizeBytes(t.partitions.at(0), 4096), quint64((135 - 8 + 1) * 4096));
    QVERIFY2(log.join('\n').contains(QStringLiteral("4096")), qPrintable(log.join('\n')));
}

// —— 真样本：备份窗口（头在窗口最后一扇区、条目在窗口开头）——
void TestMtkGpt::parsesRealBackupWindow()
{
    QString why;
    if (!samplesReady(&why)) {
#if MTK_SAMPLES_REQUIRED
        QFAIL(qPrintable(why + QStringLiteral("（MTK_SAMPLES_REQUIRED=ON）")));
#else
        QSKIP(qPrintable(why));
#endif
    }
    const QString path = QDir(mtktest::samplesDir()).filePath(QStringLiteral("SGPT.img"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    QCOMPARE(raw.indexOf(QByteArray("EFI PART", 8)), 7 * 4096);      // 头在窗口第 8（最后）扇区
    Table t;
    QString err;
    QVERIFY2(mtkgpt::parseBackup(raw, 4096, 124960760ull, t, &err), qPrintable(err));   // 窗口起始 LBA 来自样本头部字段
    QCOMPARE(t.partitions.size(), 61);
    QVERIFY(t.usedBackup);
    QCOMPARE(t.partitions.at(0).name, QStringLiteral("misc"));
    QCOMPARE(t.partitions.at(0).firstLba, quint64(8));
}

// —— 主 GPT 头部 CRC 坏 → readTable 走备份兜底 ——
void TestMtkGpt::fallsBackToBackupWhenPrimaryCrcBad()
{
    QString why;
    if (!samplesReady(&why)) {
#if MTK_SAMPLES_REQUIRED
        QFAIL(qPrintable(why + QStringLiteral("（MTK_SAMPLES_REQUIRED=ON）")));
#else
        QSKIP(qPrintable(why));
#endif
    }
    QFile fp(QDir(mtktest::samplesDir()).filePath(QStringLiteral("PGPT.img")));
    QFile fs(QDir(mtktest::samplesDir()).filePath(QStringLiteral("SGPT.img")));
    QVERIFY(fp.open(QIODevice::ReadOnly) && fs.open(QIODevice::ReadOnly));
    QByteArray head = fp.readAll().left(0x22 * 4096);       // 主 GPT 头 + 条目 + 余量
    const QByteArray tail = fs.readAll();                   // 备份窗口（8 扇区）
    head[4096 + 0x10] = char(quint8(head.at(4096 + 0x10)) ^ 0xFF);   // 改坏主头 CRC 字段

    const quint64 diskSectors = 124960768ull;               // 样本 backup_lba + 1
    const quint64 tailBytes = quint64(tail.size());
    const quint64 tailStart = diskSectors * 4096ull - tailBytes;     // = 124960760 × 4096
    mtkgpt::ReadFn reader = [head, tail, tailStart, tailBytes](quint64 off, int len, QByteArray *out, QString *) {
        if (off + quint64(len) <= quint64(head.size())) { *out = head.mid(int(off), len); return true; }
        if (off >= tailStart && off + quint64(len) <= tailStart + tailBytes) {
            *out = tail.mid(int(off - tailStart), len);
            return true;
        }
        return false;
    };
    Table t;
    QStringList log;
    QString err;
    QVERIFY2(mtkgpt::readTable(reader, diskSectors, t, &log, &err), qPrintable(err));
    QVERIFY2(t.usedBackup, "主头 CRC 坏必须走备份 GPT");
    QCOMPARE(t.partitions.size(), 61);
    QVERIFY2(log.join('\n').contains(QStringLiteral("备份")), qPrintable(log.join('\n')));
}

// —— 合成负向：垃圾签名 / revision 不符 ——
void TestMtkGpt::rejectsGarbageSignatureAndBadRevision()
{
    Table t;
    QString err;
    QByteArray raw(0x22 * 512, '\0');
    raw.replace(512, 4, "XXXX");
    QVERIFY(!mtkgpt::parsePrimary(raw, 512, t, &err));
    QVERIFY(!err.isEmpty());

    err.clear();
    QByteArray raw2(0x22 * 512, '\0');
    raw2.replace(512, 8, QByteArray("EFI PART", 8));
    raw2[512 + 0x0C] = 92;                    // header_size = 92（合法）
    raw2[512 + 0x08] = 1;                     // revision = 0x00000001（错）
    QVERIFY(!mtkgpt::parsePrimary(raw2, 512, t, &err));
    QVERIFY2(err.contains(QStringLiteral("revision")) || err.contains(QStringLiteral("版本")), qPrintable(err));
}

// —— 合成负向：头部 CRC 改坏 → 拒绝（fail-closed）——
void TestMtkGpt::rejectsBadCrc()
{
    const QByteArray good = mtkgpt::testBuildSyntheticGpt(512, 64);
    Table t;
    QString err;
    QVERIFY2(mtkgpt::parsePrimary(good, 512, t, &err), qPrintable(err));
    QByteArray bad = good;
    bad[512 + 0x10] = char(quint8(bad.at(512 + 0x10)) ^ 0x01);
    err.clear();
    QVERIFY(!mtkgpt::parsePrimary(bad, 512, t, &err));
    QVERIFY2(err.contains(QStringLiteral("CRC")), qPrintable(err));
}

// —— 合成正向：1 个分区的合法 GPT（正确双 CRC）——
void TestMtkGpt::parsesSyntheticGpt()
{
    const QByteArray raw = mtkgpt::testBuildSyntheticGpt(512, 64);
    Table t;
    QString err;
    QVERIFY2(mtkgpt::parsePrimary(raw, 512, t, &err), qPrintable(err));
    QCOMPARE(t.partitions.size(), 1);
    QCOMPARE(t.partitions.at(0).name, QStringLiteral("boot"));
    QCOMPARE(t.partitions.at(0).firstLba, quint64(34));
    QCOMPARE(mtkgpt::sizeBytes(t.partitions.at(0), 512), quint64(2 * 512));
}
QTEST_APPLESS_MAIN(TestMtkGpt)
#include "test_mtk_gpt.moc"
```

- [ ] **Step 2: 跑一遍，确认失败**（目标还不存在 → 先按 Step 5 注册，再跑；预期编译失败）

- [ ] **Step 3: 实现**

`src/core/modes/mtk_gpt.h`：

```cpp
#pragma once

// MTK GPT 解析（Phase D2/D3）—— **纯函数**，只依赖 Qt Core；设备数据经 ReadFn 注入。
//
// 与上游（mtkclient `Library/Partitions/gpt.py`、`Library/partition.py`、`Library/realtime.py`）的**有意差异**：
//   ① **头部 CRC 与条目表 CRC 都校验**（上游 `gpt.py:48` 解析但从不比较）→ 不符即 fail-closed
//   ② **备份 GPT 兜底真的生效**（上游 `partition.py:45` 的 seek 被 `gpt.py:161` 的绝对 seek 覆盖）
//   ③ **扇区大小可探测**（512→4096）——上游 eMMC 恒 512（`XFL:448` 读出的 `emmc.block_size` 从未赋给它）
//   ④ **空条目判据 = type GUID 全 0**（UEFI 规范；上游 `gpt.py:192-193` 用 unique GUID）
//   ⑤ 条目区起点只认头部的 `part_entry_start_lba × sectorSize`（上游命令行覆盖项被当**字节偏移**用）
//
// 真样本布局（实测，`reference/mtk-samples/`）：
//   PGPT.img = 8×4096 字节，"EFI PART"@4096（LBA1），条目@8192（LBA2），双 CRC OK
//   SGPT.img = 8×4096 字节，"EFI PART"@28672（**窗口最后一扇区**），part_entry_start_lba=124960760
//             （= 窗口起始 LBA）→ **条目在窗口开头、头在末尾**（UEFI 备份布局）

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <functional>

namespace mtkgpt {

struct Partition {
    QString name;
    quint64 firstLba = 0;
    quint64 lastLba = 0;      // 含端点
    QByteArray typeGuid;      // 16 字节原始
    QByteArray uniqueGuid;
    quint64 flags = 0;
};

struct Table {
    quint32 sectorSize = 512;
    QList<Partition> partitions;
    bool usedBackup = false;
    quint64 diskSectors = 0;
    QByteArray diskGuid;      // 16 字节
};

using ReadFn = std::function<bool(quint64 byteOffset, int length, QByteArray *out, QString *error)>;

bool readTable(const ReadFn &read, quint64 diskSectors, Table &out,
               QStringList *log = nullptr, QString *error = nullptr);
bool parsePrimary(const QByteArray &raw, quint32 sectorSize, Table &out, QString *error = nullptr);
bool parseBackup(const QByteArray &raw, quint32 sectorSize, quint64 windowFirstLba, Table &out,
                 QString *error = nullptr);

quint64 offsetBytes(const Partition &p, quint32 sectorSize);
quint64 sizeBytes(const Partition &p, quint32 sectorSize);

// **用例辅助**（生产 .cpp 里，理由见计划 Step 3 注）：造一份含单分区 "boot"（LBA 34..35）的合法 GPT
QByteArray testBuildSyntheticGpt(quint32 sectorSize, quint32 sectorCount);

} // namespace mtkgpt
```

`src/core/modes/mtk_gpt.cpp`：

```cpp
#include "core/modes/mtk_gpt.h"

namespace mtkgpt {
namespace {

// 备份窗口**上限**（不是固定值）：真样本 SGPT.img 的窗口 = 8 扇区（= 条目起点 → 磁盘末尾）。
// ⚠️ 实施期更正：上游/文档常说"末尾 34 扇区"，但那是**最大**布局；写死 34 会在真样本上取错起点
//（窗口第一扇区 LBA 必须正好等于头里的 part_entry_start_lba）。正确做法见 readTable 的备份段。
constexpr quint64 kBackupWindowSectors = 34;

quint32 rdU32(const QByteArray &b, int off)
{
    return quint32(quint8(b.at(off))) | (quint32(quint8(b.at(off + 1))) << 8)
         | (quint32(quint8(b.at(off + 2))) << 16) | (quint32(quint8(b.at(off + 3))) << 24);
}

void wrU32(QByteArray &b, int off, quint32 v)
{
    b[off] = char(v & 0xFF); b[off + 1] = char((v >> 8) & 0xFF);
    b[off + 2] = char((v >> 16) & 0xFF); b[off + 3] = char((v >> 24) & 0xFF);
}

void wrU64(QByteArray &b, int off, quint64 v)
{
    wrU32(b, off, quint32(v & 0xFFFFFFFFu));
    wrU32(b, off + 4, quint32(v >> 32));
}

quint64 rdU64(const QByteArray &b, int off)
{
    return quint64(rdU32(b, off)) | (quint64(rdU32(b, off + 4)) << 32);
}

// CRC-32/ISO-HDLC（与 zlib.crc32 同参数；Qt 的 qChecksum 是 CRC-16，不能用）
quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xFFFFFFFFu;
    for (const char ch : data) {
        crc ^= quint8(ch);
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xEDB88320u & (quint32(0) - (crc & 1u)));
    }
    return ~crc;
}

bool guidAllZero(const QByteArray &g) { return g.size() == 16 && g == QByteArray(16, '\0'); }

QString nameOf(const QByteArray &entry)
{
    const QByteArray raw = entry.mid(56, 72);                 // UTF-16LE，\x00\x00 终止（上游 ustring(72)）
    int end = 0;
    while (end + 1 < raw.size() && !(raw.at(end) == '\0' && raw.at(end + 1) == '\0'))
        end += 2;
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(raw.constData()), end / 2);
}

// headerPos = 头在 raw 里的偏移；entriesBase = **条目区在 raw 里的偏移**
bool parseAt(const QByteArray &raw, quint32 sectorSize, int headerPos, int entriesBase,
             quint64 diskSectors, bool isBackup, Table &out, QString *error)
{
    if (sectorSize != 512 && sectorSize != 4096) {
        if (error) *error = QStringLiteral("扇区大小不支持（%1）").arg(sectorSize);
        return false;
    }
    if (headerPos < 0 || headerPos + 0x5C > raw.size()) {
        if (error) *error = QStringLiteral("GPT 头读取不足（raw %1 字节，头偏移 %2）").arg(raw.size()).arg(headerPos);
        return false;
    }
    const QByteArray hdr = raw.mid(headerPos, 0x5C);
    if (hdr.left(8) != QByteArray("EFI PART", 8)) {
        if (error) *error = QStringLiteral("GPT 签名不符（读到 %1）").arg(QString::fromLatin1(hdr.left(8).toHex()));
        return false;
    }
    const quint32 revision = rdU32(hdr, 0x08);
    if (revision != 0x00010000u) {
        if (error) *error = QStringLiteral("GPT revision 不符（0x%1，期望 0x10000）")
                                .arg(revision, 8, 16, QLatin1Char('0'));
        return false;
    }
    const quint32 headerSize = rdU32(hdr, 0x0C);
    if (headerSize < 0x5C || headerSize > sectorSize) {
        if (error) *error = QStringLiteral("GPT header_size 异常（%1）").arg(headerSize);
        return false;
    }
    // 头部 CRC：**把 crc32 字段清零后再算**（UEFI 规定；上游从不校验）
    QByteArray hdrForCrc = hdr.left(int(headerSize));
    hdrForCrc.replace(0x10, 4, QByteArray(4, '\0'));
    const quint32 crcStored = rdU32(hdr, 0x10);
    const quint32 crcCalc = crc32(hdrForCrc);
    if (crcStored != crcCalc) {
        if (error) *error = QStringLiteral("GPT 头部 CRC 不符（存 0x%1，算 0x%2）")
                                .arg(crcStored, 8, 16, QLatin1Char('0')).arg(crcCalc, 8, 16, QLatin1Char('0'));
        return false;
    }

    const quint32 entryCount = rdU32(hdr, 0x50);
    const quint32 entrySize = rdU32(hdr, 0x54);
    if (entrySize < 128 || entrySize > 4096 || entryCount == 0 || entryCount > 4096) {
        if (error) *error = QStringLiteral("GPT 条目参数异常（count=%1 size=%2）").arg(entryCount).arg(entrySize);
        return false;
    }
    const quint64 entryBytes = quint64(entryCount) * entrySize;
    if (entriesBase < 0 || quint64(entriesBase) + entryBytes > quint64(raw.size())) {
        if (error) *error = QStringLiteral("GPT 条目表读取不足（需要到 %1，raw 只有 %2）")
                                .arg(quint64(entriesBase) + entryBytes).arg(raw.size());
        return false;
    }
    const QByteArray entries = raw.mid(entriesBase, int(entryBytes));
    const quint32 entriesCrcStored = rdU32(hdr, 0x58);
    const quint32 entriesCrcCalc = crc32(entries);
    if (entriesCrcStored != entriesCrcCalc) {
        if (error) *error = QStringLiteral("GPT 条目表 CRC 不符（存 0x%1，算 0x%2）")
                                .arg(entriesCrcStored, 8, 16, QLatin1Char('0')).arg(entriesCrcCalc, 8, 16, QLatin1Char('0'));
        return false;
    }

    Table t;
    t.sectorSize = sectorSize;
    t.diskSectors = diskSectors;
    t.usedBackup = isBackup;
    t.diskGuid = hdr.mid(0x38, 16);
    for (quint32 i = 0; i < entryCount; ++i) {
        const QByteArray e = entries.mid(int(i) * int(entrySize), int(entrySize));
        if (guidAllZero(e.left(16)))
            continue;                                   // UEFI：type GUID 全 0 = 未使用（上游用 unique GUID，不复刻）
        Partition p;
        p.typeGuid = e.left(16);
        p.uniqueGuid = e.mid(16, 16);
        p.firstLba = rdU64(e, 32);
        p.lastLba = rdU64(e, 40);
        p.flags = rdU64(e, 48);
        p.name = nameOf(e);
        if (p.lastLba < p.firstLba)
            continue;                                   // 端点反了的条目跳过（不静默产出负长度）
        t.partitions.append(p);
    }
    out = t;
    return true;
}

} // namespace

quint64 offsetBytes(const Partition &p, quint32 sectorSize) { return p.firstLba * quint64(sectorSize); }

quint64 sizeBytes(const Partition &p, quint32 sectorSize)
{
    return p.lastLba >= p.firstLba ? (p.lastLba - p.firstLba + 1) * quint64(sectorSize) : 0;
}

bool parsePrimary(const QByteArray &raw, quint32 sectorSize, Table &out, QString *error)
{
    if (raw.size() < int(2 * sectorSize) + 0x5C) {
        if (error) *error = QStringLiteral("主 GPT 读取不足（%1 字节）").arg(raw.size());
        return false;
    }
    if (raw.mid(int(sectorSize), 8) != QByteArray("EFI PART", 8)) {
        if (error) *error = QStringLiteral("主 GPT 签名不符（LBA1 读到 %1）")
                                .arg(QString::fromLatin1(raw.mid(int(sectorSize), 8).toHex()));
        return false;
    }
    const quint64 entryLba = rdU64(raw.mid(int(sectorSize), 0x5C), 0x48);
    return parseAt(raw, sectorSize, int(sectorSize), int(entryLba * sectorSize), 0, false, out, error);
}

bool parseBackup(const QByteArray &raw, quint32 sectorSize, quint64 windowFirstLba, Table &out, QString *error)
{
    if (raw.size() < int(2 * sectorSize) + 0x5C || raw.size() % int(sectorSize) != 0) {
        if (error) *error = QStringLiteral("备份 GPT 窗口大小异常（%1 字节）").arg(raw.size());
        return false;
    }
    const int headerPos = raw.size() - int(sectorSize);          // 头在窗口**最后一扇区**（真样本实测）
    if (raw.mid(headerPos, 8) != QByteArray("EFI PART", 8)) {
        if (error) *error = QStringLiteral("备份 GPT 签名不符（窗口末扇区读到 %1）")
                                .arg(QString::fromLatin1(raw.mid(headerPos, 8).toHex()));
        return false;
    }
    const quint64 entryLba = rdU64(raw.mid(headerPos, 0x5C), 0x48);
    if (entryLba < windowFirstLba) {
        if (error) *error = QStringLiteral("备份 GPT 条目起始 LBA（%1）在窗口（%2 起）之前")
                                .arg(entryLba).arg(windowFirstLba);
        return false;
    }
    const int entriesBase = int((entryLba - windowFirstLba) * quint64(sectorSize));
    const quint64 windowSectors = quint64(raw.size()) / quint64(sectorSize);
    return parseAt(raw, sectorSize, headerPos, entriesBase, windowFirstLba + windowSectors, true, out, error);
}

bool readTable(const ReadFn &read, quint64 diskSectors, Table &out, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    auto fail = [error](const QString &m) { if (error) *error = m; return false; };

    if (!read)
        return fail(QStringLiteral("GPT 读取失败：读回调为空"));

    // 1) 主 GPT：扇区大小按 512 → 4096 探测（顺序同上游 gpt.py:219，真样本就是 4096）
    //    ⚠️ 实施期更正：**不能**一上来就 read(0, 0x22*4096) —— 文件型读回调（PGPT.img 只有 32768 B =
    //    8×4096）会因越界直接失败，两条真样本用例全挂。先读"能覆盖两种布局的 LBA1 头"的少量字节，
    //    再按头部字段（条目起始 LBA + 条数 + 条目大小）算准需要多少字节、按扇区上取整补读一次。
    QByteArray head;
    if (!read(0, kProbeLen, &head, error) || head.size() < 0x5C + 512)
        return fail(QStringLiteral("GPT 头部读取失败（读回调返回不足）"));
    quint32 sectorSize = 0;
    for (quint32 ss : {512u, 4096u}) {
        if (head.size() >= int(ss) + 8 && head.mid(int(ss), 8) == QByteArray("EFI PART", 8)) {
            sectorSize = ss;
            break;
        }
    }
    if (sectorSize == 0)
        return fail(QStringLiteral("未找到 GPT 签名（LBA1 的 512 与 4096 偏移都不是 EFI PART）"));
    say(QStringLiteral("GPT：扇区大小探测为 %1 字节").arg(sectorSize));

    // parsePrimary 的契约 = "头 + 整张条目表在同一个缓冲里"：按头字段算准范围后补读一次
    //（上限 kPrimaryAreaMax = 0x22 扇区；头字段不可用/超上限则不补读，交给 parsePrimary 报精确原因）
    quint64 needBytes = 0;
    if (primaryExtent(head, sectorSize, &needBytes) && needBytes > quint64(head.size())
        && needBytes <= kPrimaryAreaMax) {
        QByteArray bigger;
        if (read(0, int((needBytes + sectorSize - 1) / sectorSize * sectorSize), &bigger, nullptr)
            && bigger.size() > head.size())
            head = bigger;
    }
    quint32 sectorSize = 0;
    for (quint32 ss : {512u, 4096u}) {
        if (head.size() >= int(2 * ss) + 8 && head.mid(int(ss), 8) == QByteArray("EFI PART", 8)) {
            sectorSize = ss;
            break;
        }
    }
    if (sectorSize == 0)
        return fail(QStringLiteral("未找到 GPT 签名（LBA1 的 512 与 4096 偏移都不是 EFI PART）"));
    say(QStringLiteral("GPT：扇区大小探测为 %1 字节").arg(sectorSize));

    QString primaryErr;
    if (parsePrimary(head, sectorSize, out, &primaryErr)) {
        out.diskSectors = diskSectors;
        return true;
    }
    say(QStringLiteral("主 GPT 不可用（%1）").arg(primaryErr));

    // 2) 备份兜底（上游这段实际不生效：partition.py:45 的 seek 被 gpt.py:161 的绝对 seek 覆盖）
    //    ⚠️ 实施期更正：窗口起点 = 备份头里的 part_entry_start_lba（读磁盘**末扇区**得到），
    //    终点 = 磁盘末尾；写死"末尾 34 扇区"在真样本（8 扇区窗口）上会把请求整体落到窗口之外。
    if (diskSectors < kBackupWindowSectors)
        return fail(QStringLiteral("主 GPT 不可用且磁盘扇区数未知/过小（%1）—— 无法读备份 GPT").arg(diskSectors));
    const quint64 headerLba = diskSectors - 1;
    QByteArray backHdr;
    if (!read(headerLba * sectorSize, int(sectorSize), &backHdr, nullptr) || backHdr.size() < 0x5C
        || backHdr.left(8) != QByteArray("EFI PART", 8))
        return fail(QStringLiteral("主 GPT 与备份 GPT 都不可用：主（%1）；备（末扇区 LBA %2 无 EFI PART）")
                        .arg(primaryErr).arg(headerLba));
    const quint64 windowFirstLba = rdU64(backHdr, 0x48);       // 备份条目区起点 = 窗口起始 LBA
    if (windowFirstLba >= diskSectors || diskSectors - windowFirstLba > kBackupWindowSectors)
        return fail(QStringLiteral("备份 GPT 条目区起点异常（LBA %1，磁盘 %2 扇区）").arg(windowFirstLba).arg(diskSectors));
    const quint64 windowSectors = diskSectors - windowFirstLba;
    QByteArray tail;
    if (!read(windowFirstLba * sectorSize, int(windowSectors * sectorSize), &tail, error))
        return fail(QStringLiteral("备份 GPT 读取失败（LBA %1 起 %2 扇区）").arg(windowFirstLba).arg(windowSectors));
    QString backupErr;
    if (!parseBackup(tail, sectorSize, windowFirstLba, out, &backupErr))
        return fail(QStringLiteral("主 GPT 与备份 GPT 都不可用：主（%1）；备（%2）").arg(primaryErr, backupErr));
    out.diskSectors = diskSectors;
    say(QStringLiteral("已使用**备份** GPT（窗口 LBA %1 起 %2 扇区）").arg(windowFirstLba).arg(windowSectors));
    return true;
}

QByteArray testBuildSyntheticGpt(quint32 sectorSize, quint32 sectorCount)
{
    // 单分区 "boot"（LBA 34..35）：头部合法 + 双 CRC 正确（供正/负向用例）
    const quint32 entryCount = 4;
    const quint32 entrySize = 128;
    QByteArray raw(int(sectorCount * sectorSize), '\0');
    raw[0x1FE] = char(0x55); raw[0x1FF] = char(0xAA);             // 保护性 MBR
    const int hdrPos = int(sectorSize);
    raw.replace(hdrPos, 8, QByteArray("EFI PART", 8));
    wrU32(raw, hdrPos + 0x08, 0x00010000u);                       // revision
    wrU32(raw, hdrPos + 0x0C, 92);                                // header_size
    wrU64(raw, hdrPos + 0x18, 1);                                 // current_lba
    wrU64(raw, hdrPos + 0x20, sectorCount - 1);                   // backup_lba
    wrU64(raw, hdrPos + 0x28, 34);                                // first_usable
    wrU64(raw, hdrPos + 0x30, sectorCount - 34);                  // last_usable
    wrU64(raw, hdrPos + 0x48, 2);                                 // part_entry_start_lba
    wrU32(raw, hdrPos + 0x50, entryCount);
    wrU32(raw, hdrPos + 0x54, entrySize);
    const int entriesPos = int(2 * sectorSize);
    QByteArray entries(int(entryCount * entrySize), '\0');
    entries[0] = char(0xEB); entries[1] = char(0xA0); entries[2] = char(0xD0);   // 非 0 type GUID
    wrU64(entries, 32, 34);
    wrU64(entries, 40, 35);
    const QString nm = QStringLiteral("boot");
    for (int i = 0; i < nm.size(); ++i) {
        entries[56 + i * 2] = char(nm.at(i).unicode() & 0xFF);
        entries[56 + i * 2 + 1] = char(nm.at(i).unicode() >> 8);
    }
    raw.replace(entriesPos, entries.size(), entries);
    wrU32(raw, hdrPos + 0x58, crc32(entries));                    // 条目表 CRC
    QByteArray hdrForCrc = raw.mid(hdrPos, 92);
    hdrForCrc.replace(0x10, 4, QByteArray(4, '\0'));
    wrU32(raw, hdrPos + 0x10, crc32(hdrForCrc));                  // 头部 CRC
    return raw;
}

} // namespace mtkgpt
```

> **实现者注意**：`testBuildSyntheticGpt` 放生产 .cpp 是**刻意的** —— 用例要造"双 CRC 正确的合法 GPT"，而 CRC 实现只此一份；在测试里复抄一遍 CRC 会失去"同一实现产/消"的判别力。它已在头文件里标注为用例辅助。

- [ ] **Step 4: 跑测试，确认通过**

```bash
cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_mtk_gpt
./build/image_engine_tests_test_mtk_gpt      # 预期 6 passed / 0 skipped
```

- [ ] **Step 5: 注册测试（CMakeLists.txt）**

① `IMAGE_TEST_SOURCES` 追加 `tests/test_mtk_gpt.cpp`；
② `foreach` 内追加：

```cmake
                elseif(_test_stem STREQUAL "test_mtk_gpt")
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_gpt.cpp)
```

③ 把 `test_mtk_gpt` 加进既有 `MTK_SAMPLES_DIR` 宏的 stem 名单（与 `test_mtk_da_file`/`test_mtk_preloader_emi`/`test_mtk_flash_plan` 并列）。

- [ ] **Step 6: 提交**

```bash
git add src/core/modes/mtk_gpt.h src/core/modes/mtk_gpt.cpp tests/test_mtk_gpt.cpp CMakeLists.txt
git commit -m "feat(mtk): GPT 解析纯函数（真 4096 扇区样本 + 双 CRC fail-closed + 备份兜底 + 扇区探测）"
```


---

### Task 2: XFlash 帧层 `mtk_xflash_session`（12B 帧 / status / send_param / 0x6781 特例）

**Files:**
- Create: `src/core/modes/mtk_xflash_session.{h,cpp}`
- Create: `tests/test_mtk_xflash_session.cpp`
- Modify: `CMakeLists.txt`（注册）

**Interfaces:**
- Consumes: `IBromUsb`（D1 交付；`readExact` 已实现精确读）
- Produces:
  ```cpp
  namespace mtkbrom {
  // XFlash 命令常量（XFP；devctrl 子命令另见 Task 4）
  enum XCmd : quint32 {
      X_CMD_FORMAT = 0x010003, X_CMD_WRITE_DATA = 0x010004, X_CMD_READ_DATA = 0x010005,
      X_CMD_SHUTDOWN = 0x010007, X_CMD_BOOT_TO = 0x010008, X_CMD_DEVICE_CTRL = 0x010009,
      X_CMD_INIT_EXT_RAM = 0x01000A, X_CMD_SETUP_ENV = 0x010100, X_CMD_SETUP_HW_INIT = 0x010101,
  };
  constexpr quint32 kXMagic = 0xFEEEEEEF;         // XFP:2（xflash_param.py 的 MAGIC）
  constexpr quint32 kXSync = 0x434E5953;          // "SYNC"（小端帧里即 ASCII SYNC）
  constexpr quint32 kXDataProtocolFlow = 1;       // DT_PROTOCOL_FLOW（XFP:89）
  constexpr quint32 kXDataMessage = 2;            // DT_MESSAGE（XFP:89-90）
  constexpr quint32 kXEmitVersionMismatch = 0xC0040050;   // EMI 版本不匹配：上游**静默 return False**（XFL:181-186）
                                                          // —— 不是"容忍"；显式 preloader 路径据此整链中止（XFL:1147-1149）

  class XFlashSession {
  public:
      XFlashSession(IBromUsb *usb, quint16 dacode)
          : m_usb(usb), m_dacode(dacode) {}

      bool xsend(const QByteArray &payload, QString *error = nullptr);            // 帧头+载荷两次写
      bool xsendInt(quint32 v, QString *error = nullptr);                         // pack("<I", v)
      bool xsendInt64(quint64 v, QString *error = nullptr);                       // pack("<Q", v)
      bool xread(QByteArray &payload, quint32 *datatype = nullptr, QString *error = nullptr);
      // 见铁律 6：length==2 → <H 为 0 才成功；length==4 → <I 为 0 或 0xFEEEEEEF 都成功；其它 → 载荷首 u32
      bool readStatus(quint32 &code, QString *error = nullptr);
      bool checkStatus(QString *error = nullptr);                                 // readStatus + 错误文案
      bool ack(QString *error = nullptr);                                         // 0x6781 一次 16B；其余两次
      // ⚠️ 实施期更正（T2）：**帧头一次 + 载荷分块**（上游 XFL:163-177 / :273-281）——不是"整帧 xsend 再分块"（会写两遍载荷）
      bool sendParam(const QList<QByteArray> &params, QString *error = nullptr);  // 0x200 分块 + 一次 status
      bool sendData(const QByteArray &data, QString *error = nullptr);            // 帧头 + wMaxPacketSize 分块 + 一次 status
      bool sendDevCtrl(quint32 subcmd, const QByteArray &param, QByteArray *reply, QString *error = nullptr);
      // 有回包的查询：sendDevCtrl(无参) + **尾部 status**（上游调用方自己读；唯一例外 GET_PARTITION_TBL_CATA 不调本函数）
      bool devCtrlQuery(quint32 subcmd, QByteArray *reply, QString *error = nullptr);
  private:
      // **只写 12B 帧头**（sendParam/sendData 用；无生产消费者 → T2 审查后定为 private，YAGNI）
      bool xsendHeader(quint32 length, QString *error = nullptr);
      IBromUsb *m_usb;
      quint16 m_dacode;
  };
  }
  ```

- [ ] **Step 1: 写失败用例**（`tests/test_mtk_xflash_session.cpp`）

```cpp
#include <QtTest>
#include <QByteArray>

#include "core/modes/mtk_xflash_session.h"
#include "core/modes/mtk_brom.h"

class MockUsbChannel : public mtkbrom::IBromUsb
{
public:
    QByteArray writes;
    QList<QByteArray> writeFrames;
    QList<QByteArray> reads;
    int pktSize = 0x400;
    bool open(QString *) override { return true; }
    bool write(const QByteArray &data, QString *) override { writes += data; writeFrames << data; return true; }
    bool read(QByteArray &out, int maxLen, int, QString *) override
    {
        if (reads.isEmpty()) { out.clear(); return false; }
        const QByteArray r = reads.takeFirst();
        out = r.left(maxLen);
        return !r.isEmpty();
    }
    int maxPacketSize() const override { return pktSize; }
    bool close() override { return true; }
};

namespace {
QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
    return b;
}
// ⚠️ **一帧 = 两笔队列项**：默认 `IBromUsb::readExact` 是"单次读 + 严格长度"（`mtk_brom.cpp:219-231`），
// 本层先读 12B 帧头、再读载荷 —— **每次 readExact 消耗一笔队列项**。把整帧塞成一笔会让"读头"吃掉载荷、
// 后续读全部错位（D1 的 LEGACY 通道是逐字节 read()，不受此约束）。负向用例同理：要"短读失败"就少给字节。
QList<QByteArray> frameReads(quint32 dt, const QByteArray &payload)
{
    return {le32(0xFEEEEEEF) + le32(dt) + le32(quint32(payload.size())), payload};
}
QList<QByteArray> statusReads(quint32 code)   // length==4 → <I
{
    return frameReads(1, le32(code));
}
} // namespace

class TestMtkXflashSession : public QObject
{
    Q_OBJECT
private slots:
    void xsendWritesHeaderThenPayload();
    void ackUsesSixteenByteWriteFor6781Only();
    void statusParsesByLength();
    void statusTreatsMagicAsSuccess();
    void sendParamChunksAt0x200AndFailsOnEmiVersionMismatch();
    void sendParamRejectsHardErrorCodes();
    void sendDataChunksByMaxPacketSize();
};

// 帧头一次写、载荷第二次写（铁律 3）
void TestMtkXflashSession::xsendWritesHeaderThenPayload()
{
    MockUsbChannel m;
    mtkbrom::XFlashSession s(&m, 0x6765);
    QString err;
    QVERIFY2(s.xsend(QByteArray("\x01\x02\x03\x04", 4), &err), qPrintable(err));
    QCOMPARE(m.writeFrames.size(), 2);
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));
    QCOMPARE(m.writeFrames.at(1), QByteArray("\x01\x02\x03\x04", 4));
}

// ack：0x6781 一次 16 字节；其他芯片 12B 头 + 4B 载荷（铁律 4）
void TestMtkXflashSession::ackUsesSixteenByteWriteFor6781Only()
{
    {
        MockUsbChannel m;
        mtkbrom::XFlashSession s(&m, 0x6781);
        QString err;
        QVERIFY2(s.ack(&err), qPrintable(err));
        QCOMPARE(m.writeFrames.size(), 1);
        QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4) + le32(0));
        QCOMPARE(m.writeFrames.at(0).size(), 16);
    }
    {
        MockUsbChannel m;
        mtkbrom::XFlashSession s(&m, 0x6765);
        QString err;
        QVERIFY2(s.ack(&err), qPrintable(err));
        QCOMPARE(m.writeFrames.size(), 2);
        QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));
        QCOMPARE(m.writeFrames.at(1), le32(0));
    }
}

// status 按 length 分支（铁律 6）：length==2 用 <H，非 0 即错误码
void TestMtkXflashSession::statusParsesByLength()
{
    {
        MockUsbChannel m;
        m.reads << frameReads(1, QByteArray("\x00\x00", 2));
        mtkbrom::XFlashSession s(&m, 0x6765);
        quint32 code = 0xDEAD;
        QString err;
        QVERIFY2(s.readStatus(code, &err), qPrintable(err));
        QCOMPARE(code, quint32(0));
    }
    {
        MockUsbChannel m;
        m.reads << frameReads(1, QByteArray("\x50\x00", 2));     // 0x0050（BE 读作 0x5000? 注意 <H 是小端 → 0x0050）
        mtkbrom::XFlashSession s(&m, 0x6765);
        quint32 code = 0;
        QVERIFY(s.readStatus(code, nullptr));
        QCOMPARE(code, quint32(0x0050));                    // 非 0 = 错误码（由 checkStatus 查表）
    }
}

// length==4 且值 == magic 视为成功（铁律 6）
void TestMtkXflashSession::statusTreatsMagicAsSuccess()
{
    MockUsbChannel m;
    m.reads << statusReads(0xFEEEEEEF);
    mtkbrom::XFlashSession s(&m, 0x6765);
    quint32 code = 1;
    QString err;
    QVERIFY2(s.readStatus(code, &err), qPrintable(err));
    QCOMPARE(code, quint32(0));
}

// send_param：0x200 分块 + 最后读一次 status；0xC0040050 **失败但不报硬错**（上游 XFL:181-186）
void TestMtkXflashSession::sendParamChunksAt0x200AndFailsOnEmiVersionMismatch()
{
    {
        MockUsbChannel m;
        m.reads << statusReads(mtkbrom::kXEmitVersionMismatch);
        mtkbrom::XFlashSession s(&m, 0x6765);
        const QByteArray big(0x300, '\x5A');                // 0x200 + 0x100 两块
        QString err;
        QVERIFY(!s.sendParam({big}, &err));                 // **失败**（上游 return False）
        QVERIFY2(err.contains(QStringLiteral("0xC0040050")), qPrintable(err));   // 但文案说清是"版本不匹配"
        QCOMPARE(m.writeFrames.size(), 3);                  // 分块照发：帧头 + 块1 + 块2
        QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(0x300));
        QCOMPARE(m.writeFrames.at(1).size(), 0x200);
        QCOMPARE(m.writeFrames.at(2).size(), 0x100);
    }
    {
        MockUsbChannel m;
        m.reads << statusReads(0);
        mtkbrom::XFlashSession s(&m, 0x6765);
        QString err;
        QVERIFY2(s.sendParam({le32(0), le32(0x1234)}, &err), qPrintable(err));   // 两个参数 = 两个帧
        QCOMPARE(m.writeFrames.size(), 4);                  // 帧1头 + 帧1载荷 + 帧2头 + 帧2载荷
    }
}

// send_param：硬错误码（0xC0020053 anti-rollback / 0xC0020004 DL forbidden）→ 明确失败
void TestMtkXflashSession::sendParamRejectsHardErrorCodes()
{
    MockUsbChannel m;
    m.reads << statusReads(0xC0020053);
    mtkbrom::XFlashSession s(&m, 0x6765);
    QString err;
    QVERIFY(!s.sendParam({le32(0)}, &err));
    QVERIFY(!err.isEmpty());
}

// send_data：帧头 + 按 maxPacketSize 分块 + 读一次 status
void TestMtkXflashSession::sendDataChunksByMaxPacketSize()
{
    MockUsbChannel m;
    m.pktSize = 0x100;
    m.reads << statusReads(0);
    mtkbrom::XFlashSession s(&m, 0x6765);
    const QByteArray data(0x250, '\x7E');
    QString err;
    QVERIFY2(s.sendData(data, &err), qPrintable(err));
    QCOMPARE(m.writeFrames.size(), 4);                      // 帧头 + 0x100 + 0x100 + 0x50
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(0x250));
    QCOMPARE(m.writeFrames.at(1).size(), 0x100);
    QCOMPARE(m.writeFrames.at(3).size(), 0x50);
}
QTEST_APPLESS_MAIN(TestMtkXflashSession)
#include "test_mtk_xflash_session.moc"
```

- [ ] **Step 2: 跑一遍，确认失败**（先按 Step 5 注册）

- [ ] **Step 3: 实现**

`src/core/modes/mtk_xflash_session.h`：按上面的 Interfaces 段照抄（含常量与类定义；`#pragma once` + 注释里写明铁律 3/4/5/6 与出处行号）。

`src/core/modes/mtk_xflash_session.cpp`：

```cpp
#include "core/modes/mtk_xflash_session.h"

namespace mtkbrom {
namespace {
constexpr int kParamChunk = 0x200;      // XFL:172
constexpr int kStatusTimeoutMs = 3000;

QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
    return b;
}

QByteArray le64(quint64 v) { return le32(quint32(v & 0xFFFFFFFFu)) + le32(quint32(v >> 32)); }

quint32 leToU32(const QByteArray &b, int off)
{
    return quint32(quint8(b.at(off))) | (quint32(quint8(b.at(off + 1))) << 8)
         | (quint32(quint8(b.at(off + 2))) << 16) | (quint32(quint8(b.at(off + 3))) << 24);
}
} // namespace

// 只写帧头（sendParam/sendData 用；它们自己分块写载荷，避免"整帧 + 分块"把载荷写两遍）
bool XFlashSession::xsendHeader(quint32 length, QString *error)
{
    if (!m_usb) {
        if (error) *error = QStringLiteral("XFlash：USB 通道未设置");
        return false;
    }
    return m_usb->write(le32(kXMagic) + le32(kXDataProtocolFlow) + le32(length), error);
}

bool XFlashSession::xsend(const QByteArray &payload, QString *error)
{
    if (!xsendHeader(quint32(payload.size()), error))            // 头一次写
        return false;
    if (!payload.isEmpty() && !m_usb->write(payload, error))     // 载荷第二次写
        return false;
    return true;
}

bool XFlashSession::xsendInt(quint32 v, QString *error) { return xsend(le32(v), error); }
bool XFlashSession::xsendInt64(quint64 v, QString *error) { return xsend(le64(v), error); }

bool XFlashSession::xread(QByteArray &payload, quint32 *datatype, QString *error)
{
    QByteArray header;
    if (!m_usb->readExact(header, 12, kStatusTimeoutMs, error))
        return false;
    if (leToU32(header, 0) != kXMagic) {
        if (error) *error = QStringLiteral("XFlash：应答帧 magic 不符（读到 0x%1）")
                                .arg(leToU32(header, 0), 8, 16, QLatin1Char('0'));
        return false;
    }
    const quint32 dt = leToU32(header, 4);
    const quint32 len = leToU32(header, 8);
    if (len > 1u << 20) {                        // 防御：单帧不超过 1 MB
        if (error) *error = QStringLiteral("XFlash：应答帧长度异常（%1）").arg(len);
        return false;
    }
    payload.clear();
    if (len > 0 && !m_usb->readExact(payload, int(len), kStatusTimeoutMs, error))
        return false;
    if (datatype) *datatype = dt;
    return true;
}

// 铁律 6：见头文件注释
bool XFlashSession::readStatus(quint32 &code, QString *error)
{
    QByteArray payload;
    if (!xread(payload, nullptr, error))
        return false;
    if (payload.size() == 2) {
        const quint16 v = quint16(quint8(payload.at(0))) | (quint16(quint8(payload.at(1))) << 8);
        code = v;                                // 为 0 才算成功（由调用方/checkStatus 判定）
        return true;
    }
    if (payload.size() == 4) {
        const quint32 v = leToU32(payload, 0);
        code = (v == kXMagic) ? 0u : v;          // == magic 视为成功
        return true;
    }
    if (payload.size() < 4) {
        if (error) *error = QStringLiteral("XFlash：状态帧长度异常（%1）").arg(payload.size());
        return false;
    }
    code = leToU32(payload, 0);                  // 其它长度：取载荷首个 u32
    return true;
}

bool XFlashSession::checkStatus(QString *error)
{
    quint32 code = 0;
    if (!readStatus(code, error))
        return false;
    if (code == 0)
        return true;
    if (code == kXEmitVersionMismatch) {         // EMI 版本不匹配：上游静默 False（XFL:181-186）——**失败**，只是不打错误
        if (error) *error = QStringLiteral("XFlash：EMI 版本不匹配（0xC0040050）—— 上游在此静默返回失败（XFL:181-186）");
        return false;
    }
    if (error)
        *error = QStringLiteral("XFlash：设备返回错误码 0x%1").arg(code, 8, 16, QLatin1Char('0'));
    return false;
}

bool XFlashSession::ack(QString *error)
{
    if (m_dacode == 0x6781) {                    // 一次 16 字节（头+载荷合并，XFL:88）
        const QByteArray merged = le32(kXMagic) + le32(kXDataProtocolFlow) + le32(4) + le32(0);
        return m_usb->write(merged, error);
    }
    return xsendInt(0, error);                   // 其余芯片：两次写（XFL:90-94）
}

bool XFlashSession::sendParam(const QList<QByteArray> &params, QString *error)
{
    // ⚠️ 实施期更正（T2，上游 `xflash_lib.py:163-177`）：**帧头写一次、载荷按 0x200 分块**（不是"整帧 xsend"再分块
    // ——那会把载荷写两遍）。每块都是一次独立 write（xsend 的"头/载荷两次写"在这里退化为"头一次 + N 次分块"）。
    for (const QByteArray &p : params) {
        if (!xsendHeader(quint32(p.size()), error))
            return false;
        for (int off = 0; off < p.size(); off += kParamChunk) {
            const QByteArray chunk = p.mid(off, kParamChunk);
            if (!m_usb->write(chunk, error))
                return false;
        }
    }
    return checkStatus(error);                   // 全部写完读一次 status（XFL:178）
}

bool XFlashSession::sendData(const QByteArray &data, QString *error)
{
    // 同 sendParam（上游 `xflash_lib.py:273-281`）：头一次 + 载荷按 maxPacketSize 分块；**不整写一遍**
    if (!xsendHeader(quint32(data.size()), error))
        return false;
    const int chunk = m_usb->maxPacketSize() > 0 ? m_usb->maxPacketSize() : 0x400;
    for (int off = 0; off < data.size(); off += chunk) {
        if (!m_usb->write(data.mid(off, chunk), error))
            return false;
    }
    return checkStatus(error);
}

bool XFlashSession::sendDevCtrl(quint32 subcmd, const QByteArray &param, QByteArray *reply, QString *error)
{
    if (!xsendInt(X_CMD_DEVICE_CTRL, error))     // 0x010009
        return false;
    if (!checkStatus(error))
        return false;
    if (!xsendInt(subcmd, error))                // 子命令
        return false;
    if (!checkStatus(error))
        return false;
    if (param.isEmpty()) {
        QByteArray payload;
        if (!xread(payload, nullptr, error))     // 无参 → 读回包
            return false;
        if (reply) *reply = payload;
        return true;
    }
    return sendParam({param}, error);
}

// ⚠️ **尾部 status 是逐查询不同的（不得统一化）** —— T4 实施期实测（上游逐函数读过）：
//   · 有回包的查询（GET_CHIP_ID `XFL:396-418`、GET_PACKET_LENGTH `:623-635`、GET_EXPIRE_DATE `:571-578`、
//     GET_CONNECTION_AGENT `:330-338`）：`send_devctrl` 返回回包后，**调用方再读一次 status**（读总数为 4）。
//   · **唯一例外**：GET_PARTITION_TBL_CATA（`XFL:612-621`）**不读尾部 status**（连 status 校验都没有）。
//   我们在 `sendDevCtrl` 里保持上游 `send_devctrl` 的原样（不替调用方读尾巴），查询类走 `devCtrlQuery`。
bool XFlashSession::devCtrlQuery(quint32 subcmd, QByteArray *reply, QString *error)
{
    if (!sendDevCtrl(subcmd, QByteArray(), reply, error))
        return false;
    return checkStatus(error);                   // 上游调用方的尾部 status（唯一例外见上：分区表类别查询不调本函数）
}

} // namespace mtkbrom
```

> ✅ **已裁决（T2 实施期，见本任务 brief 尾部的控制方裁决）**：上游 `xflash_lib.py:163-177`（`send_param`）与 `:273-281`（`send_data`）
> 都是 **"帧头写一次 + 载荷按 0x200 / wMaxPacketSize 循环写"**，**没有**"整段载荷写一次"这回事 —— 本计划原稿"整帧 `xsend` + 再分块"
> 会把载荷**写两遍**（T2 实施者实证抓出）。现状：`xsendHeader(len)`（private）+ 分块写载荷；`xsend` 仍保留"头 + 整段载荷"供
> 16B 参数帧之类的整帧场景使用。测试以**每笔 write 的次数与大小**为判据（拼接字节流不作为判据，否则"总字节相同"骗得过）。

- [ ] **Step 4: 跑测试，确认通过**

```bash
cmake -B build -G Ninja && cmake --build build --target image_engine_tests_test_mtk_xflash_session
./build/image_engine_tests_test_mtk_xflash_session     # 预期 7 passed / 0 skipped
```

- [ ] **Step 5: 注册测试（CMakeLists.txt）**

```cmake
                elseif(_test_stem STREQUAL "test_mtk_xflash_session")
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_xflash_session.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_brom.cpp)
```
（+ `IMAGE_TEST_SOURCES` 追加 `tests/test_mtk_xflash_session.cpp`；+ 该目标进 libusb 链接名单）

- [ ] **Step 6: 提交**

```bash
git add src/core/modes/mtk_xflash_session.h src/core/modes/mtk_xflash_session.cpp tests/test_mtk_xflash_session.cpp CMakeLists.txt
git commit -m "feat(mtk): XFlash 帧层（12B 帧/status 判定/send_param 0x200/0x6781 一次 16 字节 ack）"
```

---

### Task 3: EMI 提取扩 XFlash 切片（整块 912B）+ 832 样本两代对拍

**Files:**
- Modify: `src/core/modes/mtk_preloader_emi.{h,cpp}`
- Modify: `tests/test_mtk_preloader_emi.cpp`
- Modify: `CMakeLists.txt`（该目标已注册；若新增依赖源则同步）

**Interfaces:**
- Consumes: D1 的 `extractEmiLegacy` 与 `EmiData`
- Produces:
  ```cpp
  namespace mtkbrom {
  // XFlash 代：EMI = **整个 dramsize 窗口**（上游 `daconfig.py:137-139` 的 `idx==0 且 damode==XFLASH` 分支
  // `return ver, data`）。与 LEGACY（`data[MTK_BIN+0xC:]`）在**同一份 preloader 上取不同切片**：
  // 真样本 preloader.bin 实测 XFlash 912 B / LEGACY 800 B。
  bool extractEmiXflash(const QByteArray &preloader, EmiData &out, QString *error = nullptr);
  }
  ```

- [ ] **Step 1: 写失败用例**（追加到 `tests/test_mtk_preloader_emi.cpp`）

```cpp
// XFlash：整块切片（真样本 912 B），与 LEGACY 的 800 B 在同一 preloader 上并存
void TestMtkPreloaderEmi::xflashSliceIsWholeWindow()
{
    const QString path = mtktest::samplesDir() + QStringLiteral("/preloader.bin");
    if (!QFile::exists(path)) {
#if MTK_SAMPLES_REQUIRED
        QFAIL("真样本缺失：preloader.bin（MTK_SAMPLES_REQUIRED=ON）");
#else
        QSKIP("真样本缺失（reference/mtk-samples/，gitignored）");
#endif
    }
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray pre = f.readAll();

    mtkbrom::EmiData legacy, xflash;
    QString err;
    QVERIFY2(mtkbrom::extractEmiLegacy(pre, legacy, &err), qPrintable(err));
    err.clear();
    QVERIFY2(mtkbrom::extractEmiXflash(pre, xflash, &err), qPrintable(err));

    QCOMPARE(legacy.ver, quint32(35));
    QCOMPARE(xflash.ver, quint32(35));
    QCOMPARE(legacy.bytes.size(), 800);       // MTK_BIN+0xC 起
    QCOMPARE(xflash.bytes.size(), 912);       // **整块**（dramsize 窗口）
    QCOMPARE(legacy.bytes.size() + 112, xflash.bytes.size());
    QVERIFY(xflash.bytes.mid(112) == legacy.bytes);      // LEGACY 切片 = XFlash 切片去掉前 112 字节
}

// 合成：两种切片在同一夹具上的关系（不依赖真样本）
void TestMtkPreloaderEmi::xflashAndLegacySlicesFromSyntheticFixture()
{
    const QByteArray pre = buildPreloader(64);            // 既有夹具（含标记 + MTK_BIN + 尾部 EMI）
    mtkbrom::EmiData legacy, xflash;
    QString err;
    QVERIFY2(mtkbrom::extractEmiLegacy(pre, legacy, &err), qPrintable(err));
    err.clear();
    QVERIFY2(mtkbrom::extractEmiXflash(pre, xflash, &err), qPrintable(err));
    // ⚠️ 实施期更正（T3 审查 M3）：原稿只断言 endsWith —— 那在"xflash = window、legacy = window.mid(bin+0xC)"下
    //    恒真（等于只断言"两者不同"）。改成**判别性**判据：XFlash 切片从 MTK_BIN+0xC 起必须与 LEGACY 逐字节相同，
    //    且 XFlash 长度必须等于窗口长度（即"整块"）。
    const int bin = pre.indexOf(QByteArray("MTK_BIN"));
    QVERIFY(bin != -1);
    QVERIFY(xflash.bytes.size() > legacy.bytes.size());
    QCOMPARE(xflash.bytes.mid(bin + 0xC), legacy.bytes);
    QCOMPARE(xflash.bytes.size(), pre.size());        // 整块 = 整个窗口（合成夹具无 MMM → 窗口 = 整个输入）
}
```

- [ ] **Step 2: 跑一遍，确认失败**（`extractEmiXflash` 未声明 → 编译失败）

- [ ] **Step 3: 实现**

`mtk_preloader_emi.h` 追加声明（含上面注释里的两代切片对照与真样本实测值）；`mtk_preloader_emi.cpp` 把 D1 的公共前缀抽成文件内静态函数，两个公开函数各自取切片：

```cpp
namespace {
// 公共前缀（D1 已实现，逐句对照 daconfig.py:120-145）：
//   MMM 分支（mlen/siglen/dramsize 三字段**小端**）→ 搜 MTK_BLOADER_INFO_v → 解析版本（去尾 NUL 后须全数字）
// 返回：data 已裁剪到 dramsize 窗口；markerIdx 标记偏移；version 版本
bool emiCommon(const QByteArray &preloader, QByteArray &window, QString &version, QString *error);
} // namespace

bool extractEmiLegacy(const QByteArray &preloader, EmiData &out, QString *error)
{
    QByteArray window;
    QString ver;
    if (!emiCommon(preloader, window, ver, error))
        return false;
    const int bin = window.indexOf(QByteArray("MTK_BIN"));
    if (bin == -1) {
        if (error) *error = QStringLiteral("未找到 MTK_BIN（LEGACY 切片起点未知）");
        return false;
    }
    out.bytes = window.mid(bin + 0xC);            // **LEGACY 切片**（真样本 800 B）
    if (out.bytes.isEmpty()) {
        if (error) *error = QStringLiteral("LEGACY 切片为空（MTK_BIN+0xC 越过末尾）");
        return false;
    }
    out.ver = version.toUInt();
    out.branch = QStringLiteral("LEGACY");
    return true;
}

bool extractEmiXflash(const QByteArray &preloader, EmiData &out, QString *error)
{
    QByteArray window;
    QString ver;
    if (!emiCommon(preloader, window, ver, error))
        return false;
    out.bytes = window;                          // **整块窗口**（真样本 912 B）
    if (out.bytes.isEmpty()) {
        if (error) *error = QStringLiteral("XFlash EMI 切片为空");
        return false;
    }
    out.ver = version.toUInt();
    out.branch = QStringLiteral("XFLASH");
    return true;
}
```

> **实现者注意**：D1 的 `extractEmiLegacy` **已经是这个前缀逻辑**（含版本非数字即失败的收紧）——请**只做抽取重构**（把公共段提出来），**不得改变 D1 的任何判据与文案**；重构后 D1 的既有 10 条用例必须**原样全绿**（它们就是这次重构的回归网）。

- [ ] **Step 4: 跑测试，确认通过**

```bash
cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build --target image_engine_tests_test_mtk_preloader_emi
./build/image_engine_tests_test_mtk_preloader_emi     # 预期 13 passed / 0 skipped
# ⚠️ 实施期更正（T3 审查）：D1 的测试类只有 **8** 个 slot（不是 10）→ 8 + 新 3 = 11 个用例，加 init/cleanup = 13。
#    原稿"12 passed"是错的，**不得当验收门**；判据是"0 skipped 且失败数为 0"。
```

- [ ] **Step 5: 832 样本两代对拍（一次性验证脚本，不进仓库）**

```bash
# 用 Python 把两代语义各移植一遍（D1 已做过 LEGACY），跑遍 mtkclient 自带 832 个 preloader，与本实现比对 ok/ver/len
python3 /tmp/emi_xflash_crosscheck.py      # 脚本写在 /tmp；把统计结果贴进报告（预期 XFlash 切片 = LEGACY 切片 + 112 B 的关系在 829 个非 MMM 样本上成立）
```

- [ ] **Step 6: 提交**

```bash
git add src/core/modes/mtk_preloader_emi.h src/core/modes/mtk_preloader_emi.cpp tests/test_mtk_preloader_emi.cpp
git commit -m "feat(mtk): EMI 提取扩 XFlash 整块切片（与 LEGACY 800B 并存；832 样本两代对拍）"
```


---

### Task 4: XFlash 引导 `mtk_xflash_payload` ①（七步握手 + bring-up 四步 + 只读查询）

**Files:**
- Create: `src/core/modes/mtk_xflash_payload.{h,cpp}`
- Create: `tests/test_mtk_xflash_payload.cpp`
- Modify: `CMakeLists.txt`（注册）

**Interfaces:**
- Consumes: `XFlashSession`（T2）、`BromSession`（D1：`getHwCode`/`getHwSwVer`/`getBromVer`/`getBlVer`/`sendDa`/`jumpDa`）、`DaFile`/`DaSelection`（D1）
- Produces:
  ```cpp
  namespace mtkbrom {
  // devctrl 子命令（XFP:50 起）
  enum XDevCtrl : quint32 {
      X_CTRL_GET_CHIP_ID         = 0x04000D,   // 5×u16 = (hw_code, hw_sub_code, hw_version, sw_version, chip_evolution)
      X_CTRL_GET_PACKET_LENGTH   = 0x040007,   // <II = (write_packet_length, read_packet_length)
      X_CTRL_GET_CONNECTION_AGENT= 0x04000A,   // 字符串（b"brom" / b"preloader"）
      X_CTRL_GET_PARTITION_CATA  = 0x040009,   // <I：0x64=GPT / 0x65=PMT（上游名 GET_PARTITION_TBL_CATA，XFP:55）
      X_CTRL_GET_RAM_INFO        = 0x04000C,   // 24B(32 位) / 48B(64 位)：3 元组 ×2 = (sram, dram)
      X_CTRL_SET_CHECKSUM_LEVEL  = 0x020003,   // <I（PLAIN=0/CRC32=1/MD5=2）；mtkclient 恒设 0（XFL:1106 / XFP:62）
      // ⚠️ 计划期更正（控制方对照 `xflash_param.py:57-66` 整表核过，2026-09-16）：
      //   · SET_RESET_KEY 是 **0x020004**，原稿写的 0x020005 其实是 **SET_HOST_INFO**（会发错命令！）
      //   · GET_EXPIRE_DATE 是 **0x040011**，原稿写的 0x040002 其实是 **GET_NAND_INFO**（同上）
      X_CTRL_SET_RESET_KEY       = 0x020004,   // <I（上游传 0x68，XFL:1104 / XFP:63）
      X_CTRL_GET_EXPIRE_DATE     = 0x040011,   // 无参数；回包见 get_expire_date() 本体（XFL:571-579 / XFP:57）
      // ⚠️ 上游这个值是 **0x800005**（比同段 0x0800xx 少一个 0，是上游自己的写法）—— **照发**，不要"修正"成 0x080005
      X_CTRL_CC_OPTIONAL_DOWNLOAD_ACT = 0x800005,   // 写完一个分区后的可选下载动作（XFL:883 / XFP:50 段）
  };
  struct XPacketLength { quint32 writeLength = 0; quint32 readLength = 0; };
  struct XChipId { quint16 hwCode = 0, hwSubCode = 0, hwVersion = 0, swVersion = 0, chipEvolution = 0; };
  enum class PartitionCata { Gpt, Pmt, Unknown };

  // ① **七步握手**（XFL:979-995）：读 1B == 0xC0 → sync(0x434E5953) → SETUP_ENV(20B) → SETUP_HW_INIT(4B)
  //    → 读回 == "SYNC"。**注意**：上游对两处 setup 的返回值不检查（失败也继续）——我们**检查**并在失败时明确报错
  //    （更严；理由：静默继续会让后续帧全错位）
  bool xflashDa1Handshake(XFlashSession &x, QStringList *log, QString *error = nullptr);
  // ② bring-up 四步（XFL:1103-1107 顺序）：get_expire_date → set_reset_key(0x68) → set_checksum_level(0x0) → get_connection_agent
  bool xflashBringUpSteps(XFlashSession &x, QByteArray *connectionAgent, QStringList *log, QString *error = nullptr);
  // ③ 只读查询
  bool xflashGetChipId(XFlashSession &x, XChipId &out, QString *error = nullptr);
  bool xflashGetPacketLength(XFlashSession &x, XPacketLength &out, QString *error = nullptr);
  bool xflashGetPartitionCata(XFlashSession &x, PartitionCata &out, QString *error = nullptr);
  bool xflashGetRamInfo(XFlashSession &x, QByteArray *raw, QString *error = nullptr);   // 原始 24/48B（解析留给 D4/诊断）
  }
  ```

- [ ] **Step 1: 写失败用例**（`tests/test_mtk_xflash_payload.cpp`）

```cpp
#include <QtTest>
#include <QByteArray>

#include "core/modes/mtk_xflash_payload.h"
#include "core/modes/mtk_xflash_session.h"
#include "core/modes/mtk_brom.h"

// mock 与 T2 同款（**从 T2 的测试文件复制一份到本文件**；两个测试文件各自独立，符合既有惯例）
class MockUsbChannel : public mtkbrom::IBromUsb
{
public:
    QByteArray writes;
    QList<QByteArray> writeFrames;
    QList<QByteArray> reads;
    int pktSize = 0x400;
    bool open(QString *) override { return true; }
    bool write(const QByteArray &data, QString *) override { writes += data; writeFrames << data; return true; }
    bool read(QByteArray &out, int maxLen, int, QString *) override
    {
        if (reads.isEmpty()) { out.clear(); return false; }
        const QByteArray r = reads.takeFirst();
        out = r.left(maxLen);
        return !r.isEmpty();
    }
    int maxPacketSize() const override { return pktSize; }
    bool close() override { return true; }
};

namespace {
QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
    return b;
}
QByteArray le16(quint16 v) { QByteArray b(2, '\0'); b[0] = char(v & 0xFF); b[1] = char(v >> 8); return b; }
// ⚠️ **一帧 = 两笔队列项**：默认 `IBromUsb::readExact` 是"单次读 + 严格长度"（`mtk_brom.cpp:219-231`），
// 本层先读 12B 帧头、再读载荷 —— **每次 readExact 消耗一笔队列项**。把整帧塞成一笔会让"读头"吃掉载荷、
// 后续读全部错位（D1 的 LEGACY 通道是逐字节 read()，不受此约束）。负向用例同理：要"短读失败"就少给字节。
QList<QByteArray> frameReads(quint32 dt, const QByteArray &payload)
{
    return {le32(0xFEEEEEEF) + le32(dt) + le32(quint32(payload.size())), payload};
}
QList<QByteArray> statusReads(quint32 code) { return frameReads(1, le32(code)); }
} // namespace

class TestMtkXflashPayload : public QObject
{
    Q_OBJECT
private slots:
    void handshakeOrderAndBytes();
    void handshakeRejectsNonSync();
    void bringUpStepsOrder();
    void getChipIdParsesFiveShorts();
    void getPacketLengthParsesTwoU32();
    void getPartitionCataMapsGptAndPmt();
};

// 七步握手：0xC0 → SYNC 帧 → SETUP_ENV（20B）→ SETUP_HW_INIT（4B）→ 读回 "SYNC"
void TestMtkXflashPayload::handshakeOrderAndBytes()
{
    MockUsbChannel m;
    // 回包顺序：SYNC 的 status（0）→ SETUP_ENV 的 status（0）→ SETUP_HW_INIT 的 status（0）→ 最终 SYNC 帧
    // ⚠️ 计划期更正（T4）：SYNC **只发不读**（XFL:903-907）→ 握手读 = SETUP_ENV status + SETUP_HW_INIT status + SYNC 回帧
    m.reads << statusReads(0) << statusReads(0) << frameReads(1, le32(0x434E5953));
    mtkbrom::XFlashSession x(&m, 0x6765);
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::xflashDa1Handshake(x, &log, &err), qPrintable(err));

    // 写帧顺序：SYNC 值 → SETUP_ENV 帧头 → SETUP_ENV 载荷(20B) → SETUP_HW_INIT 帧头 → SETUP_HW_INIT 载荷(4B)
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));   // SYNC 帧头
    QCOMPARE(m.writeFrames.at(1), le32(0x434E5953));                       // SYNC 值
    QCOMPARE(m.writeFrames.at(2), le32(0xFEEEEEEF) + le32(1) + le32(20));  // SETUP_ENV 帧头
    QCOMPARE(m.writeFrames.at(3).size(), 20);                              // 载荷 = 20 字节
    QCOMPARE(m.writeFrames.at(5), le32(0xFEEEEEEF) + le32(1) + le32(4));   // SETUP_HW_INIT 帧头
    QCOMPARE(m.writeFrames.at(6), le32(0));                                // 参数 0
    // SETUP_ENV 载荷字段（小端）：uartloglevel, log_channel, OS_LINUX=1, 0, 0
    const QByteArray env = m.writeFrames.at(3);
    QCOMPARE(env.mid(8, 4), le32(1));                                      // system_os = OS_LINUX
    QCOMPARE(env.mid(12, 8), le32(0) + le32(0));                           // ufs_provision=0、保留 0
}

// 最终读回不是 SYNC → 明确失败（铁律 1）
void TestMtkXflashPayload::handshakeRejectsNonSync()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0) << frameReads(1, le32(0xDEADBEEF));
    mtkbrom::XFlashSession x(&m, 0x6765);
    QString err;
    QVERIFY(!mtkbrom::xflashDa1Handshake(x, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("SYNC")), qPrintable(err));
}

// bring-up 四步顺序（XFL:1103-1107）：expire_date → reset_key(0x68) → checksum_level(0) → connection_agent
void TestMtkXflashPayload::bringUpStepsOrder()
{
    MockUsbChannel m;
    // 四步各自 devctrl：每步 = 0x010009 的 status + 子命令的 status + 回包/参数结果
    // ⚠️ 计划期更正（T4）：**有回包的查询在回包后还要读一次尾部 status**（上游 `if res != b"": status = self.status()`，
    //    XFL:571-578 / :330-338）→ expire = 3×status + 回包；SET_* = 3×status（devctrl 2 + 参数 1）；connagent = 3×status + 回包
    m.reads << statusReads(0) << statusReads(0) << frameReads(1, QByteArray("0x20240101")) << statusReads(0)
            << statusReads(0) << statusReads(0)                                        // set_reset_key（2 + 参数 status）
            << statusReads(0) << statusReads(0)                                        // set_checksum_level（2 + 参数 status）
            << statusReads(0) << statusReads(0) << frameReads(1, QByteArray("brom")) << statusReads(0);
    mtkbrom::XFlashSession x(&m, 0x6765);
    QByteArray agent;
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::xflashBringUpSteps(x, &agent, &log, &err), qPrintable(err));
    QCOMPARE(agent, QByteArray("brom"));
    // 四步的子命令值都要出现在写流里；reset_key 的参数是 0x68（小端 >I）
    bool sawResetKeyCmd = false, sawChecksumCmd = false, sawResetKeyParam = false;
    for (const QByteArray &f : std::as_const(m.writeFrames)) {
        if (f == le32(quint32(mtkbrom::X_CTRL_SET_RESET_KEY)))      sawResetKeyCmd = true;
        if (f == le32(quint32(mtkbrom::X_CTRL_SET_CHECKSUM_LEVEL))) sawChecksumCmd = true;
        if (f == le32(0x68))                                        sawResetKeyParam = true;
    }
    QVERIFY2(sawResetKeyCmd, "必须发 SET_RESET_KEY 子命令");
    QVERIFY2(sawChecksumCmd, "必须发 SET_CHECKSUM_LEVEL 子命令");
    QVERIFY2(sawResetKeyParam, "set_reset_key 的参数必须是 0x68");
}

// GET_CHIP_ID：5×u16
void TestMtkXflashPayload::getChipIdParsesFiveShorts()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0)
            << frameReads(1, le16(0x6765) + le16(0x8A00) + le16(0xCA00) + le16(0x0000) + le16(1))
            << statusReads(0);                                    // 尾部 status（XFL:396-418）
    mtkbrom::XFlashSession x(&m, 0x6765);
    mtkbrom::XChipId id;
    QString err;
    QVERIFY2(mtkbrom::xflashGetChipId(x, id, &err), qPrintable(err));
    QCOMPARE(id.hwCode, quint16(0x6765));
    QCOMPARE(id.hwSubCode, quint16(0x8A00));
    QCOMPARE(id.hwVersion, quint16(0xCA00));
    QCOMPARE(id.swVersion, quint16(0x0000));
    QCOMPARE(id.chipEvolution, quint16(1));
}

// GET_PACKET_LENGTH：<II
void TestMtkXflashPayload::getPacketLengthParsesTwoU32()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0) << frameReads(1, le32(0x10000) + le32(0x20000))
            << statusReads(0);                                    // 尾部 status（XFL:623-635）
    mtkbrom::XFlashSession x(&m, 0x6765);
    mtkbrom::XPacketLength pl;
    QString err;
    QVERIFY2(mtkbrom::xflashGetPacketLength(x, pl, &err), qPrintable(err));
    QCOMPARE(pl.writeLength, quint32(0x10000));
    QCOMPARE(pl.readLength, quint32(0x20000));
}

// GET_PARTITION_TBL_CATA：0x64=GPT / 0x65=PMT / 其它=Unknown
void TestMtkXflashPayload::getPartitionCataMapsGptAndPmt()
{
    {
        MockUsbChannel m;
        // GET_PARTITION_TBL_CATA 是**唯一不读尾部 status** 的查询（XFL:612-621）→ 用"毒药帧"钉住：
        // 队列里多放一帧 status；若实现多读了一帧，它会被消费掉 → 断言 reads 仍有剩余
        m.reads << statusReads(0) << statusReads(0) << frameReads(1, le32(0x64));
        mtkbrom::XFlashSession x(&m, 0x6765);
        mtkbrom::PartitionCata c = mtkbrom::PartitionCata::Unknown;
        QVERIFY(mtkbrom::xflashGetPartitionCata(x, c, nullptr));
        QCOMPARE(c, mtkbrom::PartitionCata::Gpt);
    }
    {
        MockUsbChannel m;
        m.reads << statusReads(0) << statusReads(0) << frameReads(1, le32(0x65));
        mtkbrom::XFlashSession x(&m, 0x6765);
        mtkbrom::PartitionCata c = mtkbrom::PartitionCata::Unknown;
        QVERIFY(mtkbrom::xflashGetPartitionCata(x, c, nullptr));
        QCOMPARE(c, mtkbrom::PartitionCata::Pmt);
    }
    {
        MockUsbChannel m;
        m.reads << statusReads(0) << statusReads(0) << frameReads(1, le32(0x63));
        mtkbrom::XFlashSession x(&m, 0x6765);
        mtkbrom::PartitionCata c = mtkbrom::PartitionCata::Gpt;
        QVERIFY(mtkbrom::xflashGetPartitionCata(x, c, nullptr));
        QCOMPARE(c, mtkbrom::PartitionCata::Unknown);      // 其它值 → Unknown（调用方按"两者都试"处理）
    }
}
QTEST_APPLESS_MAIN(TestMtkXflashPayload)
#include "test_mtk_xflash_payload.moc"
```

- [ ] **Step 2: 跑一遍，确认失败**（先按 Step 5 注册）

- [ ] **Step 3: 实现**

`mtk_xflash_payload.h`：按 Interfaces 照抄（含常量与函数声明；注释带行号出处）。

`mtk_xflash_payload.cpp`：

```cpp
#include "core/modes/mtk_xflash_payload.h"

namespace mtkbrom {
namespace {
QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
    return b;
}
quint16 le16At(const QByteArray &b, int off) { return quint16(quint8(b.at(off))) | (quint16(quint8(b.at(off + 1))) << 8); }
quint32 le32At(const QByteArray &b, int off)
{
    return quint32(quint8(b.at(off))) | (quint32(quint8(b.at(off + 1))) << 8)
         | (quint32(quint8(b.at(off + 2))) << 16) | (quint32(quint8(b.at(off + 3))) << 24);
}
} // namespace

bool xflashDa1Handshake(XFlashSession &x, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    // 1) SYNC：**只发不读**（上游 `sync()` = `xsend(SYNC_SIGNAL)` 且无 status 读，XFL:903-907）
    //    ⚠️ 计划期更正（T4 实施期实测）：原稿在这里**多读了一帧 status** —— 真机上那一读会吃掉设备
    //    后续才发的帧（或直接超时）→ 握手必失败。整段握手的读**只有 3 次**：SETUP_ENV 的 status、
    //    SETUP_HW_INIT 的 status、最后读回的 SYNC 帧（XFL:986-994 的读序列）。
    if (!x.xsendInt(kXSync, error))
        return false;
    say(QStringLiteral("XFlash：SYNC 已发送（无回读）"));
    // 2) SETUP_ENVIRONMENT：0x010100 + 20B（XFL:909-924）
    QByteArray env;
    env += le32(0);            // da_log_level（uartloglevel，默认 0）
    env += le32(1);            // log_channel：UART=1（XFL:913-918 的默认 "UART"）
    env += le32(1);            // system_os = FtSystemOSE.OS_LINUX（XFP:83-85）
    env += le32(0);            // ufs_provision
    env += le32(0);            // 保留
    if (env.size() != 20) {
        if (error) *error = QStringLiteral("内部错误：SETUP_ENVIRONMENT 载荷 %1 字节（应为 20）").arg(env.size());
        return false;
    }
    if (!x.xsendInt(X_CMD_SETUP_ENV, error))
        return false;
    if (!x.sendParam({env}, error))            // 载荷 + 一次 status
        return false;
    // 3) SETUP_HW_INIT_PARAMS：0x010101 + 4B 0（XFL:926-932）
    if (!x.xsendInt(X_CMD_SETUP_HW_INIT, error))
        return false;
    if (!x.sendParam({le32(0)}, error))
        return false;
    // 4) 读回必须是 "SYNC"（XFL:991-994）
    QByteArray reply;
    if (!x.xread(reply, nullptr, error))
        return false;
    if (reply.size() != 4 || le32At(reply, 0) != kXSync) {
        if (error) *error = QStringLiteral("XFlash：DA1 未回 SYNC（读到 %1 字节 / 0x%2）")
                                .arg(reply.size())
                                .arg(reply.size() == 4 ? le32At(reply, 0) : 0u, 8, 16, QLatin1Char('0'));
        return false;
    }
    say(QStringLiteral("XFlash：七步握手完成（SYNC 应答）"));
    return true;
}

bool xflashBringUpSteps(XFlashSession &x, QByteArray *connectionAgent, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    // 顺序照 XFL:1103-1107：get_expire_date → set_reset_key(0x68) → set_checksum_level(0) → get_connection_agent
    QByteArray date;
    if (!x.sendDevCtrl(X_CTRL_GET_EXPIRE_DATE, QByteArray(), &date, error))
        return false;
    say(QStringLiteral("XFlash：expire_date = %1").arg(QString::fromLatin1(date)));
    if (!x.sendDevCtrl(X_CTRL_SET_RESET_KEY, le32(0x68), nullptr, error))
        return false;
    if (!x.sendDevCtrl(X_CTRL_SET_CHECKSUM_LEVEL, le32(0x0), nullptr, error))   // mtkclient 恒设 PLAIN（XFL:1106）
        return false;
    QByteArray agent;
    if (!x.sendDevCtrl(X_CTRL_GET_CONNECTION_AGENT, QByteArray(), &agent, error))
        return false;
    if (connectionAgent) *connectionAgent = agent;
    say(QStringLiteral("XFlash：connection_agent = %1").arg(QString::fromLatin1(agent)));
    return true;
}

bool xflashGetChipId(XFlashSession &x, XChipId &out, QString *error)
{
    QByteArray r;
    if (!x.sendDevCtrl(X_CTRL_GET_CHIP_ID, QByteArray(), &r, error))
        return false;
    if (r.size() < 10) {
        if (error) *error = QStringLiteral("GET_CHIP_ID 回包长度不符（%1）").arg(r.size());
        return false;
    }
    out.hwCode = le16At(r, 0);
    out.hwSubCode = le16At(r, 2);
    out.hwVersion = le16At(r, 4);
    out.swVersion = le16At(r, 6);
    out.chipEvolution = le16At(r, 8);
    return true;
}

bool xflashGetPacketLength(XFlashSession &x, XPacketLength &out, QString *error)
{
    QByteArray r;
    if (!x.sendDevCtrl(X_CTRL_GET_PACKET_LENGTH, QByteArray(), &r, error))
        return false;
    if (r.size() < 8) {
        if (error) *error = QStringLiteral("GET_PACKET_LENGTH 回包长度不符（%1）").arg(r.size());
        return false;
    }
    out.writeLength = le32At(r, 0);
    out.readLength = le32At(r, 4);
    return true;
}

bool xflashGetPartitionCata(XFlashSession &x, PartitionCata &out, QString *error)
{
    QByteArray r;
    if (!x.sendDevCtrl(X_CTRL_GET_PARTITION_CATA, QByteArray(), &r, error))
        return false;
    if (r.size() < 4) {
        if (error) *error = QStringLiteral("GET_PARTITION_TBL_CATA 回包长度不符（%1）").arg(r.size());
        return false;
    }
    const quint32 v = le32At(r, 0);
    out = (v == 0x64) ? PartitionCata::Gpt : (v == 0x65) ? PartitionCata::Pmt : PartitionCata::Unknown;
    return true;
}

bool xflashGetRamInfo(XFlashSession &x, QByteArray *raw, QString *error)
{
    QByteArray r;
    if (!x.sendDevCtrl(X_CTRL_GET_RAM_INFO, QByteArray(), &r, error))
        return false;
    if (r.size() != 24 && r.size() != 48) {
        if (error) *error = QStringLiteral("GET_RAM_INFO 回包长度异常（%1，应为 24 或 48）").arg(r.size());
        return false;
    }
    if (raw) *raw = r;
    return true;
}

} // namespace mtkbrom
```

> ✅ **已核（计划期，控制方亲自读上游）**：`get_expire_date()` 本体在 `xflash_lib.py:571-579` —— **无参数**、`send_devctrl(GET_EXPIRE_DATE)` 后读回包（`res != b""` 且 `status == 0` 才返回 res），子命令号 **0x040011**（`xflash_param.py:74` 的 `GET_EXPIRE_DATE`）。
> 其余同类细节一并核过：`set_reset_key` = `send_devctrl(SET_RESET_KEY, pack("<I", 0x68))`（`XFL:206-209`）、`set_checksum_level` = `pack("<I", 0)`（`XFL:241-244`）、
> `get_connection_agent` = 无参数 + 回包字符串（`XFL:330-338`）、`setup_env` 载荷 = `pack("<IIIII", uartloglevel, log_channel, system_os, ufs_provision, 0)` = **20B**（`XFL:909-922`）、
> `setup_hw_init` 载荷 = `pack("<I", 0)` = **4B**（`XFL:926-931`）。原稿两处子命令号错的证据：`0x020005` = `SET_HOST_INFO`、`0x040002` = `GET_NAND_INFO`（`xflash_param.py:57-66`）。

- [ ] **Step 4: 跑测试，确认通过**

```bash
cmake -B build -G Ninja && cmake --build build --target image_engine_tests_test_mtk_xflash_payload
./build/image_engine_tests_test_mtk_xflash_payload    # 预期 6 passed / 0 skipped
```

- [ ] **Step 5: 注册测试（CMakeLists.txt）**

```cmake
                elseif(_test_stem STREQUAL "test_mtk_xflash_payload")
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_xflash_session.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_xflash_payload.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_brom.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_emmc.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_da_file.cpp)
```
（+ `IMAGE_TEST_SOURCES` 追加 `tests/test_mtk_xflash_payload.cpp`；+ 进 libusb 链接名单 + 真样本宏名单）

- [ ] **Step 6: 提交**

```bash
git add src/core/modes/mtk_xflash_payload.h src/core/modes/mtk_xflash_payload.cpp tests/test_mtk_xflash_payload.cpp CMakeLists.txt
git commit -m "feat(mtk): XFlash 引导①（七步握手 + bring-up 四步 + 只读查询）"
```

---

### Task 5: XFlash 载荷 `mtk_xflash_payload` ②（INIT_EXT_RAM EMI + boot_to 剥签名 + SHUTDOWN）

**Files:**
- Modify: `src/core/modes/mtk_xflash_payload.{h,cpp}`
- Modify: `tests/test_mtk_xflash_payload.cpp`

**Interfaces:**
- Consumes: T2/T4 的产物；`EmiData`（T3）
- Produces:
  ```cpp
  namespace mtkbrom {
  // EMI（XFL:251-270）：INIT_EXT_RAM → status 0 → sleep(10ms) → xsend(<I len) → send_param(emi)
  bool xflashSendEmi(XFlashSession &x, const QByteArray &emi, QString *error = nullptr);
  // boot_to（XFL:288-328）：BOOT_TO → status 0 → xsend(<QQ addr,len) → send_data(da2) → sleep(500ms) → status ∈ {0, SYNC}
  // da2 必须**已剥尾部签名**（调用方给；见 D1 的 DaSelection.da2Bytes 与 m_sig_len）
  // ⚠️ **true 只表示最终 status 字匹配（0 或 0x434E5953），不证明 DA2 已在跑** —— 上游证明"DA2 活着"靠
  //    后面那串 reinit 查询（事实报告 §7.4），我们**不跑**（T7 的日志只能写"已上传"，不得写"已就绪/已验证"）
  bool xflashBootTo(XFlashSession &x, quint64 addr, const QByteArray &da2, QString *error = nullptr);
  // SHUTDOWN（XFL:813-833）：32B 参数 pack("<IIIIIIII", hasflags, enablewdt, async_mode, bootmode, dl_bit, dont_resetrtc, leaveusb, 0)
  bool xflashShutdown(XFlashSession &x, quint32 bootmode = 0, QString *error = nullptr);
  }
  ```

- [ ] **Step 1: 写失败用例**（追加到 `tests/test_mtk_xflash_payload.cpp`）

```cpp
// EMI：INIT_EXT_RAM → status → sleep → **长度单独一帧** → 数据分块（0x200）→ 一次 status
void TestMtkXflashPayload::sendEmiSequence()
{
    MockUsbChannel m;
    m.reads << statusReads(0)          // INIT_EXT_RAM 的 status
            << statusReads(0);         // send_param 的 status
    mtkbrom::XFlashSession x(&m, 0x6765);
    const QByteArray emi(912, '\xA5');
    QString err;
    QVERIFY2(mtkbrom::xflashSendEmi(x, emi, &err), qPrintable(err));
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));   // INIT_EXT_RAM 帧头
    QCOMPARE(m.writeFrames.at(1), le32(0x01000A));                         // 命令值
    QCOMPARE(m.writeFrames.at(2), le32(0xFEEEEEEF) + le32(1) + le32(4));   // 长度帧头
    QCOMPARE(m.writeFrames.at(3), le32(912));                              // **长度单独一帧**
    QCOMPARE(m.writeFrames.at(4), le32(0xFEEEEEEF) + le32(1) + le32(912)); // EMI 帧头
    int dataBytes = 0;
    for (int i = 5; i < m.writeFrames.size(); ++i) dataBytes += m.writeFrames.at(i).size();
    QCOMPARE(dataBytes, 912);                                              // 载荷分块（0x200）合计
}

// boot_to：BOOT_TO 帧 → <QQ addr,len> → 数据 → sleep → status（0 或 SYNC 都算成功）
void TestMtkXflashPayload::bootToAcceptsZeroOrSyncStatus()
{
    const QByteArray da2(0x300, '\x11');         // 已剥签名的 DA2
    for (quint32 st : {0u, 0x434E5953u}) {
        MockUsbChannel m;
        // ⚠️ 计划期更正（T5 实施期实测）：boot_to 共读 **3** 个 status —— BOOT_TO 自己（XFL:290）、
        //    send_data 内部那个（:295 → :282）、以及最终 status（:307/:315）。原稿只排 2 个会把**正确实现判红**。
        m.reads << statusReads(0) << statusReads(0) << statusReads(st);
        mtkbrom::XFlashSession x(&m, 0x6765);
        QString err;
        QVERIFY2(mtkbrom::xflashBootTo(x, 0x40000000ull, da2, &err), qPrintable(err));
        QCOMPARE(m.reads.size(), 0);                              // 精确排空（读多/读少都要判红）
        QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));    // BOOT_TO 帧头
        QCOMPARE(m.writeFrames.at(1), le32(0x010008));                         // 命令值
        QCOMPARE(m.writeFrames.at(2), le32(0xFEEEEEEF) + le32(1) + le32(16));   // 参数帧头（16B）
        QCOMPARE(m.writeFrames.at(3), le32(quint32(0x40000000)) + le32(0) + le32(quint32(da2.size())) + le32(0));
    }
}

// boot_to：最终 status 既非 0 也非 SYNC → 明确失败
void TestMtkXflashPayload::bootToRejectsBadStatus()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0) << statusReads(0xDEAD);   // 三段 status（见上）
    mtkbrom::XFlashSession x(&m, 0x6765);
    QString err;
    QVERIFY(!mtkbrom::xflashBootTo(x, 0x40000000ull, QByteArray(16, '\x11'), &err));
    QVERIFY(!err.isEmpty());
}

// SHUTDOWN：32B 参数（hasflags 依 async_mode/dl_bit/bootmode 推导）
void TestMtkXflashPayload::shutdownParameterLayout()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0);        // SHUTDOWN 的两处 status
    mtkbrom::XFlashSession x(&m, 0x6765);
    QString err;
    QVERIFY2(mtkbrom::xflashShutdown(x, /*bootmode=*/0, &err), qPrintable(err));
    QCOMPARE(m.writeFrames.at(2), le32(0xFEEEEEEF) + le32(1) + le32(32));
    const QByteArray p = m.writeFrames.at(3);
    QCOMPARE(p.size(), 32);
    QCOMPARE(p.mid(0, 4), le32(0));                     // hasflags = 0（NORMAL、未异步）
    QCOMPARE(p.mid(4, 4), le32(0));                     // enablewdt = 0
    QCOMPARE(p.mid(8, 4), le32(0));                     // async_mode
    QCOMPARE(p.mid(12, 4), le32(0));                    // bootmode = NORMAL
}
```

- [ ] **Step 2: 跑一遍，确认失败**

- [ ] **Step 3: 实现**（追加到 `mtk_xflash_payload.cpp`）

```cpp
bool xflashSendEmi(XFlashSession &x, const QByteArray &emi, QString *error)
{
    if (emi.isEmpty()) {
        if (error) *error = QStringLiteral("XFlash EMI 数据为空");
        return false;
    }
    if (!x.xsendInt(X_CMD_INIT_EXT_RAM, error))       // 0x01000A
        return false;
    if (!x.checkStatus(error))                        // status 必须 0（XFL:255-259）
        return false;
    QThread::msleep(10);                              // XFL:260 的 time.sleep(0.01)
    if (!x.xsendInt(quint32(emi.size()), error))      // **长度单独一帧**（XFL:262）
        return false;
    return x.sendParam({emi}, error);                 // EMI 本体（0x200 分块 + 一次 status）
}

bool xflashBootTo(XFlashSession &x, quint64 addr, const QByteArray &da2, QString *error)
{
    if (da2.isEmpty()) {
        if (error) *error = QStringLiteral("XFlash boot_to：DA2 为空（调用方须给已剥签名的切片）");
        return false;
    }
    if (!x.xsendInt(X_CMD_BOOT_TO, error))            // 0x010008
        return false;
    if (!x.checkStatus(error))
        return false;
    QByteArray param;
    param += le64(addr);
    param += le64(quint64(da2.size()));
    if (param.size() != 16) {
        if (error) *error = QStringLiteral("内部错误：boot_to 参数 %1 字节（应为 16）").arg(param.size());
        return false;
    }
    if (!x.xsend(param, error))                       // 16B 作为**独立帧**（XFL:290）
        return false;
    if (!x.sendData(da2, error))                      // 数据块 + 一次 status
        return false;
    QThread::msleep(500);                             // XFL:293 的 time.sleep(timeout=0.5)
    quint32 st = 0;
    if (!x.readStatus(st, error))
        return false;
    if (st != 0 && st != kXSync) {                    // XFL:315：0 或 SYNC 都算成功
        if (error) *error = QStringLiteral("XFlash boot_to：DA2 未就绪（status = 0x%1）")
                                .arg(st, 8, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

bool xflashShutdown(XFlashSession &x, quint32 bootmode, QString *error)
{
    if (!x.xsendInt(X_CMD_SHUTDOWN, error))           // 0x010007
        return false;
    if (!x.checkStatus(error))
        return false;
    const quint32 asyncMode = 0, dlBit = 0;
    const quint32 hasFlags = (asyncMode || dlBit || bootmode != 0) ? 1u : 0u;   // XFL:818-825
    QByteArray p;
    p += le32(hasFlags);
    p += le32(0);              // enablewdt = 0（禁用看门狗）
    p += le32(asyncMode);
    p += le32(bootmode);
    p += le32(dlBit);
    p += le32(0);              // dont_resetrtc
    p += le32(0);              // leaveusb
    p += le32(0);              // 保留
    if (p.size() != 32) {
        if (error) *error = QStringLiteral("内部错误：SHUTDOWN 参数 %1 字节（应为 32）").arg(p.size());
        return false;
    }
    if (!x.xsend(p, error))
        return false;
    return x.checkStatus(error);
}
```

（新增 `#include <QThread>`；`le64` 用 T2 的等价实现——**本文件加一个** `le64`（与 T2 的文件内实现同款，跨文件不共享匿名命名空间）。

- [ ] **Step 4: 跑测试，确认通过**（预期 10 passed / 0 skipped）

- [ ] **Step 5: 提交**

```bash
git add src/core/modes/mtk_xflash_payload.h src/core/modes/mtk_xflash_payload.cpp tests/test_mtk_xflash_payload.cpp
git commit -m "feat(mtk): XFlash 载荷②（INIT_EXT_RAM EMI + boot_to 剥签名 + SHUTDOWN）"
```


---

### Task 6: XFlash 载荷 ③（WRITE_DATA / READ_DATA：56B 参数 + 分块校验和 + 读数据路径）

**Files:**
- Modify: `src/core/modes/mtk_xflash_payload.{h,cpp}`
- Modify: `tests/test_mtk_xflash_payload.cpp`

**Interfaces:**
- Consumes: T2/T4 的产物
- Produces:
  ```cpp
  namespace mtkbrom {
  // 56B 存储参数（铁律 15）：pack("<IIQQ", …)=24B + 8×u32 NandExtension(全 0)=32B
  QByteArray xflashStorageParam(quint32 storage, quint32 partType, quint64 addr, quint64 length);
  // 写（XFL:670-685 + 852-899）：xsend(WRITE_DATA) → status 0 → 56B 参数帧 → 按 writePacketLength 分块：
  //   每块 data（**补零到 512 的整数倍**）→ checksum = sum(data) & 0xFFFF → send_param([<I 0>, <I checksum>, data])
  bool xflashWriteData(XFlashSession &x, quint64 addr, const QByteArray &data,
                       quint32 storage, quint32 partType, quint32 writePacketLength,
                       QStringList *log = nullptr, QString *error = nullptr);
  // 读（XFL:687-704 + 731-768）：xsend(READ_DATA) → status 0 → 48B 参数 → 数据帧循环
  bool xflashReadData(XFlashSession &x, quint64 addr, quint32 length,
                      quint32 storage, quint32 partType, QByteArray &out, QString *error = nullptr);
  }
  ```

- [ ] **Step 1: 写失败用例**（追加到 `tests/test_mtk_xflash_payload.cpp`）

```cpp
// 56B 参数布局：<IIQQ(24B) + 8×u32(32B，全 0)
void TestMtkXflashPayload::storageParamLayout()
{
    const QByteArray p = mtkbrom::xflashStorageParam(0x1, 0x8, 0x100000, 0x2000);
    QCOMPARE(p.size(), 56);                                     // ⚠️ 原稿 48 是算错（见铁律 15）
    QCOMPARE(p.mid(0, 4), le32(0x1));            // storage = EMMC
    QCOMPARE(p.mid(4, 4), le32(0x8));            // partType = USER
    QCOMPARE(p.mid(8, 8), le32(0x100000) + le32(0));            // addr（<Q 小端）
    QCOMPARE(p.mid(16, 8), le32(0x2000) + le32(0));             // length
    QCOMPARE(p.mid(24, 32), QByteArray(32, '\0'));              // NandExtension 8×u32 = 32B
}

// 写：WRITE_DATA → status → 48B → 每块补零到 512 + 校验和 + 三段参数
void TestMtkXflashPayload::writeDataChunkingAndChecksum()
{
    MockUsbChannel m;
    // 第一块（0x300 → 补零到 0x400）：三段参数各自读… 注意 send_param 只在**全部**参数写完读一次 status
    m.reads << statusReads(0) << statusReads(0)      // WRITE_DATA 命令 status + 56B 参数帧的 status
            << statusReads(0) << statusReads(0)      // 两块各一次 send_param 的 status
            << statusReads(0)                        // **循环后的最终 status**（XFL:876）
            << statusReads(0) << statusReads(0) << frameReads(1, QByteArray());   // CC_OPTIONAL_DOWNLOAD_ACT 的 devctrl 二连 + 空回包
    mtkbrom::XFlashSession x(&m, 0x6765);
    const QByteArray data(0x300, '\x01');            // 0x300 → 两块：0x300（补到 0x400）
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::xflashWriteData(x, 0x200000, data, 0x1, 0x8, /*writePacketLength=*/0x400, &log, &err),
             qPrintable(err));
    // 找到 48B 参数帧（在命令帧之后）
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));
    QCOMPARE(m.writeFrames.at(1), le32(0x010004));                       // WRITE_DATA
    QCOMPARE(m.writeFrames.at(2), le32(0xFEEEEEEF) + le32(1) + le32(56));   // 参数帧头
    QCOMPARE(m.writeFrames.at(3).size(), 56);
    // 第一块的三段：<I 0>、<I checksum>、数据（补零后 0x400）
    QCOMPARE(m.writeFrames.at(4), le32(0xFEEEEEEF) + le32(1) + le32(4));    // 段1 帧头
    QCOMPARE(m.writeFrames.at(5), le32(0));                                 // 段1 值 0
    QCOMPARE(m.writeFrames.at(6), le32(0xFEEEEEEF) + le32(1) + le32(4));    // 段2 帧头
    const quint32 expectChecksum = quint32(0x01 * 0x300);                   // sum(data)&0xFFFF（补零部分不贡献）
    QCOMPARE(m.writeFrames.at(7), le32(expectChecksum));
    QByteArray blk;
    for (int i = 8; i < m.writeFrames.size(); ++i) blk += m.writeFrames.at(i);
    QCOMPARE(blk.size(), 0x400);                                          // **补零到 512 整数倍**
    QCOMPARE(quint8(blk.at(0x300)), quint8(0));                            // 补的是 0
}

// 读：READ_DATA → status → 48B → 数据帧（slength>4 收下并 ack(false)）→ flag 帧（slength==4，必须 0）→ 结束帧
void TestMtkXflashPayload::readDataCollectsFramesAndAcks()
{
    MockUsbChannel m;
    const QByteArray blk1(0x100, '\xAB'), blk2(0x80, '\xCD');
    m.reads << statusReads(0) << statusReads(0) << statusReads(0)   // 命令 status + 参数 status + **参数后的第二个 status**
            << frameReads(1, blk1)                                    // 数据帧 1
            << frameReads(1, le32(0))                                 // flag 帧（slength==4，值 0 = 正常）
            << frameReads(1, blk2)                                    // 数据帧 2
            << frameReads(1, le32(0))                                 // 结束 flag（值 0）
            ;
    mtkbrom::XFlashSession x(&m, 0x6765);
    QByteArray got;
    QString err;
    QVERIFY2(mtkbrom::xflashReadData(x, 0, 0x180, 0x1, 0x8, got, &err), qPrintable(err));
    QCOMPARE(got, blk1 + blk2);                                  // 只收数据帧，不收 flag
    // 每个数据帧之后主机要回 ack(rstatus=False)：写 ack 帧（12B 头 + 4B 0）且**不读 status**
    int acks = 0;
    for (const QByteArray &f : std::as_const(m.writeFrames))
        if (f == le32(0)) ++acks;
    QCOMPARE(acks, 2);                                           // 两个数据帧 → 两次 ack
}

// 读：flag 帧非 0 → 明确失败
void TestMtkXflashPayload::readDataRejectsNonZeroFlag()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0) << statusReads(0)
            << frameReads(1, le32(0x1234))                            // flag 非 0 = 读失败
            ;
    mtkbrom::XFlashSession x(&m, 0x6765);
    QByteArray got;
    QString err;
    QVERIFY(!mtkbrom::xflashReadData(x, 0, 0x100, 0x1, 0x8, got, &err));
    QVERIFY(!err.isEmpty());
}
```

- [ ] **Step 2: 跑一遍，确认失败**

- [ ] **Step 3: 实现**（追加到 `mtk_xflash_payload.cpp`）

```cpp
QByteArray xflashStorageParam(quint32 storage, quint32 partType, quint64 addr, quint64 length)
{
    QByteArray p;
    p += le32(storage);
    p += le32(partType);
    p += le64(addr);
    p += le64(length);
    p += QByteArray(32, '\0');       // NandExtension **8×u32 = 32B**（上游类 9 属性，pack 里跳过 operation_type，XFL:679-680）
    return p;                        // 恒 **56** 字节（24 + 32；⚠️ 原稿写 48 是算错，见铁律 15）
}

bool xflashWriteData(XFlashSession &x, quint64 addr, const QByteArray &data,
                     quint32 storage, quint32 partType, quint32 writePacketLength,
                     QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    if (writePacketLength == 0) {                 // 铁律 14：**无回退**，拿不到就不写
        if (error) *error = QStringLiteral("XFlash 写：write_packet_length 为 0（GET_PACKET_LENGTH 未取到）");
        return false;
    }
    // ⚠️ 实施期新增（T6 审查 Minor 4，**控制方裁决：比上游更严，覆盖审查者"只加注释"的建议**）：
    //    分块循环只在 `writePacketLength % 512 == 0` 时才与上游等价；否则每块是"先切原始数据、再补零"，
    //    补的零落在**实时数据之间**、实发字节也不等于参数里承诺的总长 = **静默写坏镜像**。
    //    真机报的 0x200/0x400/0x1000 全部对齐 → 分支不可达，但"不可达且静默破坏"正是本项目 fail-closed 的一类
    //    （同 GPT CRC、未知分区表、未知代际）。**在任何写之前拒绝**（与上面的 0 值检查同位）。
    if (writePacketLength % 512 != 0) {
        if (error) *error = QStringLiteral("XFlash 写：write_packet_length = 0x%1 不是 512 的整数倍 —— 拒绝写入（分块补齐会把零插进数据中间）")
                                .arg(writePacketLength, 0, 16);
        return false;
    }
    if (!x.xsendInt(X_CMD_WRITE_DATA, error))     // 0x010004
        return false;
    if (!x.checkStatus(error))
        return false;
    // ⚠️ 计划期更正（T6 实施期实测，上游 `writeflash` `XFL:847-849 → :860-861`）：上游**先把总长补零到 512 的整数倍**
    //    再 `cmd_write_data(addr, length, …)` —— 所以宣布的是**补零后**的长度，不是原始 `data.size()`。
    const QByteArray padded = padTo512(data);
    if (!x.sendParam({xflashStorageParam(storage, partType, addr, quint64(padded.size()))}, error))
        return false;

    quint64 pos = 0;
    while (pos < quint64(data.size())) {
        const int dsize = int(qMin<quint64>(writePacketLength, quint64(data.size()) - pos));
        QByteArray chunk = data.mid(int(pos), dsize);
        if (chunk.size() % 512 != 0)              // **补零到 512 的整数倍**（XFL:872-874）
            chunk.append(QByteArray(512 - (chunk.size() % 512), '\0'));
        quint32 checksum = 0;
        for (const char c : std::as_const(chunk))
            checksum = (checksum + quint8(c)) & 0xFFFF;               // sum(data) & 0xFFFF（XFL:877）
        if (!x.sendParam({le32(0), le32(checksum), chunk}, error)) {  // 三段参数，一次 status
            if (error) *error = QStringLiteral("XFlash 写：偏移 0x%1 的块写失败").arg(pos, 0, 16);
            return false;
        }
        pos += quint64(dsize);
    }
    // 循环**之后**的最终 status（XFL:876-882）：非 0 → 写失败（上游在此报 "Error on writeflash"）
    if (!x.checkStatus(error)) {
        if (error) *error = QStringLiteral("XFlash 写：addr 0x%1 收尾 status 非 0（%2）").arg(addr, 0, 16).arg(*error);
        return false;
    }
    // 成功后发一次 CC_OPTIONAL_DOWNLOAD_ACT（XFL:883；上游不检查返回值 → 我们也只告警）
    QString ccErr;
    if (!x.sendDevCtrl(X_CTRL_CC_OPTIONAL_DOWNLOAD_ACT, QByteArray(), nullptr, &ccErr))
        warn(QStringLiteral("XFlash 写：CC_OPTIONAL_DOWNLOAD_ACT 未确认（%1）—— 数据已写入").arg(ccErr));
    say(QStringLiteral("XFlash 写：addr 0x%1 共 %2 字节完成").arg(addr, 0, 16).arg(data.size()));
    return true;
}

bool xflashReadData(XFlashSession &x, quint64 addr, quint32 length,
                    quint32 storage, quint32 partType, QByteArray &out, QString *error)
{
    if (!x.xsendInt(X_CMD_READ_DATA, error))             // 0x010005
        return false;
    if (!x.checkStatus(error))
        return false;
    if (!x.sendParam({xflashStorageParam(storage, partType, addr, quint64(length))}, error))
        return false;
    // 参数帧之后的**第二个** status（XFL:698-702）：上游在这里再读一次并据此判成败。
    // 漏读会让之后每个数据帧整体错位一帧（把 status 帧当成数据收下）。
    if (!x.checkStatus(error))
        return false;

    out.clear();
    quint32 remaining = length;
    // 循环以**字节数**收尾（上游 bytestoread，XFL:730/:757）：不是"见到 flag 就停" ——
    // 中途出现的 flag==0 帧照上游继续读，读满 length 字节后才去读收尾帧。
    while (remaining > 0) {
        QByteArray payload;
        if (!x.xread(payload, nullptr, error))
            return false;
        if (payload.size() > 4) {                        // 数据帧：收下 + ack(rstatus=False)（XFL:750-757）
            out += payload;
            if (!x.ack(error))
                return false;
            remaining = (quint32(payload.size()) >= remaining) ? 0u : remaining - quint32(payload.size());
            continue;
        }
        if (payload.size() == 4) {                       // flag 帧：0 = 继续，非 0 = 设备报错（XFL:761-765）
            const quint32 flag = le32At(payload, 0);
            if (flag != 0) {
                if (error) *error = QStringLiteral("XFlash 读：设备报错（flag = %1）").arg(hexCode(flag));
                return false;
            }
            continue;
        }
        // 其它长度（含 0）：上游只打印 "Invalid slength" 就 break（XFL:766-768），本层明确报错
        if (error) *error = QStringLiteral("XFlash 读：收到未知长度的帧（%1 字节）").arg(payload.size());
        return false;
    }
    // 收尾帧（XFL:770-776）：上游**只在 slength == 4 时**解析并判 flag，其它长度既不解析也不报错。
    // 本层照做（"上游 wins"）：这是本次操作的**最后一帧**，不存在"留在设备侧让后续读错位"的风险
    // （与 devCtrlQuery 尾部 status 的取舍不同），此处更严只会让真机能用、上游能过的场景反而失败。
    QByteArray fin;
    if (!x.xread(fin, nullptr, error))
        return false;
    if (fin.size() == 4) {
        const quint32 flag = le32At(fin, 0);
        if (flag != 0) {
            if (error) *error = QStringLiteral("XFlash 读：收尾帧报错（flag = %1）").arg(hexCode(flag));
            return false;
        }
    }
    return true;
}
```

- [ ] **Step 4: 跑测试，确认通过**（预期 14 passed / 0 skipped）

- [ ] **Step 5: 提交**

```bash
git add src/core/modes/mtk_xflash_payload.h src/core/modes/mtk_xflash_payload.cpp tests/test_mtk_xflash_payload.cpp
git commit -m "feat(mtk): XFlash 载荷③（WRITE/READ_DATA：48B 参数 + 分块校验和 + 读数据帧/flag 路径）"
```

---

### Task 7: 计划层扩 XML 方言 + GPT→参照表适配

**Files:**
- Modify: `src/core/mtk_flash_plan.{h,cpp}`
- Modify: `src/ui/flash_panel.cpp`（**接线**：scatter 过滤器加 `*.xml` + 调用换成 `parseScatterAnyDialect` + log 逐条 emit）
- Modify: `tests/test_mtk_flash_plan.cpp`
- Modify: `CMakeLists.txt`（`test_mtk_flash_plan` 追加 `mtk_gpt.cpp`）

**Interfaces:**
- Consumes: `mtkgpt::Table`/`Partition`（T1）
- Produces:
  ```cpp
  namespace mtkplan {
  enum class ScatterStorage { Emmc, Ufs };

  // XML 方言 scatter（真样本 MT6789_Android_scatter.xml）：`<partition_name>`/`<partition_size>`
  //   ⚠️ **必须按 storage 过滤**：真样本 130 个 `<partition_index>` 块 = EMMC 与 UFS **两份完整副本**
  //   （同一个 SYS0 标签，一份 `<storage>HW_STORAGE_EMMC</storage>`、一份 `HW_STORAGE_UFS`）
  //   → 不过滤会让每个分区翻倍且大小对不上。每份 65 个分区。
  bool parseScatterXml(const QString &text, ScatterStorage want, QList<PartitionRef> &out,
                       QString *error = nullptr);
  // GPT 表 → 参照表（名字 + 字节大小；写入判据仍以**设备实读**为准，这里只用于预览）
  QList<PartitionRef> toPartitionRefs(const QList<mtkgpt::Partition> &parts, quint32 sectorSize);

  // **方言识别 + 双副本调和**（可测；UI 只调这一个）：文本方言 → D1 的 parseScatter；XML 方言 → 两份副本都解析，
  // 名字→大小**完全一致**才采信，不一致 → 用 EMMC 那份 + 往 log 写告警（**不猜**：预览是咨询性的，
  // 写入判据永远以设备实读的分区表为准）
  bool parseScatterAnyDialect(const QString &text, QList<PartitionRef> &out,
                              QStringList *log = nullptr, QString *error = nullptr);
  }
  ```

  > ⚠️ **计划期更正（T7 预核对）**：原稿只造了 `parseScatterXml` 却**没有任何生产调用者**（全计划里只有它自己的用例）——
  > 那是"实现+测试齐全但生产里到不了"的死 API。D1 的文本方言是被 `src/ui/flash_panel.cpp:729` 真正调用的，
  > 所以本任务必须**接线**：`flash_panel.cpp` 的 scatter 选择处（① 文件过滤器加 `*.xml`；② 把 `mtkplan::parseScatter(...)`
  > 换成 `mtkplan::parseScatterAnyDialect(...)` 并把 log 逐条 emit）—— **不新增入口**，仍是原来那一个按钮/对话框。

- [ ] **Step 1: 写失败用例**（追加到 `tests/test_mtk_flash_plan.cpp`）

```cpp
// XML 方言：**按 storage 过滤**（真样本 130 块 = EMMC/UFS 两份，每份 65）
void TestMtkFlashPlan::parsesXmlScatterFilteringStorage()
{
    const QString path = QDir(mtktest::samplesDir()).filePath(QStringLiteral("MT6789_Android_scatter.xml"));
    if (!QFile::exists(path)) {
#if MTK_SAMPLES_REQUIRED
        QFAIL("真样本缺失：MT6789_Android_scatter.xml（MTK_SAMPLES_REQUIRED=ON）");
#else
        QSKIP("真样本缺失（reference/mtk-samples/，gitignored）");
#endif
    }
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString text = QString::fromUtf8(f.readAll());

    QList<mtkplan::PartitionRef> emmc, ufs;
    QString err;
    QVERIFY2(mtkplan::parseScatterXml(text, mtkplan::ScatterStorage::Emmc, emmc, &err), qPrintable(err));
    QVERIFY2(mtkplan::parseScatterXml(text, mtkplan::ScatterStorage::Ufs, ufs, &err), qPrintable(err));
    QCOMPARE(emmc.size(), 65);                 // 真样本实测：130 块 = 两份，每份 65
    QCOMPARE(ufs.size(), 65);
    auto sizeOf = [](const QList<mtkplan::PartitionRef> &l, const QString &n) -> quint64 {
        for (const mtkplan::PartitionRef &p : l)
            if (p.name == n) return p.sizeBytes;
        return 0;
    };
    QCOMPARE(sizeOf(emmc, QStringLiteral("preloader")), quint64(0x100000));
    QCOMPARE(sizeOf(emmc, QStringLiteral("pgpt")), quint64(0x8000));
    QCOMPARE(sizeOf(emmc, QStringLiteral("misc")), quint64(0x80000));
    QCOMPARE(sizeOf(emmc, QStringLiteral("expdb")), quint64(0x8000000));
    QVERIFY(sizeOf(ufs, QStringLiteral("preloader")) > 0);      // UFS 副本也在（同一名字、不同 storage）
}

// 方言识别 + 双副本调和：XML 两份一致 → 采信且**不翻倍**；不一致 → 采 EMMC + 告警（不猜）；文本方言走原路
void TestMtkFlashPlan::scatterAnyDialectReconcilesXmlCopies()
{
    const QString same = QStringLiteral(
        "<storage_type name=\"EMMC\"><partition_index name=\"SYS0\">"
        "<partition_name>preloader</partition_name><partition_size>0x100000</partition_size>"
        "<storage>HW_STORAGE_EMMC</storage></partition_index></storage_type>"
        "<storage_type name=\"UFS\"><partition_index name=\"SYS0\">"
        "<partition_name>preloader</partition_name><partition_size>0x100000</partition_size>"
        "<storage>HW_STORAGE_UFS</storage></partition_index></storage_type>");
    QList<mtkplan::PartitionRef> refs;
    QStringList log;
    QString err;
    QVERIFY2(mtkplan::parseScatterAnyDialect(same, refs, &log, &err), qPrintable(err));
    QCOMPARE(refs.size(), 1);                                  // 两份副本 → **不翻倍**
    QCOMPARE(refs.at(0).name, QStringLiteral("preloader"));
    QCOMPARE(refs.at(0).sizeBytes, quint64(0x100000));

    // 不一致：UFS 副本大小不同 → 采信 EMMC + 日志说明分歧
    QString diff = same;
    diff.replace(QStringLiteral("0x100000</partition_size><storage>HW_STORAGE_UFS"),
                 QStringLiteral("0x200000</partition_size><storage>HW_STORAGE_UFS"));
    QList<mtkplan::PartitionRef> refs2;
    QStringList log2;
    QVERIFY2(mtkplan::parseScatterAnyDialect(diff, refs2, &log2, &err), qPrintable(err));
    QCOMPARE(refs2.at(0).sizeBytes, quint64(0x100000));        // EMMC 那份
    QVERIFY2(log2.join(QLatin1Char('\n')).contains(QStringLiteral("UFS")), qPrintable(log2.join(QLatin1Char('\n'))));

    // 文本方言仍走原路（同一入口两种方言）
    QList<mtkplan::PartitionRef> refs3;
    QVERIFY2(mtkplan::parseScatterAnyDialect(QStringLiteral("preloader 0x100000\n"), refs3, nullptr, &err),
             qPrintable(err));
    QCOMPARE(refs3.size(), 1);
}

// XML 方言：坏输入 / 空结果 → 明确失败
void TestMtkFlashPlan::xmlScatterRejectsGarbage()
{
    QList<mtkplan::PartitionRef> out;
    QString err;
    QVERIFY(!mtkplan::parseScatterXml(QStringLiteral("hello"), mtkplan::ScatterStorage::Emmc, out, &err));
    QVERIFY(!err.isEmpty());
}

// GPT → 参照表（用 T1 的合成 GPT，避免真样本依赖）
void TestMtkFlashPlan::gptToPartitionRefs()
{
    const QByteArray raw = mtkgpt::testBuildSyntheticGpt(512, 64);
    mtkgpt::Table t;
    QString err;
    QVERIFY2(mtkgpt::parsePrimary(raw, 512, t, &err), qPrintable(err));
    const QList<mtkplan::PartitionRef> refs = mtkplan::toPartitionRefs(t.partitions, t.sectorSize);
    QCOMPARE(refs.size(), 1);
    QCOMPARE(refs.at(0).name, QStringLiteral("boot"));
    QCOMPARE(refs.at(0).sizeBytes, quint64(2 * 512));
}
```

（本文件需要 `#include "core/modes/mtk_gpt.h"`。）

- [ ] **Step 2: 跑一遍，确认失败**

- [ ] **Step 3: 实现**（`mtk_flash_plan.{h,cpp}` 追加；`#include "core/modes/mtk_gpt.h"`）

```cpp
bool parseScatterAnyDialect(const QString &text, QList<PartitionRef> &out, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    if (!text.contains(QStringLiteral("<partition_index")))
        return parseScatter(text, out, error);            // 文本方言：D1 既有实现原样
    QList<PartitionRef> emmc, ufs;
    QString emmcErr, ufsErr;
    const bool okE = parseScatterXml(text, ScatterStorage::Emmc, emmc, &emmcErr);
    const bool okU = parseScatterXml(text, ScatterStorage::Ufs, ufs, &ufsErr);
    if (!okE && !okU) {
        if (error) *error = QStringLiteral("XML scatter 两份副本都解析失败：EMMC（%1）；UFS（%2）").arg(emmcErr, ufsErr);
        return false;
    }
    if (!okE || !okU) {                                   // 只有一份可用 → 用它，并说明另一份为何不可用
        say(QStringLiteral("XML scatter：只有 %1 副本可用（另一份：%2）")
                .arg(okE ? QStringLiteral("EMMC") : QStringLiteral("UFS")).arg(okE ? ufsErr : emmcErr));
        out = okE ? emmc : ufs;
        return true;
    }
    if (emmc.size() == ufs.size()) {                      // 两份都成功 → 名字→大小完全一致才采信
        bool same = true;
        for (int i = 0; i < emmc.size() && same; ++i)
            same = (emmc.at(i).name == ufs.at(i).name && emmc.at(i).sizeBytes == ufs.at(i).sizeBytes);
        if (same) { out = emmc; return true; }
    }
    say(QStringLiteral("XML scatter：EMMC 与 UFS 两份副本不一致（%1 vs %2 条）—— 预览采用 EMMC 副本，"
                       "**写入判据仍以设备实读的分区表为准**").arg(emmc.size()).arg(ufs.size()));
    out = emmc;
    return true;
}

bool parseScatterXml(const QString &text, ScatterStorage want, QList<PartitionRef> &out, QString *error)
{
    out.clear();
    const QString wantStorage = (want == ScatterStorage::Emmc) ? QStringLiteral("HW_STORAGE_EMMC")
                                                               : QStringLiteral("HW_STORAGE_UFS");
    // 以 <partition_index ...>…</partition_index> 为块切分（真样本的块标签带 name 属性）
    static const QRegularExpression blockRe(QStringLiteral("<partition_index[^>]*>(.*?)</partition_index>"),
                                            QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression nameRe(QStringLiteral("<partition_name>([^<]*)</partition_name>"));
    static const QRegularExpression sizeRe(QStringLiteral("<partition_size>([^<]*)</partition_size>"));
    static const QRegularExpression storageRe(QStringLiteral("<storage>([^<]*)</storage>"));

    auto it = blockRe.globalMatch(text);
    while (it.hasNext()) {
        const QString body = it.next().captured(1);
        const QString storage = storageRe.match(body).captured(1).trimmed();
        if (storage != wantStorage)
            continue;                                  // **按 storage 过滤**（EMMC/UFS 两份副本）
        const QString name = nameRe.match(body).captured(1).trimmed();
        if (name.isEmpty())
            continue;
        const QString sizeText = sizeRe.match(body).captured(1).trimmed();
        bool ok = false;
        const quint64 size = sizeText.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)
                                 ? sizeText.mid(2).toULongLong(&ok, 16)
                                 : sizeText.toULongLong(&ok, 10);
        PartitionRef r;
        r.name = name;
        r.sizeBytes = ok ? size : 0;                   // 解析不出 → 0（= 未知，不参与大小校验）
        out << r;
    }
    if (out.isEmpty()) {
        if (error) *error = QStringLiteral("scatter XML 里没有解析出任何 %1 分区（partition_name/partition_size）")
                                .arg(wantStorage);
        return false;
    }
    return true;
}

QList<PartitionRef> toPartitionRefs(const QList<mtkgpt::Partition> &parts, quint32 sectorSize)
{
    QList<PartitionRef> out;
    out.reserve(parts.size());
    for (const mtkgpt::Partition &p : parts) {
        PartitionRef r;
        r.name = p.name;
        r.sizeBytes = mtkgpt::sizeBytes(p, sectorSize);
        out << r;
    }
    return out;
}
```

（`mtk_flash_plan.h` 加 `#include <QRegularExpression>` 不需要——它是 .cpp 的事；头文件只加 `#include "core/modes/mtk_gpt.h"` 与上面两个声明。）

- [ ] **Step 4: 跑测试，确认通过**（`-DMTK_SAMPLES_REQUIRED=ON`，预期 20 passed / 0 skipped）

- [ ] **Step 5: CMake**：`test_mtk_flash_plan` 的 `_test_extra_sources` 追加 `${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_gpt.cpp`

- [ ] **Step 6: 提交**

```bash
git add src/core/mtk_flash_plan.h src/core/mtk_flash_plan.cpp tests/test_mtk_flash_plan.cpp CMakeLists.txt
git commit -m "feat(mtk): 计划层扩 XML scatter 方言（按 storage 过滤 EMMC/UFS 双副本）+ GPT→参照表适配"
```


---

### Task 8: XML 帧层 `mtk_xml_session`（文本帧 / OK / OK@0x<len> / OK!EOT / get_command_result）

**Files:**
- Create: `src/core/modes/mtk_xml_session.{h,cpp}`
- Create: `tests/test_mtk_xml_session.cpp`
- Modify: `CMakeLists.txt`（注册）

**Interfaces:**
- Consumes: `IBromUsb`
- Produces:
  ```cpp
  namespace mtkbrom {
  class XmlSession {
  public:
      explicit XmlSession(IBromUsb *usb) : m_usb(usb) {}
      IBromUsb *usb() { return m_usb; }

      // 发送：str 载荷 → 12B 头（magic, DT_PROTOCOL_FLOW, len+1）+ utf8 字节 + **NUL**（XL:146-153）
      bool xsendText(const QString &text, QString *error = nullptr);
      // 发送：原始字节（写数据块用；length = 字节数，不加 NUL）（XL:151-153 的 bytes 分支）
      bool xsendBytes(const QByteArray &data, quint32 datatype = 1, QString *error = nullptr);
      // 读一帧：头 12B（DT_MESSAGE 为 16B → length -= 4）→ 载荷由调用方读（XL:112-135）
      bool xreadHeader(quint32 &datatype, quint32 &length, QString *error = nullptr);
      bool readPayload(quint32 length, QByteArray &out, QString *error = nullptr);
      // 读一条**文本**响应（DT_PROTOCOL_FLOW → rstrip NUL → utf8）（XL:221-232）。
      // 途中的 DA 日志帧（DT_MESSAGE）被**跳过**并交给 logSink（上游同姿态：日志帧不打断协议，只记 UART log）
      using LogSink = std::function<void(const QString &)>;
      void setLogSink(LogSink sink) { m_logSink = std::move(sink); }
      bool getResponse(QString &text, QString *error = nullptr);
      bool ack(QString *error = nullptr);                        // xsend("OK\0")（XL:158-159）
      bool ackValue(quint32 length, QString *error = nullptr);   // xsend("OK@0x<hex>\0")（XL:161-163）

      struct Result {
          QString command;            // "CMD:START" / "CMD:END" / "CMD:DOWNLOAD-FILE" / ""（裸 OK@ 数据路径）
          QString text;               // END 的 result / 错误文本
          QByteArray bytes;           // 裸数据路径的字节
          quint32 packetLength = 0;   // DOWNLOAD-FILE 的 packet_length（**十六进制解析**，XL:422）
          QString info, file;         // info / source_file / target_file
          bool hasPacketLength = false;
      };
      // XL:369-449 的 C++ 形态：读响应 → 解析 <command> → 分派（含 PROGRESS-REPORT 的 OK!EOT 保活）
      bool readCommandResult(Result &out, QStringList *log = nullptr, QString *error = nullptr);
      // XL:188-219：xsend → 响应必须 "OK" → （非 noack）readCommandResult → CMD:END/CMD:START 收尾
      // ⚠️ **调用方契约（T8 审查后补）**：`Result.command` 为空的帧可能是"无名帧"（sendCommand 已拦成失败），也可能是
      //    **具名但未列举**的命令（如 `CMD:CUSTOM*`；上游 `get_command_result` 返回 `(cmd,"")` 交调用方判，`XL:449`）。
      //    ⇒ **调用方必须自己查 `out.command`**（T9 的 `CMD:START` 校验、T10 的 `CMD:DOWNLOAD-FILE`/`UPLOAD-FILE`/`FILE-SYS-OPERATION` 判定都依赖这条）。
      bool sendCommand(const QString &xml, Result *out, bool noack = false, QString *error = nullptr);

      // 纯函数辅助（可单测）：信封与字段
      static QString envelope(const QString &command, const QStringList &argItems = {}, const QString &version = QStringLiteral("1.0"));
      static QString field(const QString &xml, const QString &name);
  private:
      static constexpr int kMaxLogFramesToSkip = 64;   // 连续日志帧上限（防御设备刷屏；上游无上限，我们只加上界）
      LogSink m_logSink;
      IBromUsb *m_usb;
  };
  }
  ```

- [ ] **Step 1: 写失败用例**（`tests/test_mtk_xml_session.cpp`）

```cpp
#include <QtTest>
#include <QByteArray>

#include "core/modes/mtk_xml_session.h"
#include "core/modes/mtk_brom.h"

class MockUsbChannel : public mtkbrom::IBromUsb   // 与 T2/T4 同款
{
public:
    QByteArray writes;
    QList<QByteArray> writeFrames;
    QList<QByteArray> reads;
    int pktSize = 0x400;
    bool open(QString *) override { return true; }
    bool write(const QByteArray &data, QString *) override { writes += data; writeFrames << data; return true; }
    bool read(QByteArray &out, int maxLen, int, QString *) override
    {
        if (reads.isEmpty()) { out.clear(); return false; }
        const QByteArray r = reads.takeFirst();
        out = r.left(maxLen);
        return !r.isEmpty();
    }
    int maxPacketSize() const override { return pktSize; }
    bool close() override { return true; }
};

namespace {
QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
    return b;
}
// 文本帧：length = 可见字节数 + 1（NUL）
QByteArray textFrame(const QString &s)
{
    const QByteArray body = s.toUtf8() + QByteArray(1, '\0');
    return le32(0xFEEEEEEF) + le32(1) + le32(quint32(body.size())) + body;
}
QByteArray textFrameWithLen(const QString &s, quint32 len)      // 用于"声明长度与实际不符"的负向
{
    const QByteArray body = s.toUtf8() + QByteArray(1, '\0');
    return le32(0xFEEEEEEF) + le32(1) + le32(len) + body;
}
// 读队列用：同上"一帧 = 两笔队列项"的拆分（见 T2 的 frameReads 注释）
QList<QByteArray> textReads(const QString &s)
{
    const QByteArray body = s.toUtf8() + QByteArray(1, '\0');
    return {le32(0xFEEEEEEF) + le32(1) + le32(quint32(body.size())), body};
}
} // namespace

class TestMtkXmlSession : public QObject
{
    Q_OBJECT
private slots:
    void envelopeAndFieldHelpers();
    void xsendTextAddsNulAndLengthPlusOne();
    void getResponseStripsNulAndDecodes();
    void sendCommandRequiresOkAndEndsWithEndThenStart();
    void readCommandResultParsesDownloadFilePacketLengthHex();
    void readCommandResultHandlesProgressReportKeepAlive();
    void readCommandResultHandlesBareOkAtDataPath();
};

void TestMtkXmlSession::envelopeAndFieldHelpers()
{
    const QString xml = mtkbrom::XmlSession::envelope(QStringLiteral("NOTIFY-INIT-HW"));
    QCOMPARE(xml, QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><da><version>1.0</version>"
                                 "<command>CMD:NOTIFY-INIT-HW</command></da>"));
    const QString withArgs = mtkbrom::XmlSession::envelope(QStringLiteral("READ-FLASH"),
                                                           {QStringLiteral("<partition>EMMC-USER</partition>"),
                                                            QStringLiteral("<offset>0x0</offset>")});
    QVERIFY(withArgs.contains(QStringLiteral("<arg><partition>EMMC-USER</partition><offset>0x0</offset></arg>")));
    QCOMPARE(mtkbrom::XmlSession::field(QStringLiteral("<host><command>CMD:START</command></host>"),
                                        QStringLiteral("command")),
             QStringLiteral("CMD:START"));
}

// 铁律 17：str 载荷 length = len+1 且带 NUL
void TestMtkXmlSession::xsendTextAddsNulAndLengthPlusOne()
{
    MockUsbChannel m;
    mtkbrom::XmlSession s(&m);
    QString err;
    QVERIFY2(s.xsendText(QStringLiteral("OK"), &err), qPrintable(err));
    QCOMPARE(m.writeFrames.size(), 2);
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(3));   // "OK" 3 字节（含 NUL）
    QCOMPARE(m.writeFrames.at(1), QByteArray("OK\0", 3));
}

void TestMtkXmlSession::getResponseStripsNulAndDecodes()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("OK"));
    mtkbrom::XmlSession s(&m);
    QString text;
    QString err;
    QVERIFY2(s.getResponse(text, &err), qPrintable(err));
    QCOMPARE(text, QStringLiteral("OK"));
}

// sendCommand：xsend → 响应必须 OK → 非 noack 时等 CMD:END(result OK) → ack → CMD:START
void TestMtkXmlSession::sendCommandRequiresOkAndEndsWithEndThenStart()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("OK"))                                        // 命令被接受
            << textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>OK</result></arg></host>"))
            << textReads(QStringLiteral("<host><command>CMD:START</command></host>"));
    mtkbrom::XmlSession s(&m);
    QString err;
    QVERIFY2(s.sendCommand(QStringLiteral("<da><command>CMD:FOO</command></da>"), nullptr, false, &err), qPrintable(err));
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(quint32(QByteArray("<da><command>CMD:FOO</command></da>").size() + 1)));
}

// DOWNLOAD-FILE 的 packet_length 是**十六进制**（XL:422）
void TestMtkXmlSession::readCommandResultParsesDownloadFilePacketLengthHex()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral(
        "<host><version>1.0</version><command>CMD:DOWNLOAD-FILE</command><arg>"
        "<checksum>CHK_NO</checksum><info>2nd-DA</info>"
        "<source_file>MEM://0x7fe83c09a04c:0x50c78</source_file>"
        "<packet_length>0x1000</packet_length></arg></host>"));
    mtkbrom::XmlSession s(&m);
    mtkbrom::XmlSession::Result r;
    QString err;
    QVERIFY2(s.readCommandResult(r, nullptr, &err), qPrintable(err));
    QCOMPARE(r.command, QStringLiteral("CMD:DOWNLOAD-FILE"));
    QVERIFY(r.hasPacketLength);
    QCOMPARE(r.packetLength, quint32(0x1000));
    QCOMPARE(r.info, QStringLiteral("2nd-DA"));
    QCOMPARE(r.file, QStringLiteral("MEM://0x7fe83c09a04c:0x50c78"));
}

// PROGRESS-REPORT：ack 保活直到 "OK!EOT"，再读下一条命令（XL:390-407）
void TestMtkXmlSession::readCommandResultHandlesProgressReportKeepAlive()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("<host><command>CMD:PROGRESS-REPORT</command></host>"))
            << textReads(QStringLiteral("OK!EOT"))
            << textReads(QStringLiteral("<host><command>CMD:START</command></host>"));
    mtkbrom::XmlSession s(&m);
    mtkbrom::XmlSession::Result r;
    QString err;
    QVERIFY2(s.readCommandResult(r, nullptr, &err), qPrintable(err));
    QCOMPARE(r.command, QStringLiteral("CMD:START"));
    int acks = 0;
    for (const QByteArray &f : std::as_const(m.writeFrames))
        if (f == QByteArray("OK\0", 3)) ++acks;
    QVERIFY2(acks >= 2, "PROGRESS-REPORT 期间必须持续 ack");
}

// 裸 "OK@0x<len>" 数据路径（无 <command>）：解析长度 → ack → 收数据（XL:373-388）
void TestMtkXmlSession::readCommandResultHandlesBareOkAtDataPath()
{
    MockUsbChannel m;
    const QByteArray payload(0x40, '\x99');
    m.reads << textReads(QStringLiteral("OK@0x40"))
            << textReads(QStringLiteral("OK"))
            << le32(0xFEEEEEEF) + le32(1) + le32(quint32(payload.size())) + payload;
    mtkbrom::XmlSession s(&m);
    mtkbrom::XmlSession::Result r;
    QString err;
    QVERIFY2(s.readCommandResult(r, nullptr, &err), qPrintable(err));
    QCOMPARE(r.bytes, payload);
}
QTEST_APPLESS_MAIN(TestMtkXmlSession)
#include "test_mtk_xml_session.moc"
```

- [ ] **Step 2: 跑一遍，确认失败**（先按 Step 5 注册）

- [ ] **Step 3: 实现**（`mtk_xml_session.cpp`；逐段对应 XL 行号写注释）

```cpp
#include "core/modes/mtk_xml_session.h"

namespace mtkbrom {
namespace {
constexpr quint32 kXmlMagic = 0xFEEEEEEF;
constexpr quint32 kDtProtocolFlow = 1;
constexpr quint32 kDtMessage = 2;
constexpr int kTimeoutMs = 5000;

QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
    return b;
}
quint32 le32At(const QByteArray &b, int off)
{
    return quint32(quint8(b.at(off))) | (quint32(quint8(b.at(off + 1))) << 8)
         | (quint32(quint8(b.at(off + 2))) << 16) | (quint32(quint8(b.at(off + 3))) << 24);
}
} // namespace

QString XmlSession::envelope(const QString &command, const QStringList &argItems, const QString &version)
{
    QString out = QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><da><version>%1</version>"
                                 "<command>CMD:%2</command>").arg(version, command);
    if (!argItems.isEmpty())
        out += QStringLiteral("<arg>") + argItems.join(QString()) + QStringLiteral("</arg>");
    out += QStringLiteral("</da>");
    return out;
}

QString XmlSession::field(const QString &xml, const QString &name)
{
    const QString open = QStringLiteral("<%1>").arg(name);
    const int i = xml.indexOf(open);
    if (i < 0)
        return QString();
    const int j = xml.indexOf(QStringLiteral("</%1>").arg(name), i + open.size());
    if (j < 0)
        return QString();
    return xml.mid(i + open.size(), j - i - open.size());
}

bool XmlSession::xsendText(const QString &text, QString *error)
{
    const QByteArray body = text.toUtf8() + QByteArray(1, '\0');    // **NUL 结尾**（XL:146-153）
    if (!m_usb->write(le32(kXmlMagic) + le32(kDtProtocolFlow) + le32(quint32(body.size())), error))
        return false;
    return m_usb->write(body, error);
}

bool XmlSession::xsendBytes(const QByteArray &data, quint32 datatype, QString *error)
{
    if (!m_usb->write(le32(kXmlMagic) + le32(datatype) + le32(quint32(data.size())), error))
        return false;
    return data.isEmpty() ? true : m_usb->write(data, error);
}

bool XmlSession::xreadHeader(quint32 &datatype, quint32 &length, QString *error)
{
    QByteArray h;
    if (!m_usb->readExact(h, 12, kTimeoutMs, error))
        return false;
    if (le32At(h, 0) != kXmlMagic) {
        if (error) *error = QStringLiteral("XML：应答帧 magic 不符（0x%1）").arg(le32At(h, 0), 8, 16, QLatin1Char('0'));
        return false;
    }
    datatype = le32At(h, 4);
    length = le32At(h, 8);
    if (length > (1u << 22)) {
        if (error) *error = QStringLiteral("XML：应答帧长度异常（%1）").arg(length);
        return false;
    }
    if (datatype == kDtMessage) {                      // DA 日志帧：16B 头（多 priority:u32）→ length -= 4（XL:124-127）
        QByteArray prio;
        if (!m_usb->readExact(prio, 4, kTimeoutMs, error))
            return false;
        if (length >= 4)
            length -= 4;
    }
    return true;
}

bool XmlSession::readPayload(quint32 length, QByteArray &out, QString *error)
{
    out.clear();
    return length == 0 ? true : m_usb->readExact(out, int(length), kTimeoutMs, error);
}

bool XmlSession::getResponse(QString &text, QString *error)
{
    // ⚠️ 计划期更正（T8 预核对）：上游 `xread()` 是**循环**的 —— `DT_MESSAGE`（DA 日志帧）的载荷被读掉、
    //    追加进 UART log，然后**继续读下一帧**（`xml_lib.py:107-132`）。只有 `DT_PROTOCOL_FLOW` 才返回给调用方。
    //    原稿在收到日志帧时直接失败 → 设备只要在应答前插一条日志我们就硬失败，上游不会。
    //    这里同样跳过（文本交给 logSink），但**限定跳过次数**避免设备刷屏导致死循环。
    text.clear();
    for (int skipped = 0; skipped < kMaxLogFramesToSkip; ++skipped) {
        quint32 dt = 0, len = 0;
        if (!xreadHeader(dt, len, error))
            return false;
        QByteArray payload;
        if (!readPayload(len, payload, error))
            return false;
        if (dt == kDtProtocolFlow) {
            text = QString::fromUtf8(payload).remove(QChar('\0'));   // rstrip NUL 的等价物（中段 NUL 也去掉更稳）
            return true;
        }
        if (m_logSink)
            m_logSink(QString::fromUtf8(payload).remove(QChar('\0')));   // DA 日志：进 UART log，不打断协议
    }
    if (error) *error = QStringLiteral("XML：连续收到 %1 帧 DA 日志仍未等到文本响应（上限 %2）")
                            .arg(kMaxLogFramesToSkip).arg(kMaxLogFramesToSkip);
    return false;
}

bool XmlSession::ack(QString *error) { return xsendText(QStringLiteral("OK"), error); }

bool XmlSession::ackValue(quint32 length, QString *error)
{
    return xsendText(QStringLiteral("OK@%1").arg(QLatin1String(QByteArray::number(length, 16).insert(0, "0x").constData())),
                     error);
}
```

> **实现者注意**：`ackValue` 的文案格式照上游 `f"OK@{hex(length)}\0"`（`XL:161-163`）——`hex()` 是**小写 `0x` 前缀**。上面那行写得绕，请改成清晰实现：
> ```cpp
> const QString s = QStringLiteral("OK@0x%1").arg(length, 0, 16);   // 小写十六进制、无零填充
> return xsendText(s, error);
> ```
> 并加一条用例钉住 `ackValue(0x1000)` 发出的字节 = `"OK@0x1000\0"`。

```cpp
bool XmlSession::readCommandResult(Result &out, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    out = Result{};
    QString data;
    if (!getResponse(data, error))
        return false;
    QString cmd = field(data, QStringLiteral("command"));

    // 裸数据路径：无 <command> 但含 "OK@"（XL:373-388）
    if (cmd.isEmpty() && data.contains(QStringLiteral("OK@"))) {
        const QString after = data.section(QLatin1Char('@'), 1);
        bool ok = false;
        const quint32 len = after.startsWith(QLatin1String("0x"))
                                ? after.mid(2).toUInt(&ok, 16) : after.toUInt(&ok, 16);
        if (!ok) {
            if (error) *error = QStringLiteral("XML：OK@ 长度解析失败（%1）").arg(after);
            return false;
        }
        if (!ack(error))
            return false;
        QString done;
        if (!getResponse(done, error))
            return false;
        if (!done.contains(QStringLiteral("OK"))) {
            if (error) *error = QStringLiteral("XML：数据路径的确认响应不是 OK（%1）").arg(done);
            return false;
        }
        if (!ack(error))
            return false;
        // ⚠️ **两条数据路径的 ack 节奏不同，别混**（T8 实施期实测，控制方逐行核对上游）：
        //   · 本函数对应上游 `get_command_result`（`XL:373-387`）：ack → 读 "OK" → ack → **收完数据（不逐帧 ack）** → **尾部一次 ack**。
        //   · 上游 `download_raw`（`XL:508-589`，**T10 的 READ-FLASH 读路径**）才是"每帧 ack → 读 OK → 再 ack"。
        //   我此前把后者错按到本函数上（会把设备等不到的多余 ack 发出去、并多吃一帧），已按上游改回。
        QByteArray bytes;
        for (quint32 got = 0; got < len;) {
            quint32 dt = 0, flen = 0;
            if (!xreadHeader(dt, flen, error))             // 数据帧 = 12B 头 + 载荷
                return false;
            // ⚠️ **计划期更正（T8 审查，控制方原判断错）**：数据路径**也跳** DT_MESSAGE 日志帧 —— 跳帧发生在
            //    `xread()` 内部（`while True`，`XL:112-135`），而 `get_response_data`（`XL:234-246`）第 235 行就调它。
            //    我此前只看 `get_response_data` 的 datatype 判断就说"数据路径不跳"，是错的（我引的 `:221-233` 其实是 `get_response`）。
            //    真实后果：DA 一发日志帧，我们的多帧读就会**中途失败**，而上游能读完。
            if (dt == kDtMessage) {
                QString logMsg;
                if (!readPayload(flen, logMsg, error))
                    return false;
                if (m_logSink)
                    m_logSink(logMsg.remove(QChar('\0')));
                continue;                                  // 不计入 bytes/got（日志帧不是数据）
            }
            if (dt != kDtProtocolFlow) {                   // 其它 datatype：上游 loop 里什么都不做（会死循环），我们明确报错（fail-closed）
                if (error) *error = QStringLiteral("XML：数据路径收到非协议流帧（datatype=%1）").arg(dt);
                return false;
            }
            QByteArray chunk;
            if (!readPayload(flen, chunk, error))
                return false;
            bytes += chunk;
            got += quint32(chunk.size());
        }
        if (bytes.size() != int(len)) {
            if (error) *error = QStringLiteral("XML：数据长度不符（要 %1，得 %2）").arg(len).arg(bytes.size());
            return false;
        }
        if (!ack(error))                                   // **尾部一次** ack（`XL:388`）
            return false;
        out.command = QString();
        out.bytes = bytes;
        return true;
    }

    // 保活：PROGRESS-REPORT 直到 "OK!EOT"（XL:390-407）
    if (cmd == QStringLiteral("CMD:PROGRESS-REPORT")) {
        say(QStringLiteral("XML：DA 上报进度（保活）"));
        if (!ack(error))
            return false;
        QString line;
        do {
            if (!getResponse(line, error))
                return false;
            if (!ack(error))
                return false;
        } while (line != QStringLiteral("OK!EOT"));
        if (!getResponse(data, error))
            return false;
        cmd = field(data, QStringLiteral("command"));
    }

    out.command = cmd;
    if (cmd == QStringLiteral("CMD:START")) {
        if (!ack(error))
            return false;
        out.text = QStringLiteral("START");
        return true;
    }
    if (cmd == QStringLiteral("CMD:DOWNLOAD-FILE")) {
        out.info = field(data, QStringLiteral("info"));
        out.file = field(data, QStringLiteral("source_file"));
        const QString pl = field(data, QStringLiteral("packet_length"));
        bool ok = false;
        out.packetLength = pl.startsWith(QLatin1String("0x")) ? pl.mid(2).toUInt(&ok, 16) : pl.toUInt(&ok, 16);
        if (!ok || out.packetLength == 0) {
            if (error) *error = QStringLiteral("XML：DOWNLOAD-FILE 的 packet_length 非法（%1）").arg(pl);
            return false;
        }
        out.hasPacketLength = true;
        if (!ack(error))
            return false;
        return true;
    }
    if (cmd == QStringLiteral("CMD:UPLOAD-FILE")) {
        out.info = field(data, QStringLiteral("info"));
        out.file = field(data, QStringLiteral("target_file"));
        if (!ack(error))
            return false;
        return true;
    }
    if (cmd == QStringLiteral("CMD:FILE-SYS-OPERATION")) {
        out.info = field(data, QStringLiteral("key"));
        out.file = field(data, QStringLiteral("file_path"));
        if (!ack(error))
            return false;
        return true;
    }
    if (cmd == QStringLiteral("CMD:END")) {
        out.text = field(data, QStringLiteral("result"));
        if (out.text != QStringLiteral("OK")) {
            const QString msg = field(data, QStringLiteral("message"));
            if (!msg.isEmpty())
                out.text = msg;
        }
        return true;                                        // 由 sendCommand 负责 ack 与后续 CMD:START
    }
    return true;                                            // 其它命令：原样返回 command/text
}

bool XmlSession::sendCommand(const QString &xml, Result *out, bool noack, QString *error)
{
    if (!xsendText(xml, error))
        return false;
    QString resp;
    if (!getResponse(resp, error))
        return false;
    if (resp == QStringLiteral("ERR!UNSUPPORTED")) {         // XL:212-217
        Result r;
        if (readCommandResult(r, nullptr, error)) { /* 排空前导帧 */ }
        if (!ack(nullptr)) { /* 尽力 */ }
        if (error) *error = QStringLiteral("XML：设备不支持该命令（ERR!UNSUPPORTED）");
        return false;
    }
    if (resp.contains(QStringLiteral("ERR!"))) {
        if (error) *error = QStringLiteral("XML：设备返回错误（%1）").arg(resp);
        return false;
    }
    if (resp != QStringLiteral("OK")) {
        if (error) *error = QStringLiteral("XML：命令未被接受（响应 %1）").arg(resp);
        return false;
    }
    if (noack)
        return true;
    Result r;
    if (!readCommandResult(r, nullptr, error))
        return false;
    if (r.command == QStringLiteral("CMD:END")) {
        if (r.text != QStringLiteral("OK")) {
            if (error) *error = QStringLiteral("XML：命令以 CMD:END 结束但结果非 OK（%1）").arg(r.text);
            return false;
        }
        if (!ack(error))
            return false;
        Result s;
        if (!readCommandResult(s, nullptr, error))
            return false;
        if (s.command != QStringLiteral("CMD:START")) {
            if (error) *error = QStringLiteral("XML：CMD:END 之后未收到 CMD:START（收到 %1）").arg(s.command);
            return false;
        }
        if (out) *out = r;
        return true;
    }
    if (out) *out = r;
    return true;
}

} // namespace mtkbrom
```

> ⚠️ **上面 `readCommandResult` 的"保活循环"与 `sendCommand` 的结束握手是**最易错的两段**（上游用 `while data != "OK!EOT"` + 两次 `get_command_result`）。实现时请**逐句对照 `XL:369-449` 与 `XL:188-219`**，把每一行映射到你的代码；**若发现本计划的序列与上游有出入，以上游为准并记录**。

- [ ] **Step 4: 跑测试，确认通过**（预期 7 passed / 0 skipped）

- [ ] **Step 5: 注册测试（CMakeLists.txt）**

```cmake
                elseif(_test_stem STREQUAL "test_mtk_xml_session")
                    set(_test_extra_sources
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_xml_session.cpp
                        ${CMAKE_CURRENT_SOURCE_DIR}/src/core/modes/mtk_brom.cpp)
```
（+ `IMAGE_TEST_SOURCES` 追加；+ 进 libusb 链接名单）

- [ ] **Step 6: 提交**

```bash
git add src/core/modes/mtk_xml_session.h src/core/modes/mtk_xml_session.cpp tests/test_mtk_xml_session.cpp CMakeLists.txt
git commit -m "feat(mtk): XML 帧层（文本帧/OK/OK@0x<len>/OK!EOT 保活/get_command_result 分派）"
```


---

### Task 9: XML 载荷 ①（`CMD:START` 握手 + setup_env / setup_hw_init / set-host-info）

**Files:**
- Create: `src/core/modes/mtk_xml_payload.{h,cpp}`
- Create: `tests/test_mtk_xml_payload.cpp`
- Modify: `CMakeLists.txt`（注册）

**Interfaces:**
- Consumes: `XmlSession`（T8）、`DaSelection`（D1）
- Produces:
  ```cpp
  namespace mtkbrom {
  // DA1 后握手（XL:271-321）：**等设备发 CMD:START 文本消息**（不是 XFlash 的 0xC0 单字节），
  // 随后 setup_env → setup_hw_init → set_host_info。三步的返回值上游不检查（`XL:311-313`）——我们检查。
  bool xmlDa1Handshake(XmlSession &x, QStringList *log, QString *error = nullptr);
  // SET-RUNTIME-PARAMETER（XL:167-186 + XC:120-135）：version="1.1"、battery_exist=AUTO-DETECT、
  //   initialize_dram=YES（**XML 的 DRAM 初始化就这一句，不发 EMI**）、checksum_level=NONE、
  //   da_log_level 字符串（TRACE..ERROR）、log_channel（UART/USB/BOTH）、system_os=LINUX
  bool xmlSetupEnv(XmlSession &x, QString *error = nullptr);
  // setup_hw_init（XL:323-327）：HOST-SUPPORTED-COMMANDS（能力串）+ NOTIFY-INIT-HW，都要 OK。
  // ⚠️ **实施期更正（T9，读实现而非 docstring）**：`NOTIFY-INIT-HW` **没有 `<arg>`** —— 上游 `cmd_notify_init_hw` 调
  //    `create_cmd("NOTIFY-INIT-HW")` 时 content=None，`create_cmd` 的 `if content is not None` 直接跳过（`XC:18-25`/`:32-42`）。
  //    该函数的 **docstring 画的 `<arg></arg>` 与实现不符**（我预核对时只读了 docstring，判定错）。本层 `envelope(cmd)` 对空列表**不写** `<arg>` ✓ 与上游同形。
  bool xmlSetupHwInit(XmlSession &x, QString *error = nullptr);
  // SET-HOST-INFO（XL:329-331 + XC:614）：<info>%Y%m%dT%H%M%S</info>
  bool xmlSetHostInfo(XmlSession &x, QString *error = nullptr);
  }
  ```

- [ ] **Step 1: 写失败用例**（`tests/test_mtk_xml_payload.cpp`；mock 与 T8 同款，复制一份到本文件）

```cpp
// 握手：等 CMD:START → setup_env → setup_hw_init → set_host_info（四条命令各自 OK）
void TestMtkXmlPayload::handshakeSequence()
{
    MockUsbChannel m;
    // ⚠️ **实施期更正（T9）**：`sendCommand(noack=false)` 每条命令消费 **3 帧**（首个 "OK" + CMD:END(OK) + CMD:START），
    //    四命令 = 12 帧，加上设备先发的 CMD:START = **13 帧**（原稿只排 5 帧，会当场错位）。
    m.reads << textReads(QStringLiteral("<host><command>CMD:START</command></host>"))   // 设备先发
            << textReads(QStringLiteral("OK"))                                          // SET-RUNTIME-PARAMETER：接受
            << textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>OK</result></arg></host>"))
            << textReads(QStringLiteral("<host><command>CMD:START</command></host>"))
            << textReads(QStringLiteral("OK"))                                          // HOST-SUPPORTED-COMMANDS：接受
            << textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>OK</result></arg></host>"))
            << textReads(QStringLiteral("<host><command>CMD:START</command></host>"))
            << textReads(QStringLiteral("OK"))                                          // NOTIFY-INIT-HW：接受
            << textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>OK</result></arg></host>"))
            << textReads(QStringLiteral("<host><command>CMD:START</command></host>"))
            << textReads(QStringLiteral("OK"))                                          // SET-HOST-INFO：接受
            << textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>OK</result></arg></host>"))
            << textReads(QStringLiteral("<host><command>CMD:START</command></host>"))
    mtkbrom::XmlSession x(&m);
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::xmlDa1Handshake(x, &log, &err), qPrintable(err));

    // 四条命令的文本（按写帧顺序）：SET-RUNTIME-PARAMETER(1.1/initialize_dram YES) → HOST-SUPPORTED-COMMANDS → NOTIFY-INIT-HW → SET-HOST-INFO
    QStringList sent;
    for (const QByteArray &f : std::as_const(m.writeFrames))
        if (f.startsWith("<?xml")) sent << QString::fromUtf8(f).remove(QChar('\0'));
    QCOMPARE(sent.size(), 4);                     // **四条**命令；CMD:START **不**回 ack（事实报告 §5.4，XL:271-321）
    QVERIFY2(sent.at(0).contains(QStringLiteral("<version>1.1</version>"))
             && sent.at(0).contains(QStringLiteral("CMD:SET-RUNTIME-PARAMETER"))
             && sent.at(0).contains(QStringLiteral("<initialize_dram>YES</initialize_dram>")),
             qPrintable(sent.at(0)));
    QVERIFY(sent.at(1).contains(QStringLiteral("CMD:HOST-SUPPORTED-COMMANDS")));
    QVERIFY(sent.at(1).contains(QStringLiteral("CMD:DOWNLOAD-FILE^1@CMD:FILE-SYS-OPERATION^1@CMD:PROGRESS-REPORT^1@CMD:UPLOAD-FILE^1@")));
    QVERIFY(sent.at(2).contains(QStringLiteral("CMD:NOTIFY-INIT-HW")));
    QVERIFY(sent.at(3).contains(QStringLiteral("CMD:SET-HOST-INFO")));
    QVERIFY2(log.join('\n').contains(QStringLiteral("CMD:START")), qPrintable(log.join('\n')));
}

// DA 日志帧（DT_MESSAGE，16B 头）夹在文本响应之前 → 必须**跳过**并把日志交给 sink，仍读到 "OK"
void TestMtkXmlSession::logFrameIsSkippedNotFatal()
{
    MockUsbChannel m;
    const QByteArray logMsg("da: init dram ok", 17);
    // 日志帧：16B 头（含 priority）+ NUL 结尾的文本；随后才是协议响应 "OK"
    m.reads << le32(0xFEEEEEEF) + le32(2) + le32(quint32(logMsg.size() + 1)) + le32(0)   // DT_MESSAGE + priority
            << logMsg + QByteArray(1, '\0')
            << textReads(QStringLiteral("OK"));
    mtkbrom::XmlSession x(&m);
    QStringList logs;
    x.setLogSink([&logs](const QString &s) { logs << s; });
    QString text;
    QString err;
    QVERIFY2(x.getResponse(text, &err), qPrintable(err));
    QCOMPARE(text, QStringLiteral("OK"));
    QCOMPARE(logs.size(), 1);
    QVERIFY2(logs.at(0).contains(QStringLiteral("init dram")), qPrintable(logs.at(0)));
    QCOMPARE(m.reads.size(), 0);                       // 两帧都读掉了（日志帧没有被留在队列里）
}

// 设备没发 CMD:START（发别的命令）→ 明确失败
void TestMtkXmlPayload::handshakeRejectsUnexpectedFirstCommand()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("<host><command>CMD:PROGRESS-REPORT</command></host>"))
            << textReads(QStringLiteral("OK!EOT"))
            << textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>OK</result></arg></host>"));
    mtkbrom::XmlSession x(&m);
    QString err;
    QVERIFY(!mtkbrom::xmlDa1Handshake(x, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("CMD:START")), qPrintable(err));
}

// setup_env 的命令文本要素（版本 1.1 / initialize_dram / checksum_level / system_os / log_channel）
void TestMtkXmlPayload::setupEnvPayloadFields()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("OK"));
    mtkbrom::XmlSession x(&m);
    QString err;
    QVERIFY2(mtkbrom::xmlSetupEnv(x, &err), qPrintable(err));
    const QString sent = QString::fromUtf8(m.writeFrames.at(1)).remove(QChar('\0'));
    QVERIFY(sent.contains(QStringLiteral("<version>1.1</version>")));
    QVERIFY(sent.contains(QStringLiteral("<battery_exist>AUTO-DETECT</battery_exist>")));
    QVERIFY(sent.contains(QStringLiteral("<checksum_level>NONE</checksum_level>")));
    QVERIFY(sent.contains(QStringLiteral("<system_os>LINUX</system_os>")));
    QVERIFY(sent.contains(QStringLiteral("<log_channel>UART</log_channel>")));
    QVERIFY(sent.contains(QStringLiteral("<initialize_dram>YES</initialize_dram>")));
}
```

- [ ] **Step 2: 跑一遍，确认失败**（先按 Step 5 注册）

- [ ] **Step 3: 实现**（`mtk_xml_payload.cpp`）

```cpp
#include "core/modes/mtk_xml_payload.h"

#include <QDateTime>

namespace mtkbrom {

namespace {
// SET-RUNTIME-PARAMETER 的**整段** XML（含 <arg> 与独立的 <adv> 块，`XC:120-135`）——信封的通用形式装不下 adv
QString setRuntimeParameterXml()
{
    return QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><da><version>1.1</version>"
                          "<command>CMD:SET-RUNTIME-PARAMETER</command><arg>"
                          "<checksum_level>NONE</checksum_level>"
                          "<battery_exist>AUTO-DETECT</battery_exist>"
                          "<da_log_level>INFO</da_log_level>"
                          "<log_channel>UART</log_channel>"
                          "<system_os>LINUX</system_os>"
                          "</arg><adv><initialize_dram>YES</initialize_dram></adv></da>");
}
} // namespace

bool xmlSetupEnv(XmlSession &x, QString *error)
{
    return x.sendCommand(setRuntimeParameterXml(), nullptr, false, error);
}

bool xmlSetupHwInit(XmlSession &x, QString *error)
{
    // HOST-SUPPORTED-COMMANDS 的能力串（`XC:139-140` 默认值，caret 分隔、@ 结尾）
    const QString caps = QStringLiteral("CMD:DOWNLOAD-FILE^1@CMD:FILE-SYS-OPERATION^1@"
                                        "CMD:PROGRESS-REPORT^1@CMD:UPLOAD-FILE^1@");
    const QString cmd1 = XmlSession::envelope(QStringLiteral("HOST-SUPPORTED-COMMANDS"),
                                              {QStringLiteral("<host_capability>%1</host_capability>").arg(caps)});
    if (!x.sendCommand(cmd1, nullptr, false, error))
        return false;
    const QString cmd2 = XmlSession::envelope(QStringLiteral("NOTIFY-INIT-HW"));
    return x.sendCommand(cmd2, nullptr, false, error);
}

bool xmlSetHostInfo(XmlSession &x, QString *error)
{
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddThhmmss"));
    const QString cmd = XmlSession::envelope(QStringLiteral("SET-HOST-INFO"),
                                             {QStringLiteral("<info>%1</info>").arg(stamp)});
    return x.sendCommand(cmd, nullptr, false, error);
}

bool xmlDa1Handshake(XmlSession &x, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    XmlSession::Result first;
    if (!x.readCommandResult(first, log, error))
        return false;
    if (first.command != QStringLiteral("CMD:START")) {
        if (error) *error = QStringLiteral("XML：DA1 后期待 CMD:START，收到 %1")
                                .arg(first.command.isEmpty() ? QStringLiteral("(空)") : first.command);
        return false;
    }
    say(QStringLiteral("XML：收到 CMD:START（DA1 就绪）"));
    if (!xmlSetupEnv(x, error))
        return false;
    if (!xmlSetupHwInit(x, error))
        return false;
    if (!xmlSetHostInfo(x, error))
        return false;
    say(QStringLiteral("XML：setup_env / setup_hw_init / set_host_info 完成"));
    return true;
}

} // namespace mtkbrom
```

- [ ] **Step 4: 跑测试，确认通过**（预期 3 passed / 0 skipped）

- [ ] **Step 5: CMake**：注册 `test_mtk_xml_payload`（额外源：`mtk_xml_session.cpp`、`mtk_xml_payload.cpp`、`mtk_brom.cpp`）+ 进 libusb 名单

- [ ] **Step 6: 提交**

```bash
git add src/core/modes/mtk_xml_payload.h src/core/modes/mtk_xml_payload.cpp tests/test_mtk_xml_payload.cpp CMakeLists.txt
git commit -m "feat(mtk): XML 载荷①（CMD:START 握手 + setup_env/setup_hw_init/set-host-info）"
```

---

### Task 10: XML 载荷 ②（WRITE-FLASH / READ-FLASH 与数据流）

**Files:**
- Modify: `src/core/modes/mtk_xml_payload.{h,cpp}`
- Modify: `tests/test_mtk_xml_payload.cpp`

**Interfaces:**
- Consumes: T8/T9 的产物
- Produces:
  ```cpp
  namespace mtkbrom {
  // 写一个分区（XL:943-980 + upload `:451-506` 逐句）：
  //   ① WRITE-FLASH（noack）→ ② FileSysOp(key 必须 "FILE-SIZE"）→ ③ ackValue(**length**) → ④ 读 DwnFile（拿 packet_length）
  //   → ⑤ **再 ackValue(length) 一次 + 读 "OK"**（`upload()` 内部自己又发一次长度 ack，见 `:459-461`；**漏掉它会整体错位**）
  //   → ⑥ 数据补零到 512 → ⑦ 逐包：ackValue(0) → 读 "OK" → xsend(块) → 读 "OK"
  //   → ⑧ ack() → 读 CMD:END(OK) → ack() → 读 CMD:START
  // mem_offset 恒 0x8000000（`XFL` 无关；`XC:461` 的默认值），source_file = MEM://0x<mem_offset>:0x<length>
  bool xmlWritePartition(XmlSession &x, const QString &partition, const QByteArray &data,
                         QStringList *log = nullptr, QString *error = nullptr);
  // 数据帧读取（**上游 download_raw 形状**，`XL:508-589`）：读 "OK@0x<len>" → ack → 读 "OK" → ack
  //   → 循环{ 收一帧 → ack → 读 "OK" → ack } → 返回数据。**不要**复用 `XmlSession::readCommandResult` 的裸 OK@ 路径
  //   —— 那条对应 `get_command_result`（单次尾 ack），节奏不同（T8 实施期实测 + 控制方核对）。
  bool xmlReadDataFrames(XmlSession &x, quint32 length, QByteArray &out, QString *error = nullptr);
  // 读一个分区（XL:918-941）：READ-FLASH（noack）→ UpFile → **xmlReadDataFrames（逐帧 ack）** → 收尾 CMD:START
  bool xmlReadPartition(XmlSession &x, const QString &partition, quint64 offset, quint32 length,
                        QByteArray &out, QString *error = nullptr);
  // 收尾复位（XC:439 REBOOT；IMMEDIATE / DISCONNECT）
  bool xmlReboot(XmlSession &x, bool disconnect = true, QString *error = nullptr);
  }
  ```

- [ ] **Step 1: 写失败用例**（追加到 `tests/test_mtk_xml_payload.cpp`）

```cpp
// 写一个分区：命令文本 + FileSysOp/ackValue/DwnFile + 数据包循环 + END/START 收尾
void TestMtkXmlPayload::writePartitionSequence()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("OK"))                                     // ① WRITE-FLASH 被接受（noack）
            << textReads(QStringLiteral("<host><command>CMD:FILE-SYS-OPERATION</command><arg>"
                                        "<key>FILE-SIZE</key><file_path>MEM://0x8000000:0x600</file_path></arg></host>"))
            // ⚠️ 顺序（T10 预核对更正）：③ ack(length) 的应答就是 **DwnFile**；⑤ 再 ack(length) 的应答才是 "OK"
            << textReads(QStringLiteral("<host><command>CMD:DOWNLOAD-FILE</command><arg>"
                                        "<checksum>CHK_NO</checksum><info>2nd-DA</info>"
                                        "<source_file>MEM://0x8000000:0x600</source_file>"
                                        "<packet_length>0x400</packet_length></arg></host>"))
            << textReads(QStringLiteral("OK"))                                     // ⑤ 第二次 ack(length) 的应答
            << textReads(QStringLiteral("OK"))                                     // ⑥ 第一包前的 ack(0) 响应
            << textReads(QStringLiteral("OK"))                                     // 第一包数据后的响应
            << textReads(QStringLiteral("OK"))                                     // 第二包前的 ack(0) 响应
            << textReads(QStringLiteral("OK"))                                     // 第二包数据后的响应
            << textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>OK</result></arg></host>"))
            << textReads(QStringLiteral("OK"))                                     // CMD:END 后的 ack 响应
            << textReads(QStringLiteral("<host><command>CMD:START</command></host>"));
    mtkbrom::XmlSession x(&m);
    const QByteArray data(0x600, '\x42');                                          // 0x600 → 两包（0x400 + 0x200）
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::xmlWritePartition(x, QStringLiteral("EMMC-USER"), data, &log, &err), qPrintable(err));

    QStringList cmds;
    for (const QByteArray &f : std::as_const(m.writeFrames))
        if (f.startsWith("<?xml")) cmds << QString::fromUtf8(f).remove(QChar('\0'));
    QVERIFY(cmds.at(0).contains(QStringLiteral("CMD:WRITE-FLASH")));
    QVERIFY(cmds.at(0).contains(QStringLiteral("MEM://0x8000000:0x600")));
    QVERIFY(cmds.at(1).startsWith(QStringLiteral("OK@0x600")));                    // ③ ackValue(**length**)
    int ackZeros = 0, dataFrames = 0;
    for (const QByteArray &f : std::as_const(m.writeFrames)) {
        if (f.startsWith("OK@0x0")) ++ackZeros;
        if (f.size() == 0x400 || f.size() == 0x200) ++dataFrames;
    }
    QCOMPARE(ackZeros, 2);
    QCOMPARE(dataFrames, 2);
}

// FileSysOp 的 key 不是 FILE-SIZE → 明确失败（XL:961-962）
void TestMtkXmlPayload::writePartitionRejectsWrongFileSysOpKey()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("OK"))
            << textReads(QStringLiteral("<host><command>CMD:FILE-SYS-OPERATION</command><arg>"
                                        "<key>OTHER</key><file_path>MEM://0x8000000:0x100</file_path></arg></host>"));
    mtkbrom::XmlSession x(&m);
    QString err;
    QVERIFY(!mtkbrom::xmlWritePartition(x, QStringLiteral("EMMC-USER"), QByteArray(0x100, '\x01'), nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("FILE-SIZE")), qPrintable(err));
}

// 读一个分区：READ-FLASH（noack）→ UpFile → 裸 OK@ 数据 → CMD:START
void TestMtkXmlPayload::readPartitionSequence()
{
    MockUsbChannel m;
    const QByteArray payload(0x200, '\x77');
    // ⚠️ 逐帧形状（T10 预核对更正，上游 download_raw `XL:508-589`）：收一帧 → ack → 读 "OK" → **再 ack**
    m.reads << textReads(QStringLiteral("OK"))                                     // READ-FLASH 被接受（noack）
            << textReads(QStringLiteral("<host><command>CMD:UPLOAD-FILE</command><arg>"
                                        "<checksum>CHK_NO</checksum><info>ROM_0</info>"
                                        "<target_file>ROM_0</target_file></arg></host>"))
            << textReads(QStringLiteral("OK@0x200"))                               // 裸数据路径：长度
            << textReads(QStringLiteral("OK"))                                     // 长度确认（首个 ack 的应答）
            << frameReads(1, payload)                                              // 数据帧（头、载荷两笔）
            << textReads(QStringLiteral("OK"))                                     // 逐帧确认（该帧第一个 ack 的应答）
            // 该帧第二个 ack 之后循环结束 → 收尾读 CMD:START（由 readCommandResult 消费）
            << textReads(QStringLiteral("<host><command>CMD:START</command></host>"));
    mtkbrom::XmlSession x(&m);
    QByteArray got;
    QString err;
    QVERIFY2(mtkbrom::xmlReadPartition(x, QStringLiteral("EMMC-USER"), 0x0, 0x200, got, &err), qPrintable(err));
    QCOMPARE(got, payload);
}
```

- [ ] **Step 2: 跑一遍，确认失败**

- [ ] **Step 3: 实现**（追加到 `mtk_xml_payload.cpp`）

```cpp
namespace {
constexpr quint32 kMemOffset = 0x8000000;      // XC:461 的默认 mem_offset（source_file 的 MEM:// 基址）

QString memDescriptor(quint32 length)
{
    return QStringLiteral("MEM://0x%1:0x%2").arg(kMemOffset, 0, 16).arg(length, 0, 16);
}

// 数据补零到 512 的整数倍（XL:969-972）
QByteArray padTo512(const QByteArray &data)
{
    if (data.size() % 512 == 0)
        return data;
    QByteArray out = data;
    out.append(QByteArray(512 - (data.size() % 512), '\0'));
    return out;
}
} // namespace

bool xmlWritePartition(XmlSession &x, const QString &partition, const QByteArray &data,
                       QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    if (data.isEmpty()) {
        if (error) *error = QStringLiteral("XML 写：分区 %1 的数据为空").arg(partition);
        return false;
    }
    const quint32 length = quint32(padTo512(data).size());
    // ① WRITE-FLASH（noack：只等 "OK"，不消费 END/START —— 收尾在 ⑦）
    const QString cmd = XmlSession::envelope(
        QStringLiteral("WRITE-FLASH"),
        {QStringLiteral("<partition>%1</partition>").arg(partition),
         QStringLiteral("<source_file>%1</source_file>").arg(memDescriptor(length))});
    if (!x.sendCommand(cmd, nullptr, /*noack=*/true, error))
        return false;
    // ② FileSysOp：key 必须 FILE-SIZE（XL:961-962）
    XmlSession::Result fileOp;
    if (!x.readCommandResult(fileOp, nullptr, error))
        return false;
    if (fileOp.command != QStringLiteral("CMD:FILE-SYS-OPERATION") || fileOp.info != QStringLiteral("FILE-SIZE")) {
        if (error) *error = QStringLiteral("XML 写：期待 FILE-SYS-OPERATION 且 key=FILE-SIZE（收到 %1/%2）")
                                .arg(fileOp.command, fileOp.info);
        return false;
    }
    // ③ ackValue(**length**) → ④ DwnFile（拿 packet_length）
    if (!x.ackValue(length, error))
        return false;
    XmlSession::Result dwn;
    if (!x.readCommandResult(dwn, nullptr, error))
        return false;
    if (dwn.command != QStringLiteral("CMD:DOWNLOAD-FILE") || !dwn.hasPacketLength) {
        if (error) *error = QStringLiteral("XML 写：期待 CMD:DOWNLOAD-FILE（收到 %1）").arg(dwn.command);
        return false;
    }
    // ⑤ **再确认一次长度**：上游 `upload()` 自己又发一次 `ack_value(length)` 并等一个 "OK"（`XL:459-461`）——
    //    漏掉这一发一读，后面每一帧都会错位（计划期预核对抓到的差异）。
    if (!x.ackValue(length, error))
        return false;
    QString dwnOk;
    if (!x.getResponse(dwnOk, error))
        return false;
    if (!dwnOk.contains(QStringLiteral("OK"))) {
        if (error) *error = QStringLiteral("XML 写：DOWNLOAD-FILE 之后未获 OK（收到 %1）").arg(dwnOk);
        return false;
    }
    // ⑥⑦ 数据包循环（XL:472-489）：每包先 ackValue(0) 期望 OK，再发原始帧块期望 OK
    const QByteArray padded = padTo512(data);
    for (int pos = 0; pos < padded.size(); pos += int(dwn.packetLength)) {
        const QByteArray block = padded.mid(pos, int(dwn.packetLength));
        if (!x.ackValue(0, error))
            return false;
        QString resp;
        if (!x.getResponse(resp, error))
            return false;
        if (!resp.contains(QStringLiteral("OK"))) {
            if (error) *error = QStringLiteral("XML 写：偏移 %1 处的 ack(0) 未获 OK（%2）").arg(pos).arg(resp);
            return false;
        }
        if (!x.xsendBytes(block, 1, error))
            return false;
        if (!x.getResponse(resp, error))
            return false;
        if (!resp.contains(QStringLiteral("OK"))) {
            if (error) *error = QStringLiteral("XML 写：偏移 %1 处的数据未被接受（%2）").arg(pos).arg(resp);
            return false;
        }
    }
    // ⑦ 收尾（raw 路径，XL:490-506）：ack → CMD:END(OK) → ack → CMD:START
    if (!x.ack(error))
        return false;
    XmlSession::Result end;
    if (!x.readCommandResult(end, nullptr, error))
        return false;
    if (end.command != QStringLiteral("CMD:END") || end.text != QStringLiteral("OK")) {
        if (error) *error = QStringLiteral("XML 写：收尾期待 CMD:END(OK)（收到 %1/%2）").arg(end.command, end.text);
        return false;
    }
    if (!x.ack(error))
        return false;
    XmlSession::Result start;
    if (!x.readCommandResult(start, nullptr, error))
        return false;
    if (start.command != QStringLiteral("CMD:START")) {
        if (error) *error = QStringLiteral("XML 写：收尾后未收到 CMD:START（收到 %1）").arg(start.command);
        return false;
    }
    say(QStringLiteral("XML 写：分区 %1 共 %2 字节完成（packet_length = 0x%3）")
            .arg(partition).arg(padded.size()).arg(dwn.packetLength, 0, 16));
    return true;
}

bool xmlReadPartition(XmlSession &x, const QString &partition, quint64 offset, quint32 length,
                      QByteArray &out, QString *error)
{
    const QString cmd = XmlSession::envelope(
        QStringLiteral("READ-FLASH"),
        {QStringLiteral("<partition>%1</partition>").arg(partition),
         QStringLiteral("<offset>0x%1</offset>").arg(offset, 0, 16),
         QStringLiteral("<length>0x%1</length>").arg(length, 0, 16),
         QStringLiteral("<target_file>ROM_0</target_file>")});
    if (!x.sendCommand(cmd, nullptr, /*noack=*/true, error))
        return false;
    XmlSession::Result up;
    if (!x.readCommandResult(up, nullptr, error))
        return false;
    if (up.command != QStringLiteral("CMD:UPLOAD-FILE")) {
        if (error) *error = QStringLiteral("XML 读：期待 CMD:UPLOAD-FILE（收到 %1）").arg(up.command);
        return false;
    }
    XmlSession::Result dataRes;
    if (!x.readCommandResult(dataRes, nullptr, error))     // 裸 OK@ 路径（T8 已实现）
        return false;
    if (dataRes.bytes.isEmpty()) {
        if (error) *error = QStringLiteral("XML 读：未收到数据（裸 OK@ 路径返回空）");
        return false;
    }
    out = dataRes.bytes;
    XmlSession::Result start;
    if (!x.readCommandResult(start, nullptr, error))
        return false;
    if (start.command != QStringLiteral("CMD:START")) {
        if (error) *error = QStringLiteral("XML 读：收尾未收到 CMD:START（收到 %1）").arg(start.command);
        return false;
    }
    return true;
}

bool xmlReboot(XmlSession &x, bool disconnect, QString *error)
{
    const QString action = disconnect ? QStringLiteral("DISCONNECT") : QStringLiteral("IMMEDIATE");
    const QString cmd = XmlSession::envelope(QStringLiteral("REBOOT"),
                                             {QStringLiteral("<action>%1</action>").arg(action)});
    return x.sendCommand(cmd, nullptr, /*noack=*/false, error);
}

} // namespace mtkbrom
```

- [ ] **Step 4: 跑测试，确认通过**（预期 6 passed / 0 skipped）

- [ ] **Step 5: 提交**

```bash
git add src/core/modes/mtk_xml_payload.h src/core/modes/mtk_xml_payload.cpp tests/test_mtk_xml_payload.cpp
git commit -m "feat(mtk): XML 载荷②（WRITE-FLASH/READ-FLASH + 数据包循环 + END/START 收尾 + REBOOT）"
```


---

### Task 11: 三代路由 + XFlash 链整合（`bromFlashOnSession` 分派）

**Files:**
- Modify: `src/core/modes/mtk_payload.{h,cpp}`（新增 `xflashBringUpDa` + 路由分派）
- Modify: `tests/test_mtk_payload.cpp`
- Modify: `CMakeLists.txt`（`test_mtk_payload` 追加 `mtk_xflash_session.cpp`、`mtk_xflash_payload.cpp`、`mtk_gpt.cpp`）

**Interfaces:**
- Consumes: T1–T7 全部产物；D1 的 `bromBringUpDa`/`flashPartition`/计划层
- Produces:
  ```cpp
  namespace mtkbrom {
  enum class MtkGeneration { Legacy, XFlash, Xml };
  // 纯函数（单测）：芯片表 damode + DA 文件 v6 → 代际（`DC:216`）。表外芯片/iot/v6 与 damode 冲突 → false + 明确 error
  bool decideGeneration(const ChipInfo *chip, bool daIsV6, MtkGeneration &out, QString *error = nullptr);

  // XFlash 引导链（可离线测，不含枚举/打开）：
  //   sendDa1(region[1]) → 读 1B == 0xC0 → xflashDa1Handshake（七步）→ xflashBringUpSteps（四步）
  //   → connection_agent 判定（preloader=跳过 EMI / brom=需要 / 其它=失败）
  //   → [需要 EMI:] extractEmiXflash + xflashSendEmi（缺 preloader → **告警不中止**，上游同姿态）
  //   → xflashBootTo(region[2].m_start_addr, **剥签名的 da2**)
  bool xflashBringUpDa(BromSession &brom, XFlashSession &x, const DaSelection &sel,
                       const PreloaderResult &pre, QStringList *log, QString *error = nullptr);
  // 日志用名（"LEGACY"/"XFLASH"/"XML"）——纯函数
  QString generationName(MtkGeneration g);
  // GPT 读回调适配器：mtkgpt::ReadFn 的 (byteOffset, len) → XFlash READ_DATA（storage/parttype 见 ST:16-49）
  mtkgpt::ReadFn xflashSectorReader(XFlashSession &x, quint32 storage = 0x1, quint32 partType = 0x8);
  }
  ```
  并在 `bromFlashOnSession` 里把"选完 DA 之后"的路径按 `decideGeneration` 分派：
  - `Legacy` → D1 既有（`bromBringUpDa` + PMT + `flashPartition`）
  - `XFlash` → `xflashBringUpDa` + GPT/PMT + `xflashWriteData`
  - `Xml` → T12 的 XML 链（本任务先留 `return false` + 明确文案"XML 链见 Task 12"，T12 替换）

- [ ] **Step 1: 写失败用例**（追加到 `tests/test_mtk_payload.cpp`）

先在文件顶部补 include 与助手（**本任务需要它们，D1 的这份测试文件里没有**）：

```cpp
// 追加 include（D1 的 include 之后）
#include "core/modes/mtk_gpt.h"
#include "core/modes/mtk_xflash_session.h"
#include "core/modes/mtk_xflash_payload.h"
#include "core/modes/mtk_xml_session.h"
#include "core/modes/mtk_xml_payload.h"

// 追加到匿名 namespace（与 be32 并列）
QByteArray le16(quint16 v)
{
    QByteArray b(2, '\0');
    b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
    return b;
}
QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
    return b;
}
// ⚠️ **一帧 = 两笔队列项**：默认 `IBromUsb::readExact` 是"单次读 + 严格长度"（`mtk_brom.cpp:219-231`），
// XFlash/XML 层先读 12B 帧头、再读载荷 —— 每次 readExact 消耗**一笔**队列项。把整帧塞成一笔会让"读头"
// 吃掉载荷、后续读全部错位。本文件 D1 的 LEGACY 夹具（逐字节 read）不受此约束，保持原样。
QList<QByteArray> frameReads(quint32 dt, const QByteArray &payload)
{
    return {le32(0xFEEEEEEF) + le32(dt) + le32(quint32(payload.size())), payload};
}
QList<QByteArray> statusReads(quint32 code) { return frameReads(1, le32(code)); }
QList<QByteArray> textReads(const QString &s)     // XML 文本帧（T12 用；同一拆分规则）
{
    const QByteArray body = s.toUtf8() + QByteArray(1, '\0');
    return {le32(0xFEEEEEEF) + le32(1) + le32(quint32(body.size())), body};
}
```

并把本任务与 T12 的新用例名加进 `TestMtkPayload` 的 `private slots:`（**漏加 = 用例不会跑，且 ctest 仍是绿的**）：

```cpp
    void decideGenerationMatrix();
    void xflashChainOrderWithEmi();
    void xflashChainSkipsEmiForPreloaderAgent();
    void xflashChainWarnsButContinuesWithoutPreloader();
    void xmlChainSendsCmdStartHandshakeWithoutEmi();
    void xmlChainRejectsNonStartFirstCommand();
    void xmlSectorReaderRoundTripsDeviceBytes();
```

```cpp
// 代际判定：表外 / iot / damode 与 v6 的优先关系（v6 强制 XML，DC:216）
void TestMtkPayload::decideGenerationMatrix()
{
    mtkbrom::ChipInfo legacy;
    legacy.hwCode = 0x6752; legacy.damode = mtkbrom::DaMode::Legacy; legacy.iot = false;
    mtkbrom::ChipInfo iot = legacy; iot.hwCode = 0x6226; iot.iot = true;
    mtkbrom::ChipInfo xf = legacy; xf.hwCode = 0x6765; xf.damode = mtkbrom::DaMode::XFlash;
    mtkbrom::ChipInfo xml = legacy; xml.hwCode = 0x0907; xml.damode = mtkbrom::DaMode::Xml;

    mtkbrom::MtkGeneration g = mtkbrom::MtkGeneration::Xml;
    QString err;
    QVERIFY(mtkbrom::decideGeneration(&legacy, false, g, &err));
    QCOMPARE(g, mtkbrom::MtkGeneration::Legacy);
    QVERIFY(mtkbrom::decideGeneration(&xf, false, g, &err));
    QCOMPARE(g, mtkbrom::MtkGeneration::XFlash);
    QVERIFY(mtkbrom::decideGeneration(&xml, false, g, &err));
    QCOMPARE(g, mtkbrom::MtkGeneration::Xml);
    QVERIFY(mtkbrom::decideGeneration(&legacy, /*v6=*/true, g, &err));      // **v6 强制 XML**
    QCOMPARE(g, mtkbrom::MtkGeneration::Xml);
    err.clear();
    QVERIFY(!mtkbrom::decideGeneration(nullptr, false, g, &err));           // 表外
    QVERIFY(!err.isEmpty());
    err.clear();
    QVERIFY(!mtkbrom::decideGeneration(&iot, false, g, &err));              // IoT 明确拒绝
    QVERIFY2(err.contains(QStringLiteral("IoT")), qPrintable(err));
}

// XFlash 全链（常规路径 agent=brom 且有 preloader）：0xC0 → 七步握手 → 四步 bring-up
//   → INIT_EXT_RAM + EMI → boot_to（**已剥签名**的 DA2）
void TestMtkPayload::xflashChainOrderWithEmi()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x6765, sel, &err), qPrintable(err));

    // preloader 夹具：T3 的 extractEmiXflash 切片 = **整个窗口**（不是 MTK_BIN+0xC），期望原样出现在写流里
    const QByteArray preBytes = mtktest::buildEmiPreloader("38", QByteArray(0x340, '\xA5'));
    mtkbrom::PreloaderResult pre;
    pre.origin = mtkbrom::PreloaderOrigin::Explicit;
    pre.path = QStringLiteral("/tmp/preloader_6765.bin");
    pre.bytes = preBytes;

    // 读队列：D1 的 DA1 上传（9 笔，含 SEND_DA + JUMP_DA）→ 0xC0 → 握手（**2×status** + SYNC 回读）
    //        → bring-up 四步（**12×status + 2 回包**）→ EMI 两笔 → boot_to 三笔。**一帧 = 两笔**（见 T2 注释）
    // （数字按 T4 实测更正：SYNC 只发不读；四个查询的尾部 status 见 T4 的逐查询表）
    m->reads = da1UploadReads(sel);
    m->reads << QByteArray("\xC0", 1)
             << statusReads(0) << statusReads(0) << frameReads(1, le32(0x434E5953))
             << statusReads(0) << statusReads(0) << frameReads(1, QByteArray("0x20240101")) << statusReads(0)
             << statusReads(0) << statusReads(0) << statusReads(0)                // set_reset_key（2 + 参数 status）
             << statusReads(0) << statusReads(0) << statusReads(0)                // set_checksum_level（同上）
             << statusReads(0) << statusReads(0) << frameReads(1, QByteArray("brom")) << statusReads(0)
             << statusReads(0) << statusReads(0)                                  // INIT_EXT_RAM / EMI 数据
             << statusReads(0) << statusReads(0) << statusReads(0);               // BOOT_TO / 数据 / 最终 status

    mtkbrom::BromSession brom(std::move(usb), mtkbrom::BromDevice{});
    mtkbrom::XFlashSession x(m, 0x6765);
    QStringList log;
    QVERIFY2(mtkbrom::xflashBringUpDa(brom, x, sel, pre, &log, &err), qPrintable(err));

    // 顺序断言：在**字节流**上比首次出现下标（0x434E5953 / 0x01000A / 0x010008 都是本链独占值）
    const QByteArray &stream = m->writes;
    const int iSync = stream.indexOf(le32(0x434E5953));
    const int iEmi  = stream.indexOf(le32(mtkbrom::X_CMD_INIT_EXT_RAM));
    const int iBoot = stream.indexOf(le32(mtkbrom::X_CMD_BOOT_TO));
    QVERIFY2(iSync >= 0, "缺 XFlash SYNC 帧 —— 七步握手没跑");
    QVERIFY2(iEmi > iSync, "INIT_EXT_RAM 必须在七步握手之后");
    QVERIFY2(iBoot > iEmi, "BOOT_TO 必须在 EMI 之后");
    QVERIFY2(stream.contains(preBytes), "EMI 必须原样发出（XFlash 整块切片）");

    // DA2 剥签名：写流里**有** da2NoSig、**没有**完整 da2Bytes
    // （夹具按 region 填 0x01/0x02/0x03，两串在本链里都唯一 → "contains" 判据有判别力）
    const QByteArray da2NoSig = sel.da2Bytes.left(sel.da2Bytes.size() - int(sel.da2.sigLen));
    QCOMPARE(sel.da2.sigLen, 16u);
    QVERIFY2(stream.contains(da2NoSig), "剥签名后的 DA2 必须发出");
    QVERIFY2(!stream.contains(sel.da2Bytes), "完整 DA2（含签名）出现在写流里 —— 签名没剥掉");
    QVERIFY2(log.join(QLatin1Char('\n')).contains(QStringLiteral("剥签名")),
             qPrintable(log.join(QLatin1Char('\n'))));
}

// connection_agent = "preloader" → **不发 EMI**（铁律 9）；此时**不需要** preloader（origin=None 也不告警）
void TestMtkPayload::xflashChainSkipsEmiForPreloaderAgent()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x6765, sel, &err), qPrintable(err));

    const mtkbrom::PreloaderResult pre;                     // origin = None
    m->reads = da1UploadReads(sel);
    m->reads << QByteArray("\xC0", 1)
             << statusReads(0) << statusReads(0) << frameReads(1, le32(0x434E5953))
             << statusReads(0) << statusReads(0) << frameReads(1, QByteArray("0x20240101")) << statusReads(0)
             << statusReads(0) << statusReads(0) << statusReads(0)
             << statusReads(0) << statusReads(0) << statusReads(0)
             << statusReads(0) << statusReads(0) << frameReads(1, QByteArray("preloader")) << statusReads(0)
             << statusReads(0) << statusReads(0) << statusReads(0);

    mtkbrom::BromSession brom(std::move(usb), mtkbrom::BromDevice{});
    mtkbrom::XFlashSession x(m, 0x6765);
    QStringList log;
    QVERIFY2(mtkbrom::xflashBringUpDa(brom, x, sel, pre, &log, &err), qPrintable(err));
    QVERIFY2(m->writes.indexOf(le32(mtkbrom::X_CMD_INIT_EXT_RAM)) == -1,
             "preloader agent 下不得发 INIT_EXT_RAM");
    QVERIFY2(m->writes.indexOf(le32(mtkbrom::X_CMD_BOOT_TO)) >= 0, "boot_to 仍必须执行");
    QVERIFY2(log.join(QLatin1Char('\n')).contains(QStringLiteral("跳过 EMI")),
             qPrintable(log.join(QLatin1Char('\n'))));
}

// agent = "brom" 但**没有** preloader → 只告警、继续（上游同姿态），**不发 EMI**
void TestMtkPayload::xflashChainWarnsButContinuesWithoutPreloader()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x6765, sel, &err), qPrintable(err));

    mtkbrom::PreloaderResult pre;                           // origin = None + skipReason
    pre.skipReason = QStringLiteral("未提供 preloader 路径");
    m->reads = da1UploadReads(sel);
    m->reads << QByteArray("\xC0", 1)
             << statusReads(0) << statusReads(0) << frameReads(1, le32(0x434E5953))
             << statusReads(0) << statusReads(0) << frameReads(1, QByteArray("0x20240101")) << statusReads(0)
             << statusReads(0) << statusReads(0) << statusReads(0)
             << statusReads(0) << statusReads(0) << statusReads(0)
             << statusReads(0) << statusReads(0) << frameReads(1, QByteArray("brom")) << statusReads(0)
             << statusReads(0) << statusReads(0) << statusReads(0);

    mtkbrom::BromSession brom(std::move(usb), mtkbrom::BromDevice{});
    mtkbrom::XFlashSession x(m, 0x6765);
    QStringList log;
    QVERIFY2(mtkbrom::xflashBringUpDa(brom, x, sel, pre, &log, &err), qPrintable(err));
    QVERIFY2(m->writes.indexOf(le32(mtkbrom::X_CMD_INIT_EXT_RAM)) == -1, "无 preloader → 无 EMI 可发");
    QVERIFY2(log.join(QLatin1Char('\n')).contains(QStringLiteral("无 preloader")),
             qPrintable(log.join(QLatin1Char('\n'))));
    QVERIFY2(log.join(QLatin1Char('\n')).contains(QStringLiteral("未提供 preloader 路径")),
             "skipReason 必须转述进日志");
}
```

- [ ] **Step 2: 跑一遍，确认失败**

- [ ] **Step 3: 实现**

`decideGeneration`（纯函数，逐字照 `DC:216` 的优先级）：

```cpp
bool decideGeneration(const ChipInfo *chip, bool daIsV6, MtkGeneration &out, QString *error)
{
    if (!chip) {
        if (error) *error = QStringLiteral("芯片表未收录该 hw_code —— 判不出代际（不猜）");
        return false;
    }
    if (chip->iot) {
        if (error) *error = QStringLiteral("IoT 芯片（hw_code=0x%1）：三代映射均未实现，明确拒绝")
                                .arg(chip->hwCode, 4, 16, QLatin1Char('0'));
        return false;
    }
    if (daIsV6 || chip->damode == DaMode::Xml) {   // v6 **强制** XML（DC:216），优先于表
        out = MtkGeneration::Xml;
        return true;
    }
    out = (chip->damode == DaMode::XFlash) ? MtkGeneration::XFlash : MtkGeneration::Legacy;
    return true;
}
```

`generationName` 与 `xflashSectorReader`（两个都在 `mtk_payload.cpp` 的 `mtkbrom` 命名空间内）：

```cpp
QString generationName(MtkGeneration g)
{
    switch (g) {
    case MtkGeneration::Legacy: return QStringLiteral("LEGACY");
    case MtkGeneration::XFlash: return QStringLiteral("XFLASH");
    case MtkGeneration::Xml:    return QStringLiteral("XML");
    }
    return QStringLiteral("?");
}

mtkgpt::ReadFn xflashSectorReader(XFlashSession &x, quint32 storage, quint32 partType)
{
    return [&x, storage, partType](quint64 off, int len, QByteArray *out, QString *e) {
        return xflashReadData(x, off, quint32(len), storage, partType, *out, e);
    };
}
```

`xflashBringUpDa`：

```cpp
bool xflashBringUpDa(BromSession &brom, XFlashSession &x, const DaSelection &sel,
                     const PreloaderResult &pre, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    // ① DA1 上传与 0xC0（与 LEGACY 共用 BROM 级帧，XFL:979-985）
    if (!sendDa1(brom, sel, error))
        return false;
    if (!waitDa1Ready(brom, 10000, error))
        return false;
    say(QStringLiteral("XFlash：DA1 已上传并就绪（0xC0）"));
    // ② 七步握手 + 四步 bring-up（T4）
    if (!xflashDa1Handshake(x, log, error))
        return false;
    QByteArray agent;
    if (!xflashBringUpSteps(x, &agent, log, error))
        return false;
    // ③ EMI 判定（铁律 9）：preloader=跳过；brom=需要（缺 EMI 只告警）；其它=失败
    if (agent == QByteArray("preloader")) {
        say(QStringLiteral("XFlash：connection_agent = preloader（设备已初始化过 DRAM）→ 跳过 EMI"));
    } else if (agent == QByteArray("brom")) {
        bool emiSent = false;
        if (pre.origin != PreloaderOrigin::None) {
            EmiData emi;
            QString emiErr;
            if (!extractEmiXflash(pre.bytes, emi, &emiErr)) {
                say(QStringLiteral("XFlash：EMI 提取失败（%1）—— 跳过 DRAM 初始化（DA2 可能起不来）").arg(emiErr));
            } else if (!xflashSendEmi(x, emi.bytes, error)) {
                return false;                        // 发失败 = 真失败（与 LEGACY 的 0xBC3 同姿态）
            } else {
                emiSent = true;
                say(QStringLiteral("XFlash：DRAM 初始化完成（EMI %1 字节，版本 %2）")
                        .arg(emi.bytes.size()).arg(emi.ver));
            }
        } else {
            for (const QString &line : std::as_const(pre.log))
                say(line);
            say(QStringLiteral("XFlash：无 preloader —— 未做 DRAM 初始化，操作可能失败（上游同姿态）：%1")
                    .arg(pre.skipReason));
        }
        if (!emiSent)
            say(QStringLiteral("XFlash：EMI 未发送（见上）"));
    } else {
        if (error) *error = QStringLiteral("XFlash：connection_agent 非 brom/preloader（收到 %1）—— 失败")
                                .arg(QString::fromLatin1(agent));
        return false;
    }
    // ④ DA2：**剥尾部签名**（XFL:970-978；与 LEGACY 相反）
    if (sel.da2.sigLen >= sel.da2Bytes.size()) {
        if (error) *error = QStringLiteral("XFlash：DA2 切片 %1 字节不足以剥掉签名 %2 字节")
                                .arg(sel.da2Bytes.size()).arg(sel.da2.sigLen);
        return false;
    }
    const QByteArray da2NoSig = sel.da2Bytes.left(sel.da2Bytes.size() - int(sel.da2.sigLen));
    if (!xflashBootTo(x, sel.da2.startAddr, da2NoSig, error))
        return false;
    say(QStringLiteral("XFlash：DA2 已上传（%1 字节，**已剥签名 %2 字节**）")
            .arg(da2NoSig.size()).arg(sel.da2.sigLen));
    return true;
}
```

> ⚠️ **Step 3 的完整用例**（实现者按此把 Step 1 的两条写实）：
> - `xflashChainOrderWithEmi`：reads 队列 = `da1UploadReads(sel)`（D1 夹具）+ `"\xC0"` + [SYNC status 0] + [SETUP_ENV status 0] + [SETUP_HW_INIT status 0] + [SYNC 回帧] + bring-up 四步的 9 帧（每步 status0 + 子命令 status0 + 回包；含 `b"brom"`）+ [INIT_EXT_RAM status 0] + [EMI send_param status 0] + [BOOT_TO status 0] + [send_data status 0] + [boot_to 最终 status 0]。断言：把 `writeFrames` 展平为字节流，`indexOf(le32(0x434E5953)) < indexOf(le32(0x010100)) < indexOf(le32(0x010101)) < indexOf(le32(0x01000A)) < indexOf(le32(0x010008))`，且 `indexOf(le32(0x68))`（reset key）在 bring-up 段内、`da2NoSig == sel.da2Bytes.left(da2Bytes.size()-sigLen)` 能在写字节流里找到。
> - `xflashChainSkipsEmiForPreloaderAgent`：同上但 agent 回包是 `b"preloader"`，断言**字节流里没有** `le32(0x01000A)`。
> （`da1UploadReads(sel)` 在 D1 的 `tests/test_mtk_payload.cpp` 已有；`makeSelection(0x6765)` + `buildEmiPreloader("38", <16B>)` 造 preloader。）

- [ ] **Step 4: `bromFlashOnSession` 分派（把 D1 的单链改成三代路由）**

改 `src/core/modes/mtk_payload.cpp` 的 `bromFlashOnSession`（D1 版 `:717-822`）。**逐段给出**（未列出的段=原样保留）：

**(a) 注释与 include**：在文件顶部 `#include "mtk_xflash_payload.h"` / `"mtk_xml_payload.h"` / `"mtk_gpt.h"`；匿名 namespace 里加
```cpp
constexpr quint32 kXStorageEmmc = 0x1;    // ST:16-49（eMMC）
constexpr quint32 kXEmmcPartUser = 0x8;   // ST:16-49（user 区）
```

**(b) 替换 D1 `:717-745`（原来的"非 LEGACY / IoT / v6 一律拒绝"三道门）**：
```cpp
    // 代际判定（spec §4）：表外 / IoT → 明确拒绝；v6 强制 XML（DC:216，优先于芯片表）
    const ChipInfo *chip = lookupChip(hwCode);
    DaFile daFile;
    if (!parseDaFile(req.daFile, daFile, error))     // **提前**到判定之前：v6 是判定的输入之一
        return false;
    MtkGeneration gen = MtkGeneration::Legacy;
    if (!decideGeneration(chip, daFile.isV6, gen, error))
        return false;
    say(QStringLiteral("代际判定：%1（chip_dacode=0x%2；DA %3）")
            .arg(generationName(gen)).arg(chip->dacode, 4, 16, QLatin1Char('0'))
            .arg(daFile.isV6 ? QStringLiteral("v6") : QStringLiteral("非 v6")));
```
（`selectDaEntry` 那一段（D1 `:746-762`）**原样保留**：查找键仍是 `chip->dacode`、`selWarn` 逐条 warn、条目日志不变。）

**(c) 替换 D1 `:774-779`（引导链调用 + 日志落地）**：
```cpp
    QStringList bringLog;
    XFlashSession xflash(session.usb(), chip->dacode);   // 通道未打开也能构造（只存指针/码）
    bool brought = false;                                // （T12 会在这里加 XmlSession xml(session.usb());）
    switch (gen) {
    case MtkGeneration::Legacy:
        brought = bromBringUpDa(session, sel, hwCode, bromVer, blVer, pre, &bringLog, error);
        break;
    case MtkGeneration::XFlash:
        brought = xflashBringUpDa(session, xflash, sel, pre, &bringLog, error);
        break;
    case MtkGeneration::Xml:
        brought = xmlBringUpDa(session, xml, sel, &bringLog, error);   // ← T12 提供；T11 先留桩（见下）
        break;
    }
    for (const QString &line : std::as_const(bringLog))
        say(line);                                       // **失败也落**：用户要看到卡在哪一步（D1 审查 M4）
    if (!brought)
        return false;
```
> T11 里 `xmlBringUpDa` 尚未存在（T12 才写）→ T11 阶段这条 `case` 写
> `if (error) *error = QStringLiteral("XML 代：链在 Task 12 接线（当前明确拒绝，不假装支持）"); return false;`
> （**T11 阶段不声明 `XmlSession` 变量**：此时它只会是"声明了没人用"的警告源；T12 在 (c) 段加它。）
> T12 落地后把它换成真调用（T12 的 Step 3 给出替换文本）。

**(d) 替换 D1 `:781-790`（设备分区表获取）——按代际取表并把"分区名 → 写入地址"建出来**：
```cpp
    QList<mtkplan::PartitionRef> refs;
    QHash<QString, quint64> partAddr;                 // 分区名 → 写入地址（XFlash/XML 用；LEGACY 不用）
    quint32 xWritePacketLength = 0;                   // XFlash 写分块（GET_PACKET_LENGTH）

    if (gen == MtkGeneration::Legacy) {
        DaStorage storage(session);                   // （D1 原文：listPartitions + 空表拒绝）
        storage.setDaActive(true);
        QList<EmPartition> deviceParts;
        if (!storage.listPartitions(deviceParts, error))
            return false;
        if (deviceParts.isEmpty()) {
            if (error) *error = QStringLiteral("设备分区表为空（read_pmt 无条目）—— 拒绝在未知分区表上写入");
            return false;
        }
        refs = toPartitionRefs(deviceParts);
    } else if (gen == MtkGeneration::XFlash) {
        XPacketLength pl;
        if (!xflashGetPacketLength(xflash, pl, error))     // 铁律 14：拿不到写分块就**不写**
            return false;
        xWritePacketLength = pl.writeLength;
        XChipId id;                                        // 只读诊断，失败只告警（不影响写入）
        if (xflashGetChipId(xflash, id, nullptr)) {
            say(QStringLiteral("XFlash chip id：hw_code=0x%1 hw_ver=0x%2 sw_ver=0x%3 evo=%4")
                    .arg(id.hwCode, 4, 16, QLatin1Char('0')).arg(id.hwVersion, 4, 16, QLatin1Char('0'))
                    .arg(id.swVersion, 4, 16, QLatin1Char('0')).arg(id.chipEvolution));
        } else {
            warn(QStringLiteral("XFlash：GET_CHIP_ID 失败（只读诊断，继续）"));
        }
        PartitionCata cata = PartitionCata::Unknown;
        if (!xflashGetPartitionCata(xflash, cata, error))
            return false;
        if (cata == PartitionCata::Pmt) {
            // 上游 PMT 分支**实际不可达**（事实报告 §7：`DL.partition_table_category()` 无条件回 "GPT"，
            // 且 `partition.py:80-81` 的 PMT 嗅探 int/bytes 比较是 bug）→ **不复刻**：明确拒绝，不猜读法
            if (error) *error = QStringLiteral("XFlash：设备分区表是 PMT（GET_PARTITION_TBL_CATA 回 0x65）——"
                                               "上游该分支不可达（事实报告 §7），本实现明确拒绝（不猜 PMT 读法）");
            return false;
        }
        if (cata != PartitionCata::Gpt) {
            if (error) *error = QStringLiteral("XFlash：分区表类型未知（GET_PARTITION_TBL_CATA 回 0x%1，"
                                               "既非 GPT(0x64) 也非 PMT(0x65)）—— 拒绝在未知表上写入")
                                    .arg(quint32(cata), 0, 16);
            return false;
        }
        mtkgpt::Table tbl;
        QStringList gptLog;
        // diskSectors = 0：XFlash 侧没有"磁盘总扇区数"的可靠来源 → 只走主 GPT（备份兜底不可用；
        // readTable 在需要时会把这一点写进日志/错误，不静默降级）
        if (!mtkgpt::readTable(xflashSectorReader(xflash, kXStorageEmmc, kXEmmcPartUser), 0, tbl, &gptLog, error))
            return false;
        for (const QString &line : std::as_const(gptLog))
            say(line);
        refs = mtkplan::toPartitionRefs(tbl.partitions, tbl.sectorSize);   // T7 的适配器（不重复实现）
        for (const mtkgpt::Partition &p : std::as_const(tbl.partitions))
            partAddr.insert(p.name, mtkgpt::offsetBytes(p, tbl.sectorSize));
        say(QStringLiteral("XFlash：GPT 读出 %1 个分区（扇区 %2 字节）")
                .arg(tbl.partitions.size()).arg(tbl.sectorSize));
    } else {
        // T12 在此写入 READ-FLASH 读表 + partAddr 填充（T11 阶段明确拒绝，不假装支持）
        if (error) *error = QStringLiteral("XML 代：分区表读取在 Task 12 接线（当前明确拒绝）");
        return false;
    }
```

**(e) 替换 D1 `:800-822`（写循环 + 收尾）——`plan.entries` 为空的安全门与 `written/progress` 保持 D1 原样**：
```cpp
    quint64 written = 0;
    for (const mtkplan::PlanEntry &e : std::as_const(plan.entries)) {
        QFile f(e.imagePath);                    // （读入 + 大小核对：D1 原样）
        if (!f.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("无法读取镜像：%1").arg(e.imagePath);
            return false;
        }
        const QByteArray image = f.readAll();
        f.close();
        if (image.size() != qsizetype(e.imageSize)) {
            if (error) *error = QStringLiteral("镜像 %1 读入字节数与计划不符（%2 != %3）")
                                    .arg(e.imagePath).arg(image.size()).arg(e.imageSize);
            return false;
        }
        say(QStringLiteral("写入分区 %1（%2 字节）…").arg(e.partition).arg(image.size()));
        if (gen == MtkGeneration::Legacy) {
            if (!flashPartition(session, storage, e.partition, image, error))   // ← storage 提到循环外（见下）
                return false;
        } else if (gen == MtkGeneration::XFlash) {
            const auto it = partAddr.constFind(e.partition);
            if (it == partAddr.constEnd()) {     // 计划条目名必来自设备表 → 缺 = 内部错位
                if (error) *error = QStringLiteral("内部错误：分区 %1 不在设备表地址映射里").arg(e.partition);
                return false;
            }
            if (!xflashWriteData(xflash, it.value(), image, kXStorageEmmc, kXEmmcPartUser,
                                 xWritePacketLength, nullptr, error))
                return false;
        } else {
            // T12 换成： if (!xmlWritePartition(xml, e.partition, image, nullptr, error)) return false;
            if (error) *error = QStringLiteral("XML 代：写入在 Task 12 接线（当前明确拒绝）");
            return false;
        }
        written += quint64(image.size());
        if (progress)
            progress(written, plan.totalBytes);
    }

    // 收尾：失败只告警（数据已落盘）
    if (gen == MtkGeneration::Legacy) {
        QString finishErr;
        if (storage.finishFlash(0, &finishErr))
            say(QStringLiteral("FINISH（0xD9）收尾完成"));
        else
            warn(QStringLiteral("FINISH 收尾失败（数据已写入）：%1").arg(finishErr));
    } else if (gen == MtkGeneration::XFlash) {
        QString shutErr;
        if (xflashShutdown(xflash, 0, &shutErr))
            say(QStringLiteral("XFlash：SHUTDOWN 收尾完成"));
        else
            warn(QStringLiteral("XFlash SHUTDOWN 收尾失败（数据已写入）：%1").arg(shutErr));
    } else {
        // T12 换成： QString rbErr; if (xmlReboot(xml, true, &rbErr)) say(...); else warn(...);
        warn(QStringLiteral("XML 代：REBOOT 收尾在 Task 12 接线"));
    }
    return true;
```

> **`storage` 的作用域**：D1 里 `DaStorage storage` 声明在 LEGACY 段内、写循环里用；本任务把它的声明提到
> 分段之前（`std::optional<DaStorage> storage;` + LEGACY 分支里 `storage.emplace(session); storage->setDaActive(true);`
> + 写循环用 `*storage`）。`xflashWriteData` 的日志参数传 `nullptr`（每分区一行由上面的 `say` 覆盖，避免刷屏）。
> **XFlash 的 GPT 分支不进 `plan.entries` 之外的分支**：`partAddr` 只在该分支被填，写循环里 trio 互斥。

- [ ] **Step 5: 跑测试，确认通过**（payload 目标从 46 增至 ~48+；0 skipped）

- [ ] **Step 6: 提交**

```bash
git add src/core/modes/mtk_payload.h src/core/modes/mtk_payload.cpp tests/test_mtk_payload.cpp CMakeLists.txt
git commit -m "feat(mtk): 三代路由（decideGeneration）+ XFlash 链整合（GPT/PMT 分区表 + 逐分区 WRITE_DATA + SHUTDOWN）"
```

---

### Task 12: XML 链整合（`xmlBringUpDa` + 分区表 + 逐分区写）

**Files:**
- Modify: `src/core/modes/mtk_payload.{h,cpp}`、`tests/test_mtk_payload.cpp`
- Modify: `CMakeLists.txt`（`test_mtk_payload` 追加 `mtk_xml_session.cpp`、`mtk_xml_payload.cpp`）

**Interfaces:**
- Produces:
  ```cpp
  namespace mtkbrom {
  // XML 引导链（XL:271-321）：sendDa1 → jumpDa → **等 CMD:START**（xmlDa1Handshake 内部做三步 setup）
  //   → 分区表经 READ-FLASH 读 GPT（同一 mtkgpt）→ 由调用方继续
  // 注意：XML **不发 EMI**（铁律 16；initialize_dram=YES 在 setup_env 里）
  bool xmlBringUpDa(BromSession &brom, XmlSession &x, const DaSelection &sel,
                    QStringList *log, QString *error = nullptr);
  // 分区表读回调适配器：把 mtkgpt::ReadFn 的 (byteOffset, len) 转成 XML 的 READ-FLASH 区间读
  //（**唯一的新缝**；GPT 解析器本身复用 T1 的 mtkgpt::readTable/parsePrimary/parseBackup）
  // partition = XML 侧的存储描述（`XC` 的 UFSPartitionType 文本；eMMC 默认 "EMMC-USER"）
  mtkgpt::ReadFn xmlSectorReader(XmlSession &x, const QString &partition = QStringLiteral("EMMC-USER"));
  }
  ```

- [ ] **Step 1: 写失败用例**（追加到 `tests/test_mtk_payload.cpp`）

```cpp
// 本文件新增的读队列拆分助手（与 test_mtk_xflash_* 的 textReads 同款；**一帧 = 两笔**，见 T2 注释）
QList<QByteArray> textReads(const QString &s)
{
    const QByteArray body = s.toUtf8() + QByteArray(1, '\0');
    return {le32(0xFEEEEEEF) + le32(1) + le32(quint32(body.size())), body};
}

// XML 链：sendDa1（BROM 级，含 JUMP_DA）→ 等 CMD:START → setup_env / setup_hw_init / set_host_info；
// **全程不发任何 DRAM/EMI 命令**（铁律 16：XML 的 DRAM 初始化是 initialize_dram=YES 那一条参数）
void TestMtkPayload::xmlChainSendsCmdStartHandshakeWithoutEmi()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x0907, sel, &err), qPrintable(err));   // 表内 XML 芯片（0x0907 = MT6983）

    m->reads = da1UploadReads(sel);                                        // 与 LEGACY/XFlash 同一份 DA1 上传夹具
    m->reads << textReads(QStringLiteral("<host><command>CMD:START</command></host>"))
             << textReads(QStringLiteral("OK"))                            // SET-RUNTIME-PARAMETER
             << textReads(QStringLiteral("OK"))                            // HOST-SUPPORTED-COMMANDS
             << textReads(QStringLiteral("OK"))                            // NOTIFY-INIT-HW
             << textReads(QStringLiteral("OK"));                           // SET-HOST-INFO

    mtkbrom::BromSession brom(std::move(usb), mtkbrom::BromDevice{});
    mtkbrom::XmlSession x(m);
    QStringList log;
    QVERIFY2(mtkbrom::xmlBringUpDa(brom, x, sel, &log, &err), qPrintable(err));

    QStringList sent;
    for (const QByteArray &f : std::as_const(m->writeFrames))
        if (f.startsWith("<?xml")) sent << QString::fromUtf8(f).remove(QChar('\0'));
    QCOMPARE(sent.size(), 4);                                              // 四条命令；CMD:START 不回 ack
    QVERIFY(sent.at(0).contains(QStringLiteral("CMD:SET-RUNTIME-PARAMETER")));
    QVERIFY(sent.at(0).contains(QStringLiteral("<initialize_dram>YES</initialize_dram>")));
    QVERIFY(sent.at(1).contains(QStringLiteral("CMD:HOST-SUPPORTED-COMMANDS")));
    QVERIFY(sent.at(2).contains(QStringLiteral("CMD:NOTIFY-INIT-HW")));
    QVERIFY(sent.at(3).contains(QStringLiteral("CMD:SET-HOST-INFO")));

    // 不发 EMI：既无 XFlash 的 INIT_EXT_RAM 帧（0x01000A），也无 LEGACY 的 ENABLE_DRAM(0xE8) 帧
    QVERIFY2(m->writes.indexOf(le32(mtkbrom::X_CMD_INIT_EXT_RAM)) == -1, "XML 不得发 XFlash 的 INIT_EXT_RAM");
    QVERIFY2(m->writes.indexOf(mtktest::be32(0xE8)) == -1, "XML 不得发 LEGACY 的 ENABLE_DRAM");
    QVERIFY2(log.join(QLatin1Char('\n')).contains(QStringLiteral("未发 EMI")),
             qPrintable(log.join(QLatin1Char('\n'))));
}

// 设备首条命令不是 CMD:START → 明确失败（不猜、不硬着头皮往下走）
void TestMtkPayload::xmlChainRejectsNonStartFirstCommand()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x0907, sel, &err), qPrintable(err));

    m->reads = da1UploadReads(sel);
    m->reads << textReads(QStringLiteral("<host><command>CMD:PROGRESS-REPORT</command></host>"))
             << textReads(QStringLiteral("OK!EOT"))
             << textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>OK</result></arg></host>"));

    mtkbrom::BromSession brom(std::move(usb), mtkbrom::BromDevice{});
    mtkbrom::XmlSession x(m);
    QString err2;
    QVERIFY(!mtkbrom::xmlBringUpDa(brom, x, sel, nullptr, &err2));
    QVERIFY2(err2.contains(QStringLiteral("CMD:START")), qPrintable(err2));
}

// 分区表读回调适配器：mtkgpt::ReadFn 的 (byteOffset, len) → XML 的 READ-FLASH 区间读（裸 OK@ 数据路径）
// ——"三代共用同一个 GPT 解析器"的**唯一新缝**就在这个适配器上（解析器本身在 T1 用真样本钉死）
void TestMtkPayload::xmlSectorReaderRoundTripsDeviceBytes()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    const QByteArray block(0x200, '\x5C');
    m->reads << textReads(QStringLiteral("OK"))                            // READ-FLASH 被接受（noack）
             << textReads(QStringLiteral("<host><command>CMD:UPLOAD-FILE</command><arg>"
                                         "<checksum>CHK_NO</checksum><info>ROM_0</info>"
                                         "<target_file>ROM_0</target_file></arg></host>"))
             << textReads(QStringLiteral("OK@0x200"))                      // 裸数据路径：长度
             << textReads(QStringLiteral("OK"))                            // 长度确认
             << frameReads(1, block)                                       // 数据帧（头、载荷两笔）
             << textReads(QStringLiteral("<host><command>CMD:START</command></host>"));

    mtkbrom::XmlSession x(m);                    // mock 由 unique_ptr 持有到用例结束（不需要 BromSession）
    const mtkgpt::ReadFn read = mtkbrom::xmlSectorReader(x);
    QByteArray got;
    QString err;
    QVERIFY2(read(0x1000, 0x200, &got, &err), qPrintable(err));
    QCOMPARE(got, block);
    QString sentXml;
    for (const QByteArray &f : std::as_const(m->writeFrames))
        if (f.startsWith("<?xml")) sentXml += QString::fromUtf8(f).remove(QChar('\0'));
    QVERIFY2(sentXml.contains(QStringLiteral("CMD:READ-FLASH")), qPrintable(sentXml));
}
```

- [ ] **Step 2: 跑一遍，确认失败**

- [ ] **Step 3: 实现**

追加到 `mtk_xml_payload.cpp`（`xmlBringUpDa` 只有三步：DA1 上传 + 等 CMD:START + 三步 setup，**没有** EMI/checksum/reset-key）：

```cpp
bool xmlBringUpDa(BromSession &brom, XmlSession &x, const DaSelection &sel, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    // sendDa1 = SEND_DA + JUMP_DA **两步都做**（mtk_payload.cpp:76-78）——不要再单独 jumpDa（会二次跳转）
    if (!sendDa1(brom, sel, error))            // BROM 级，与另两代共用（XL:271-278）
        return false;
    say(QStringLiteral("XML：DA1 已上传并跳转，等待 CMD:START"));
    if (!xmlDa1Handshake(x, log, error))       // 含 setup_env（initialize_dram=YES）/hw_init/host_info
        return false;
    say(QStringLiteral("XML：引导完成（**未发 EMI** —— 主机的 DRAM 初始化就是 setup_env 里的 initialize_dram）"));
    return true;
}

mtkgpt::ReadFn xmlSectorReader(XmlSession &x, const QString &partition)
{
    return [&x, partition](quint64 off, int len, QByteArray *out, QString *e) {
        return xmlReadPartition(x, partition, off, quint32(len), *out, e);
    };
}
```

然后在 `bromFlashOnSession` 里**替换 T11 留的两处桩**：

**(a) `case MtkGeneration::Xml:` 的桩 → 真调用**
```cpp
    case MtkGeneration::Xml:
        brought = xmlBringUpDa(session, xml, sel, &bringLog, error);
        break;
```

**(b) `else { XmlSession &x = xml; Q_UNUSED(x) ... return false; }` 整段 → XML 的分区表段**
```cpp
    } else {   // Xml
        // 分区表：**READ-FLASH + 同一个 mtkgpt**（spec §7 已核实，`mtk_daloader.py:296-302` 同款口径）
        mtkgpt::Table tbl;
        QStringList gptLog;
        // diskSectors = 0：XML 侧没有磁盘总扇区数的可靠来源（GET-HW-INFO 的分区表字段留 D4 解析）
        //   → 只走主 GPT；主 GPT 读不出来时 readTable 会明确失败（不静默降级、不假装有备份兜底）
        if (!mtkgpt::readTable(xmlSectorReader(xml), 0, tbl, &gptLog, error))
            return false;
        for (const QString &line : std::as_const(gptLog))
            say(line);
        refs = mtkplan::toPartitionRefs(tbl.partitions, tbl.sectorSize);   // T7 的适配器（不重复实现）
        for (const mtkgpt::Partition &p : std::as_const(tbl.partitions))
            partAddr.insert(p.name, mtkgpt::offsetBytes(p, tbl.sectorSize));
        say(QStringLiteral("XML：GPT 读出 %1 个分区（扇区 %2 字节）")
                .arg(tbl.partitions.size()).arg(tbl.sectorSize));
    }
```
**(c) T11 在 (c)/(d)/(e) 段留的三处 XML 桩，全部换成真代码**

```cpp
// (c) 段：在 XFlashSession 声明之后加
    XmlSession xml(session.usb());

// (d) 段：整段 else { ... return false; } → 上面那段 XML 分区表代码
// (e) 段：写循环里的 else 分支 → 
        } else {
            if (!xmlWritePartition(xml, e.partition, image, nullptr, error))
                return false;
        }
// (e) 段：收尾的 else 分支 → 
    } else {
        QString rbErr;
        if (xmlReboot(xml, true, &rbErr))
            say(QStringLiteral("XML：REBOOT 收尾完成"));
        else
            warn(QStringLiteral("XML REBOOT 收尾失败（数据已写入）：%1").arg(rbErr));
    }
```

> **`diskSectors = 0` 的结论**（写进报告与代码注释）：上游 XML 路径读 GPT 用的是 `READ-FLASH`
> `<length>0x100000`，并没有一个"先问设备总扇区数"的命令（`GET-HW-INFO` 在 `XL` 的引导链里也不是必经）；
> 本实现只依赖主 GPT（备份兜底需要总扇区数 → 该路径在 XML 侧不可用，且**如实失败而不是猜**）。

- [ ] **Step 4: 跑测试，确认通过**（0 skipped）

- [ ] **Step 5: 提交**

```bash
git add src/core/modes/mtk_payload.h src/core/modes/mtk_payload.cpp tests/test_mtk_payload.cpp CMakeLists.txt
git commit -m "feat(mtk): XML 链整合（CMD:START 引导 + READ-FLASH 分区表共用 GPT 解析 + 逐分区写 + REBOOT）"
```

---

### Task 13: 通道/UI 文案 + 文档 + 终验

**Files:**
- Modify: `src/core/flash_tool.cpp`（通道日志里写明代际）、`src/ui/flash_panel.cpp`（tooltip/文案：三代自动路由）
- Modify: `功能清单.txt`、`README.md`
- Create: `docs/superpowers/specs/mtk-xflash-facts.md`
- Modify: `docs/superpowers/specs/2026-09-16-mtk-xflash-xml-design.md`（§11 交付清单勾选 + 边界回填）
- （不改代码逻辑）

- [ ] **Step 1: 通道/UI 文案**

`flash_tool.cpp` 的 mtk-brom 分支：在"MTK BROM 刷写通道"那行后追加代际说明（由 `bromFlashOnSession` 的日志给出，无需新参数）；`flash_panel.cpp` 的 tooltip 与提示改成"按设备代际自动选择 LEGACY / XFLASH / XML 链（三代均已实现）"。

- [ ] **Step 2: `功能清单.txt`**

MTK BROM 段追加（照 D1 的写法，逐条带实测/出处粒度）：

```
[√] 代际路由: 芯片表 damode + DA 文件 v6 (v6 强制 XML) —— 设备不上报 damode; plcap/blver 那条是死代码不实现
[√] XFlash 代: 12B 小端帧 (magic/datatype/length) + 七步握手 (SYNC/SETUP_ENV/SETUP_HW_INIT) + 0x6781 单次 16B ack
[√] XFlash EMI: INIT_EXT_RAM + 长度单帧 + 0x200 分块 (整块 912B 切片, 与 LEGACY 800B 并存)
[√] XFlash 写: WRITE_DATA **56B 参数** + write_packet_length 分块（**要求 512 对齐，否则 fail-closed**）+ 循环后最终 status + CC_OPTIONAL_DOWNLOAD_ACT + 16 位加和校验 + READ_DATA 逐帧收数/flag 路径
[√] GPT 分区表: 512/4096 探测 + 头/条目双 CRC fail-closed + 备份 GPT 兜底 (真 4096 样本 PGPT/SGPT + sgdisk 对拍)
[√] XML 代: CMD:START 握手 + SET-RUNTIME-PARAMETER(initialize_dram=YES, **不发 EMI**) + HOST-SUPPORTED-COMMANDS/NOTIFY-INIT-HW + SET-HOST-INFO
[√] XML 写读: WRITE-FLASH/READ-FLASH + DOWNLOAD-FILE/UPLOAD-FILE 数据流 + CMD:END/CMD:START 收尾 + REBOOT
[√] scatter XML 方言: 按 storage 过滤 EMMC/UFS 双副本 (真样本 MT6789: 130 块 = 两份 × 65)
```

- [ ] **Step 3: `README.md`**：MTK BROM 小节改成"三代（LEGACY / XFLASH / XML）自动路由"，功能表加一行 XFlash/XML；许可段补 `mtk-gpt-tool`（MPL-2.0）样本来源说明（**只作离线样本，不随仓库分发**）。

- [ ] **Step 4: 新建 `docs/superpowers/specs/mtk-xflash-facts.md`**

必备条目（逐条带 `file:line`，从 `.superpowers/sdd/mtk-d2d3-facts-report.md` 与本次实测提炼）：
① 三代握手差异（`XFL:979-995` / `XL:271-321` / D1 的 `0xC0`）；② 12B 帧与 `ack` 的 0x6781 特例（`XFL:85-100/112`）；③ `status()` 三态判据（`XFL:138-158`）；④ `send_param` 0x200 分块 + 帧头一次/载荷分块（`XFL:163-177`）+ `0xC0040050` **静默 False**、显式 preloader 路径据此中止（`XFL:181-186`、`:1147-1149`）；⑤ XFlash EMI（`XFL:251-270`）；⑥ `boot_to` 剥签名（`XFL:970-978/288-328`）；⑦ 写数据流（`XFL:852-899`：分块+校验和 → 循环后最终 status → CC_OPTIONAL_DOWNLOAD_ACT）；⑧ XML 信封/应答/`OK!EOT`（`XC:18-27`、`XL:146-163/188-232/369-449`）；⑨ XML 无 EMI（`xml_cmd.py:100-128`）；⑩ GPT 布局 + 双 CRC + 备份窗口 + 真样本实测（本计划 T1）；⑪ 上游 GPT 4 处缺陷；⑫ 代际判定三方投票与 `plcap` 死代码；⑬ **真样本清单**（PGPT/SGPT/sgdisk/scatter.xml 的 URL、sha256、实测值）；⑭ scatter XML 的 EMMC/UFS 双副本。

**诚实边界小节**（逐条如实，不粉饰）：真机全链未验证（三代）；XML 无真实设备样本（帧与命令全部来自上游代码）；
**spec §9 的三条"实现时核实"到此结案**——① XML 分区表读取路径：设计期已核实（`CMD:READ-FLASH` + 同一 GPT，`xml_cmd.py:474-483`、`mtk_daloader.py:296-302`）；② `UFSPartitionType` 文本表示：XML 用字符串 `"EMMC-USER"`（`ST:216`、`XC:452-461`），非整数的 `UFSPartitionType` 枚举值（BOOT1=1/BOOT2=2/USER=3/RPMB=4）只在 XFlash 侧用；③ `max_address_length = 9`：**全仓库只被 import 无消费点**（`USBLIB:21`、`seriallib.py:9`；`XP:2` 定义）→ 与 plcap/blver 同类，属**死常量，本实现不实现**（不猜语义）。
GPT 样本来自第三方仓库（MPL-2.0 的 mtk-gpt-tool，非自有设备）；mock 看不见"设备实际读了多少字节"；"0 warning" 只是编译器默认档（非 `-Wall -Wextra`）；`0x6781` 的 16 字节 ack 仅按上游实现，无真机复核。

- [ ] **Step 5: spec 勾选与"不做项回收"**

  - 本 spec（`2026-09-16-mtk-xflash-xml-design.md`）：§11 交付清单逐条勾选；§9 的边界（**已在计划期结案**：
    `UFSPartitionType` 文本 = `"EMMC-USER"`；`max_address_length` = 死常量不实现）核对一致 → 若实现期发现新的偏差，改这一节。
  - D1 spec（`2026-09-15-mtk-brom-d1-design.md`）：把 §9 的"XFlash / XML 两代协议（D2/D3）"一条改为
    `**XFlash / XML 两代已于 2026-09-16 交付**（见 `2026-09-16-mtk-xflash-xml-design.md`）`；
    该文件 §8 的"无遗留 `[ ]` 项"那句同步更新（原文写"XFlash / XML 归 D2/D3"）。

- [ ] **Step 6: 终验（三步都要做，原始输出进报告）**

```bash
# (a) 真样本全量：0 skipped
cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=ON && cmake --build build 2>&1 | tail -3
ctest --test-dir build --output-on-failure 2>&1 | tail -8
for t in test_mtk_gpt test_mtk_xflash_payload test_mtk_xml_payload test_mtk_flash_plan test_mtk_preloader_emi; do
    ./build/image_engine_tests_$t 2>&1 | tail -2; done
> **终验的计数基准**（别用我手写的数字当门）：ctest 目标数 = T1–T6 之后 **54**；T8/T9/T10 再各注册 1 个（`test_mtk_xml_session` / `test_mtk_xml_payload`）= **56**；
> T7 只扩既有目标不加新目标。判据一律是"**0 failed 且 0 skipped**"。
# (b) 全新目录构建的警告数
cmake -B /tmp/pt-fresh-d2d3 -G Ninja && cmake --build /tmp/pt-fresh-d2d3 2>&1 | grep -ci warning
# (c) 回到默认
cmake -B build -G Ninja -DMTK_SAMPLES_REQUIRED=OFF
```

- [ ] **Step 7: 提交**

```bash
git add src/core/flash_tool.cpp src/ui/flash_panel.cpp 功能清单.txt README.md docs/superpowers/specs/mtk-xflash-facts.md docs/superpowers/specs/2026-09-16-mtk-xflash-xml-design.md
git commit -m "docs: MTK BROM Phase D2+D3 交付记录（XFlash/XML 三代 + 事实报告 + 边界回填 + 终验）"
```

---

## 执行纪律提醒（给 SDD 控制者）

- **严格串行**：`mtk_payload`（T11/T12）、`mtk_flash_plan`（T7）、`CMakeLists.txt`（几乎每个任务）被多任务触碰 → **不得并行派实现者**。
- **每任务两段审查**（spec 合规 + 代码质量）+ **对抗变异实验**（至少一组"改坏实现 → 必须有用例红"）。
- **LEDGER**：`.superpowers/sdd/progress.md`；派单禁止使用任务工具。
- **计划与上游冲突时以 `mtkclient` 源码为准**，并把结论回写本计划 + 交付文档；本计划里标 ⚠️ 的"实现时须核对上游"共 4 处（T4 的 `get_expire_date`、T6/T8 的 send_param 分块与保活序列、T12 的 `GET-HW-INFO`/diskSectors 来源、T1 备份窗口在真样本上的布局确认——**已实测**）。
- **不得把没验证的说成已验证**：真机、XML 真实样本、第三方样本来源三处务必如实。






