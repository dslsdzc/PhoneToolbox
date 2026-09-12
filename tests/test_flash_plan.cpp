#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include <limits>
#include "core/edl/flash_plan.h"
#include "image_engine/sparse_image.h"
#include "flash_plan_helpers.h"

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

// 合成 28 字节 sparse 头（AOSP / qdl `sparse_header_t` 布局，`reference/qdl/src/sparse.h:11-33`：
// file_hdr_sz u16@8、chunk_hdr_sz u16@10、blk_sz u32@12、total_blks u32@16），供"sparse 条目按
// 文件头校正扇区数"的用例落盘用。
static QByteArray sparseHeaderBytes(quint32 totalBlks, quint32 blkSz)
{
    QByteArray h(28, '\0');
    auto put16 = [&h](int off, quint16 v) { h[off] = char(v); h[off + 1] = char(v >> 8); };
    auto put32 = [&h](int off, quint32 v) {
        h[off] = char(v & 0xFF); h[off + 1] = char((v >> 8) & 0xFF);
        h[off + 2] = char((v >> 16) & 0xFF); h[off + 3] = char((v >> 24) & 0xFF);
    };
    put32(0, 0xED26FF3A);
    put16(4, 1); put16(6, 0);       // major 1 / minor 0
    put16(8, 28); put16(10, 12);    // file_hdr_sz / chunk_hdr_sz
    put32(12, blkSz);
    put32(16, totalBlks);
    put32(20, 1);                   // total_chunks（本函数不读）
    return h;
}

class TestFlashPlan : public QObject
{
    Q_OBJECT
private slots:
    void parsesProgramEntries();
    void skipsMalformedEntryWithWarning();
    void parsesEraseTag();
    void parsesPatchEntriesAndSkipsNonDisk();
    void eraseTagWithoutRangeMeansWholeLun();
    void malformedXmlReportsPathAndReaderError();
    void keepsExpressionStartSector();
    void keepsExpressionStartSectorInProgram();
    void eraseWithUnparseableCountIsDropped();
    void finalizePlanSortsAndSums();
    void validateRejectsAndWarns();
    void validateAcceptsPlanAtCapacityAndAdjacent();
    void validateMissingLunGeometryIsError();
    void validateSkipsExpressionEntries();
    void validateSparseHeaderCorrectsNumSectors();
    void validateSparseCorrectionFeedsBoundsCheck();
    void normalizeMissingImageFails();
    void normalizeAcceptsSparseFlagOnRawFile();
    void normalizeRejectsWraparoundSparseDeclaration();
    void validateRejectsWraparoundGeometry();
    void errorNamesEraseEntryUniquely();
    void sparseNumSectorsFromHeader();
    // Task 3：来源探测 + OPS 元数据 + GPT 回填对账
    void opsSourceUsesMetadataAndGpt();
    void opsReconcilesGptLayout4096();
    void opsReconcilesGptLayout512();
    void opsKeepsMetadataWhenGptLacksPartition();
    void opsAcceptsMetadataConsistentWithGpt();
    void opsPatchGroupsAndMissingPatchWarning();
    void opsGroupTagAndUfsProvisionGiveLun();
    void dirPrefersRawprogramAndFallsBackWhenEmpty();
    void dirNormalizesSparseImageFromHeader();
    void dirWithoutPlanReportsXmlList();
    void storageTypeFromProgrammerFile();
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

// brief 补充用例：<erase> 省略 start_sector/num_partition_sectors = 整 LUN 擦
// （reference/qdl/src/firehose.c:611-628 的语义；qdl 的 XML 加载器反而要求四属性齐全，见 program.c:39-59）
void TestFlashPlan::eraseTagWithoutRangeMeansWholeLun()
{
    QTemporaryDir dir;
    const QString xml = writeFile(dir.path(), "rawprogram0.xml",
        "<data>\n"
        "  <erase SECTOR_SIZE_IN_BYTES=\"4096\" physical_partition_number=\"0\" />\n"
        "</data>\n");
    QList<edl::PlanEntry> out; QStringList warn; QString err;
    QVERIFY2(edl::parseRawprogramXml(xml, 0, out, warn, &err), qPrintable(err));
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].action, edl::PlanEntry::Action::Erase);
    QCOMPARE(out[0].startSector, quint64(0));   // 0/0 = 整 LUN，由会话层解释
    QCOMPARE(out[0].numSectors, quint64(0));
    QCOMPARE(warn.size(), 0);
}

// brief 补充用例：XML 格式错误 → 返回 false，错误文案带文件名与 reader 错误串
void TestFlashPlan::malformedXmlReportsPathAndReaderError()
{
    QTemporaryDir dir;
    const QString xml = writeFile(dir.path(), "rawprogram0.xml",
        "<data>\n  <program SECTOR_SIZE_IN_BYTES=\"4096\" filename=\"x.img\" />\n");  // 未闭合
    QVERIFY(!xml.isEmpty());
    QList<edl::PlanEntry> out; QStringList warn; QString err;
    QVERIFY(!edl::parseRawprogramXml(xml, 0, out, warn, &err));
    QVERIFY(err.contains(xml));         // 带文件名，便于定位
    QVERIFY(!err.isEmpty());            // 含 reader.errorString()（文本随 Qt 版本变化，只判非空）
    QVERIFY(!err.contains(QLatin1String("(null)")));   // errorString() 必须真的拼进来了
}

// lead 裁决补充：patch 的 start_sector 是 firehose 表达式 → 条目不丢、原样保留、不记 warning
// （形态取自真实样本 reference/qdl/tests/data/patch0.xml 的 Backup-GPT 头修补条目）
void TestFlashPlan::keepsExpressionStartSector()
{
    QTemporaryDir dir;
    const QString xml = writeFile(dir.path(), "patch0.xml",
        "<patches>\n"
        "  <patch SECTOR_SIZE_IN_BYTES=\"4096\" byte_offset=\"168\" filename=\"DISK\"\n"
        "         physical_partition_number=\"0\" size_in_bytes=\"8\"\n"
        "         start_sector=\"NUM_DISK_SECTORS-5.\" value=\"NUM_DISK_SECTORS-6.\"\n"
        "         what=\"Update last partition 2 with actual size in Backup Header.\" />\n"
        "</patches>\n");
    QList<edl::PlanEntry> out; QStringList warn; QString err;
    QVERIFY2(edl::parsePatchXml(xml, 0, out, warn, &err), qPrintable(err));
    QCOMPARE(out.size(), 1);                                                  // 表达式条目不再被丢弃
    QCOMPARE(warn.size(), 0);                                                 // 表达式不是错误
    QCOMPARE(out[0].startSectorExpr, QStringLiteral("NUM_DISK_SECTORS-5."));  // 原样保留
    QCOMPARE(out[0].startSector, quint64(0));                                 // 契约：expr 非空时 startSector==0
    QCOMPARE(out[0].value, QStringLiteral("NUM_DISK_SECTORS-6."));
    QCOMPARE(out[0].imageFile, QStringLiteral("DISK"));
}

