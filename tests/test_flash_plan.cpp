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
    void eraseTagWithoutRangeMeansWholeLun();
    void malformedXmlReportsPathAndReaderError();
    void keepsExpressionStartSector();
    void keepsExpressionStartSectorInProgram();
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

QTEST_APPLESS_MAIN(TestFlashPlan)
#include "test_flash_plan.moc"
