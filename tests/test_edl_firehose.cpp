// tests/test_edl_firehose.cpp
//
// Firehose 命令构造 + 响应解析（src/core/edl/firehose.cpp）单元测试。
// 命令构造是纯函数 → 断言**实际发出的 XML 文本**（逐个属性、乃至整串逐字节），不是"非空"。
// configure 的协商/换类型/鉴权拒绝三条路径用 MockEdlTransport 预置设备响应驱动。
//
// 属性集与判定口径全部来自协议速查 §1-§3，注释里标 qdl / bkerler 的行号。
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
    void configureStopsAfterOneNegotiationRound();
    void configureClampsAbsurdMaxPayload();

    // —— 以下为本任务补充的用例（brief 的 5 条之外的边界）——
    void programXmlMatchesReferenceExactly();
    void programXmlPassesStartSectorExprVerbatim();
    void patchXmlMatchesReferenceExactly();
    void patchXmlPassesStartSectorExprVerbatim();
    void eraseNumericRangeKeepsRange();
    void eraseWithExprKeepsRange();
    void configureXmlHasReferenceAttrSet();
    void attributeValuesAreXmlEscaped();
    void logValueIsEntityDecoded();
    void logOnlyResponseIsNeitherAckNorNak();
    void miscCommandsHaveReferenceShapes();
    void sendCommandWrapsInDataAndWaitsForResponse();
    void sendCommandFailsWithoutResponseElement();
    void configureSwitchesStorageOnMemoryNameNak();
    void configureStopsWithoutRetryOnAuthRequired();
    void configureReportsDeviceTextOnOtherNak();
    void parseStorageInfoReadsJsonGeometry();
    void parseStorageInfoAcceptsPageSize();
    void parseStorageInfoFallsBackToTextKeys();
    void parseStorageInfoFailsWithoutGeometry();
    void parseStorageInfoFailsWithoutTotalBlocks();

    // —— Task 7 集成修正：命令帧 ZLP 的悬空责任 + listPartitions 的 LUN 数来源 ——
    void addsZlpWhenCommandPayloadHitsPacketBoundary();
    void doesNotAddZlpForShortCommand();
    void parseLunCountReadsTextKeys();
    void drainConsumesResidualUntilSilent();
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

// 协商**最多两次发送**：设备每轮回报不同的 Supported 时，第二轮之后只采纳不再发
// （qdl 恰好两次 send：reference/qdl/src/firehose.c:534-548；否则每轮 10s 预算会无限重发=挂死）。
void TestEdlFirehose::configureStopsAfterOneNegotiationRound()
{
    edl::MockEdlTransport t;
    t.reads << QByteArray("<response value=\"ACK\" MaxPayloadSizeToTargetInBytesSupported=\"1048576\" />")
            << QByteArray("<response value=\"ACK\" MaxPayloadSizeToTargetInBytesSupported=\"2097152\" />")
            << QByteArray("<response value=\"ACK\" MaxPayloadSizeToTargetInBytesSupported=\"4194304\" />");
    QString name = QStringLiteral("ufs"); quint32 maxPayload = 0; QString err;
    QVERIFY2(edl::firehoseConfigure(t, name, maxPayload, &err), qPrintable(err));
    QCOMPARE(t.writes.size(), 2);                              // 只发了两次
    QVERIFY(t.writes[1].contains("MaxPayloadSizeToTargetInBytes=\"1048576\""));  // 第二次用首轮协商值
    QCOMPARE(maxPayload, quint32(2097152));                    // 第二轮回报值被采纳（不再重发）
    QCOMPARE(t.reads.size(), 1);                               // 第三条响应没被消费 = 确实没有第三轮
}

// ---------------------------------------------------------------------------
// 补充用例
// ---------------------------------------------------------------------------