// program 侧同款（真实样本 rawprogram0.xml 的 label=BackupGPT）
void TestFlashPlan::keepsExpressionStartSectorInProgram()
{
    QTemporaryDir dir;
    const QString xml = writeFile(dir.path(), "rawprogram0.xml",
        "<data>\n"
        "  <program SECTOR_SIZE_IN_BYTES=\"4096\" filename=\"gpt_backup0.bin\" label=\"BackupGPT\"\n"
        "           num_partition_sectors=\"5\" physical_partition_number=\"0\"\n"
        "           start_sector=\"NUM_DISK_SECTORS-5.\" file_sector_offset=\"0\" />\n"
        "</data>\n");
    QList<edl::PlanEntry> out; QStringList warn; QString err;
    QVERIFY2(edl::parseRawprogramXml(xml, 0, out, warn, &err), qPrintable(err));
    QCOMPARE(out.size(), 1);                                                  // 表达式条目不再被丢弃
    QCOMPARE(warn.size(), 0);
    QCOMPARE(out[0].startSectorExpr, QStringLiteral("NUM_DISK_SECTORS-5."));
    QCOMPARE(out[0].startSector, quint64(0));
    QCOMPARE(out[0].partitionName, QStringLiteral("BackupGPT"));
    QCOMPARE(out[0].numSectors, quint64(5));
    QCOMPARE(out[0].imageFile, dir.path() + "/gpt_backup0.bin");
}

// Task 1 审查发现的**安全问题**（控制器指派的 Task 2 修复项）：
// `<erase num_partition_sectors="abc">` 属性存在但不可解析时被静默当成 0，而 0 在本模型里的
// 语义是"整 LUN 擦"—— 等于把定点擦静默放大成整盘擦。qdl 对这类输入是直接拒绝的
// （reference/qdl/src/program.c:56-62 "erase tag with num_sectors=0 not allowed"）。
// 修复后的语义：**只有属性确实缺失才保留"整 LUN 擦"**；其余（不可解析 / 表达式 / 显式 0）
// 一律丢弃该条目 + 中文 warning（与本文件"宁可少条目并告警"的原则一致）。
void TestFlashPlan::eraseWithUnparseableCountIsDropped()
{
    QTemporaryDir dir;
    const QString xml = writeFile(dir.path(), "rawprogram0.xml",
        "<data>\n"
        "  <erase SECTOR_SIZE_IN_BYTES=\"4096\" physical_partition_number=\"0\"\n"
        "         start_sector=\"2048\" num_partition_sectors=\"abc\" />\n"                    // 不可解析
        "  <erase SECTOR_SIZE_IN_BYTES=\"4096\" physical_partition_number=\"0\"\n"
        "         start_sector=\"4096\" num_partition_sectors=\"NUM_DISK_SECTORS-1.\" />\n"  // 表达式
        "  <erase SECTOR_SIZE_IN_BYTES=\"4096\" physical_partition_number=\"1\"\n"
        "         start_sector=\"8192\" num_partition_sectors=\"0\" />\n"                    // 显式 0
        "  <erase SECTOR_SIZE_IN_BYTES=\"4096\" physical_partition_number=\"0\"\n"
        "         start_sector=\"65536\" num_partition_sectors=\"0x800\" />\n"               // 十六进制
        "  <erase SECTOR_SIZE_IN_BYTES=\"4096\" physical_partition_number=\"0\" />\n"        // 缺失 = 整 LUN
        "</data>\n");
    QList<edl::PlanEntry> out; QStringList warn; QString err;
    QVERIFY2(edl::parseRawprogramXml(xml, 0, out, warn, &err), qPrintable(err));
    QCOMPARE(out.size(), 2);                       // 十六进制计数 + "属性确实缺失"的整 LUN 各一条
    QCOMPARE(out[0].action, edl::PlanEntry::Action::Erase);
    QCOMPARE(out[0].lun, quint32(0));
    // 计数是**纯数值属性**：base 0 解析（0x 前缀当数值接受），与 qdl attr_as_unsigned →
    // strtoul(value, NULL, 0) 一致（reference/qdl/src/util.c:80）—— 别把它当"表达式"丢掉
    QCOMPARE(out[0].numSectors, quint64(0x800));
    QCOMPARE(out[1].lun, quint32(0));
    QCOMPARE(out[1].numSectors, quint64(0));       // 0/0 = 整 LUN（由会话层解释）
    QCOMPARE(warn.size(), 3);                      // 三条被丢弃，每条都有 warning
    for (const QString &w : warn)
        QVERIFY(w.contains(QStringLiteral("num_partition_sectors")));
}

// ---- Task 2：排序/统计 + validatePlan + sparse 扇区换算 ----

// 排序：Erase 一律在前，Program 按 (lun, startSector) 升序，Patch 一律最后；
// totalBytes = Program 条目 rawBytes（为 0 时退化为 numSectors × sectorSize）之和，Patch 不计入。
void TestFlashPlan::finalizePlanSortsAndSums()
{
    edl::FlashPlan plan;
    edl::PlanEntry pat;                                  // Patch：应排最后
    pat.action = edl::PlanEntry::Action::Patch; pat.partitionName = QStringLiteral("p-disk");
    pat.lun = 1; pat.startSector = 3; pat.sizeInBytes = 4; pat.imageFile = QStringLiteral("DISK");
    edl::PlanEntry p2;                                   // lun 1 的 Program
    p2.action = edl::PlanEntry::Action::Program; p2.partitionName = QStringLiteral("p2");
    p2.lun = 1; p2.startSector = 10; p2.numSectors = 2; p2.sectorSize = 4096;
    edl::PlanEntry er;                                   // Erase：应排最前
    er.action = edl::PlanEntry::Action::Erase; er.lun = 1; er.sectorSize = 4096;
    edl::PlanEntry p1;                                   // lun 0，startSector 5：rawBytes 优先
    p1.action = edl::PlanEntry::Action::Program; p1.partitionName = QStringLiteral("p1");
    p1.lun = 0; p1.startSector = 5; p1.numSectors = 1; p1.sectorSize = 4096; p1.rawBytes = 4096;
    edl::PlanEntry p0;                                   // lun 0，startSector 1：rawBytes=0 → 用 4×4096
    p0.action = edl::PlanEntry::Action::Program; p0.partitionName = QStringLiteral("p0");
    p0.lun = 0; p0.startSector = 1; p0.numSectors = 4; p0.sectorSize = 4096;
    plan.entries = {pat, p2, er, p1, p0};

    edl::finalizePlan(plan);
    QCOMPARE(plan.entries.size(), 5);
    QCOMPARE(plan.entries[0].action, edl::PlanEntry::Action::Erase);
    QCOMPARE(plan.entries[1].partitionName, QStringLiteral("p0"));   // lun 0，扇区 1
    QCOMPARE(plan.entries[2].partitionName, QStringLiteral("p1"));   // lun 0，扇区 5
    QCOMPARE(plan.entries[3].partitionName, QStringLiteral("p2"));   // lun 1，扇区 10
    QCOMPARE(plan.entries[4].action, edl::PlanEntry::Action::Patch);
    QCOMPARE(plan.totalBytes, quint64(4096 + 4 * 4096 + 2 * 4096));  // Patch 不计入
}

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

