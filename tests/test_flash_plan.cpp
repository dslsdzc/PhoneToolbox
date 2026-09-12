#include <QtTest>
#include <QFile>
#include <QTemporaryDir>
#include "core/edl/flash_plan.h"
#include "image_engine/sparse_image.h"

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
    void validateMissingImageIsError();
    void sparseNumSectorsFromHeader();
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
        "  <erase SECTOR_SIZE_IN_BYTES=\"4096\" physical_partition_number=\"0\" />\n"        // 缺失 = 整 LUN
        "</data>\n");
    QList<edl::PlanEntry> out; QStringList warn; QString err;
    QVERIFY2(edl::parseRawprogramXml(xml, 0, out, warn, &err), qPrintable(err));
    QCOMPARE(out.size(), 1);                       // 只剩"属性确实缺失"的那条
    QCOMPARE(out[0].action, edl::PlanEntry::Action::Erase);
    QCOMPARE(out[0].lun, quint32(0));
    QCOMPARE(out[0].numSectors, quint64(0));       // 0/0 = 整 LUN（由会话层解释）
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

    QList<edl::StorageInfo> dev;
    dev.append({0, 1000, 4096});
    const edl::PlanCheck chk = edl::validatePlan(plan, dev);
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
void TestFlashPlan::validateSkipsExpressionEntries()
{
    QTemporaryDir dir;
    const QString img = writeFile(dir.path(), "gpt_backup0.bin", QByteArray(4096 * 5, '\x00'));
    QVERIFY(!img.isEmpty());

    edl::FlashPlan plan;
    edl::PlanEntry a; a.action = edl::PlanEntry::Action::Program;
    a.partitionName = QStringLiteral("BackupGPT"); a.imageFile = img;
    a.lun = 0; a.numSectors = 5; a.sectorSize = 4096;
    a.startSectorExpr = QStringLiteral("NUM_DISK_SECTORS-5.");
    edl::PlanEntry b = a; b.partitionName = QStringLiteral("BackupGPT2");
    b.startSectorExpr = QStringLiteral("NUM_DISK_SECTORS-11.");
    plan.entries = {a, b};

    QList<edl::StorageInfo> dev;
    dev.append({0, 1000, 4096});
    const edl::PlanCheck chk = edl::validatePlan(plan, dev);
    QVERIFY2(chk.ok, qPrintable(chk.errors.join(QStringLiteral(" | "))));
    QCOMPARE(chk.warnings.size(), 1);
    QVERIFY(chk.warnings[0].contains(QStringLiteral("2 个条目")));
    QVERIFY(chk.warnings[0].contains(QStringLiteral("表达式")));
}

// 规则 5：sparse 头声明的去 sparse 大小与 XML 的 numSectors × sectorSize 不符 → 以头为准修正 + warning。
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

    QList<edl::StorageInfo> dev;
    dev.append({0, 1000, 4096});
    const edl::PlanCheck chk = edl::validatePlan(plan, dev);
    QVERIFY2(chk.ok, qPrintable(chk.errors.join(QStringLiteral(" | "))));
    QCOMPARE(plan.entries[0].numSectors, quint64(100));                    // 头声明 100 扇区
    QCOMPARE(plan.entries[0].rawBytes, quint64(100 * 4096));               // 顺带回填 rawBytes
    QCOMPARE(chk.warnings.size(), 1);
    QVERIFY(chk.warnings[0].contains(QStringLiteral("system")));
    QVERIFY(chk.warnings[0].contains(QStringLiteral("100")));
}

// 修正后的扇区数必须**参与**几何校验（先换算、后 bounds）：
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

    QList<edl::StorageInfo> dev;
    dev.append({0, 1000, 4096});
    const edl::PlanCheck chk = edl::validatePlan(plan, dev);
    QVERIFY(!chk.ok);
    bool hasOob = false;
    for (const QString &e : chk.errors)
        if (e.contains(QStringLiteral("越界"))) hasOob = true;
    QVERIFY(hasOob);
}

// 规则 4：Program 条目的镜像文件必须存在可读；Patch 的 imageFile=="DISK" 是"打设备磁盘"哨兵，不查文件。
void TestFlashPlan::validateMissingImageIsError()
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

    QList<edl::StorageInfo> dev;
    dev.append({0, 1000, 4096});
    const edl::PlanCheck chk = edl::validatePlan(plan, dev);
    QVERIFY(!chk.ok);
    QCOMPARE(chk.errors.size(), 1);                            // 只有缺文件那条；Patch DISK 不查文件
    QVERIFY(chk.errors[0].contains(QStringLiteral("gone")));
    QVERIFY(chk.errors[0].contains(QStringLiteral("nope.img")));
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

QTEST_APPLESS_MAIN(TestFlashPlan)
#include "test_flash_plan.moc"