// 终审 M-2：设备回报的 MaxPayloadSizeToTargetInBytesSupported 采纳前必须钳到
// kMaxNegotiatedPayloadBytes。该值一路变成数据面的单块字节数（edl_session.cpp 的 chunkBytes）→
// LibusbEdlTransport::write 交给 libusb 的 **int length**：设备报 ~4 GiB 时不钳制会先尝试超大分配、
// 再以负 int 进 libusb（离线用例永远看不见，mock 的分块都是小值）。
// 断言三件事：① 出参被钳；② **重发的 configure 命令里也是钳制值**（设备会按命令里的值安排缓冲，
// 命令与出参不一致就等于说谎）；③ 入参初值同样被钳（函数出口的不变量对任何调用方都成立）。
void TestEdlFirehose::configureClampsAbsurdMaxPayload()
{
    edl::MockEdlTransport t;
    t.reads << QByteArray("<response value=\"ACK\" MaxPayloadSizeToTargetInBytesSupported=\"4294967280\" />")
            << QByteArray("<response value=\"ACK\" />");
    QString name = QStringLiteral("ufs"); quint32 maxPayload = 0; QString err;
    QVERIFY2(edl::firehoseConfigure(t, name, maxPayload, &err), qPrintable(err));
    QCOMPARE(maxPayload, edl::kMaxNegotiatedPayloadBytes);                  // ①
    QCOMPARE(t.writes.size(), 2);
    QVERIFY(t.writes[1].contains(QByteArray("MaxPayloadSizeToTargetInBytes=\"8388608\"")));  // ②
    QVERIFY(!t.writes[1].contains("4294967280"));                           // 未钳制的值一个字节都不许出

    // 入参初值（调用方给的）同样钳制：设备未回报时出口值就是它
    edl::MockEdlTransport t2;
    t2.reads << QByteArray("<response value=\"ACK\" />");
    QString name2 = QStringLiteral("ufs"); quint32 maxPayload2 = 0xFFFFFFFFu; QString err2;
    QVERIFY2(edl::firehoseConfigure(t2, name2, maxPayload2, &err2), qPrintable(err2));
    QCOMPARE(maxPayload2, edl::kMaxNegotiatedPayloadBytes);                 // ③
    QVERIFY(t2.writes[0].contains(QByteArray("MaxPayloadSizeToTargetInBytes=\"8388608\"")));
}

// 属性顺序与集合逐字节对齐 qdl：SECTOR_SIZE_IN_BYTES → num_partition_sectors →
// physical_partition_number → start_sector → filename（reference/qdl/src/firehose.c:1021-1030）。
// 逐字节断言（而非 contains）才能杀掉"多发了参照没有的属性""顺序错位"这类实现。
void TestEdlFirehose::programXmlMatchesReferenceExactly()
{
    edl::PlanEntry e; e.action = edl::PlanEntry::Action::Program;
    e.imageFile = QStringLiteral("/tmp/xbl.img"); e.lun = 1;
    e.startSector = 1234; e.numSectors = 8192; e.sectorSize = 4096;
    QCOMPARE(edl::xmlProgram(e),
             QByteArray("<program SECTOR_SIZE_IN_BYTES=\"4096\" num_partition_sectors=\"8192\" "
                        "physical_partition_number=\"1\" start_sector=\"1234\" filename=\"xbl.img\"/>"));
}

// start_sector 是 firehose 表达式时必须原样透传：参照注释明确"解析会写错地址"
// （reference/qdl/src/firehose.c:874-879）；此时 PlanEntry::startSector==0
// （flash_plan.h:17-19 的模型契约），绝不能把这个 0 发出去。
void TestEdlFirehose::programXmlPassesStartSectorExprVerbatim()
{
    edl::PlanEntry e; e.action = edl::PlanEntry::Action::Program;
    e.imageFile = QStringLiteral("gpt_backup0.bin"); e.lun = 0;
    e.startSector = 0; e.startSectorExpr = QStringLiteral("NUM_DISK_SECTORS-5.");
    e.numSectors = 5; e.sectorSize = 4096;
    const QByteArray xml = edl::xmlProgram(e);
    QVERIFY(xml.contains("start_sector=\"NUM_DISK_SECTORS-5.\""));
    QVERIFY(!xml.contains("start_sector=\"0\""));
}