// 正路径 + 边界：相邻区间不算重叠；末条目恰好顶到设备容量（start+num == totalBlocks）必须通过；
// sha256 非空只记 warning（不读整文件，规则 6）。
void TestFlashPlan::validateAcceptsPlanAtCapacityAndAdjacent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString img = writeFile(dir.path(), "a.img", QByteArray(4096 * 16, '\x11'));
    QVERIFY(!img.isEmpty());

    edl::FlashPlan plan;
    edl::PlanEntry a; a.action = edl::PlanEntry::Action::Program;
    a.partitionName = QStringLiteral("a"); a.imageFile = img;
    a.lun = 0; a.startSector = 0; a.numSectors = 3; a.sectorSize = 4096;
    edl::PlanEntry b = a; b.partitionName = QStringLiteral("b");
    b.startSector = 3; b.numSectors = 4;                       // [3,7) 紧邻 [0,3)：不重叠
    b.sha256 = QString(64, QLatin1Char('a'));
    edl::PlanEntry last = a; last.partitionName = QStringLiteral("last");
    last.startSector = 900; last.numSectors = 100;             // [900,1000) 恰好顶到容量
    plan.entries = {a, b, last};

    QStringList nwarn; QString nerr;
    QVERIFY2(edl::normalizePlan(plan, nwarn, &nerr), qPrintable(nerr));   // 调用链：normalize → validate
    QCOMPARE(nwarn.size(), 0);                                 // 无 sparse、文件都在 → 无归一化告警

    QList<edl::StorageInfo> dev;
    dev.append({0, 1000, 4096});
    // 归一化后是纯校验：把 plan 当 const 传，钉死"validatePlan 不改入参"的签名契约
    const edl::FlashPlan constPlan = plan;
    const edl::PlanCheck chk = edl::validatePlan(constPlan, dev);
    QVERIFY2(chk.ok, qPrintable(chk.errors.join(QStringLiteral(" | "))));
    QVERIFY(chk.errors.isEmpty());
    QCOMPARE(chk.warnings.size(), 1);                          // 仅 sha256 提示
    QVERIFY(chk.warnings[0].contains(QStringLiteral("sha256")));
    QVERIFY(chk.warnings[0].contains(QStringLiteral("b")));
}

// 缺该 LUN 的 StorageInfo（getstorageinfo 未返回）→ error（规则 1），且逐 LUN 只报一次。
void TestFlashPlan::validateMissingLunGeometryIsError()
{
    QTemporaryDir dir;
    const QString img = writeFile(dir.path(), "a.img", QByteArray(4096, '\x11'));
    QVERIFY(!img.isEmpty());

    edl::FlashPlan plan;
    edl::PlanEntry a; a.action = edl::PlanEntry::Action::Program;
    a.partitionName = QStringLiteral("a"); a.imageFile = img;
    a.lun = 3; a.startSector = 0; a.numSectors = 1; a.sectorSize = 4096;
    edl::PlanEntry b = a; b.partitionName = QStringLiteral("b2");
    plan.entries = {a, b};

    QStringList nwarn; QString nerr;
    QVERIFY2(edl::normalizePlan(plan, nwarn, &nerr), qPrintable(nerr));

    QList<edl::StorageInfo> dev;
    dev.append({0, 1000, 4096});                               // 只有 LUN 0 的几何
    const edl::PlanCheck chk = edl::validatePlan(plan, dev);
    QVERIFY(!chk.ok);
    QCOMPARE(chk.errors.size(), 1);                            // 同一 LUN 只报一次，不逐条目刷屏
    QVERIFY(chk.errors[0].contains(QStringLiteral("LUN 3")));
    QVERIFY(chk.errors[0].contains(QStringLiteral("几何")));
}

// 表达式条目（startSectorExpr 非空）：跳过规则 1/2，且汇总成**一条** warning（不是每条一次）。
// 两条 expression 条目的 startSector 都是 0 —— 若参与几何校验会互相判为重叠。
//
// **回归钉死（lead 审查 Important）**：规则 3（sectorSize）/ 规则 6（sha256）**不受**规则 7 影响，
// 表达式条目照样要记 —— 所以这里给 expr 条目配上"扇区不符 + sha256"，并按**内容**逐条断言。
// 若把 expr 的 continue 提到规则 3/6 之前（pre-fix 顺序），sectorWarn/shaWarn 会掉到 0，本用例必红；
// 旧写法只断言 `warnings.size() == 1`，退回 pre-fix 顺序照样通过，等于没钉住。
void TestFlashPlan::validateSkipsExpressionEntries()
{
    QTemporaryDir dir;
    const QString img = writeFile(dir.path(), "gpt_backup0.bin", QByteArray(4096 * 5, '\x00'));
    QVERIFY(!img.isEmpty());

    edl::FlashPlan plan;
    edl::PlanEntry a; a.action = edl::PlanEntry::Action::Program;
    a.partitionName = QStringLiteral("BackupGPT"); a.imageFile = img;
    a.lun = 0; a.numSectors = 5; a.sectorSize = 512;                 // 与设备 blockSize 4096 不符 → 规则 3
    a.sha256 = QStringLiteral("abcdef0123456789abcdef0123456789");   // → 规则 6
    a.startSectorExpr = QStringLiteral("NUM_DISK_SECTORS-5.");
    edl::PlanEntry b = a; b.partitionName = QStringLiteral("BackupGPT2");
    b.startSectorExpr = QStringLiteral("NUM_DISK_SECTORS-11.");
    plan.entries = {a, b};

    QStringList nwarn; QString nerr;
    QVERIFY2(edl::normalizePlan(plan, nwarn, &nerr), qPrintable(nerr));
    QCOMPARE(nwarn.size(), 0);                                       // 归一化不管这些（非 sparse、文件在）

    QList<edl::StorageInfo> dev;
    dev.append({0, 1000, 4096});
    const edl::PlanCheck chk = edl::validatePlan(plan, dev);
    QVERIFY2(chk.ok, qPrintable(chk.errors.join(QStringLiteral(" | "))));   // 表达式条目不是 error

    int sectorWarn = 0, shaWarn = 0, exprWarn = 0;
    for (const QString &w : chk.warnings) {
        if (w.contains(QStringLiteral("sectorSize"))) {
            ++sectorWarn;
            QVERIFY(w.contains(QStringLiteral("512")) && w.contains(QStringLiteral("4096")));
        }
        if (w.contains(QStringLiteral("sha256"))) {
            ++shaWarn;
            QVERIFY(w.contains(QStringLiteral("abcdef012345")));     // 截断哈希可定位到条目
        }
        if (w.contains(QStringLiteral("表达式"))) {
            ++exprWarn;
            QVERIFY(w.contains(QStringLiteral("2 个条目")));
            QVERIFY(w.contains(QStringLiteral("未参与设备几何校验")));
        }
    }
    QCOMPARE(sectorWarn, 2);        // 规则 3：两条 expr 条目各一条（不是 0 —— 见上面的回归说明）
    QCOMPARE(shaWarn, 2);           // 规则 6：同上
    QCOMPARE(exprWarn, 1);          // 规则 7：汇总一条，不是每条一次
    QCOMPARE(chk.warnings.size(), 5);
}

// 规则 5：sparse 头声明的去 sparse 大小与 XML 的 numSectors × sectorSize 不符 →
// normalizePlan 以头为准修正 numSectors + warning（回填 rawBytes）；validatePlan 不改 plan。
void TestFlashPlan::validateSparseHeaderCorrectsNumSectors()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString img = writeFile(dir.path(), "system.img", sparseHeaderBytes(100, 4096));
    QVERIFY(!img.isEmpty());

    edl::FlashPlan plan;
    edl::PlanEntry a; a.action = edl::PlanEntry::Action::Program;
    a.partitionName = QStringLiteral("system"); a.imageFile = img; a.sparse = true;
    a.lun = 0; a.startSector = 0; a.numSectors = 10; a.sectorSize = 4096;   // XML 声明 10 扇区（错）
    plan.entries = {a};

    QStringList nwarn; QString nerr;
    QVERIFY2(edl::normalizePlan(plan, nwarn, &nerr), qPrintable(nerr));
    QCOMPARE(plan.entries[0].numSectors, quint64(100));                    // 头声明 100 扇区
    QCOMPARE(plan.entries[0].rawBytes, quint64(100 * 4096));               // 顺带回填 rawBytes
    QCOMPARE(nwarn.size(), 1);
    QVERIFY(nwarn[0].contains(QStringLiteral("system")));
    QVERIFY(nwarn[0].contains(QStringLiteral("100")));

    QList<edl::StorageInfo> dev;
    dev.append({0, 1000, 4096});
    const edl::PlanCheck chk = edl::validatePlan(plan, dev);
    QVERIFY2(chk.ok, qPrintable(chk.errors.join(QStringLiteral(" | "))));
    QVERIFY(chk.warnings.isEmpty());                                       // 修正已在归一化阶段报过，不重复
}

// 归一化后的扇区数必须**参与**几何校验（先 normalize、后 validate）：
// 头声明 2000 扇区 > LUN 0 的 1000 扇区 → 越界；若按 XML 的 10 扇区放行，下盘就会写穿分区。
void TestFlashPlan::validateSparseCorrectionFeedsBoundsCheck()
{
    QTemporaryDir dir;
    const QString img = writeFile(dir.path(), "system.img", sparseHeaderBytes(2000, 4096));
    QVERIFY(!img.isEmpty());

    edl::FlashPlan plan;
    edl::PlanEntry a; a.action = edl::PlanEntry::Action::Program;
    a.partitionName = QStringLiteral("system"); a.imageFile = img; a.sparse = true;
    a.lun = 0; a.startSector = 0; a.numSectors = 10; a.sectorSize = 4096;
    plan.entries = {a};

    QStringList nwarn; QString nerr;
    QVERIFY2(edl::normalizePlan(plan, nwarn, &nerr), qPrintable(nerr));
    QCOMPARE(plan.entries[0].numSectors, quint64(2000));

    QList<edl::StorageInfo> dev;
    dev.append({0, 1000, 4096});
    const edl::PlanCheck chk = edl::validatePlan(plan, dev);
    QVERIFY(!chk.ok);
    bool hasOob = false;
    for (const QString &e : chk.errors)
        if (e.contains(QStringLiteral("越界"))) hasOob = true;
    QVERIFY(hasOob);
}

// 规则 4：Program 条目的镜像文件必须存在可读 → 归一化阶段失败（error 里逐行列出全部失败条目）。
// Patch 的 imageFile=="DISK" 是"打设备磁盘"哨兵，不是本地文件，不查也不报。
void TestFlashPlan::normalizeMissingImageFails()
{
    QTemporaryDir dir;
    edl::FlashPlan plan;
    edl::PlanEntry miss; miss.action = edl::PlanEntry::Action::Program;
    miss.partitionName = QStringLiteral("gone");
    miss.imageFile = dir.path() + QStringLiteral("/nope.img");
    miss.lun = 0; miss.startSector = 0; miss.numSectors = 1; miss.sectorSize = 4096;
    edl::PlanEntry pat; pat.action = edl::PlanEntry::Action::Patch;
    pat.partitionName = QStringLiteral("patch-gpt"); pat.imageFile = QStringLiteral("DISK");
    pat.lun = 0; pat.startSector = 1; pat.byteOffset = 8; pat.sizeInBytes = 4;
    plan.entries = {miss, pat};

    QStringList nwarn; QString nerr;
    QVERIFY(!edl::normalizePlan(plan, nwarn, &nerr));
    QVERIFY(nerr.contains(QStringLiteral("gone")));            // 带条目名
    QVERIFY(nerr.contains(QStringLiteral("nope.img")));        // 带路径
    QVERIFY(!nerr.contains(QStringLiteral("DISK")));           // Patch DISK 不查文件

    // 只有 Patch DISK 时归一化必须成功（哨兵不是本地文件）
    edl::FlashPlan patchOnly;
    patchOnly.entries = {pat};
    QStringList w2; QString e2;
    QVERIFY2(edl::normalizePlan(patchOnly, w2, &e2), qPrintable(e2));
    QCOMPARE(w2.size(), 0);
    QCOMPARE(e2, QString());
}

// 规则 5 的 qdl 先例分支（reference/qdl/src/program.c:79-93）：XML 标了 sparse="true" 但文件不是
// sparse —— 文件大小与 SECTOR_SIZE_IN_BYTES × num_partition_sectors 相等 → 判为标记写错，
// 翻转 sparse=false + warning；对不上 → 归一化失败（不猜文件结构）。
void TestFlashPlan::normalizeAcceptsSparseFlagOnRawFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString img = writeFile(dir.path(), "xbl.img", QByteArray(4096 * 8, '\x5A'));
    QVERIFY(!img.isEmpty());

    edl::FlashPlan plan;
    edl::PlanEntry a; a.action = edl::PlanEntry::Action::Program;
    a.partitionName = QStringLiteral("xbl"); a.imageFile = img; a.sparse = true;   // 标记错
    a.lun = 0; a.startSector = 0; a.numSectors = 8; a.sectorSize = 4096;           // 8×4096 == 文件大小
    plan.entries = {a};

    QStringList nwarn; QString nerr;
    QVERIFY2(edl::normalizePlan(plan, nwarn, &nerr), qPrintable(nerr));
    QVERIFY(!plan.entries[0].sparse);                          // 翻转标记
    QCOMPARE(nwarn.size(), 1);
    QVERIFY(nwarn[0].contains(QStringLiteral("xbl")));
    QVERIFY(nwarn[0].contains(QStringLiteral("非 sparse")));

    // 大小对不上 → 失败（同一份文件、声明 9 扇区）
    edl::FlashPlan bad = plan;
    bad.entries[0].sparse = true;
    bad.entries[0].numSectors = 9;
    QStringList w2; QString e2;
    QVERIFY(!edl::normalizePlan(bad, w2, &e2));
    QVERIFY(e2.contains(QStringLiteral("xbl")));
    QVERIFY(e2.contains(QStringLiteral("sparse")));
}

// 规则 1 的回绕防护（lead 审查 Minor）：损坏 XML 里 startSector/numSectors 取极大值时
// `startSector + numSectors` 会回绕成**小值**（max-5 + 10 → 4），旧写法会判"不越界"而放行；
// 现在改用减法/先除后比，必须拒绝，并且文案给"（溢出 u64）"而不是回绕后的垃圾数字。
void TestFlashPlan::validateRejectsWraparoundGeometry()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 18446744073709551610 = 2^64-6（max-5）；+10 回绕成 4 < totalBlocks
    const QString xml = writeFile(dir.path(), "rawprogram0.xml",
        "<data>\n"
        "  <program SECTOR_SIZE_IN_BYTES=\"4096\" filename=\"wrap.img\" label=\"wrap\"\n"
        "           num_partition_sectors=\"10\" physical_partition_number=\"0\"\n"
        "           start_sector=\"18446744073709551610\" file_sector_offset=\"0\" />\n"
        "</data>\n");
    QVERIFY(!xml.isEmpty());
    QList<edl::PlanEntry> out; QStringList warn; QString err;
    QVERIFY2(edl::parseRawprogramXml(xml, 0, out, warn, &err), qPrintable(err));
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].startSector, std::numeric_limits<quint64>::max() - 5);
    QCOMPARE(out[0].numSectors, quint64(10));
    QVERIFY(!writeFile(dir.path(), "wrap.img", QByteArray(4096, '\0')).isEmpty());   // 文件在 → 归一化不拦

    edl::FlashPlan plan;
    plan.entries = out;
    QStringList nwarn; QString nerr;
    QVERIFY2(edl::normalizePlan(plan, nwarn, &nerr), qPrintable(nerr));

    QList<edl::StorageInfo> dev;
    dev.append({0, 1000, 4096});
    const edl::PlanCheck chk = edl::validatePlan(plan, dev);
    QVERIFY(!chk.ok);                                   // 回绕不能让越界条目蒙混过关
    QCOMPARE(chk.errors.size(), 1);
    QVERIFY(chk.errors[0].contains(QStringLiteral("越界")));
    QVERIFY(chk.errors[0].contains(QStringLiteral("溢出")));
    QVERIFY(chk.errors[0].contains(QStringLiteral("wrap")));
}