// patch 的 7 个出站属性 + 无 what，逐字节对齐 reference/qdl/src/firehose.c:1414-1424
// （bkerler 同款 7 属性、同样不发 what：edl/edlclient/Library/firehose.py:435-443）。
void TestEdlFirehose::patchXmlMatchesReferenceExactly()
{
    edl::PlanEntry e; e.action = edl::PlanEntry::Action::Patch;
    e.lun = 0; e.startSector = 2; e.byteOffset = 0; e.sizeInBytes = 4;
    e.value = QStringLiteral("NUM_DISK_SECTORS-6."); e.sectorSize = 4096;
    e.imageFile = QStringLiteral("DISK"); e.what = QStringLiteral("Update LBA");
    QCOMPARE(edl::xmlPatch(e),
             QByteArray("<patch SECTOR_SIZE_IN_BYTES=\"4096\" byte_offset=\"0\" filename=\"DISK\" "
                        "physical_partition_number=\"0\" size_in_bytes=\"4\" "
                        "start_sector=\"2\" value=\"NUM_DISK_SECTORS-6.\"/>"));
}

// erase 的三种形态之二：给定范围（numSectors>0）→ 两个属性都在，逐字节对齐
// reference/qdl/src/firehose.c:618-621。
void TestEdlFirehose::eraseNumericRangeKeepsRange()
{
    edl::PlanEntry e; e.action = edl::PlanEntry::Action::Erase;
    e.lun = 0; e.startSector = 6; e.numSectors = 2048; e.sectorSize = 4096;
    QCOMPARE(edl::xmlErase(e),
             QByteArray("<erase SECTOR_SIZE_IN_BYTES=\"4096\" physical_partition_number=\"0\" "
                        "num_partition_sectors=\"2048\" start_sector=\"6\"/>"));
}

// patch 的 start_sector 同样允许表达式、同样必须原样透传
// （reference/qdl/src/firehose.c:1420-1421 把 start_sector 与 value 一起原样下发；
// 真实样本 reference/qdl/tests/data/patch1.xml 的 start_sector="NUM_DISK_SECTORS-5."）。
// 逐字节断言：kills "表达式为空时发十进制 0"这类实现（模型契约 startSector==0）。
void TestEdlFirehose::patchXmlPassesStartSectorExprVerbatim()
{
    edl::PlanEntry e; e.action = edl::PlanEntry::Action::Patch;
    e.lun = 1; e.startSector = 0; e.startSectorExpr = QStringLiteral("NUM_DISK_SECTORS-5.");
    e.byteOffset = 0; e.sizeInBytes = 4; e.value = QStringLiteral("0"); e.sectorSize = 4096;
    e.imageFile = QStringLiteral("DISK");
    e.what = QStringLiteral("Zero Out Header CRC in Backup Header.");
    QCOMPARE(edl::xmlPatch(e),
             QByteArray("<patch SECTOR_SIZE_IN_BYTES=\"4096\" byte_offset=\"0\" filename=\"DISK\" "
                        "physical_partition_number=\"1\" size_in_bytes=\"4\" "
                        "start_sector=\"NUM_DISK_SECTORS-5.\" value=\"0\"/>"));
}

// 表达式条目即使 numSectors==0 也不是"整 LUN"（Task 1 报告 §6 标注的模型组合）：
// 省略 start/count 只在"无表达式且 numSectors==0"时成立
// （reference/qdl/src/firehose.c:611-621，判断条件是 program->num_sectors > 0）。
void TestEdlFirehose::eraseWithExprKeepsRange()
{
    edl::PlanEntry e; e.action = edl::PlanEntry::Action::Erase;
    e.lun = 2; e.numSectors = 0; e.startSector = 0;
    e.startSectorExpr = QStringLiteral("NUM_DISK_SECTORS-5."); e.sectorSize = 4096;
    const QByteArray xml = edl::xmlErase(e);
    QVERIFY(xml.contains("start_sector=\"NUM_DISK_SECTORS-5.\""));
    QVERIFY(xml.contains("num_partition_sectors=\"0\""));
}