// 乘法侧的回绕防护：XML 声明 numSectors = 2^52+1、sectorSize 4096 → 真值 2^64+4096 超出 u64，
// 写出会回绕成 4096 == 文件大小，旧写法（numSectors × sectorSize == 文件大小）会误判成
// "sparse 标记写错"而**放行**。现在按扇区数比（先除后比）→ 必须失败。
void TestFlashPlan::normalizeRejectsWraparoundSparseDeclaration()
{
    QTemporaryDir dir;
    const QString img = writeFile(dir.path(), "wrap.img", QByteArray(4096, '\x11'));  // 原始文件，非 sparse
    QVERIFY(!img.isEmpty());

    edl::FlashPlan plan;
    edl::PlanEntry a; a.action = edl::PlanEntry::Action::Program;
    a.partitionName = QStringLiteral("wrap"); a.imageFile = img; a.sparse = true;
    a.lun = 0; a.startSector = 0; a.sectorSize = 4096;
    a.numSectors = (std::numeric_limits<quint64>::max() / 4096) + 2;   // ×4096 回绕成 4096 = 文件大小
    plan.entries = {a};

    QStringList nwarn; QString nerr;
    QVERIFY(!edl::normalizePlan(plan, nwarn, &nerr));
    QVERIFY(nerr.contains(QStringLiteral("wrap")));
    QVERIFY(nerr.contains(QStringLiteral("sparse")));
    QVERIFY(plan.entries[0].sparse);      // 没有被"容错"分支翻转成非 sparse 而放行
}

// lead 审查 Minor：erase 在 XML 里没有名字，文案要带 LUN 与起始扇区（整 LUN 擦写"整 LUN"），
// 多条 erase 才分得清是哪一条。
void TestFlashPlan::errorNamesEraseEntryUniquely()
{
    edl::FlashPlan plan;
    edl::PlanEntry e1; e1.action = edl::PlanEntry::Action::Erase;
    e1.lun = 0; e1.startSector = 10; e1.numSectors = 5; e1.sectorSize = 4096;        // 正常，无告警
    edl::PlanEntry e2 = e1; e2.lun = 1; e2.startSector = 0; e2.numSectors = 0;       // 整 LUN
    e2.sectorSize = 512;                                                             // → 规则 3 warning
    edl::PlanEntry e3 = e1; e3.lun = 0; e3.startSector = 900000; e3.numSectors = 10; // → 越界 error
    plan.entries = {e1, e2, e3};

    QList<edl::StorageInfo> dev;
    dev.append({0, 1000, 4096});
    dev.append({1, 1000, 4096});
    const edl::PlanCheck chk = edl::validatePlan(plan, dev);
    QVERIFY(!chk.ok);
    QCOMPARE(chk.errors.size(), 1);
    QVERIFY(chk.errors[0].contains(QStringLiteral("erase(lun=0, start=900000)")));
    QCOMPARE(chk.warnings.size(), 1);
    QVERIFY(chk.warnings[0].contains(QStringLiteral("erase(lun=1, 整 LUN)")));
}

void TestFlashPlan::sparseNumSectorsFromHeader()
{
    // 合成 sparse 头：magic 0xED26FF3A, version 1.0, header_sz 28, blk_sz 4096, total_blks 3
    // 注：brief 给的字节写在偏移 8/12/16/20/24 上（把 28 字节头当成 7 个 u32 字段），与真实
    // AOSP 布局不符 —— file_hdr_sz/chunk_hdr_sz 是 **u16**（qdl `sparse.h:19-24`、本仓库既有
    // 回归用例 test_sparse.cpp:aospHeaderU16Layout），blk_sz 在 u32@12、total_blks 在 u32@16。
    // 此处按真实布局修正字节位置，断言（raw == 3×4096）原样保留。
    QByteArray h(28, '\0');
    auto put16 = [&h](int off, quint16 v) { h[off] = char(v); h[off + 1] = char(v >> 8); };
    auto put32 = [&h](int off, quint32 v) {
        h[off] = char(v & 0xFF); h[off+1] = char((v >> 8) & 0xFF);
        h[off+2] = char((v >> 16) & 0xFF); h[off+3] = char((v >> 24) & 0xFF);
    };
    put32(0, 0xED26FF3A);
    put16(4, 1); put16(6, 0);        // major 1 / minor 0
    put16(8, 28); put16(10, 12);     // file_hdr_sz 28 / chunk_hdr_sz 12
    put32(12, 4096);                 // blk_sz
    put32(16, 3);                    // total_blks
    put32(20, 1);                    // total_chunks
    quint64 raw = 0;
    QVERIFY(imgsparse::sparseRawSizeFromHeader(h, raw));
    QCOMPARE(raw, quint64(3 * 4096));
    QVERIFY(!imgsparse::sparseRawSizeFromHeader(QByteArray(8, '\0'), raw));  // 头太短
}

// ================= Task 3：来源探测 + OPS 元数据 + GPT 回填对账 =================

// brief 的用例（断言原样保留）。**两处夹具修正**，都在注释里写明：
//   1) 补写 `xbl.img` —— brief 只写了 settings.xml + gpt_main0.bin，但 normalizePlan（Task 2 契约）
//      要求 Program 条目的镜像可打开；缺文件会让 buildPlanFromDir 按设计失败（fail-closed）。
//   2) `buildGptWithPartition` 按 `disk_image.cpp` 的**实际读取条件**构造（见 flash_plan_helpers.h）。
// 断言之外补三条：imageFile 指向包目录、无 label 时用文件名主干作条目标识、不一致告警点名 gpt_main0.bin。
// 本条用的夹具是**默认 512 字节 LBA**（解析器原生布局）；4096 字节 LBA（真实包形态）另见
// opsReconcilesGptLayout4096，两条布局用例都断言"对账真的发生"。
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
    QVERIFY(writeBytes(dir.path() + "/xbl.img", QByteArray(4096, '\x5A')));

    edl::FlashPlan plan; QString err;
    QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
    QCOMPARE(plan.entries.size(), 1);
    QCOMPARE(plan.entries[0].lun, quint32(0));
    QCOMPARE(plan.entries[0].startSector, quint64(4096));                 // 以 GPT 为准
    QCOMPARE(plan.entries[0].numSectors, quint64(12287 - 4096 + 1));
    QVERIFY(plan.source.contains(QStringLiteral("OPS")));
    QVERIFY(!plan.warnings.isEmpty());                                    // 记录了与元数据不符
    // 追加断言：镜像绝对路径、条目标识（无 label → 文件名主干）、元数据 sha256 原样带走、
    // 不一致告警文本点名是哪个 GPT 文件
    QCOMPARE(plan.entries[0].imageFile, dir.path() + "/xbl.img");
    QCOMPARE(plan.entries[0].partitionName, QStringLiteral("xbl"));
    QCOMPARE(plan.entries[0].sha256, QStringLiteral("00"));
    bool mismatchWarned = false;
    for (const QString &w : plan.warnings)
        if (w.contains(QStringLiteral("不一致")) && w.contains(QStringLiteral("gpt_main0.bin")))
            mismatchWarned = true;
    QVERIFY(mismatchWarned);
    // 默认存储类型告警（目录里没有 prog_*_firehose_*）
    bool storageWarned = false;
    for (const QString &w : plan.warnings)
        if (w.contains(QStringLiteral("默认")))
            storageWarned = true;
    QVERIFY(storageWarned);

    // 重复调用不累积：计划对象是本函数的输出（入口清空 entries/warnings）
    edl::FlashPlan again = plan;
    QVERIFY2(edl::buildPlanFromDir(dir.path(), again, &err), qPrintable(err));
    QCOMPARE(again.entries.size(), 1);
    QCOMPARE(again.warnings.size(), plan.warnings.size());
}