// configure 属性集照 reference/qdl/src/firehose.c:510-515：MemoryName、
// MaxPayloadSizeToTargetInBytes（0 → 省略）、Verbose=0、ZlpAwareHost=1、SkipStorageInit=0。
void TestEdlFirehose::configureXmlHasReferenceAttrSet()
{
    QCOMPARE(edl::xmlConfigure(QStringLiteral("ufs"), 1048576),
             QByteArray("<configure MemoryName=\"ufs\" MaxPayloadSizeToTargetInBytes=\"1048576\" "
                        "Verbose=\"0\" ZlpAwareHost=\"1\" SkipStorageInit=\"0\"/>"));
    QCOMPARE(edl::xmlConfigure(QStringLiteral("emmc")),
             QByteArray("<configure MemoryName=\"emmc\" Verbose=\"0\" ZlpAwareHost=\"1\" "
                        "SkipStorageInit=\"0\"/>"));
}

// 属性值里的 XML 元字符必须转义（& " < >），否则命令在设备侧是非法 XML。
void TestEdlFirehose::attributeValuesAreXmlEscaped()
{
    edl::PlanEntry e; e.action = edl::PlanEntry::Action::Program;
    e.imageFile = QStringLiteral("/tmp/a&b\"c<d>.img"); e.lun = 0;
    e.startSector = 1; e.numSectors = 2; e.sectorSize = 4096;
    const QByteArray xml = edl::xmlProgram(e);
    QVERIFY(xml.contains("filename=\"a&amp;b&quot;c&lt;d&gt;.img\""));
    QVERIFY(!xml.contains("a&b\"c"));
}

// <log value="..."> 取出时必须做 XML 反转义（&quot; → " 等）—— 用 QXmlStreamReader 的
// 属性解码（firehose.c:1871-1875 与 firehose.py:1303-1317 都是"取 value 属性的文本值"）。
void TestEdlFirehose::logValueIsEntityDecoded()
{
    const auto r = edl::parseFirehoseResponse(
        QByteArray("<log value=\"failed: &quot;bad&quot; &amp; worse &lt;here&gt;\" />"
                   "<response value=\"NAK\" />"));
    QCOMPARE(r.errorText, QStringLiteral("failed: \"bad\" & worse <here>"));
    QVERIFY(r.nak && !r.ack);
}

// 只有 log、没有 <response> 时既不是 ACK 也不是 NAK（未收到响应由发送层报错）。
void TestEdlFirehose::logOnlyResponseIsNeitherAckNorNak()
{
    const auto r = edl::parseFirehoseResponse(QByteArray("<log value=\"ACK\" />"));
    QVERIFY(!r.ack && !r.nak);
}

// 其余命令的形态：read / getstorageinfo / setbootablestoragedrive / reset。
// read 的属性名参照 reference/qdl/src/firehose.c:1197-1207（既有 edl_handler.cpp（重写前 515cc57）:716-725
// 的 num_sectors 是错名，故这里显式断言"没有 num_sectors="）；
// reset 参照 :1590-1592 的 <power value="reset" DelayInSeconds="10"/>。
void TestEdlFirehose::miscCommandsHaveReferenceShapes()
{
    const QByteArray rd = edl::xmlRead(1, 100, 8, 4096);
    QCOMPARE(rd, QByteArray("<read SECTOR_SIZE_IN_BYTES=\"4096\" num_partition_sectors=\"8\" "
                            "physical_partition_number=\"1\" start_sector=\"100\"/>"));
    QVERIFY(!rd.contains("num_sectors=\""));

    QCOMPARE(edl::xmlGetStorageInfo(3),
             QByteArray("<getstorageinfo physical_partition_number=\"3\"/>"));
    QCOMPARE(edl::xmlSetBootableStorageDrive(1),
             QByteArray("<setbootablestoragedrive value=\"1\"/>"));
    QCOMPARE(edl::xmlReset(),
             QByteArray("<power value=\"reset\" DelayInSeconds=\"10\"/>"));
}

// firehoseSendCommand：XML 前无长度前缀、但要有 <data> 包裹（两参照的线上形态都是
// <data> 根 —— reference/qdl/src/firehose.c:399-405 把 root=<data> 的文档整篇
// xmlDocDumpMemory 出去；bkerler edl/edlclient/Library/firehose.py:921-922 的
// connectcmd 亦以 <data> 包裹）；响应可能分多次读到达，读到 <response 才停
// （firehose.py:269-281 的 `while b"<response value" not in rdata` 循环）。
void TestEdlFirehose::sendCommandWrapsInDataAndWaitsForResponse()
{
    edl::MockEdlTransport t;
    t.reads << QByteArray("<log value=\"first\" />")          // 第 1 次读：只有 log
            << QByteArray("<response value=\"ACK\" />");      // 第 2 次读：响应到齐
    edl::PlanEntry e; e.action = edl::PlanEntry::Action::Program;
    e.imageFile = QStringLiteral("x.img"); e.lun = 0; e.startSector = 1;
    e.numSectors = 2; e.sectorSize = 4096;

    edl::FirehoseResponse resp; QString err;
    QVERIFY2(edl::firehoseSendCommand(t, edl::xmlProgram(e), resp, 1000, &err), qPrintable(err));
    QVERIFY(resp.ack);
    QCOMPARE(resp.errorText, QStringLiteral("first"));
    QCOMPARE(t.writes.size(), 1);
    QCOMPARE(t.writes[0], QByteArray("<?xml version=\"1.0\" encoding=\"UTF-8\" ?><data>"
                                     "<program SECTOR_SIZE_IN_BYTES=\"4096\" num_partition_sectors=\"2\" "
                                     "physical_partition_number=\"0\" start_sector=\"1\" filename=\"x.img\"/>"
                                     "</data>"));
}