// 布局② **4096 字节 LBA = 真实包 `gpt_main{N}.bin` 的形态**（证据见 flash_plan_helpers.h 的 lbaSize
// 注释：edl 子模块真实样本 gpt_sm8180x.bin 24576 B、EFI PART@0x1000；qdl 的 gpt_main1.bin = 6×4096）。
// imgdisk::parseGpt 只按 512 定位 → 本用例钉住 buildPlanFromDir 内的**布局适配**（解析仍走 parseGpt）。
// 夹具是**手写字节**（buildGptWithPartition 的 lbaSize=4096），不读子模块里的真实样本。
// 断言到"对账真的发生"：元数据故意写 2048/16 → 结果必须是 GPT 的 4096/8192 + 不一致告警。
void TestFlashPlan::opsReconcilesGptLayout4096()
{
    QTemporaryDir dir;
    const QString settings =
        "<Firehose><Program0>"
        "<program filename=\"super.img\" label=\"super\" sparse=\"false\" "
        "physical_partition_number=\"0\" start_sector=\"2048\" num_partition_sectors=\"16\" />"
        "</Program0></Firehose>";
    QVERIFY(writeBytes(dir.path() + "/settings.xml", settings.toUtf8()));
    QVERIFY(writeBytes(dir.path() + "/gpt_main0.bin",
                       buildGptWithPartition("super", 4096, 12287, /*lbaSize=*/4096)));
    QVERIFY(writeBytes(dir.path() + "/super.img", QByteArray(65536, '\x11')));

    edl::FlashPlan plan; QString err;
    QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
    QCOMPARE(plan.entries.size(), 1);
    QCOMPARE(plan.entries[0].startSector, quint64(4096));   // 元数据 2048 → 以 GPT 为准
    QCOMPARE(plan.entries[0].numSectors, quint64(12287 - 4096 + 1));
    bool mismatchWarned = false;
    for (const QString &w : plan.warnings)
        if (w.contains(QStringLiteral("不一致")) && w.contains(QStringLiteral("gpt_main0.bin")))
            mismatchWarned = true;
    QVERIFY(mismatchWarned);                                 // 对账确实发生（不是只 parse 成功）
}

// 布局① **512 字节 LBA = 解析器原生布局**（适配分支不介入，直接交 parseGpt）。与 4096 那条成对，
// 同样断言"对账真的发生"：元数据 7/2 与 GPT 6/901 不一致 → 以 GPT 为准 + 告警点名 gpt_main0.bin。
// （数字取自 reference/qdl/tests/data/rawprogram1.xml:5 的真实 xbl_a 几何，便于对照。）
void TestFlashPlan::opsReconcilesGptLayout512()
{
    QTemporaryDir dir;
    const QString settings =
        "<Firehose><Program0>"
        "<program filename=\"boot.img\" label=\"boot_a\" sparse=\"false\" "
        "physical_partition_number=\"0\" start_sector=\"7\" num_partition_sectors=\"2\" />"
        "</Program0></Firehose>";
    QVERIFY(writeBytes(dir.path() + "/settings.xml", settings.toUtf8()));
    QVERIFY(writeBytes(dir.path() + "/gpt_main0.bin",
                       buildGptWithPartition("boot_a", 6, 906, /*lbaSize=*/512)));
    QVERIFY(writeBytes(dir.path() + "/boot.img", QByteArray(4096, '\x12')));

    edl::FlashPlan plan; QString err;
    QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
    QCOMPARE(plan.entries.size(), 1);
    QCOMPARE(plan.entries[0].startSector, quint64(6));       // 元数据 7 → 以 GPT 为准
    QCOMPARE(plan.entries[0].numSectors, quint64(901));      // 元数据 2 → 以 GPT 为准
    bool mismatchWarned = false;
    for (const QString &w : plan.warnings)
        if (w.contains(QStringLiteral("不一致")) && w.contains(QStringLiteral("gpt_main0.bin")))
            mismatchWarned = true;
    QVERIFY(mismatchWarned);                                 // 对账确实发生
}

// 对账态之二：GPT 里**查不到**该分区名 → 保留元数据几何 + warning。
// 同时钉住"包内偏移字段一律忽略"：FileOffsetInSrc/SizeInSectorInSrc/SizeInByteInSrc 若被误当几何，
// startSector/numSectors 会变成 999/777/555，本用例断言它们**仍是元数据的 2/3**（协议速查 §5）。
void TestFlashPlan::opsKeepsMetadataWhenGptLacksPartition()
{
    QTemporaryDir dir;
    const QString settings =
        "<Firehose><Program0>"
        "<program filename=\"xbl.img\" label=\"xbl\" sparse=\"false\" "
        "FileOffsetInSrc=\"999\" SizeInSectorInSrc=\"777\" SizeInByteInSrc=\"555\" "
        "physical_partition_number=\"0\" start_sector=\"2\" num_partition_sectors=\"3\" />"
        "</Program0></Firehose>";
    QVERIFY(writeBytes(dir.path() + "/settings.xml", settings.toUtf8()));
    QVERIFY(writeBytes(dir.path() + "/gpt_main0.bin",
                       buildGptWithPartition("modem", 4096, 12287)));     // 表里没有 xbl
    QVERIFY(writeBytes(dir.path() + "/xbl.img", QByteArray(4096, '\x22')));

    edl::FlashPlan plan; QString err;
    QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
    QCOMPARE(plan.entries.size(), 1);
    QCOMPARE(plan.entries[0].startSector, quint64(2));       // 元数据值，InSrc 未被误用
    QCOMPARE(plan.entries[0].numSectors, quint64(3));
    bool keepWarned = false;
    for (const QString &w : plan.warnings)
        if (w.contains(QStringLiteral("查不到")))
            keepWarned = true;
    QVERIFY(keepWarned);
}

// 对账态之一：元数据与 GPT **一致** → 值不变、不产生"不一致/回填"告警（静默成功）。
void TestFlashPlan::opsAcceptsMetadataConsistentWithGpt()
{
    QTemporaryDir dir;
    const QString settings =
        "<Firehose><Program0>"
        "<program filename=\"xbl.img\" label=\"xbl\" sparse=\"false\" "
        "physical_partition_number=\"0\" start_sector=\"4096\" num_partition_sectors=\"8192\" />"
        "</Program0></Firehose>";
    QVERIFY(writeBytes(dir.path() + "/settings.xml", settings.toUtf8()));
    QVERIFY(writeBytes(dir.path() + "/gpt_main0.bin", buildGptWithPartition("xbl", 4096, 12287)));
    QVERIFY(writeBytes(dir.path() + "/xbl.img", QByteArray(4096, '\x33')));

    edl::FlashPlan plan; QString err;
    QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
    QCOMPARE(plan.entries.size(), 1);
    QCOMPARE(plan.entries[0].startSector, quint64(4096));
    QCOMPARE(plan.entries[0].numSectors, quint64(8192));
    for (const QString &w : plan.warnings) {
        QVERIFY2(!w.contains(QStringLiteral("不一致")), qPrintable(w));
        QVERIFY2(!w.contains(QStringLiteral("回填")), qPrintable(w));
    }
}