// 一个 <response> 都没读到 → 失败 + 中文错误（不得把 log 文本当成响应）。
void TestEdlFirehose::sendCommandFailsWithoutResponseElement()
{
    edl::MockEdlTransport t;                                   // reads 空 → read() 报超时
    edl::FirehoseResponse resp; QString err;
    QVERIFY(!edl::firehoseSendCommand(t, edl::xmlReset(), resp, 1000, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(!resp.ack && !resp.nak);
}

// NAK 文案含 "Not support configure MemoryName" → 换另一存储类型重试一次
// （edl/edlclient/Library/firehose.py:936-940：eMMC → UFS）。
void TestEdlFirehose::configureSwitchesStorageOnMemoryNameNak()
{
    edl::MockEdlTransport t;
    t.reads << QByteArray("<response value=\"NAK\" />"
                          "<log value=\"Not support configure MemoryName eMMC\" />")
            << QByteArray("<response value=\"ACK\" />");
    QString name = QStringLiteral("emmc"); quint32 maxPayload = 1024; QString err;
    QVERIFY2(edl::firehoseConfigure(t, name, maxPayload, &err), qPrintable(err));
    QCOMPARE(name, QStringLiteral("ufs"));                     // 换成了另一类型（回写给出参）
    QCOMPARE(t.writes.size(), 2);                              // 只重试一次
    QVERIFY(t.writes[1].contains("MemoryName=\"ufs\""));       // 重试用新类型
    QVERIFY(t.writes[1].contains("MaxPayloadSizeToTargetInBytes=\"1024\""));
}

// NAK 文案含 "Only nop and sig tag can be" → 设备要求 EDL 鉴权：**直接失败、不重试**
// （edl/edlclient/Library/firehose.py:941-956 是小米鉴权分支，spec §8 明确不做）。
void TestEdlFirehose::configureStopsWithoutRetryOnAuthRequired()
{
    const QByteArray authLog("Only nop and sig tag can be received before authentication");
    edl::MockEdlTransport t;
    t.reads << QByteArray("<response value=\"NAK\" /><log value=\"")
                   + authLog + "\" />";
    QString name = QStringLiteral("ufs"); quint32 maxPayload = 0; QString err;
    QVERIFY(!edl::firehoseConfigure(t, name, maxPayload, &err));
    QCOMPARE(t.writes.size(), 1);                              // 不重试（也不换类型）
    QVERIFY(err.contains(QStringLiteral("鉴权")));
    QVERIFY(err.contains(QStringLiteral("暂不支持")));
    QVERIFY(err.contains(QString::fromUtf8(authLog)));         // 带设备返回原文
}

// 其它 NAK（非上述两种文案）→ 失败一次即返回，错误里带设备原文。
void TestEdlFirehose::configureReportsDeviceTextOnOtherNak()
{
    edl::MockEdlTransport t;
    t.reads << QByteArray("<response value=\"NAK\" /><log value=\"Unable to open the SDCC Device\" />");
    QString name = QStringLiteral("ufs"); quint32 maxPayload = 0; QString err;
    QVERIFY(!edl::firehoseConfigure(t, name, maxPayload, &err));
    QCOMPARE(t.writes.size(), 1);
    QVERIFY(err.contains(QStringLiteral("Unable to open the SDCC Device")));
}

// getstorageinfo 的 JSON 口径（reference/qdl/src/firehose.c:1874-1884：
// <log> 里的 storage_info.total_blocks / block_size）。
void TestEdlFirehose::parseStorageInfoReadsJsonGeometry()
{
    edl::FirehoseResponse r;
    r.storageInfoJson = QStringLiteral(
        "{\"storage_info\":{\"total_blocks\":61079552,\"block_size\":512,\"mem_type\":\"UFS\"}}");
    edl::StorageInfo info; QString err;
    QVERIFY2(edl::parseStorageInfo(r, 2, info, &err), qPrintable(err));
    QCOMPARE(info.lun, quint32(2));
    QCOMPARE(info.totalBlocks, quint64(61079552));
    QCOMPARE(info.blockSize, quint32(512));
}

// bkerler 侧把 page_size 当扇区大小（edl/edlclient/Library/firehose.py:1326-1327），
// 且 JSON 常带 "INFO:" 前缀（:1312-1313）—— 两者都要能吃下。
void TestEdlFirehose::parseStorageInfoAcceptsPageSize()
{
    edl::FirehoseResponse r;
    r.storageInfoJson = QStringLiteral(
        "{\"storage_info\":{\"total_blocks\":8192,\"page_size\":4096}}");  // 前缀已在解析时切掉
    edl::StorageInfo info; QString err;
    QVERIFY2(edl::parseStorageInfo(r, 0, info, &err), qPrintable(err));
    QCOMPARE(info.totalBlocks, quint64(8192));
    QCOMPARE(info.blockSize, quint32(4096));
}

// 文本键回退（edl/edlclient/Library/firehose.py:1272-1275 的 SECTOR_SIZE_IN_BYTES /
// num_physical_partitions，:1303-1317 用 "=" 或 ":" 分隔）：只补**扇区大小**并覆盖默认 4096，
// total_blocks 仍必须来自 JSON。num_physical_partitions 是 LUN 数、StorageInfo 无对应字段。
void TestEdlFirehose::parseStorageInfoFallsBackToTextKeys()
{
    edl::FirehoseResponse r;
    r.storageInfoJson = QStringLiteral("{\"storage_info\":{\"total_blocks\":61079552}}");
    r.errorText = QStringLiteral("SECTOR_SIZE_IN_BYTES=512\nnum_physical_partitions=6");
    edl::StorageInfo info; QString err;
    QVERIFY2(edl::parseStorageInfo(r, 1, info, &err), qPrintable(err));
    QCOMPARE(info.lun, quint32(1));
    QCOMPARE(info.totalBlocks, quint64(61079552));
    QCOMPARE(info.blockSize, quint32(512));                          // 文本键覆盖默认 4096

    edl::FirehoseResponse r2;
    r2.storageInfoJson = QStringLiteral("{\"storage_info\":{\"total_blocks\":8192}}");
    r2.errorText = QStringLiteral("SECTOR_SIZE_IN_BYTES: 4096");     // bkerler 的另一种分隔
    edl::StorageInfo info2; QString err2;
    QVERIFY2(edl::parseStorageInfo(r2, 0, info2, &err2), qPrintable(err2));
    QCOMPARE(info2.totalBlocks, quint64(8192));
    QCOMPARE(info2.blockSize, quint32(4096));
}

// 只拿到扇区大小（无 total_blocks）**不得**算成功：totalBlocks==0 会让 validatePlan 的越界判定
// （flash_plan.cpp:608-609）把**每条 Program 条目**报成"越界"而非"LUN 无设备几何"——
// 假几何比"几何不可用"难查得多，宁可在此失败并说清缺什么。
void TestEdlFirehose::parseStorageInfoFailsWithoutTotalBlocks()
{
    edl::FirehoseResponse r;
    r.storageInfoJson = QStringLiteral("{\"storage_info\":{\"page_size\":4096}}");
    r.errorText = QStringLiteral("SECTOR_SIZE_IN_BYTES=512");
    edl::StorageInfo info; QString err;
    QVERIFY(!edl::parseStorageInfo(r, 0, info, &err));
    QVERIFY(err.contains(QStringLiteral("total_blocks")));            // 文案要点名缺的东西
    QCOMPARE(info.totalBlocks, quint64(0));                           // 不给假几何
}

// 两条来源都拿不到几何 → 失败 + 中文 error（绝不返回"默认 4096 全 0"的假几何：
// totalBlocks=0 会让 validatePlan 的越界判定恒定拒绝，见 flash_plan.cpp 规则 1/2）。
void TestEdlFirehose::parseStorageInfoFailsWithoutGeometry()
{
    edl::FirehoseResponse r;
    r.errorText = QStringLiteral("no geometry in this log");
    edl::StorageInfo info; QString err;
    QVERIFY(!edl::parseStorageInfo(r, 0, info, &err));
    QVERIFY(!err.isEmpty());
}

// ---------------------------------------------------------------------------
// Task 7 集成修正：命令帧的 ZLP（悬空责任补齐）
// ---------------------------------------------------------------------------

// 命令载荷长度**恰为端点包长整数倍**时，设备侧的 bulk 读看不到短包 → 真机会卡住/超时。
// qdl 的规则（reference/qdl/src/usb.c:548-553）是对**所有写**生效的；本项目把责任落在
// "发起该笔写的上层"：数据块在 edl_session 的数据面，命令帧在 firehoseSendCommand（本用例钉住）。
// 传输层是纯字节管道（LibusbEdlTransport::write() 不补 ZLP），故这条只能在这里发起。
void TestEdlFirehose::addsZlpWhenCommandPayloadHitsPacketBoundary()
{
    edl::MockEdlTransport t;                        // maxPacket 默认 1024
    t.reads << QByteArray("<response value=\"ACK\" />");

    // 造一条**包裹后**长度恰为 1024 整数倍的命令：`<patch value="…"/>` 的 value 补到刚好整除
    const QByteArray wrapHead("<?xml version=\"1.0\" encoding=\"UTF-8\" ?><data>");
    const QByteArray wrapTail("</data>");
    const QByteArray head("<patch value=\"");
    const QByteArray tail("\"/>");
    const int fixed = wrapHead.size() + wrapTail.size() + head.size() + tail.size();
    const QByteArray body = head + QByteArray(1024 - (fixed % 1024), 'A') + tail;
    QCOMPARE((wrapHead + body + wrapTail).size() % t.maxPacket, 0);   // 前提成立（不是"碰巧"）

    edl::FirehoseResponse resp; QString err;
    QVERIFY2(edl::firehoseSendCommand(t, body, resp, 1000, &err), qPrintable(err));
    QVERIFY(resp.ack);

    QCOMPARE(t.writes.size(), 2);                   // payload + 追加的 0 字节写
    QCOMPARE(t.writes.at(0), wrapHead + body + wrapTail);
    QVERIFY(t.writes.at(1).isEmpty());              // ZLP：一次 0 字节写（qdl usb.c:548-553）
}

// 不是整数倍 → 一笔写，不得多补 ZLP（多补会让设备把 ZLP 当成下一笔传输的开头）
void TestEdlFirehose::doesNotAddZlpForShortCommand()
{
    edl::MockEdlTransport t;
    t.reads << QByteArray("<response value=\"ACK\" />");

    edl::FirehoseResponse resp; QString err;
    QVERIFY2(edl::firehoseSendCommand(t, edl::xmlGetStorageInfo(0), resp, 1000, &err), qPrintable(err));
    QVERIFY(resp.ack);
    QCOMPARE(t.writes.size(), 1);
    QVERIFY(!t.writes.at(0).isEmpty());
}

// ---------------------------------------------------------------------------
// Task 7 集成修正：getstorageinfo 响应里的 LUN 数（listPartitions 据此决定查几个 LUN）
// ---------------------------------------------------------------------------

// bkerler 的存储信息键值（edl/edlclient/Library/firehose.py:1266-1275）：
//   * `num_physical_partitions` —— 十进制（UFS/eMMC 都可能有）
//   * `UFS Total Active LU`     —— 十六进制（bkerler 用 int(x, 16) 读，UFS 侧）
// 键值形态由本仓既有的 textKeyValue 口径解析（先 '=' 后 ':'，见 firehose.cpp）。
// 0 = 没报（调用方按"只查 LUN 0"处理）；防呆上限 8（设备报怪值时不得让调用方发上万条查询）。
void TestEdlFirehose::parseLunCountReadsTextKeys()
{
    edl::FirehoseResponse r;
    r.ack = true;

    r.errorText = QStringLiteral("num_physical_partitions = \"6\"\nSECTOR_SIZE_IN_BYTES = \"4096\"");
    QCOMPARE(edl::parseLunCount(r), quint32(6));

    r.errorText = QStringLiteral("bNumberLu: 3");
    QCOMPARE(edl::parseLunCount(r), quint32(3));

    r.errorText = QStringLiteral("UFS Total Active LU = 0x6");
    QCOMPARE(edl::parseLunCount(r), quint32(6));            // 十六进制源（带 0x 前缀）

    r.errorText = QStringLiteral("nothing useful here");
    QCOMPARE(edl::parseLunCount(r), quint32(0));            // 没报 → 0

    r.errorText = QStringLiteral("num_physical_partitions = \"4294967295\"");
    QCOMPARE(edl::parseLunCount(r), quint32(8));            // 防呆上限
}

// ---------------------------------------------------------------------------
// Task 7 集成修正：drainResidual 成为会话层与 EDLHandler 的共享实现
// ---------------------------------------------------------------------------

// 端点里有残留（"响应之后才到的 <log>"）→ 一次 drain 读干净；端点静默后**立即**返回
// （不空转：静默时只多一次轮询调用）。红线依据：qdl firehose.c:249-252（不消费完后续写会超时）。
void TestEdlFirehose::drainConsumesResidualUntilSilent()
{
    edl::MockEdlTransport t;
    t.residual << QByteArray("<log value=\"late-one\" />") << QByteArray("tail-without-tag");

    edl::drainResidual(t);
    QVERIFY(t.residual.isEmpty());                    // 全部消费（不是只取一段）
    QVERIFY(t.reads.isEmpty());                       // drain 不得动 reads 队列（mock 的 0 超时契约）

    const int callsBefore = t.calls.size();
    edl::drainResidual(t);                            // 已静默：一次轮询即返回
    QCOMPARE(t.calls.size() - callsBefore, 1);
}

QTEST_APPLESS_MAIN(TestEdlFirehose)
#include "test_edl_firehose.moc"