// <Patch{N}> 组 → Action::Patch（8 属性同 patch XML，直接复用 loadPatchTag 的规则：非 DISK 跳过）。
// 另半边：**没有** patch 分组时必须出"GPT 头定点修补缺失"告警（spec §3.4 要求不静默）。
void TestFlashPlan::opsPatchGroupsAndMissingPatchWarning()
{
    {
        QTemporaryDir dir;
        const QString settings =
            "<Firehose>\n"
            "  <Program0>\n"
            "  </Program0>\n"
            "  <Patch0>\n"
            "    <patch start_sector=\"1\" byte_offset=\"16\" physical_partition_number=\"0\"\n"
            "           size_in_bytes=\"4\" value=\"CRC32(1,92)\" filename=\"DISK\"\n"
            "           SECTOR_SIZE_IN_BYTES=\"4096\" what=\"Update Primary Header with CRC.\" />\n"
            "    <patch start_sector=\"1\" byte_offset=\"16\" physical_partition_number=\"0\"\n"
            "           size_in_bytes=\"4\" value=\"CRC32(1,92)\" filename=\"gpt_main0.bin\"\n"
            "           SECTOR_SIZE_IN_BYTES=\"4096\" what=\"离线改 bin 用\" />\n"
            "  </Patch0>\n"
            "</Firehose>\n";
        QVERIFY(writeBytes(dir.path() + "/settings.xml", settings.toUtf8()));

        edl::FlashPlan plan; QString err;
        QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
        QCOMPARE(plan.entries.size(), 1);                       // 非 DISK 那条被跳过
        QCOMPARE(plan.entries[0].action, edl::PlanEntry::Action::Patch);
        QCOMPARE(plan.entries[0].byteOffset, quint64(16));
        QCOMPARE(plan.entries[0].sizeInBytes, quint32(4));
        QCOMPARE(plan.entries[0].value, QStringLiteral("CRC32(1,92)"));  // 表达式原样
        QCOMPARE(plan.entries[0].imageFile, QStringLiteral("DISK"));
        bool skipWarned = false, missingPatchWarned = false;
        for (const QString &w : plan.warnings) {
            if (w.contains(QStringLiteral("非 DISK"))) skipWarned = true;
            if (w.contains(QStringLiteral("未找到 patch 分组"))) missingPatchWarned = true;
        }
        QVERIFY(skipWarned);
        QVERIFY(!missingPatchWarned);                           // 有 Patch 组就不该报"缺失"
    }
    {
        QTemporaryDir dir;
        // 只有 Program 组（条目几何留在元数据里，GPT 缺失 → 只是一条"未对账"告警）
        const QString settings =
            "<Firehose><Program0>"
            "<program filename=\"xbl.img\" label=\"xbl\" sparse=\"false\" "
            "physical_partition_number=\"0\" start_sector=\"1\" num_partition_sectors=\"1\" />"
            "</Program0></Firehose>";
        QVERIFY(writeBytes(dir.path() + "/settings.xml", settings.toUtf8()));
        QVERIFY(writeBytes(dir.path() + "/xbl.img", QByteArray(4096, '\x44')));

        edl::FlashPlan plan; QString err;
        QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
        bool missingPatchWarned = false;
        for (const QString &w : plan.warnings)
            if (w.contains(QStringLiteral("未找到 patch 分组"))
                && w.contains(QStringLiteral("刷后可能无法引导")))
                missingPatchWarned = true;
        QVERIFY(missingPatchWarned);
    }
}

// 组标签 → lun：`Program{N}` 取末尾数字（同 rawprogram{N}.xml 的约定），`UFS_PROVISION` 恒 0。
// 另钉住两条既有语义：
//   * `<File Path=…>`（真实 UFS_PROVISION 子元素形态，无 filename）**不产条目**；
//   * 条目属性 `physical_partition_number` 与组序号不一致 → 走既有 warnLunMismatch 告警通道，
//     且**以属性为准**（Task 1 契约，本任务不得删该通道）。
void TestFlashPlan::opsGroupTagAndUfsProvisionGiveLun()
{
    QTemporaryDir dir;
    const QString settings =
        "<Firehose>\n"
        "  <Program1>\n"
        "    <program filename=\"xbl.img\" label=\"xbl_a\" sparse=\"false\"\n"
        "             physical_partition_number=\"1\" start_sector=\"6\" num_partition_sectors=\"901\"\n"
        "             SECTOR_SIZE_IN_BYTES=\"4096\" />\n"
        "  </Program1>\n"
        "  <UFS_PROVISION>\n"
        "    <File Path=\"provision.xml\" FileOffsetInSrc=\"0\" SizeInByteInSrc=\"10\" />\n"
        "    <program filename=\"persist.img\" label=\"persist\" sparse=\"false\"\n"
        "             physical_partition_number=\"0\" start_sector=\"100\" num_partition_sectors=\"8\"\n"
        "             SECTOR_SIZE_IN_BYTES=\"4096\" />\n"
        "  </UFS_PROVISION>\n"
        "  <Program2>\n"
        "    <program filename=\"modem.img\" label=\"modem_a\" sparse=\"false\"\n"
        "             physical_partition_number=\"3\" start_sector=\"200\" num_partition_sectors=\"9\"\n"
        "             SECTOR_SIZE_IN_BYTES=\"4096\" />\n"
        "  </Program2>\n"
        "</Firehose>\n";
    QVERIFY(writeBytes(dir.path() + "/settings.xml", settings.toUtf8()));
    QVERIFY(writeBytes(dir.path() + "/xbl.img", QByteArray(4096, '\x55')));
    QVERIFY(writeBytes(dir.path() + "/persist.img", QByteArray(4096, '\x66')));
    QVERIFY(writeBytes(dir.path() + "/modem.img", QByteArray(4096, '\x77')));

    edl::FlashPlan plan; QString err;
    QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
    QCOMPARE(plan.entries.size(), 3);                 // provision.xml（无 filename）不产条目
    QHash<QString, edl::PlanEntry> byName;
    for (const edl::PlanEntry &e : plan.entries)
        byName.insert(e.partitionName, e);
    QVERIFY(byName.contains(QStringLiteral("xbl_a")));
    QCOMPARE(byName.value(QStringLiteral("xbl_a")).lun, quint32(1));    // Program1 → 1
    QVERIFY(byName.contains(QStringLiteral("persist")));
    QCOMPARE(byName.value(QStringLiteral("persist")).lun, quint32(0));  // UFS_PROVISION → 0
    QCOMPARE(byName.value(QStringLiteral("persist")).startSector, quint64(100));
    QVERIFY(byName.contains(QStringLiteral("modem_a")));
    QCOMPARE(byName.value(QStringLiteral("modem_a")).lun, quint32(3));  // 属性 wins（Program2 → 告警）
    QVERIFY(!byName.contains(QStringLiteral("provision.xml")));
    // finalizePlan 确实在 buildPlanFromDir 内部跑了：顺序按 (lun, startSector)、totalBytes 是
    // Program 条目 numSectors × sectorSize 之和（rawBytes 为 0 时的退化口径，见 finalizePlan 注释）
    QCOMPARE(plan.entries[0].partitionName, QStringLiteral("persist"));   // lun 0
    QCOMPARE(plan.entries[1].partitionName, QStringLiteral("xbl_a"));     // lun 1
    QCOMPARE(plan.entries[2].partitionName, QStringLiteral("modem_a"));   // lun 3
    QCOMPARE(plan.totalBytes, quint64(8 + 901 + 9) * 4096);

    bool lunMismatchWarned = false;
    for (const QString &w : plan.warnings)
        if (w.contains(QStringLiteral("不一致")) && w.contains(QStringLiteral("lun=3")))
            lunMismatchWarned = true;
    QVERIFY(lunMismatchWarned);
}

// 来源优先级：有 rawprogram*.xml 就用它（即使 settings.xml 同时在，也不产 OPS 条目/告警）；
// rawprogram*.xml 存在但**产出 0 条目**时回退 settings.xml（并记 warning，不静默）。
void TestFlashPlan::dirPrefersRawprogramAndFallsBackWhenEmpty()
{
    {
        QTemporaryDir dir;
        QVERIFY(writeBytes(dir.path() + "/rawprogram0.xml",
                           "<data><program SECTOR_SIZE_IN_BYTES=\"4096\" filename=\"boot.img\" "
                           "label=\"boot_a\" num_partition_sectors=\"16\" "
                           "physical_partition_number=\"0\" start_sector=\"1234\" "
                           "file_sector_offset=\"0\" /></data>"));
        QVERIFY(writeBytes(dir.path() + "/boot.img", QByteArray(65536, '\x88')));
        QVERIFY(writeBytes(dir.path() + "/settings.xml",
                           "<Firehose><Program0><program filename=\"never.img\" label=\"never\" "
                           "physical_partition_number=\"0\" start_sector=\"5\" "
                           "num_partition_sectors=\"1\" /></Program0></Firehose>"));

        edl::FlashPlan plan; QString err;
        QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
        QCOMPARE(plan.entries.size(), 1);
        QCOMPARE(plan.entries[0].partitionName, QStringLiteral("boot_a"));   // 只有 XML 里那份
        QCOMPARE(plan.entries[0].startSector, quint64(1234));
        QVERIFY(plan.source.contains(QStringLiteral("rawprogram")));
        for (const QString &w : plan.warnings)                              // OPS 分支没被跑
            QVERIFY2(!w.contains(QStringLiteral("未找到 patch 分组")), qPrintable(w));
    }
    {
        QTemporaryDir dir;
        QVERIFY(writeBytes(dir.path() + "/rawprogram0.xml", "<data></data>"));   // 空 → 0 条目
        QVERIFY(writeBytes(dir.path() + "/settings.xml",
                           "<Firehose><Program0><program filename=\"xbl.img\" label=\"xbl\" "
                           "sparse=\"false\" physical_partition_number=\"0\" start_sector=\"7\" "
                           "num_partition_sectors=\"2\" /></Program0></Firehose>"));
        QVERIFY(writeBytes(dir.path() + "/xbl.img", QByteArray(4096, '\x99')));

        edl::FlashPlan plan; QString err;
        QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
        QCOMPARE(plan.entries.size(), 1);
        QCOMPARE(plan.entries[0].partitionName, QStringLiteral("xbl"));
        QCOMPARE(plan.entries[0].startSector, quint64(7));
        QVERIFY(plan.source.contains(QStringLiteral("OPS")));
        bool fallbackWarned = false;
        for (const QString &w : plan.warnings)
            if (w.contains(QStringLiteral("改用 OPS")))
                fallbackWarned = true;
        QVERIFY(fallbackWarned);
    }
}

// buildPlanFromDir 内部**确实**跑了 normalizePlan（硬要求 3 的调用链）：OPS 元数据声明 sparse="true"
// 且扇区数写错（1），镜像 sparse 头声明 3 块 × 4096 → 以文件头为准修正为 3 扇区 + warning、rawBytes 回填。
// 注：镜像只落 28 字节头 + 1 字节数据 —— normalizePlan 只读头（Task 2 契约），chunk 流校验属于发送侧。
void TestFlashPlan::dirNormalizesSparseImageFromHeader()
{
    QTemporaryDir dir;
    const QString settings =
        "<Firehose><Program0>"
        "<program filename=\"system.img\" label=\"system\" sparse=\"true\" "
        "physical_partition_number=\"0\" start_sector=\"10\" num_partition_sectors=\"1\" />"
        "</Program0></Firehose>";
    QVERIFY(writeBytes(dir.path() + "/settings.xml", settings.toUtf8()));
    QVERIFY(writeBytes(dir.path() + "/system.img",
                       sparseHeaderBytes(3, 4096) + QByteArray(1, '\x01')));   // 头声明 3 块 × 4096

    edl::FlashPlan plan; QString err;
    QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
    QCOMPARE(plan.entries.size(), 1);
    QCOMPARE(plan.entries[0].sparse, true);
    QCOMPARE(plan.entries[0].startSector, quint64(10));      // 元数据值（本目录无 GPT 可对账）
    QCOMPARE(plan.entries[0].numSectors, quint64(3));        // 文件头为准（元数据写 1）
    QCOMPARE(plan.entries[0].rawBytes, quint64(3 * 4096));
    QCOMPARE(plan.totalBytes, quint64(3 * 4096));            // finalizePlan 也跑了（rawBytes 为分母）
    bool corrected = false;
    for (const QString &w : plan.warnings)
        if (w.contains(QStringLiteral("以文件头为准")))
            corrected = true;
    QVERIFY(corrected);
}

// brief 的用例（断言原样保留）+ 补一条：列的是**全部** .xml，且不含非 .xml 文件（帮助诊断）。
void TestFlashPlan::dirWithoutPlanReportsXmlList()
{
    QTemporaryDir dir;
    QVERIFY(writeBytes(dir.path() + "/notes.xml", QByteArray("<x/>")));
    QVERIFY(writeBytes(dir.path() + "/extra.xml", QByteArray("<y/>")));
    QVERIFY(writeBytes(dir.path() + "/readme.txt", QByteArray("<z/>")));
    edl::FlashPlan plan; QString err;
    QVERIFY(!edl::buildPlanFromDir(dir.path(), plan, &err));
    QVERIFY(err.contains(QStringLiteral("未找到")));
    QVERIFY(err.contains(QStringLiteral("notes.xml")));                    // 列举帮助诊断
    QVERIFY(err.contains(QStringLiteral("extra.xml")));
    QVERIFY(!err.contains(QStringLiteral("readme.txt")));
}

// storageType 探测：prog_ufs_firehose_* → "ufs"；prog_emmc_firehose_* → "emmc"；都无 → "ufs" + warning。
// 用"整 LUN 擦"条目做计划源：Erase 不需要镜像文件，能让三个目录都构建成功（最小夹具）。
void TestFlashPlan::storageTypeFromProgrammerFile()
{
    const QByteArray eraseXml = "<data><erase SECTOR_SIZE_IN_BYTES=\"4096\" "
                                "physical_partition_number=\"0\"/></data>";
    {
        QTemporaryDir dir;
        QVERIFY(writeBytes(dir.path() + "/rawprogram0.xml", eraseXml));
        QVERIFY(writeBytes(dir.path() + "/prog_ufs_firehose_ddr.elf", QByteArray(4, '\x01')));
        edl::FlashPlan plan; QString err;
        QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
        QCOMPARE(plan.storageType, QStringLiteral("ufs"));
        for (const QString &w : plan.warnings)
            QVERIFY2(!w.contains(QStringLiteral("默认")), qPrintable(w));
    }
    {
        QTemporaryDir dir;
        QVERIFY(writeBytes(dir.path() + "/rawprogram0.xml", eraseXml));
        QVERIFY(writeBytes(dir.path() + "/prog_emmc_firehose_660.elf", QByteArray(4, '\x02')));
        edl::FlashPlan plan; QString err;
        QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
        QCOMPARE(plan.storageType, QStringLiteral("emmc"));
    }
    {
        QTemporaryDir dir;
        QVERIFY(writeBytes(dir.path() + "/rawprogram0.xml", eraseXml));
        edl::FlashPlan plan; QString err;
        QVERIFY2(edl::buildPlanFromDir(dir.path(), plan, &err), qPrintable(err));
        QCOMPARE(plan.storageType, QStringLiteral("ufs"));
        bool defaultWarned = false;
        for (const QString &w : plan.warnings)
            if (w.contains(QStringLiteral("默认")) && w.contains(QStringLiteral("ufs")))
                defaultWarned = true;
        QVERIFY(defaultWarned);
    }
}

QTEST_APPLESS_MAIN(TestFlashPlan)
#include "test_flash_plan.moc"
