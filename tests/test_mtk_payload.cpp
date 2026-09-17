#include <QtTest>
#include <QFile>
#include <QPair>
#include <QTemporaryDir>
#include <memory>
#include <utility>

#include "core/modes/mtk_brom.h"
#include "core/modes/mtk_chip_table.h"   // ChipInfo / DaMode（decideGeneration 用例直接用）
#include "core/modes/mtk_da_file.h"
#include "core/modes/mtk_emmc.h"
#include "core/modes/mtk_gpt.h"
#include "core/modes/mtk_payload.h"
#include "core/modes/mtk_preloader_fetch.h"
#include "core/modes/mtk_xflash_payload.h"
#include "core/modes/mtk_xflash_session.h"
#include "core/modes/mtk_xml_session.h"   // T12：xmlBringUpDa 的 XmlSession（mock 逐帧测）
#include "mtk_test_helpers.h"

using mtktest::be32;

// MockUsbChannel 与 test_mtk_brom.cpp 相同（复制；测试间不共享 TU）
class MockUsbChannel : public mtkbrom::IBromUsb {
public:
    QByteArray writes;                 // 拼接（既有断言用）
    QList<QByteArray> writeFrames;     // 逐笔（每次 write() 一笔，**含空包**）—— 帧级断言用
    QList<QByteArray> reads;
    bool failOpen = false;
    QString openError;
    int pktSize = 0x400;

    bool open(QString *error) override
    {
        if (failOpen) { if (error) *error = openError; return false; }
        return true;
    }
    bool write(const QByteArray &data, QString *error) override
    {
        Q_UNUSED(error)
        writes += data;
        writeFrames << data;
        return true;
    }
    bool read(QByteArray &out, int maxLen, int timeoutMs, QString *error) override
    {
        Q_UNUSED(timeoutMs) Q_UNUSED(error)
        if (reads.isEmpty()) { out.clear(); return false; }
        QByteArray r = reads.takeFirst();
        out = r.left(maxLen);
        return !r.isEmpty() || maxLen == 0;
    }
    int maxPacketSize() const override { return pktSize; }
    bool close() override { return true; }
};

namespace {

// ---- D2/D3：XFlash 应答帧夹具（**一帧 = 两笔队列项**）----
// 默认 `IBromUsb::readExact` 是"单次读 + 严格长度"（mtk_brom.cpp:219-231）：XFlash 层先读 12B 帧头、
// 再读载荷，每次 readExact 消耗**一笔**队列项。把整帧塞成一笔会让"读头"吃掉载荷、后续读全部错位。
// 本文件 D1 的 LEGACY 夹具（逐字节 read）不受此约束，保持原样。
QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
    return b;
}
// 12B 帧头（小端：magic + datatype + length）+ 载荷 = 两笔。**载荷为空时只有一笔**（xread 的
// `len > 0` 前置判断不发第二次读）—— 需要"空回包"时别用本函数（那会留下一笔不被消耗的队列项）。
QList<QByteArray> frameReads(quint32 dt, const QByteArray &payload)
{
    return {le32(0xFEEEEEEF) + le32(dt) + le32(quint32(payload.size())), payload};
}
// status 帧（datatype 1 + 4B 载荷）：既是一条 status 应答，也是读数据路径里"flag = 0"的收尾帧。
QList<QByteArray> statusReads(quint32 code) { return frameReads(1, le32(code)); }

// read_flash_info 的公共读序列（NOR info + NAND info(0x11) + info2 + EMMC + SDC + flashconfig）
// —— 4 条 read_flash_info 用例共用，避免逐字重复。
// **不塞任何"哨兵"**：NAND id 计数两级都为 0 时上游 usbread(-4) 读 **0 字节**
// （usblib.py:462-483），设备不会为这一步发出任何字节 —— 队列必须与真机一致。
void queueFlashInfoHead(MockUsbChannel *m)
{
    m->reads << QByteArray(0x1C, '\0')      // NOR info
             << QByteArray(0x11, '\0')      // NAND info（id 计数 @15 与 @11 都是 0 → 本步读 0B）
             << QByteArray(9, '\0')         // info2
             << QByteArray(0x5C, '\0')      // EMMC info
             << QByteArray(0x1C, '\0')      // SDC info
             << QByteArray(0x26, '\0');     // flashconfig
}

// ---- D1-T9：引导链（bromBringUpDa）的整条读序列夹具 ----
// 逐条对齐各函数实际会读的字节数与顺序（漏一节 = 后续全错位，夹具必须与真机同构）。

// 0xC0 之后的固定读序列：存储信息交换（605-631）→ stage2（279-284）→ [可选 EMI 段] → boot_to → read_flash_info
QList<QByteArray> storageExchangeReads(bool emmc = true)
{
    QByteArray emmcIds(16, '\0');
    if (emmc) emmcIds[3] = char(0x2A);
    return {QByteArray("\x00\x00\xBC\x04", 4), QByteArray("\x00\x00", 2),
            QByteArray("\x00\x00\x00\x01", 4), emmcIds, QByteArray("\x5A\x5A\x5A", 3)};
}
QList<QByteArray> stage2Reads(quint32 errorcode)
{
    return {be32(errorcode)};
}
QList<QByteArray> dramInfoReads()                            // 295-327（0xBC3 之后）
{
    return {QByteArray(4, '\0'), QByteArray(16, '\x11'), QByteArray("\x00\x00\x0B\xC4", 4),
            QByteArray("\x00\x00", 2)};
}
QList<QByteArray> emiReadsVer0()                             // 333-398（emiver = 0 → 档 D）
{
    return {QByteArray("\x5A", 1), be32(16), QByteArray("\x00\x01", 2), be32(0),
            QByteArray("\x02", 1), QByteArray("\x00", 1), QByteArray(8, '\0')};
}
QList<QByteArray> bootToReads()                              // 907-943（48B 一块）
{
    return {QByteArray("\x5A", 1), QByteArray("\x5A", 1), QByteArray("\x5A", 1)};
}
QList<QByteArray> flashInfoReads()                           // 526-551（PassInfo ack=0x5A）
{
    // 注：**没有**"nand id 表"那一笔 —— NAND info 的两级 count 都是 0 时上游读 **0 字节**
    // （usblib.py:462-467 的 `while bytestoread > 0` 不成立；T6 实测更正），本实现也不发读请求。
    // 夹具里塞一个空包会与"不发读"相抵：空包留在队列里被下一笔（9B info2）取走 → 用例假红。
    return {QByteArray(0x1C, '\0'), QByteArray(0x11, '\0'),
            QByteArray(9, '\0'), QByteArray(0x5C, '\0'), QByteArray(0x1C, '\0'),
            QByteArray(0x26, '\0'), QByteArray("\x5A\x00\x00\x00\x00\x00\x00\x00\x00\x00", 10)};
}
QList<QByteArray> da1UploadReads(const mtkbrom::DaSelection &sel)   // SEND_DA/JUMP_DA 的回显 + 状态
{
    return {QByteArray("\xD7", 1), be32(sel.da1.startAddr), be32(sel.da1.len), be32(sel.da1.sigLen),
            QByteArray("\x00\x00", 2), QByteArray("\x00\x00\x00\x00", 4),
            QByteArray("\xD5", 1), be32(sel.da1.startAddr), QByteArray("\x00\x00", 2)};
}

// ---- D3-T12：XML 链（引导 + READ-FLASH 读表 + 逐分区写 + REBOOT）的夹具 ----
// 与 T9/T10 的 XML 用例同款（T9 在 `tests/test_mtk_xml_payload.cpp` 里有逐条对照）：**一帧 = 两笔**
// （12B 帧头 + 载荷），因为默认 `IBromUsb::readExact` 是"单次读 + 严格长度"（mtk_brom.cpp:219-231）。
// ⚠️ XML 的 `sendCommand(noack=false)`（**上游默认**，XL:188-219）**一次调用消费三帧**：
//    `OK` → `CMD:END(result=OK)` → ack → `CMD:START`。按"每条命令一帧 OK"排（简令 Step 1 原稿、
//    以及 T9 的"订正 #1"）会在第二条命令处读空队列 → 用例假红。
// ⚠️ 读路径是**逐帧 ack**（`download_raw`，XL:508-559）：数据帧之后还有一发 `ack` 和它的 `OK` 应答
//    —— 简令 Step 1 的 `xmlSectorReaderRoundTripsDeviceBytes` 原稿漏了这一帧（T10 的
//    `readPartitionSequence` 夹具同款；漏掉它，收尾的 CMD:START 会被当成 ack 应答 → 假红）。

// 文本帧（DT_PROTOCOL_FLOW）：载荷 = utf8 字节 + NUL（XL:146-153：length = len + 1）
QList<QByteArray> textReads(const QString &s)
{
    const QByteArray body = s.toUtf8() + QByteArray(1, '\0');
    return {le32(0xFEEEEEEF) + le32(1) + le32(quint32(body.size())), body};
}

// 设备发来的 CMD:START（DA1 起来的同步信号，XL:309-310 —— **不是** XFlash 的 0xC0）
QList<QByteArray> xmlDeviceStartReads()
{
    return textReads(QStringLiteral("<host><command>CMD:START</command></host>"));
}

// `sendCommand(noack=false)` 一次调用的三帧（XL:188-219）—— XML 的 setup_env / setup_hw_init /
// set_host_info / REBOOT 全走这条节奏（与 T9 的 commandReads 同款）
QList<QByteArray> xmlCommandReads()
{
    return textReads(QStringLiteral("OK"))
         + textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>OK</result></arg></host>"))
         + textReads(QStringLiteral("<host><command>CMD:START</command></host>"));
}

// 读路径：设备发来的 UPLOAD-FILE（XL:425-431；readCommandResult 消费，**内部 ack**）
QList<QByteArray> xmlUploadFileReads()
{
    return textReads(QStringLiteral("<host><command>CMD:UPLOAD-FILE</command><arg>"
                                    "<checksum>CHK_NO</checksum><info>ROM_0</info>"
                                    "<target_file>ROM_0</target_file></arg></host>"));
}

// 写路径②：FileSysOp（key 必须是 FILE-SIZE，XL:967-968）。file_path 的长度按上游 `hex()` 排（无前缀）
QList<QByteArray> xmlFileSysOpReads(const QString &key, quint32 length)
{
    return textReads(QStringLiteral("<host><command>CMD:FILE-SYS-OPERATION</command><arg>"
                                    "<key>%1</key><file_path>MEM://0x8000000:0x%2</file_path></arg></host>")
                         .arg(key, QString::number(length, 16)));
}

// 写路径④：DwnFile（packet_length 用**十六进制**解析，XL:422）
QList<QByteArray> xmlDwnFileReads(quint32 packetLength, quint32 length)
{
    return textReads(QStringLiteral("<host><command>CMD:DOWNLOAD-FILE</command><arg>"
                                    "<checksum>CHK_NO</checksum><info>2nd-DA</info>"
                                    "<source_file>MEM://0x8000000:0x%1</source_file>"
                                    "<packet_length>0x%2</packet_length></arg></host>")
                         .arg(QString::number(length, 16), QString::number(packetLength, 16)));
}

// 写路径⑦收尾：`CMD:END(OK)` → `CMD:START`（**两者之间没有独立 OK 帧**，XL:487-496）
QList<QByteArray> xmlWriteTailReads()
{
    return textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>OK</result></arg></host>"))
         + textReads(QStringLiteral("<host><command>CMD:START</command></host>"));
}

// XML 命令的**线上字节**（xsendText：XML + NUL；写帧断言用）
QByteArray xmlFrame(const QString &xml) { return xml.toUtf8() + QByteArray(1, '\0'); }

// 合法但**零分区**的 GPT（512B 扇区 × 16 扇区 = 0x2000 = readTable 的探测长度）：
// 把合成 GPT 的条目表清零（type GUID 全 0 → parsePrimary 逐条 continue → 0 个分区）后
// 重算条目表 CRC 与头部 CRC（头部 CRC 覆盖 0x58 的条目表 CRC 字段 → 必须先改条目 CRC 再算头 CRC）。
// 用途：钉 XML 分支的**空表显式门**（parsePrimary 对零条目仍成功，空表要靠调用方点名）。
QByteArray emptyGptFixture()
{
    QByteArray raw = mtkgpt::testBuildSyntheticGpt(512, 16, /*firstLba=*/40);
    const int hdrPos = 512;
    const int entriesPos = 1024;
    raw.replace(entriesPos, 512, QByteArray(512, '\0'));
    auto crc32Ieee = [](const QByteArray &data) {
        quint32 crc = 0xFFFFFFFFu;                       // 与 mtk_gpt.cpp 的 crc32 同算法（反射 IEEE）
        for (const char ch : data) {
            crc ^= quint8(ch);
            for (int i = 0; i < 8; ++i)
                crc = (crc >> 1) ^ (0xEDB88320u & (quint32(0) - (crc & 1u)));
        }
        return ~crc;
    };
    auto wrU32 = [&raw](int off, quint32 v) {
        raw[off] = char(v & 0xFF); raw[off + 1] = char((v >> 8) & 0xFF);
        raw[off + 2] = char((v >> 16) & 0xFF); raw[off + 3] = char((v >> 24) & 0xFF);
    };
    wrU32(hdrPos + 0x58, crc32Ieee(raw.mid(entriesPos, 512)));
    QByteArray hdrForCrc = raw.mid(hdrPos, 92);
    hdrForCrc.replace(0x10, 4, QByteArray(4, '\0'));
    wrU32(hdrPos + 0x10, crc32Ieee(hdrForCrc));
    return raw;
}

// ---- D1-T9 审查 I1：bromFlashOnSession（整会话 + 四道安全门）的夹具 ----

// 用例用的芯片：0x6752 = 表内 LEGACY / 非 IoT / **dacode == hw_code** / 无 stage2 hwcode 追加 /
// 默认 bmt（不在 mtk_payload.cpp bmtSettings 的特例表里）—— 夹具因此最短且无特例分支。
constexpr quint16 kFlashHwCode = 0x6752;

// 会话前置读序列：get_target_config(2) + get_hw_code(2) + get_hw_sw_ver(2) + bromver(1) + blver(1)
// answerHwSwVer=false → 0xFC 那一笔插**空包**：MockUsbChannel::read 对空包返回 false 且**不消耗**
// 后续队列（不是"读到超时"，是"设备没答"）—— 正好用来测 0xFC 降级而不打乱后续读序列。
QList<QByteArray> prologueReads(quint16 hwCode, bool answerHwSwVer = true)
{
    return {QByteArray("\xD8", 1), QByteArray(6, '\0'),
            QByteArray("\xFD", 1), be32(quint32(hwCode) << 16),      // hw_code 高 16 位 / hwver = 0
            QByteArray("\xFC", 1), (answerHwSwVer ? QByteArray(8, '\0') : QByteArray()),
            QByteArray("\x05", 1), QByteArray("\x01", 1)};
}

// 0x60 型 PMT 条目（read_pmt 判据 partdata[0x48] == 0xFF；name@0、size@0x40、offset@0x50，小端）
QByteArray pmtEntry60(const QByteArray &name, quint64 size, quint64 offset)
{
    QByteArray pd(0x60, '\0');
    pd[0x48] = char(0xFF);
    pd.replace(0, name.size(), name);
    for (int i = 0; i < 8; ++i) {
        pd[0x40 + i] = char((size >> (8 * i)) & 0xFF);
        pd[0x50 + i] = char((offset >> (8 * i)) & 0xFF);
    }
    return pd;
}

// 引导链 + 刷写段（prologue 之后的全部；无 preloader、errorcode == 0、单分区写 + FINISH）。
// ⚠️ PMT **读两次**：计划层一次 + flashPartition 内部再读一次（T6 既有语义：每个分区重读设备表）。
// answerFinish=false → 省掉 FINISH 的两个 ACK（测"收尾失败只告警、仍返回 true"）。
QList<QByteArray> chainAndFlashReads(const mtkbrom::DaSelection &sel, const QByteArray &pmt,
                                     bool answerFinish = true)
{
    QList<QByteArray> r;
    r << da1UploadReads(sel) << QByteArray("\xC0", 1) << storageExchangeReads()
      << stage2Reads(0) << bootToReads() << flashInfoReads();
    r << QByteArray("\x5A", 1) << be32(quint32(pmt.size())) << pmt        // read_pmt（计划）
      << QByteArray("\x5A", 1) << be32(quint32(pmt.size())) << pmt        // read_pmt（flashPartition 内）
      << QByteArray("\x5A", 1) << QByteArray(1, char(0x69));              // 写命令 ACK + 块 CONT
    if (answerFinish)
        r << QByteArray("\x5A", 1) << QByteArray("\x5A", 1);
    return r;
}

// 整会话读序列（设备 hw_code == DA 条目 hw_code 的常规情形）
QList<QByteArray> fullSessionReads(const mtkbrom::DaSelection &sel, const QByteArray &pmt,
                                   bool answerHwSwVer = true, bool answerFinish = true)
{
    QList<QByteArray> r = prologueReads(sel.entry.hwCode, answerHwSwVer);
    r << chainAndFlashReads(sel, pmt, answerFinish);
    return r;
}

// 与 makeSelection 同规格的 **DA 原始字节**（bromFlashOnSession 自己解析 req.daFile）
QByteArray daBytesForHw(quint16 hwCode, bool v6 = false)
{
    mtktest::EntrySpec e;
    e.hwCode = hwCode;
    mtktest::RegionSpec r0; r0.startAddr = 0x200000;   r0.len = 16;
    mtktest::RegionSpec r1; r1.startAddr = 0x2007000;  r1.len = 32;
    mtktest::RegionSpec r2; r2.startAddr = 0x80000000; r2.len = 48; r2.sigLen = 0x10;
    e.regions << r0 << r1 << r2;
    return mtktest::buildDa({e}, v6);
}

// 日志/进度收集（bromFlashOnSession 的两个注入点）
struct SessionCapture {
    QStringList lines;
    QList<bool> errors;
    QList<QPair<quint64, quint64>> progress;

    mtkbrom::BromLogFn logFn()
    {
        return [this](const QString &m, bool isError) { lines << m; errors << isError; };
    }
    mtkbrom::BromProgressFn progressFn()
    {
        return [this](quint64 written, quint64 total) { progress << qMakePair(written, total); };
    }
    QString joined() const { return lines.join(QLatin1Char('\n')); }
    bool hasLineContaining(const QString &needle) const
    {
        for (const QString &l : std::as_const(lines))
            if (l.contains(needle))
                return true;
        return false;
    }
    bool hasErrorLineContaining(const QString &needle) const
    {
        for (int i = 0; i < lines.size(); ++i)
            if (errors.at(i) && lines.at(i).contains(needle))
                return true;
        return false;
    }
};

// 建一个临时镜像文件（QTemporaryDir 生命周期由调用方持有）
QString writeTempImage(QTemporaryDir &dir, const QString &fileName, const QByteArray &bytes)
{
    const QString path = dir.filePath(fileName);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(bytes);
    f.close();
    return path;
}

} // namespace

class TestMtkPayload : public QObject {
    Q_OBJECT
private slots:
    void patchPreloaderSecurityReplacesPatterns();
    void patchPreloaderSecurityNoMatchReturnsFalse();
    void sendDa1SlaFails();
    void flashPartitionWritesAtPartitionOffset();
    void flashPartitionUnknownNameFails();
    // ---- D1-T6: DA1/DA2 协议帧 ----
    void sendDa1UsesRegion1AddressAndSigLen();
    void waitDa1ReadyAcceptsOnlyC0();
    void sendEmiLegacyZeroVersionFollowsUpstreamSequence();
    void sendEmiLegacyZeroAVersionReadsInfoBeforeLength();
    void sendEmiLegacyTierAReadsLengthThenAckThenLendram();
    void sendEmiLegacyTierASkipsLendramFor8127();
    void sendEmiLegacyTierCRewritesEmiHeader();
    void sendEmiLegacyUnknownVersionSendsEmiAsIs();
    void sendEmiLegacyTierDTruncatesAndWritesDramLength();
    void sendEmiLegacyFailsOnNack();
    void sendEmiLegacyFailsWhenDramInitRejected();
    void exchangeDa1StorageInfoDetectsEmmc();
    void exchangeDa1StorageInfoDetectsNand();
    void exchangeDa1StorageInfoDetectsNor();
    void stage2ConfigWritesFieldsAndReportsNoEmiNeeded();
    void stage2ConfigReportsEmiNeededOnBc3();
    void stage2ConfigRejectsNonEmmcStorage();
    void stage2ConfigAppendsHwCodeSpecificFields();
    void beginEmiDramInfoReadsDramInfoAndChecksBc4();
    void readFlashInfoDa2ConsumesAllBytesAndChecksPassInfo();
    void readFlashInfoDa2FailsWhenPassInfoIsNotAck();
    void readFlashInfoDa2AcceptsDownloadStatusAckBranch();
    void readFlashInfoDa2ReadsExtraDwordFor8127();
    void bootToDa2LegacySendsAddressSizeAndBlocks();
    void bootToDa2ChunksEachPacketAndWaitsAckPerPacket();
    void bootToDa2FailsWhenHeaderNotAcked();
    // ---- D1-T9: DA 两阶段引导链 ----
    void bringUpOrdersEmiAfterStorageInfoAndBeforeDa2();
    void bringUpSkipsEmiWhenNotNeeded();
    void bringUpAbortsWhenDramConfigNeededWithoutPreloader();
    void bringUpAbortsBeforeDa2WhenSyncIsWrong();
    // ---- D1-T9 审查 I1: 刷写主体（bromFlashOnSession）的四道安全门 + 收尾 ----
    void bromFlashOnSessionWritesPlanAndFinishes();
    void bromFlashOnSessionRejectsEmptyPlanBeforeAnyWrite();
    void bromFlashOnSessionRejectsUnknownHwCode();
    void bromFlashOnSessionRoutesXflashEndToEnd();
    void bromFlashOnSessionRejectsIotChip();
    void bromFlashOnSessionContinuesWhenHwSwVerUnavailable();
    void bromFlashOnSessionWarnsButSucceedsWhenFinishFails();
    void bromFlashOnSessionLooksUpDaByDacode();
    // ---- D2-T11: 三代路由（decideGeneration）+ XFlash 引导链 ----
    void decideGenerationMatrix();
    void xflashChainOrderWithEmi();
    void xflashChainSkipsEmiForPreloaderAgent();
    void xflashChainWarnsButContinuesWithoutPreloader();
    // ---- D3-T12: XML 链整合（xmlBringUpDa + READ-FLASH 读表 + 逐分区写 + REBOOT） ----
    void xmlChainSendsCmdStartHandshakeWithoutEmi();
    void xmlChainRejectsNonStartFirstCommand();
    void xmlSectorReaderRoundTripsDeviceBytes();
    void bromFlashOnSessionRoutesXmlEndToEnd();
    void bromFlashOnSessionRejectsEmptyXmlGptTable();
};

void TestMtkPayload::patchPreloaderSecurityReplacesPatterns()
{
    // SBC 修补全部 5 个核心模式（GPLv3 子模块来源标注，见 mtk_payload.cpp kPatches）：
    // 每个模式：输入 fromHex(from) → 修补后 == fromHex(to)，且返回 true
    struct Case { const char *hexFrom; const char *hexTo; };
    const Case kCases[] = {
        {"10B50C680268", "10B5012010BD"},                        // ram blacklist
        {"08B5104B7B441B681B68", "00207047000000000000"},        // seclib_sec_usbdl_enabled
        {"5072656C6F61646572205374617274", "50617463686564204C205374617274"}, // Patched loader msg
        {"F0B58BB002AE20250C460746", "002070470000000000205374617274"},       // sec_img_auth
        {"FFC0F3400008BD", "FF4FF0000008BD"},                    // get_vfy_policy
    };
    for (const Case &c : kCases) {
        QByteArray pl = QByteArray::fromHex(c.hexFrom);
        QVERIFY(mtkbrom::patchPreloaderSecurity(pl));
        QCOMPARE(pl, QByteArray::fromHex(c.hexTo));
    }
}

void TestMtkPayload::patchPreloaderSecurityNoMatchReturnsFalse()
{
    QByteArray pl = QByteArray::fromHex("DEADBEEF");
    QVERIFY(!mtkbrom::patchPreloaderSecurity(pl));
    QCOMPARE(pl, QByteArray::fromHex("DEADBEEF")); // 不变
}

// 原 sendPayloadFrames（addr=0/sigLen=0 的旧帧）已被 sendDa1UsesRegion1AddressAndSigLen 取代 ——
// sendPayload 整个删除：它把**整个文件**按 addr=0/sigLen=0 发出，真机必错。
// 原 sendPayloadSlaFails 迁到这里（SLA 仍是唯一能验证「SEND_DA 状态字冒泡」的路径）。
void TestMtkPayload::sendDa1SlaFails()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x6765, sel, &err), qPrintable(err));
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    // SEND_DA 状态 0x1D0D（规格 §2.4 步骤 5：SLA 挑战）—— 回显帧照 region[1] 的值
    m->reads << QByteArray("\xD7", 1)
             << be32(sel.da1.startAddr) << be32(sel.da1.len) << be32(sel.da1.sigLen)
             << QByteArray("\x1D\x0D", 2);
    QString err2;
    QVERIFY(!mtkbrom::sendDa1(s, sel, &err2));
    QVERIFY(err2.contains("SLA"));
    // 数据一字节没发（状态字在数据之前）：帧数 = 0xD7 + 3 个参数
    QCOMPARE(m->writeFrames.size(), 4);
}

void TestMtkPayload::flashPartitionWritesAtPartitionOffset()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    mtkbrom::DaStorage st(s);
    st.setDaActive(true);
    // listPartitions（规格 §3.6 PMT）：ack + len + partdata(1 条 0x60B, name=boot, offset=0x1000)
    QByteArray pd(0x60, '\0');
    pd[0x48] = char(0xFF);
    memcpy(pd.data(), "boot", 4);
    // 0x60B 条目字段小端：0x1000 = "\x00\x10\x00\x00\x00\x00\x00\x00"
    memcpy(pd.data() + 0x40, "\x00\x10\x00\x00\x00\x00\x00\x00", 8); // size
    memcpy(pd.data() + 0x50, "\x00\x10\x00\x00\x00\x00\x00\x00", 8); // offset
    m->reads << QByteArray(1, char(0x5A))
             << QByteArray("\x00\x00\x00\x60", 4)
             << pd;
    // emmcWrite（规格 §3.5 Legacy）：0x62 → ACK → CONT
    m->reads << QByteArray(1, char(0x5A))
             << QByteArray(1, char(0x69));
    const QByteArray img("\x01\x02", 2);
    QVERIFY(mtkbrom::flashPartition(s, st, QStringLiteral("boot"), img, nullptr));
    // 帧含 addr=0x1000
    QVERIFY(m->writes.contains(QByteArray("\x62\x02\x08\x00\x00\x00\x00\x00\x00\x10\x00", 11)));
}

void TestMtkPayload::flashPartitionUnknownNameFails()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromDevice dev; dev.vid = 0x0E8D; dev.pid = 0x0003;
    mtkbrom::BromSession s(std::move(usb), dev);
    mtkbrom::DaStorage st(s);
    st.setDaActive(true);
    QByteArray pd(0x60, '\0');
    pd[0x48] = char(0xFF);
    memcpy(pd.data(), "boot", 4);
    m->reads << QByteArray(1, char(0x5A))
             << QByteArray("\x00\x00\x00\x60", 4)
             << pd;
    QString err;
    QVERIFY(!mtkbrom::flashPartition(s, st, QStringLiteral("missing"), QByteArray(), &err));
    QVERIFY(err.contains("未找到分区"));
}

// ---- D1-T6: DA1/DA2 两阶段协议帧 ----

// DA1：SEND_DA 的三个参数必须来自 region[1]（**不是** 旧版的 addr=0/sigLen=0），随后 JUMP_DA
void TestMtkPayload::sendDa1UsesRegion1AddressAndSigLen()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x6765, sel, &err), qPrintable(err));
    QCOMPARE(sel.da1.startAddr, quint32(0x2007000));
    QCOMPARE(sel.da1.len, quint32(32));
    QCOMPARE(sel.da1.sigLen, quint32(0));          // region[1] 无签名（夹具如此）
    QCOMPARE(sel.da2.sigLen, quint32(0x10));       // region[2] 带签名
    QCOMPARE(sel.da1Bytes.size(), 32);

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    m->pktSize = 0x400;                            // DA1 32B 一块发完
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    // 回显协议：SEND_DA → 0xD7/addr/len/sigLen 各回显 + 状态 2B；上传收尾读 4B（checksum+status）
    //           JUMP_DA → 0xD5 回显 + addr 回显 + 状态 2B
    m->reads << QByteArray("\xD7", 1)
             << be32(sel.da1.startAddr) << be32(sel.da1.len) << be32(sel.da1.sigLen)
             << QByteArray("\x00\x00", 2)
             << QByteArray("\x00\x00\x00\x00", 4)
             << QByteArray("\xD5", 1) << be32(sel.da1.startAddr) << QByteArray("\x00\x00", 2);
    QVERIFY2(mtkbrom::sendDa1(s, sel, &err), qPrintable(err));

    QCOMPARE(m->writeFrames.at(0), QByteArray("\xD7", 1));            // SEND_DA
    QCOMPARE(m->writeFrames.at(1), be32(0x2007000));                  // **region[1] 加载地址**（旧版发 0）
    QCOMPARE(m->writeFrames.at(2), be32(32));                         // m_len
    QCOMPARE(m->writeFrames.at(3), be32(0));                          // m_sig_len
    QCOMPARE(m->writeFrames.at(4), sel.da1Bytes);                     // DA1 载荷 = region[1] 切片
    QCOMPARE(m->writeFrames.at(5), QByteArray());                     // 收尾空包
    QCOMPARE(m->writeFrames.at(6), QByteArray("\xD5", 1));            // JUMP_DA
    QCOMPARE(m->writeFrames.at(7), be32(0x2007000));
    QCOMPARE(m->writeFrames.size(), 8);
}

// DA1 起来后只回一个 0xC0；收到别的字节必须失败（不能把任意字节当就绪）
void TestMtkPayload::waitDa1ReadyAcceptsOnlyC0()
{
    {
        auto usb = std::make_unique<MockUsbChannel>();
        MockUsbChannel *m = usb.get();
        mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
        m->reads << QByteArray("\xC0", 1);
        QString err;
        QVERIFY2(mtkbrom::waitDa1Ready(s, 1000, &err), qPrintable(err));
    }
    {
        auto usb = std::make_unique<MockUsbChannel>();
        MockUsbChannel *m = usb.get();
        mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
        m->reads << QByteArray("\x00", 1);
        QString err;
        QVERIFY(!mtkbrom::waitDa1Ready(s, 1000, &err));
        QVERIFY(err.contains(QStringLiteral("0xC0")) || !err.isEmpty());
    }
}

// EMI（emiver = 0 档）：0xE8 → >I 0xFFFFFFFF（**emiver==0 的特例**）→ 读 ACK →
//   读 >I dramlength → 写 ACK → 改写 emi 为 emi[:dramlength] → 写 >I dramlength → 写 emi →
//   读 >H checksum → 写 ACK → 写 >I 0x80000001 → 读 >I（须 0）→ 1B type → 1B cs → 读 >Q size
void TestMtkPayload::sendEmiLegacyZeroVersionFollowsUpstreamSequence()
{
    mtkbrom::EmiData emi;
    emi.bytes = QByteArray(16, '\x5A');
    emi.ver = 0;

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\x5A", 1)                 // ACK：EMI 配置被接受
             << QByteArray("\x00\x00\x00\x10", 4)     // dramlength = 16（BE）
             << QByteArray("\x00\x2A", 2)             // checksum（BE，仅日志）
             << QByteArray("\x00\x00\x00\x00", 4)     // M_EXT_RAM_RET = 0
             << QByteArray("\x02", 1) << QByteArray("\x00", 1)
             << QByteArray("\x00\x00\x00\x00\x80\x00\x00\x00", 8);
    QString err;
    QVERIFY2(mtkbrom::sendEmiLegacy(s, emi, /*hwCode=*/0x6765, &err), qPrintable(err));

    // emiver==0 档的**确切写序列**：0xE8 → >I 0xFFFFFFFF → [读 ACK/长度] → 写 ACK → 写 >I dramlength
    //   → 写 EMI 本体 → [读 checksum] → 写 ACK → 写 >I 0x80000001 → …
    QCOMPARE(m->writeFrames.at(0), QByteArray("\xE8", 1));                 // ENABLE_DRAM
    QCOMPARE(m->writeFrames.at(1), QByteArray("\xFF\xFF\xFF\xFF", 4));     // emiver==0 → 0xFFFFFFFF
    QCOMPARE(m->writeFrames.at(2), QByteArray("\x5A", 1));                 // 长度后的 ACK
    QCOMPARE(m->writeFrames.at(3), QByteArray("\x00\x00\x00\x10", 4));     // >I dramlength（16）
    QCOMPARE(m->writeFrames.at(4), emi.bytes);                             // EMI 本体（截断到 16B = 原样）
    QCOMPARE(m->writeFrames.at(5), QByteArray("\x5A", 1));                 // checksum 后的 ACK
    // >I 0x80000001 的大端编码 = 80 00 00 01（brief 原文写成 00 80 00 01，是笔误 ——
    // 上游 pack(">I", 0x80000001)；同一 brief 里 addr 0x80000000 写的就是 "\x80\x00\x00\x00"）
    QCOMPARE(m->writeFrames.at(6), QByteArray("\x80\x00\x00\x01", 4));
    QCOMPARE(m->writeFrames.size(), 7);
}

// emiver = 0x0A 档：**先读 0x10 字节 info，再读 4B dramlength，再写 ACK**（与 0 档顺序不同）
void TestMtkPayload::sendEmiLegacyZeroAVersionReadsInfoBeforeLength()
{
    mtkbrom::EmiData emi;
    emi.bytes = QByteArray(8, '\x11');
    emi.ver = 0x0A;

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\x5A", 1)                            // 版本被接受
             << QByteArray(0x10, '\x29')                          // RAM info（0x10 字节）
             << QByteArray("\x00\x00\x00\x08", 4)                 // dramlength = 8
             << QByteArray("\x00\x2A", 2)                         // checksum
             << QByteArray("\x00\x00\x00\x00", 4)                 // M_EXT_RAM_RET = 0
             << QByteArray("\x02", 1) << QByteArray("\x00", 1)
             << QByteArray("\x00\x00\x00\x00\x80\x00\x00\x00", 8);
    QString err;
    QVERIFY2(mtkbrom::sendEmiLegacy(s, emi, 0x6765, &err), qPrintable(err));

    QCOMPARE(m->writeFrames.at(0), QByteArray("\xE8", 1));
    QCOMPARE(m->writeFrames.at(1), QByteArray("\x00\x00\x00\x0A", 4));    // emiver 原样（非 0）
    QCOMPARE(m->writeFrames.at(2), QByteArray("\x5A", 1));                // ACK 在 info+length 之后
    QCOMPARE(m->writeFrames.at(3), emi.bytes);                            // 本档**不写** dramlength
    QCOMPARE(m->writeFrames.size(), 6);
}

// emiver = 0x10 档（A 档 [0xF,0x10,0x11,0x14,0x15]）：读 4B dramlength → 写 ACK →
//   **紧接着写 >I lendram（= len(emi)，未截断）** → 写 EMI 本体（与 0 档同样「先 ACK 再写长度」，
//   但写的是 lendram 而不是 dramlength）
void TestMtkPayload::sendEmiLegacyTierAReadsLengthThenAckThenLendram()
{
    mtkbrom::EmiData emi;
    emi.bytes = QByteArray(16, '\x33');
    emi.ver = 0x10;

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    // 故意让 dramlength(0x20) ≠ len(emi)(16)：本档写的是 **lendram**，写错成 dramlength 必须被这组断言抓住
    m->reads << QByteArray("\x5A", 1)                        // 版本被接受
             << QByteArray("\x00\x00\x00\x20", 4)            // dramlength = 32（**不**决定发送长度）
             << QByteArray("\x00\x2A", 2)                    // checksum
             << QByteArray("\x00\x00\x00\x00", 4)            // M_EXT_RAM_RET = 0
             << QByteArray("\x02", 1) << QByteArray("\x00", 1)
             << QByteArray("\x00\x00\x00\x00\x80\x00\x00\x00", 8);
    QString err;
    QVERIFY2(mtkbrom::sendEmiLegacy(s, emi, 0x6765, &err), qPrintable(err));

    QCOMPARE(m->writeFrames.at(0), QByteArray("\xE8", 1));
    QCOMPARE(m->writeFrames.at(1), QByteArray("\x00\x00\x00\x10", 4));    // emiver 原样
    QCOMPARE(m->writeFrames.at(2), QByteArray("\x5A", 1));                // ACK 在 dramlength 之后
    QCOMPARE(m->writeFrames.at(3), QByteArray("\x00\x00\x00\x10", 4));    // **lendram = len(emi) = 16**
    QCOMPARE(m->writeFrames.at(4), emi.bytes);                            // EMI 本体（本档不截断）
    QCOMPARE(m->writeFrames.at(5), QByteArray("\x5A", 1));
    QCOMPARE(m->writeFrames.at(6), QByteArray("\x80\x00\x00\x01", 4));
    QCOMPARE(m->writeFrames.size(), 7);
}

// A 档 + hwCode 0x8127：上游 **不写** lendram（dalegacy_lib.py:350）—— 少一帧，其余逐字节相同
void TestMtkPayload::sendEmiLegacyTierASkipsLendramFor8127()
{
    mtkbrom::EmiData emi;
    emi.bytes = QByteArray(16, '\x33');
    emi.ver = 0x0F;

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\x5A", 1)
             << QByteArray("\x00\x00\x00\x20", 4)      // 同上：与 len(emi) 不同，防"lendram 其实写了"
             << QByteArray("\x00\x2A", 2)
             << QByteArray("\x00\x00\x00\x00", 4)
             << QByteArray("\x02", 1) << QByteArray("\x00", 1)
             << QByteArray("\x00\x00\x00\x00\x80\x00\x00\x00", 8);
    QString err;
    QVERIFY2(mtkbrom::sendEmiLegacy(s, emi, /*hwCode=*/0x8127, &err), qPrintable(err));

    QCOMPARE(m->writeFrames.at(0), QByteArray("\xE8", 1));
    QCOMPARE(m->writeFrames.at(1), QByteArray("\x00\x00\x00\x0F", 4));
    QCOMPARE(m->writeFrames.at(2), QByteArray("\x5A", 1));
    QCOMPARE(m->writeFrames.at(3), emi.bytes);        // **EMI 紧跟 ACK**（lendram 被跳过）
    QCOMPARE(m->writeFrames.size(), 6);               // 与 A 档通用路径相比少正好 1 帧
}

// emiver = 0x0D 档（C 档 [0x0C,0x0D]）：读 4B dramlength → 写 ACK → **改写 EMI 本体**为
//   >I 0x100 + emi[4:dramlength] → 写完后再收 5×4B（Raw/CJ，仅 0x0D）
void TestMtkPayload::sendEmiLegacyTierCRewritesEmiHeader()
{
    mtkbrom::EmiData emi;
    emi.bytes = QByteArray::fromHex("000102030405060708090A0B0C0D0E0F");   // 16B 可辨识
    emi.ver = 0x0D;

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\x5A", 1)
             << QByteArray("\x00\x00\x00\x08", 4)            // dramlength = 8（截掉后 8 字节）
             << QByteArray("\x00\x2A", 2)                    // checksum
             << QByteArray("\x00\x00\x00\x00", 4)            // M_EXT_RAM_RET = 0
             << QByteArray("\x02", 1) << QByteArray("\x00", 1)
             << QByteArray("\x00\x00\x00\x00\x80\x00\x00\x00", 8)
             << QByteArray("\x00\x00\x00\x03", 4) << QByteArray("\x1C\x00\x40\x04", 4)
             << QByteArray("\xAA\x08\x00\x33", 4) << QByteArray("\x00\x00\x00\x13", 4)
             << QByteArray("\x00\x00\x00\x10", 4);           // 5×4B（仅 0x0D）
    QString err;
    QVERIFY2(mtkbrom::sendEmiLegacy(s, emi, 0x6765, &err), qPrintable(err));

    QCOMPARE(m->writeFrames.at(0), QByteArray("\xE8", 1));
    QCOMPARE(m->writeFrames.at(1), QByteArray("\x00\x00\x00\x0D", 4));
    QCOMPARE(m->writeFrames.at(2), QByteArray("\x5A", 1));           // ACK 在 dramlength 之后
    // 改写本体：>I 0x100 + emi[4:8]，本档**不单独写 dramlength**
    QCOMPARE(m->writeFrames.at(3), QByteArray::fromHex("00000100" "04050607"));
    QCOMPARE(m->writeFrames.at(4), QByteArray("\x5A", 1));
    QCOMPARE(m->writeFrames.at(5), QByteArray("\x80\x00\x00\x01", 4));
    QCOMPARE(m->writeFrames.size(), 6);
    QVERIFY(m->reads.isEmpty());                                     // 5×4B 确实读走了
}

// 未知 emiver：上游只 warning，**不读不写** —— EMI 原样发出（不得跳过整个 DRAM 初始化）
void TestMtkPayload::sendEmiLegacyUnknownVersionSendsEmiAsIs()
{
    mtkbrom::EmiData emi;
    emi.bytes = QByteArray(8, '\x77');
    emi.ver = 0x63;

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\x5A", 1)                        // 第 1 次读：版本 ACK
             << QByteArray("\x00\x2A", 2)                    // 第 2 次读直接是 checksum（中间无 dramlength）
             << QByteArray("\x00\x00\x00\x00", 4)
             << QByteArray("\x02", 1) << QByteArray("\x00", 1)
             << QByteArray("\x00\x00\x00\x00\x80\x00\x00\x00", 8);
    QString err;
    QVERIFY2(mtkbrom::sendEmiLegacy(s, emi, 0x6765, &err), qPrintable(err));

    QCOMPARE(m->writeFrames.at(0), QByteArray("\xE8", 1));
    QCOMPARE(m->writeFrames.at(1), QByteArray("\x00\x00\x00\x63", 4));
    QCOMPARE(m->writeFrames.at(2), emi.bytes);               // 原样（无 ACK/长度帧夹在中间）
    QCOMPARE(m->writeFrames.size(), 5);
}

// 设备回 NACK → 明确失败（上游此处 sys.exit；本实现 fail-closed 返回 false）
void TestMtkPayload::sendEmiLegacyFailsOnNack()
{
    mtkbrom::EmiData emi;
    emi.bytes = QByteArray(4, '\x11');
    emi.ver = 0;
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\xA5", 1);                       // NACK：preloader 不匹配
    QString err;
    QVERIFY(!mtkbrom::sendEmiLegacy(s, emi, 0x6765, &err));
    QVERIFY2(err.contains(QStringLiteral("NACK")), qPrintable(err));
    QCOMPARE(m->writeFrames.size(), 2);                      // 只发了 0xE8 + 版本，EMI 一字节没发
}

// emiver = 0 档 + dramlength(8) ≠ len(emi)(16)：**截断**（发前 8B）+ 写的是 >I dramlength（不是 lendram）。
// （brief 原文的 0 档用例里 dramlength == len(emi) == 16，两个值相等 → 写错成 lendram 也照样通过；
//   这里用一个不等的情形把该分支的判别力补上）
void TestMtkPayload::sendEmiLegacyTierDTruncatesAndWritesDramLength()
{
    mtkbrom::EmiData emi;
    emi.bytes = QByteArray::fromHex("000102030405060708090A0B0C0D0E0F");
    emi.ver = 0;

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\x5A", 1)
             << QByteArray("\x00\x00\x00\x08", 4)            // dramlength = 8 < len(emi) = 16
             << QByteArray("\x00\x2A", 2)
             << QByteArray("\x00\x00\x00\x00", 4)
             << QByteArray("\x02", 1) << QByteArray("\x00", 1)
             << QByteArray("\x00\x00\x00\x00\x80\x00\x00\x00", 8);
    QString err;
    QVERIFY2(mtkbrom::sendEmiLegacy(s, emi, 0x6765, &err), qPrintable(err));

    QCOMPARE(m->writeFrames.at(3), QByteArray("\x00\x00\x00\x08", 4));    // >I dramlength（8，不是 16）
    QCOMPARE(m->writeFrames.at(4), emi.bytes.left(8));                    // **截断到 8B**
    QVERIFY(m->writeFrames.at(4) != emi.bytes);                           // 反向锚点：确实截了
    QCOMPARE(m->writeFrames.size(), 7);
}

// M_EXT_RAM_RET != 0 → 明确失败（DRAM 初始化没成）
void TestMtkPayload::sendEmiLegacyFailsWhenDramInitRejected()
{
    mtkbrom::EmiData emi;
    emi.bytes = QByteArray(4, '\x22');
    emi.ver = 0;
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\x5A", 1) << QByteArray("\x00\x00\x00\x04", 4)
             << QByteArray("\x00\x01", 2)
             << QByteArray("\x00\x00\xBC\x3F", 4);                   // M_EXT_RAM_RET = 0xBC3F（非 0）
    QString err;
    QVERIFY(!mtkbrom::sendEmiLegacy(s, emi, 0x6765, &err));
    QVERIFY2(err.contains(QStringLiteral("M_EXT_RAM_RET")), qPrintable(err));
}

// 0xC0 之后的存储信息交换：逐段长度 + ACK 位置 + 存储类型判定（漏读 → DA1 不进 stage2）
void TestMtkPayload::exchangeDa1StorageInfoDetectsEmmc()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    QByteArray nandInfo(4, '\0');
    nandInfo[1] = char(0xBC); nandInfo[2] = char(0x04);              // 0x0000BC04（上游注释 0xBC4）
    QByteArray emmcIds(16, '\0');
    emmcIds[3] = char(0x2A);                                         // 首个 EMMC id != 0 → emmc
    m->reads << nandInfo
             << QByteArray("\x00\x00", 2)                            // NAND id 数 = 0（不读 id 表）
             << QByteArray("\x00\x00\x00\x01", 4)                    // EMMC_INFO
             << emmcIds
             << QByteArray("\x5A\x5A\x5A", 3);                       // ackval（3×1B）
    QStringList log;
    QString type;
    QString err;
    QVERIFY2(mtkbrom::exchangeDa1StorageInfo(s, &type, &log, &err), qPrintable(err));
    QCOMPARE(type, QStringLiteral("emmc"));                          // 上游规则：emmcids[0] != 0 → emmc
    QCOMPARE(m->writeFrames.size(), 1);
    QCOMPARE(m->writeFrames.at(0), QByteArray("\x5A", 1));           // 唯一的写：ACK
    QVERIFY2(!log.isEmpty(), "交换信息应留下日志（NAND_INFO/EMMC_INFO/id 数/存储类型）");
}

// NAND id 表首项非 0 → "nand"（D1 随后明确拒绝 —— 见 sendStage2Config）
void TestMtkPayload::exchangeDa1StorageInfoDetectsNand()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\x00\x00\xBC\x04", 4)
             << QByteArray("\x00\x02", 2)                            // NAND id 数 = 2
             << QByteArray("\x00\x11\x00\x22", 4)                    // ids[0] = 0x11 != 0
             << QByteArray("\x00\x00\x00\x2A", 4)                    // EMMC_INFO
             << QByteArray(16, '\x33')
             << QByteArray("\x5A\x5A\x5A", 3);
    QString type;
    QString err;
    QVERIFY2(mtkbrom::exchangeDa1StorageInfo(s, &type, nullptr, &err), qPrintable(err));
    QCOMPARE(type, QStringLiteral("nand"));                          // 判定优先看 NAND
}

// 两种 id 都为 0 → "nor"
void TestMtkPayload::exchangeDa1StorageInfoDetectsNor()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\x00\x00\xBC\x04", 4)
             << QByteArray("\x00\x00", 2)
             << QByteArray("\x00\x00\x00\x01", 4)
             << QByteArray(16, '\0')                                 // EMMC id 全 0
             << QByteArray("\x5A\x5A\x5A", 3);
    QString type;
    QString err;
    QVERIFY2(mtkbrom::exchangeDa1StorageInfo(s, &type, nullptr, &err), qPrintable(err));
    QCOMPARE(type, QStringLiteral("nor"));
}

// stage2 配置：逐字段写序 + errorcode == 0 → **不需要 EMI**（emiNeeded 保持 false）
void TestMtkPayload::stage2ConfigWritesFieldsAndReportsNoEmiNeeded()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\x00\x00\x00\x00", 4);                   // errorcode = 0
    bool emiNeeded = true;
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::sendStage2Config(s, 0x6765, 0x05, 0x01, QStringLiteral("emmc"),
                                       &emiNeeded, &log, &err), qPrintable(err));
    QVERIFY(!emiNeeded);                                             // **0x0 → 不需要 DRAM 配置**
    QCOMPARE(m->writeFrames.at(0), QByteArray("\x05", 1));           // bromver
    QCOMPARE(m->writeFrames.at(1), QByteArray("\x01", 1));           // blver
    QCOMPARE(m->writeFrames.at(2), QByteArray("\x00\x08", 2));       // m_nor_chip = 0x08
    QCOMPARE(m->writeFrames.at(3), QByteArray("\x00", 1));           // nor chip select
    QCOMPARE(m->writeFrames.at(4), QByteArray("\x70\x07\xFF\xFF", 4)); // m_nand_acccon（BE）
    QCOMPARE(m->writeFrames.at(5), QByteArray("\x01", 1));           // bmtflag（默认 1）
    QCOMPARE(m->writeFrames.at(6), QByteArray("\x00\x00\x00\x00", 4)); // bmtpartsize（默认 0）
    QCOMPARE(m->writeFrames.at(7), QByteArray("\x01", 1));           // force_charge
    QCOMPARE(m->writeFrames.at(8), QByteArray("\x01", 1));           // resetkeys（非 0x6583 = 1）
    QCOMPARE(m->writeFrames.at(9), QByteArray("\x02", 1));           // ext_clock
    QCOMPARE(m->writeFrames.at(10), QByteArray("\x00", 1));          // msdc_boot_ch
    QCOMPARE(m->writeFrames.size(), 11);                             // 0x6765 无 hwcode 追加
    QCOMPARE(m->writes, QByteArray("\x05\x01\x00\x08\x00\x70\x07\xFF\xFF\x01\x00\x00\x00\x00"
                                   "\x01\x01\x02\x00", 18));
}

// stage2 配置：0xBC3 → *emiNeeded = true（调用方接着读 draminfo + 发 EMI）
void TestMtkPayload::stage2ConfigReportsEmiNeededOnBc3()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\x00\x00\xBC\x3F", 4);                   // 0xBC3F（**不是** 0xBC3）
    bool emiNeeded = false;
    QString err;
    QVERIFY(!mtkbrom::sendStage2Config(s, 0x6765, 5, 1, QStringLiteral("emmc"), &emiNeeded, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("errorcode")), qPrintable(err));   // 其它值 = 失败

    m->reads.clear();
    m->reads << QByteArray("\x00\x00\x0B\xC3", 4);                   // 0x0BC3 == 0xBC3
    emiNeeded = false;
    QVERIFY2(mtkbrom::sendStage2Config(s, 0x6765, 5, 1, QStringLiteral("emmc"), &emiNeeded, nullptr, &err),
             qPrintable(err));
    QVERIFY(emiNeeded);
}

// 非 eMMC 存储 → 明确拒绝（D1 只支持 eMMC；NAND 的 BMT 分支没实现，不能瞎写）
void TestMtkPayload::stage2ConfigRejectsNonEmmcStorage()
{
    auto usb = std::make_unique<MockUsbChannel>();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    for (const QString &type : {QStringLiteral("nand"), QStringLiteral("nor")}) {
        bool emiNeeded = false;
        QString err;
        QVERIFY(!mtkbrom::sendStage2Config(s, 0x6765, 5, 1, type, &emiNeeded, nullptr, &err));
        QVERIFY2(err.contains(type), qPrintable(err));
    }
}

// 按 hwcode 追加的分支（上游 dalegacy_lib.py:256-278）：每个分支断言**确切的追加字节**，
// 外加 bmtflag/bmtpartsize（mtk_config.py:231-283 的 eMMC 分支）与 resetkeys（0x6583 = 0）。
void TestMtkPayload::stage2ConfigAppendsHwCodeSpecificFields()
{
    // 上游 :264 的字面量实测 **20 字节**（4646 + 00×14 + ff000000）—— 声明成 19 会截掉末尾 00，
    // 发给 0x6580/0x8163/0x8127 时就少一字节（mock 逐帧断言也测不出"少一字节"，故此处钉住长度）
    const QByteArray unk = QByteArray::fromHex("46460000000000000000000000000000ff000000");
    QCOMPARE(unk.size(), 20);
    struct Case { quint16 hwCode; QList<QByteArray> tail; quint8 bmtFlag; quint32 bmtPart; };
    QList<Case> cases;
    cases << Case{0x6592, {be32(0)}, 1, 0x1500000};                    // is_gpt_solution = 0
    cases << Case{0x6580, {be32(1), unk}, 1, 0};                       // slc_percent + 20B 常量
    cases << Case{0x8163, {be32(1), unk}, 1, 0};                       // 同上
    cases << Case{0x8127, {be32(0), be32(1), unk}, 1, 0x1500000};      // 多一个 is_gpt_solution
    cases << Case{0x6589, {be32(1)}, 1, 0};                            // forcedram = 1
    cases << Case{0x6583, {be32(0)}, 1, 0};                            // forcedram = 0
    cases << Case{0x6582, {be32(1)}, 2, 0x1500000};                    // newcombo = 1 + bmt 表
    cases << Case{0x6575, {}, 1, 0x1500000};                           // 无追加（只验 bmt 表）
    cases << Case{0x6571, {}, 1, 0x1500000};                           // 同上
    cases << Case{0x6572, {}, 0, 0xA8};                                // 上游把 blockcount 当 partsize（:266-273）
    cases << Case{0x6765, {}, 1, 0};                                   // 默认芯片（不上任何表）
    for (const Case &c : std::as_const(cases)) {
        auto usb = std::make_unique<MockUsbChannel>();
        MockUsbChannel *m = usb.get();
        mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
        m->reads << QByteArray("\x00\x00\x00\x00", 4);      // errorcode = 0 → 不需要 EMI
        if (c.hwCode == 0x6592)
            m->reads << QByteArray(20, '\0');               // 上游特例：0x6592 另读 5×4B（:286-291）
        bool emiNeeded = true;
        QString err;
        QVERIFY2(mtkbrom::sendStage2Config(s, c.hwCode, 5, 1, QStringLiteral("emmc"),
                                           &emiNeeded, nullptr, &err), qPrintable(err));
        QVERIFY(!emiNeeded);
        QCOMPARE(m->writeFrames.size(), 11 + c.tail.size());
        for (int i = 0; i < c.tail.size(); ++i)
            QCOMPARE(m->writeFrames.at(11 + i), c.tail.at(i));
        // bmtflag / bmtpartsize：**写出的字节** = `B flag` + `>I partSize`（emmc 分支，上游 :231-283）
        QCOMPARE(m->writeFrames.at(5), QByteArray(1, char(c.bmtFlag)));
        QCOMPARE(m->writeFrames.at(6), be32(c.bmtPart));
        // resetkeys：0x6583 为 0，其余为 1（上游 :243-247）
        QCOMPARE(m->writeFrames.at(8), QByteArray(1, char(c.hwCode == 0x6583 ? 0 : 1)));
    }
}

// beginEmiDramInfo：4B + 16B draminfo + 回执必须 0xBC4 + nand id 数
void TestMtkPayload::beginEmiDramInfoReadsDramInfoAndChecksBc4()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    const QByteArray dram(16, '\x7E');
    m->reads << QByteArray("\x00\x00\x00\x10", 4)                     // 4B（丢弃：上游 buffer += ）
             << dram
             << QByteArray("\x00\x00\x0B\xC4", 4)                     // 回执 0xBC4
             << QByteArray("\x00\x01", 2)                             // nand id 数 = 1
             << QByteArray("\x00\x42", 2);                            // 1 × 2B
    QByteArray got;
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::beginEmiDramInfo(s, &got, &log, &err), qPrintable(err));
    QCOMPARE(got, dram);
    QVERIFY2(log.join('\n').contains(QStringLiteral("7e 7e")), qPrintable(log.join('\n')));

    // 回执不是 0xBC4 → 明确失败
    m->reads.clear();
    m->reads << QByteArray(4, '\0') << QByteArray(16, '\0') << QByteArray("\x00\x00\x00\x00", 4);
    QString err2;
    QVERIFY(!mtkbrom::beginEmiDramInfo(s, nullptr, nullptr, &err2));
    QVERIFY2(err2.contains(QStringLiteral("0xBC4")), qPrintable(err2));
}

// read_flash_info：读走全部字节 + PassInfo 判活（ack=0x5A 分支）
void TestMtkPayload::readFlashInfoDa2ConsumesAllBytesAndChecksPassInfo()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    QByteArray nand(0x11, '\0');                                     // NandInfo64：count @15 = 0 → 转 NandInfo32
    nand[11] = char(0x00); nand[12] = char(0x00);                    // NandInfo32 count @11 = 0 → 读 0 字节
    QByteArray pass(0xA, '\0');
    pass[0] = char(0x5A);                                            // ack = 0x5A
    m->reads << QByteArray(0x1C, '\0')      // NOR info
             << nand                        // NAND info(0x11)（计数 0 → 上游读 0B，队列里就没有字节）
             << QByteArray(9, '\0')         // info2
             << QByteArray(0x5C, '\0')      // EMMC info
             << QByteArray(0x1C, '\0')      // SDC info
             << QByteArray(0x26, '\0')      // flashconfig
             << pass;                       // PassInfo
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::readFlashInfoDa2(s, 0x6765, &log, &err), qPrintable(err));
    QVERIFY2(log.join('\n').contains(QStringLiteral("PassInfo")), qPrintable(log.join('\n')));
}

// read_flash_info：PassInfo 不含 0x5A → DA2 未就绪（明确失败，不假装成功）
void TestMtkPayload::readFlashInfoDa2FailsWhenPassInfoIsNotAck()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    QByteArray nand(0x11, '\0');
    QByteArray pass(0xA, '\0');                                      // ack=0, download_status=0
    m->reads << QByteArray(0x1C, '\0') << nand << QByteArray(9, '\0')
             << QByteArray(0x5C, '\0') << QByteArray(0x1C, '\0') << QByteArray(0x26, '\0') << pass;
    QString err;
    QVERIFY(!mtkbrom::readFlashInfoDa2(s, 0x6765, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("DA2 未就绪")), qPrintable(err));
}

// PassInfo：ack != 0x5A 但 download_status 低字节 == 0x5A → 上游第二分支（须再读 1B 才判活）
void TestMtkPayload::readFlashInfoDa2AcceptsDownloadStatusAckBranch()
{
    {
        auto usb = std::make_unique<MockUsbChannel>();
        MockUsbChannel *m = usb.get();
        mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
        queueFlashInfoHead(m);
        QByteArray pass(0xA, '\0');
        pass[4] = char(0x5A);                                        // m_download_status = 0x0000005A
        m->reads << pass << QByteArray("\x01", 1);                   // 第二分支补读的 1B
        QStringList log;
        QString err;
        QVERIFY2(mtkbrom::readFlashInfoDa2(s, 0x6765, &log, &err), qPrintable(err));
        QVERIFY(m->reads.isEmpty());                                 // 补读的 1B 确实读了（残留 = 错位）
    }
    {
        auto usb = std::make_unique<MockUsbChannel>();
        MockUsbChannel *m = usb.get();
        mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
        queueFlashInfoHead(m);
        QByteArray pass(0xA, '\0');
        pass[4] = char(0x5A);
        m->reads << pass;                                            // 补读缺字节 → 明确失败
        QString err;
        QVERIFY(!mtkbrom::readFlashInfoDa2(s, 0x6765, nullptr, &err));
        QVERIFY(!err.isEmpty());
    }
}

// read_flash_info：hwcode ∈ {0x8127,0x8163} 时上游另读 4B（dalegacy_lib.py:543-545）
void TestMtkPayload::readFlashInfoDa2ReadsExtraDwordFor8127()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    queueFlashInfoHead(m);
    QByteArray pass(0xA, '\0');
    pass[0] = char(0x5A);
    m->reads << QByteArray("\x00\x00\x00\x00", 4)                   // hwcode 附加 4B
             << pass;
    QString err;
    QVERIFY2(mtkbrom::readFlashInfoDa2(s, 0x8127, nullptr, &err), qPrintable(err));
    QVERIFY(m->reads.isEmpty());                                     // 附加 4B 与 PassInfo 都读走

    // 同一序列在 0x6765 下会错位（多读的 4B 会被当 PassInfo 的 ack 用）→ 计数不对必须失败
    auto usb2 = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m2 = usb2.get();
    mtkbrom::BromSession s2(std::move(usb2), mtkbrom::BromDevice{});
    queueFlashInfoHead(m2);
    m2->reads << QByteArray("\x00\x00\x00\x00", 4) << pass;
    QString err2;
    QVERIFY(!mtkbrom::readFlashInfoDa2(s2, 0x6765, nullptr, &err2));
}

// boot_to(DA2)：>I addr → >I size（**region[2].m_len，LEGACY 保留签名**）→ >I 0x1000 → ACK →
//   分块写（每块 ACK）→ sleep → 写 ACK → 读 1B ACK
void TestMtkPayload::bootToDa2LegacySendsAddressSizeAndBlocks()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x6765, sel, &err), qPrintable(err));
    QCOMPARE(sel.da2.len, quint32(48));
    QCOMPARE(sel.da2Bytes.size(), 48);            // **保留签名**：长度就是 m_len
    QCOMPARE(sel.da2.startAddr, quint32(0x80000000));

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\x5A", 1)             // 头部后 ACK
             << QByteArray("\x5A", 1)             // 第一块后 ACK
             << QByteArray("\x5A", 1);            // 收尾 ACK
    QVERIFY2(mtkbrom::bootToDa2Legacy(s, sel, &err), qPrintable(err));

    QCOMPARE(m->writeFrames.size(), 5);                            // 3 头 + 1 块 + 收尾 ACK
    QCOMPARE(m->writeFrames.at(0), QByteArray("\x80\x00\x00\x00", 4));   // addr BE
    QCOMPARE(m->writeFrames.at(1), QByteArray("\x00\x00\x00\x30", 4));   // size = m_len = 48 BE
    QCOMPARE(m->writeFrames.at(2), QByteArray("\x00\x00\x10\x00", 4));   // packetsize 0x1000 BE
    QCOMPARE(m->writeFrames.at(3), sel.da2Bytes);                        // **完整切片（含尾部签名）**
    QCOMPARE(m->writeFrames.at(4), QByteArray("\x5A", 1));               // 收尾 ACK
}

// 分块与 ACK 时序：0x2500 字节 → 3 块（0x1000/0x1000/0x500），**每块之后各读 1B ACK**
void TestMtkPayload::bootToDa2ChunksEachPacketAndWaitsAckPerPacket()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x6765, sel, 0x2500, &err), qPrintable(err));
    QCOMPARE(sel.da2Bytes.size(), 0x2500);

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\x5A", 1)             // 头部
             << QByteArray("\x5A", 1) << QByteArray("\x5A", 1) << QByteArray("\x5A", 1)   // 3 块
             << QByteArray("\x5A", 1);            // 收尾
    QVERIFY2(mtkbrom::bootToDa2Legacy(s, sel, &err), qPrintable(err));

    QCOMPARE(m->writeFrames.size(), 7);                            // 3 头 + 3 块 + 收尾 ACK
    QCOMPARE(m->writeFrames.at(3).size(), 0x1000);
    QCOMPARE(m->writeFrames.at(4).size(), 0x1000);
    QCOMPARE(m->writeFrames.at(5).size(), 0x500);
    QCOMPARE(m->writeFrames.at(3) + m->writeFrames.at(4) + m->writeFrames.at(5), sel.da2Bytes);
}

// 头部之后不是 ACK → 立刻失败（不许把非 ACK 当通过）
void TestMtkPayload::bootToDa2FailsWhenHeaderNotAcked()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x6765, sel, &err), qPrintable(err));
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << QByteArray("\xA5", 1);            // NACK
    QVERIFY(!mtkbrom::bootToDa2Legacy(s, sel, &err));
    QVERIFY(!err.isEmpty());
    QCOMPARE(m->writeFrames.size(), 3);           // 只发了头部，数据一字节没发
}

// ---- D1-T9: 引导链（bromBringUpDa）----

// 引导链的**顺序**：DA1 → 0xC0 → 存储信息 → stage2(0xBC3) → draminfo → EMI(0xE8) → boot_to → read_flash_info
void TestMtkPayload::bringUpOrdersEmiAfterStorageInfoAndBeforeDa2()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x6765, sel, &err), qPrintable(err));

    mtkbrom::PreloaderResult pre;
    pre.origin = mtkbrom::PreloaderOrigin::Explicit;
    // **可提取**的合成 preloader（共享夹具）：版本 "00" → EMI 档 D（emiver == 0），
    // 载荷 16 字节 —— 与 emiReadsVer0() 里设备回的 dramlength(16) 对齐。
    pre.bytes = mtktest::buildEmiPreloader(QByteArray("00", 2), QByteArray(16, '\x11'));
    pre.path = QStringLiteral("/tmp/preloader.bin");

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << da1UploadReads(sel) << QByteArray("\xC0", 1) << storageExchangeReads()
             << stage2Reads(0xBC3) << dramInfoReads() << emiReadsVer0()
             << bootToReads() << flashInfoReads();
    QStringList log;
    QVERIFY2(mtkbrom::bromBringUpDa(s, sel, 0x6765, /*bromVer=*/5, /*blVer=*/1, pre, &log, &err),
             qPrintable(err));

    // 关键帧的**相对顺序**（帧号比"存在性"更能抓顺序错位）
    int idxDa1Payload = -1, idxEmi = -1, idxBootHeader = -1, idxStorageAck = -1, idxStage2 = -1;
    for (int i = 0; i < m->writeFrames.size(); ++i) {
        if (m->writeFrames.at(i) == sel.da1Bytes) idxDa1Payload = i;
        if (m->writeFrames.at(i) == QByteArray("\xE8", 1)) idxEmi = i;
        // m_nand_acccon（stage2 的第 5 个字段）= 全流唯一的 0x7007FFFF
        if (m->writeFrames.at(i) == QByteArray("\x70\x07\xFF\xFF", 4)) idxStage2 = i;
        if (idxDa1Payload >= 0 && idxBootHeader < 0 && m->writeFrames.at(i) == be32(sel.da2.startAddr))
            idxBootHeader = i;                       // 0x80000000 只可能是 boot_to 的地址字段
        if (idxStorageAck < 0 && idxDa1Payload >= 0 && i > idxDa1Payload
            && m->writeFrames.at(i) == QByteArray("\x5A", 1))
            idxStorageAck = i;                       // DA1 载荷之后第一个单字节 0x5A = 存储交换的 ACK
    }
    // 先钉"存在"再钉"相对位置"：缺了 EMI 时 idxEmi 保持 -1，`idxBootHeader > idxEmi` 会**无意义地成立**
    // （跳过整段的实现也过），所以两步都要。
    QVERIFY2(idxDa1Payload >= 0, "必须发过 DA1 载荷");
    QVERIFY2(idxStorageAck >= 0, "必须发过存储信息交换的 ACK");
    QVERIFY2(idxEmi >= 0, "errorcode == 0xBC3 时必须发过 ENABLE_DRAM(0xE8)");
    // 存储交换的边界由**后一段的帧**来钉（T9 审查 M2：原写法 `idxStorageAck > idxDa1Payload`
    // 因扫描条件自带 `i > idxDa1Payload` 而恒真）——stage2 的 m_nand_acccon 必须在它之后。
    QVERIFY2(idxStage2 > idxStorageAck, "stage2 配置（m_nand_acccon）必须在存储信息交换之后");
    QVERIFY2(idxEmi > idxStorageAck, "EMI(0xE8) 必须在存储信息交换之后");
    QVERIFY2(idxBootHeader > idxEmi, "boot_to 必须在 EMI 之后（顺序错 = 真机必挂）");
    QVERIFY2(log.join('\n').contains(QStringLiteral("emmc")), qPrintable(log.join('\n')));
}

// errorcode == 0 → **不发 0xE8**，流程继续（DA1 不需要 DRAM 配置；没有 preloader 也只是信息级）
void TestMtkPayload::bringUpSkipsEmiWhenNotNeeded()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x6765, sel, &err), qPrintable(err));
    mtkbrom::PreloaderResult pre;                       // origin = None
    pre.skipReason = QStringLiteral("未提供 preloader，且网络获取默认关闭");

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << da1UploadReads(sel) << QByteArray("\xC0", 1) << storageExchangeReads()
             << stage2Reads(0x0) << bootToReads() << flashInfoReads();
    QStringList log;
    QVERIFY2(mtkbrom::bromBringUpDa(s, sel, 0x6765, 5, 1, pre, &log, &err), qPrintable(err));
    for (const QByteArray &f : std::as_const(m->writeFrames))
        QVERIFY2(f != QByteArray("\xE8", 1), "errorcode==0 时不得发 ENABLE_DRAM");
    QVERIFY2(log.join('\n').contains(QStringLiteral("不需要 DRAM 配置")), qPrintable(log.join('\n')));
}

// **0xBC3 但没有 preloader → 明确中止**（上游同姿态："Preloader needed due to dram config"；
// 这是 spec §7 的更正点：缺 preloader 不中止**只在 errorcode == 0 时**成立）
void TestMtkPayload::bringUpAbortsWhenDramConfigNeededWithoutPreloader()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x6765, sel, &err), qPrintable(err));
    mtkbrom::PreloaderResult pre;                       // origin = None
    pre.skipReason = QStringLiteral("未提供 preloader");

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << da1UploadReads(sel) << QByteArray("\xC0", 1) << storageExchangeReads()
             << stage2Reads(0xBC3) << dramInfoReads();
    QStringList log;
    QVERIFY(!mtkbrom::bromBringUpDa(s, sel, 0x6765, 5, 1, pre, &log, &err));
    QVERIFY2(err.contains(QStringLiteral("DRAM 配置")), qPrintable(err));
    // 钉住**是哪条失败**（缺 preloader），不是"随便什么错都算过"：
    // 变异实测 —— 把本分支的 if 去掉后，空 preloader 会掉进"EMI 提取失败"那条路，
    // 错误文案里同样含"DRAM 配置" → 只断言前半句时**该变异不被捕获**。
    QVERIFY2(err.contains(QStringLiteral("没有可用的 preloader")), qPrintable(err));
    QVERIFY2(err.contains(pre.skipReason), qPrintable(err));   // 跳过原因必须透出给用户
    for (const QByteArray &f : std::as_const(m->writeFrames))
        QVERIFY2(f != QByteArray("\xE8", 1), "没有 EMI 时不得发 ENABLE_DRAM");
}

// 0xC0 不对 → 立即失败，且**一个 DA2 字节都不许发**
void TestMtkPayload::bringUpAbortsBeforeDa2WhenSyncIsWrong()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x6765, sel, &err), qPrintable(err));
    mtkbrom::PreloaderResult pre;
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << da1UploadReads(sel) << QByteArray("\x00", 1);      // 不是 0xC0
    QStringList log;
    QVERIFY(!mtkbrom::bromBringUpDa(s, sel, 0x6765, 5, 1, pre, &log, &err));
    QVERIFY2(err.contains(QStringLiteral("0xC0")), qPrintable(err));
    for (const QByteArray &f : std::as_const(m->writeFrames))
        QVERIFY2(f != be32(sel.da2.startAddr), "DA1 未就绪时不得进入 boot_to(DA2)");
}

// ---- D1-T9 审查 I1: 刷写主体（bromFlashOnSession）----
//
// 这 8 条把 runBromFlash 里"写之前必须先验证"的部分搬进了可离线测的层：
//   ① 空计划早拒 ② 代际拒绝 ×4（表外/非 LEGACY/IoT/v6 DA）③ 0xFC 降级继续 ④ FINISH 只告警
// 夹具见本文件匿名命名空间的 prologueReads / fullSessionReads / pmtEntry60 / daBytesForHw。

// 基线：整条流程走到底**成功**（无 preloader / errorcode == 0 / 单分区写 + FINISH 收尾）
void TestMtkPayload::bromFlashOnSessionWritesPlanAndFinishes()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(kFlashHwCode, sel, &err), qPrintable(err));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray image("\xAB\xCD", 2);
    const QString imgPath = writeTempImage(dir, QStringLiteral("boot.img"), image);
    QVERIFY(!imgPath.isEmpty());

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << fullSessionReads(sel, pmtEntry60(QByteArray("boot", 4), 0x10000, 0x1000));

    mtkbrom::BromFlashRequest req;
    req.daFile = daBytesForHw(kFlashHwCode);
    req.daLabel = QStringLiteral("合成 DA");
    req.imagePaths << imgPath;

    SessionCapture cap;
    QVERIFY2(mtkbrom::bromFlashOnSession(s, req, cap.logFn(), cap.progressFn(), &err),
             qPrintable(err));
    QVERIFY2(cap.joined().contains(QStringLiteral("FINISH（0xD9）收尾完成")), qPrintable(cap.joined()));
    QCOMPARE(cap.progress.size(), 1);                     // 进度分母/分子 = 计划总量/已写
    QCOMPARE(cap.progress.at(0).first, quint64(2));
    QCOMPARE(cap.progress.at(0).second, quint64(2));
    QVERIFY2(m->writes.contains(image), "镜像字节必须真的写出去");
    QVERIFY2(m->writes.contains(QByteArray(1, char(0x62))), "必须发过 EMMC 写命令(0x62)");
    QVERIFY2(m->writes.contains(QByteArray(1, char(0xD9))), "必须发过 FINISH(0xD9)");
}

// **空计划早拒**：设备分区表与所选镜像**全不匹配** → 写任何字节之前返回 false。
// 这条是"不许在未知分区表上写"的守卫 —— 删掉实现里的早拒时它必须红（见报告 I1 复现证据）。
void TestMtkPayload::bromFlashOnSessionRejectsEmptyPlanBeforeAnyWrite()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(kFlashHwCode, sel, &err), qPrintable(err));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray image("\xAB\xCD", 2);
    // 镜像叫 boot.img，设备表里只有 system → 匹配不上 → 零条目计划
    const QString imgPath = writeTempImage(dir, QStringLiteral("boot.img"), image);
    QVERIFY(!imgPath.isEmpty());

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << fullSessionReads(sel, pmtEntry60(QByteArray("system", 6), 0x20000, 0x2000));

    mtkbrom::BromFlashRequest req;
    req.daFile = daBytesForHw(kFlashHwCode);
    req.daLabel = QStringLiteral("合成 DA");
    req.imagePaths << imgPath;

    SessionCapture cap;
    QVERIFY(!mtkbrom::bromFlashOnSession(s, req, cap.logFn(), cap.progressFn(), &err));
    QVERIFY2(err.contains(QStringLiteral("没有任何可写入")), qPrintable(err));
    QVERIFY2(cap.progress.isEmpty(), "失败时不得报进度");
    // **写之前**：不得出现 EMMC 写命令 / FINISH，镜像字节一个都不许发
    QVERIFY2(!m->writes.contains(QByteArray(1, char(0x62))), "计划为空时不得发写命令");
    QVERIFY2(!m->writes.contains(QByteArray(1, char(0xD9))), "计划为空时不得发 FINISH");
    QVERIFY2(!m->writes.contains(image), "计划为空时镜像字节一个都不许发");
}

// 代际拒绝 ①：芯片表未收录（0x0001 不在表内）→ 明确报错（**不得**默认按 LEGACY 硬刷）
void TestMtkPayload::bromFlashOnSessionRejectsUnknownHwCode()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << prologueReads(0x0001);

    mtkbrom::BromFlashRequest req;
    req.daFile = daBytesForHw(kFlashHwCode);
    req.imagePaths << QStringLiteral("/nonexistent/x.img");
    SessionCapture cap;
    QString err;
    QVERIFY(!mtkbrom::bromFlashOnSession(s, req, cap.logFn(), cap.progressFn(), &err));
    QVERIFY2(err.contains(QStringLiteral("芯片表未收录")), qPrintable(err));
}

// 代际路由 ②：表内 **XFLASH 代**（0x0766，dacode 0x6765）现在**不再被拒**，整条 XFlash 链走到底。
// （本用例取代 D1 的 `bromFlashOnSessionRejectsNonLegacyDamode` —— 那条断言的"非 LEGACY 一律拒绝"
// 正是本任务要拆掉的门。）
// 设备 0x0766 → DA1 上传 → 0xC0 → 七步握手 → bring-up 四步（agent=brom，无 preloader → 不发 EMI）
// → boot_to（剥签名）→ GET_PACKET_LENGTH / GET_CHIP_ID / GET_PARTITION_TBL_CATA → GPT 读（READ_DATA）
// → 逐分区 xflashWriteData → SHUTDOWN 收尾。
// 判别力：**写地址必须来自 GPT 的分区条目 first_lba**（0x5000 = LBA 40 × 512；若误用头里的 first_usable(34) 会得 0x4400）、storage/parttype = eMMC/user(1/8)
// —— 地址算错就是往别的分区里写；把 XFlash 当 LEGACY 处理则会在第一条 BROM 级帧就错位。
void TestMtkPayload::bromFlashOnSessionRoutesXflashEndToEnd()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x6765, sel, &err), qPrintable(err));   // 条目 hw_code == dacode

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray image("\xAB\xCD", 2);
    const QString imgPath = writeTempImage(dir, QStringLiteral("boot.img"), image);
    QVERIFY(!imgPath.isEmpty());

    // GPT 夹具：16 扇区 × 512 = 8192 = readTable 的探测长度（kProbeLen）→ 一笔读满足，不触发补读；
    // 单分区 "boot" = **LBA 40..41**（T11 审查 Minor 4：故意让 first_lba(40) ≠ first_usable_lba(34)，
    // 这样"读错字段"的变异会给出 0x4400 而不是 0x5000 → 断言判红）。
    const QByteArray gpt = mtkgpt::testBuildSyntheticGpt(512, 16, /*firstLba=*/40);

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << prologueReads(0x0766)                                     // 设备报的 hw_code（条目键是 dacode）
             << da1UploadReads(sel) << QByteArray("\xC0", 1)
             << statusReads(0) << statusReads(0) << frameReads(1, le32(0x434E5953))              // 握手 3 帧
             << statusReads(0) << statusReads(0) << frameReads(1, QByteArray("0x20240101")) << statusReads(0)
             << statusReads(0) << statusReads(0) << statusReads(0)                               // reset_key（2+参数）
             << statusReads(0) << statusReads(0) << statusReads(0)                               // checksum_level
             << statusReads(0) << statusReads(0) << frameReads(1, QByteArray("brom")) << statusReads(0)
             << statusReads(0) << statusReads(0) << statusReads(0)                               // boot_to（无 EMI）
             << statusReads(0) << statusReads(0) << frameReads(1, le32(0x200) + le32(0x400)) << statusReads(0)
             << statusReads(0) << statusReads(0) << frameReads(1, QByteArray(10, '\0')) << statusReads(0)
             << statusReads(0) << statusReads(0) << frameReads(1, le32(0x64))                     // CATA（唯一无尾部 status）
             << statusReads(0) << statusReads(0) << statusReads(0) << frameReads(1, gpt) << statusReads(0)
             << statusReads(0) << statusReads(0) << statusReads(0) << statusReads(0)              // 写：命令/参数/块/收尾
             << statusReads(0) << statusReads(0) << frameReads(1, le32(0))                        // CC devctrl + 回包
             << statusReads(0) << statusReads(0);                                                 // SHUTDOWN

    mtkbrom::BromFlashRequest req;
    req.daFile = daBytesForHw(0x6765);            // 文件里条目的 hw_code 就等于 dacode
    req.daLabel = QStringLiteral("合成 DA（XFlash 代）");
    req.imagePaths << imgPath;

    SessionCapture cap;
    QVERIFY2(mtkbrom::bromFlashOnSession(s, req, cap.logFn(), cap.progressFn(), &err), qPrintable(err));
    QCOMPARE(m->reads.size(), 0);                 // 读队列必须正好清空（devctrl 尾部 status 漏读必错位）
    QVERIFY2(cap.joined().contains(QStringLiteral("代际判定：XFLASH")), qPrintable(cap.joined()));
    QVERIFY2(cap.joined().contains(QStringLiteral("GPT 读出 1 个分区")), qPrintable(cap.joined()));
    QVERIFY2(cap.joined().contains(QStringLiteral("SHUTDOWN 收尾完成")), qPrintable(cap.joined()));

    // 写参数必须点名 **GPT 给的分区偏移** + eMMC/user（56B = <IIQQ + 32B NandExtension，全 0）
    const QByteArray expectedParam = le32(1) + le32(8) + le32(0x5000) + le32(0) + le32(0x200) + le32(0)
                                     + QByteArray(32, '\0');
    QVERIFY2(m->writes.contains(expectedParam), "WRITE_DATA 参数里的地址必须来自 GPT 条目 first_lba（LBA 40 × 512 = 0x5000，"
                                               "误用 first_usable(34) 会得 0x4400）");
    QVERIFY2(m->writes.contains(image + QByteArray(510, '\0')), "镜像必须补零到 512 的整数倍写出");
    QCOMPARE(cap.progress.size(), 1);
    QCOMPARE(cap.progress.at(0).first, quint64(2));
    QCOMPARE(cap.progress.at(0).second, quint64(2));
    // 顺序：**先读分区表（READ_DATA）后写（WRITE_DATA）** —— T11 审查 Minor 4 的后半（原先只断言了各自出现）
    {
        const QByteArray &stream = m->writes;
        const int iRead = stream.indexOf(le32(mtkbrom::X_CMD_READ_DATA));
        const int iWrite = stream.indexOf(le32(mtkbrom::X_CMD_WRITE_DATA));
        QVERIFY2(iRead >= 0 && iWrite >= 0, "读写命令都必须出现");
        QVERIFY2(iWrite > iRead, "WRITE_DATA 必须在 READ_DATA 之后（先取表、后写）");
    }
}

// 代际拒绝 ③：**IoT 芯片**（0x6226 = LEGACY + iot）→ 明确报错（上游 IoT 走另一套 region 映射）
void TestMtkPayload::bromFlashOnSessionRejectsIotChip()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << prologueReads(0x6226);

    mtkbrom::BromFlashRequest req;
    req.daFile = daBytesForHw(kFlashHwCode);
    req.imagePaths << QStringLiteral("/nonexistent/x.img");
    SessionCapture cap;
    QString err;
    QVERIFY(!mtkbrom::bromFlashOnSession(s, req, cap.logFn(), cap.progressFn(), &err));
    QVERIFY2(err.contains(QStringLiteral("IoT")), qPrintable(err));
}

// 代际路由 ④：**DA 文件是 v6**（强制 XML 代；芯片是 LEGACY 也走 XML）—— T11 判代际、T12 接线后
// 走完整条 XML 链：DA1（BROM 级）→ 等 CMD:START → setup_env/hw_init/host_info → READ-FLASH 读 GPT
// （**同一个 mtkgpt**）→ 逐分区 WRITE-FLASH → REBOOT 收尾。
// 判别力：① v6 若被漏判成 LEGACY，会走 storageExchange/stage2/boot_to/FINISH 那条读序列 → 与本夹具错位；
//   ② XML 若被判成 XFlash，会发 0x01000A 等 XFlash 帧并读 status 帧 → 同样错位。
void TestMtkPayload::bromFlashOnSessionRoutesXmlEndToEnd()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(kFlashHwCode, sel, &err), qPrintable(err));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray image("\xAB\xCD", 2);
    const QString imgPath = writeTempImage(dir, QStringLiteral("boot.img"), image);
    QVERIFY(!imgPath.isEmpty());

    // GPT 夹具与 XFlash 用例同款：16 扇区 × 512 = 0x2000 = readTable 的探测长度（一笔读满足，
    // 不触发补读）；单分区 "boot" = **LBA 40..41**（first_lba 40 ≠ first_usable 34 → 读错字段的
    // 变异会给出 0x4400 而不是 0x5000）。
    const QByteArray gpt = mtkgpt::testBuildSyntheticGpt(512, 16, /*firstLba=*/40);

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << prologueReads(kFlashHwCode)
             << da1UploadReads(sel)                                   // DA1：SEND_DA + JUMP_DA（BROM 级）
             << xmlDeviceStartReads()                                 // 设备发 CMD:START
             << xmlCommandReads() << xmlCommandReads()
             << xmlCommandReads() << xmlCommandReads()                // setup_env + 2×hw_init + host_info
             // 分区表：READ-FLASH（noack）→ UpFile → 裸 OK@0x2000 → ack/OK → 数据帧 → 逐帧 ack/OK → CMD:START
             << textReads(QStringLiteral("OK"))
             << xmlUploadFileReads()
             << textReads(QStringLiteral("OK@0x2000"))
             << textReads(QStringLiteral("OK"))
             << frameReads(1, gpt)
             << textReads(QStringLiteral("OK"))
             << xmlDeviceStartReads()
             // 写 boot（镜像 2B → 补零 0x200）：WRITE-FLASH（noack）→ FileSysOp(FILE-SIZE) → DwnFile
             //   → 第二次长度 ack/OK → 单包 ack(0)/OK + 数据/OK → 收尾 CMD:END/CMD:START
             << textReads(QStringLiteral("OK"))
             << xmlFileSysOpReads(QStringLiteral("FILE-SIZE"), 0x200)
             << xmlDwnFileReads(0x200, 0x200)
             << textReads(QStringLiteral("OK"))
             << textReads(QStringLiteral("OK"))
             << textReads(QStringLiteral("OK"))
             << xmlWriteTailReads()
             // REBOOT 收尾
             << xmlCommandReads();

    mtkbrom::BromFlashRequest req;
    req.daFile = daBytesForHw(kFlashHwCode, /*v6=*/true);            // v6 → decideGeneration 强制 XML
    req.daLabel = QStringLiteral("合成 DA（v6 → XML 代）");
    req.imagePaths << imgPath;

    SessionCapture cap;
    QVERIFY2(mtkbrom::bromFlashOnSession(s, req, cap.logFn(), cap.progressFn(), &err), qPrintable(err));
    QCOMPARE(m->reads.size(), 0);                 // 读队列必须**正好**清空（漏读/多读都是错位）
    QVERIFY2(cap.joined().contains(QStringLiteral("代际判定：XML")), qPrintable(cap.joined()));
    QVERIFY2(cap.joined().contains(QStringLiteral("v6")), "日志必须点名 DA 是 v6（判定依据）");
    QVERIFY2(cap.joined().contains(QStringLiteral("XML：GPT 读出 1 个分区")), qPrintable(cap.joined()));
    QVERIFY2(cap.joined().contains(QStringLiteral("未发 EMI")), qPrintable(cap.joined()));
    QVERIFY2(cap.joined().contains(QStringLiteral("REBOOT 收尾完成")), qPrintable(cap.joined()));
    QCOMPARE(cap.progress.size(), 1);
    QCOMPARE(cap.progress.at(0).first, quint64(2));
    QCOMPARE(cap.progress.at(0).second, quint64(2));

    // 写入命令：`<partition>` = **存储描述符**（XC:452-462 的 UFSPartitionType 文本，`ST:216`；
    // 不是分区名）、`<offset>` = **GPT 条目地址**（上游 `writeflash(addr=partition.sector *
    // pagesize)`：`v6.py:1095-1097`、`mtk_da_handler.py:544-548`）。LBA 40 × 512 = 0x5000；
    // 误用头里的 first_usable(34) 得 0x4400、漏传地址（恒 0x0）得 0x0。
    const QString expectWrite = mtkbrom::XmlSession::envelope(
        QStringLiteral("WRITE-FLASH"),
        {QStringLiteral("<partition>EMMC-USER</partition>"),
         QStringLiteral("<offset>0x5000</offset>"),
         QStringLiteral("<source_file>MEM://0x8000000:0x200</source_file>")});
    QVERIFY2(m->writes.contains(xmlFrame(expectWrite)), qPrintable(expectWrite));
    QVERIFY2(m->writes.contains(image + QByteArray(510, '\0')), "镜像必须补零到 512 的整数倍写出");
    // 先读表、后写（顺序错位 = 往未知地址写）
    {
        const QByteArray &stream = m->writes;
        const int iRead = stream.indexOf(QStringLiteral("CMD:READ-FLASH").toUtf8());
        const int iWrite = stream.indexOf(QStringLiteral("CMD:WRITE-FLASH").toUtf8());
        QVERIFY2(iRead >= 0 && iWrite >= 0, "READ-FLASH 与 WRITE-FLASH 都必须出现");
        QVERIFY2(iWrite > iRead, "WRITE-FLASH 必须在 READ-FLASH 之后（先取表、后写）");
    }
    QVERIFY2(m->writes.indexOf(le32(mtkbrom::X_CMD_INIT_EXT_RAM)) == -1, "XML 代不得发 XFlash 的 INIT_EXT_RAM");
}

// XML 分支的空表显式门（T11 在 XFlash 分支补的同款）：GPT 合法但**零个有效条目** →
// 在**任何写之前**点名"设备分区表为空"。没有这道门时计划层会走 derived 分支，最终报成
// "内部错误：分区 X 不在设备表地址映射里" —— 把真因（分区表为空）说成内部错位。
void TestMtkPayload::bromFlashOnSessionRejectsEmptyXmlGptTable()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(kFlashHwCode, sel, &err), qPrintable(err));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray image("\xAB\xCD", 2);
    const QString imgPath = writeTempImage(dir, QStringLiteral("boot.img"), image);
    QVERIFY(!imgPath.isEmpty());

    const QByteArray gpt = emptyGptFixture();
    QCOMPARE(gpt.size(), 0x2000);

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << prologueReads(kFlashHwCode)
             << da1UploadReads(sel)
             << xmlDeviceStartReads()
             << xmlCommandReads() << xmlCommandReads()
             << xmlCommandReads() << xmlCommandReads()
             << textReads(QStringLiteral("OK"))
             << xmlUploadFileReads()
             << textReads(QStringLiteral("OK@0x2000"))
             << textReads(QStringLiteral("OK"))
             << frameReads(1, gpt)
             << textReads(QStringLiteral("OK"))
             << xmlDeviceStartReads();

    mtkbrom::BromFlashRequest req;
    req.daFile = daBytesForHw(kFlashHwCode, /*v6=*/true);
    req.imagePaths << imgPath;

    SessionCapture cap;
    QVERIFY(!mtkbrom::bromFlashOnSession(s, req, cap.logFn(), cap.progressFn(), &err));
    QVERIFY2(err.contains(QStringLiteral("设备分区表为空")), qPrintable(err));
    QVERIFY2(cap.joined().contains(QStringLiteral("XML：GPT 读出 0 个分区")), qPrintable(cap.joined()));
    QVERIFY2(!m->writes.contains(QStringLiteral("CMD:WRITE-FLASH").toUtf8()), "空表不得发写命令");
    QVERIFY2(!m->writes.contains(QStringLiteral("CMD:REBOOT").toUtf8()), "空表不得发 REBOOT 收尾");
    QVERIFY2(cap.progress.isEmpty(), "失败时不得报进度");
    QCOMPARE(m->reads.size(), 0);
}

// **0xFC 降级**：设备不答 0xFC → 告警 + 按 0/0 继续（上游口径：不清零继续 → 版本过滤维旁路），
// 整条流程仍能走到底成功。
void TestMtkPayload::bromFlashOnSessionContinuesWhenHwSwVerUnavailable()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(kFlashHwCode, sel, &err), qPrintable(err));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString imgPath = writeTempImage(dir, QStringLiteral("boot.img"), QByteArray("\xAB\xCD", 2));
    QVERIFY(!imgPath.isEmpty());

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << fullSessionReads(sel, pmtEntry60(QByteArray("boot", 4), 0x10000, 0x1000),
                                 /*answerHwSwVer=*/false);

    mtkbrom::BromFlashRequest req;
    req.daFile = daBytesForHw(kFlashHwCode);
    req.imagePaths << imgPath;
    SessionCapture cap;
    QVERIFY2(mtkbrom::bromFlashOnSession(s, req, cap.logFn(), cap.progressFn(), &err),
             qPrintable(err));                        // **不中止**
    QVERIFY2(cap.hasErrorLineContaining(QStringLiteral("0xFC")), "0xFC 失败必须落**告警**（isError=true）");
    QVERIFY2(cap.joined().contains(QStringLiteral("旁路")), qPrintable(cap.joined()));
    QVERIFY2(cap.joined().contains(QStringLiteral("DA 条目")), "降级后必须继续做条目选择（0/0 旁路版本维）");
    QVERIFY2(cap.joined().contains(QStringLiteral("FINISH（0xD9）收尾完成")), qPrintable(cap.joined()));
}

// **FINISH 只告警**：写成功后 FINISH 无 ACK → 仍返回 true（数据已落盘），但必须有 error 级日志
void TestMtkPayload::bromFlashOnSessionWarnsButSucceedsWhenFinishFails()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(kFlashHwCode, sel, &err), qPrintable(err));
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray image("\xAB\xCD", 2);
    const QString imgPath = writeTempImage(dir, QStringLiteral("boot.img"), image);
    QVERIFY(!imgPath.isEmpty());

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    m->reads << fullSessionReads(sel, pmtEntry60(QByteArray("boot", 4), 0x10000, 0x1000),
                                 /*answerHwSwVer=*/true, /*answerFinish=*/false);

    mtkbrom::BromFlashRequest req;
    req.daFile = daBytesForHw(kFlashHwCode);
    req.imagePaths << imgPath;
    SessionCapture cap;
    QVERIFY2(mtkbrom::bromFlashOnSession(s, req, cap.logFn(), cap.progressFn(), &err),
             "FINISH 失败不得把成功报成失败（数据已落盘）");
    QVERIFY2(cap.hasErrorLineContaining(QStringLiteral("FINISH 收尾失败")), qPrintable(cap.joined()));
    QVERIFY2(m->writes.contains(image), "数据确实写了（所以 FINISH 失败只能是告警）");
}

// **DA 条目查找键必须是 chip->dacode**（T9 复审修复）：设备报的 hw_code 与芯片表的 dacode 不同时
// 必须用 dacode 去 DA 文件里找条目 —— 0x0321（表内 LEGACY）的 dacode 是 0x6735，DA 文件里只有
// hw_code == 0x6735 的条目。上游 `daconfig.py:208-209` 用 chipconfig.dacode 查、`dasetup` 按条目
// 自己的 hw_code 建（:190/:192）→ 键 = dacode；本仓 `mtk_chip_table.h:29-30` 同口径。
// 判别力：把实现里的键改回 hwCode（变异）→ 本用例必须红（选不中条目 → 无候选）。
void TestMtkPayload::bromFlashOnSessionLooksUpDaByDacode()
{
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x6735, sel, &err), qPrintable(err));   // 条目 hw_code = dacode
    QCOMPARE(sel.entry.hwCode, quint16(0x6735));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray image("\xAB\xCD", 2);
    const QString imgPath = writeTempImage(dir, QStringLiteral("boot.img"), image);
    QVERIFY(!imgPath.isEmpty());

    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::BromSession s(std::move(usb), mtkbrom::BromDevice{});
    // 设备报 **0x0321**（其 dacode = 0x6735）；引导链/刷写段与常规路径完全一致
    m->reads << prologueReads(0x0321)
             << chainAndFlashReads(sel, pmtEntry60(QByteArray("boot", 4), 0x10000, 0x1000));

    mtkbrom::BromFlashRequest req;
    req.daFile = daBytesForHw(0x6735);          // 文件里条目的 hw_code 就等于 dacode
    req.daLabel = QStringLiteral("合成 DA（dacode 键）");
    req.imagePaths << imgPath;

    SessionCapture cap;
    QVERIFY2(mtkbrom::bromFlashOnSession(s, req, cap.logFn(), cap.progressFn(), &err),
             qPrintable(err));
    QVERIFY2(cap.joined().contains(QStringLiteral("DA 条目")), qPrintable(cap.joined()));
    QVERIFY2(cap.joined().contains(QStringLiteral("FINISH（0xD9）收尾完成")), qPrintable(cap.joined()));
    QVERIFY2(m->writes.contains(image), "镜像字节必须真的写出去");
}

// ---- D2-T11: 三代路由 + XFlash 引导链 ----

// 代际判定（纯函数）：表外 / IoT / damode 与 v6 的优先关系（上游 daconfig.py:216 `DC:216`）
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

    // preloader 夹具：extractEmiXflash 的切片 = **整个窗口**（不是 MTK_BIN+0xC），期望原样出现在写流里
    const QByteArray preBytes = mtktest::buildEmiPreloader("38", QByteArray(0x340, '\xA5'));
    mtkbrom::PreloaderResult pre;
    pre.origin = mtkbrom::PreloaderOrigin::Explicit;
    pre.path = QStringLiteral("/tmp/preloader_6765.bin");
    pre.bytes = preBytes;

    // 读队列：D1 的 DA1 上传（9 笔，含 SEND_DA + JUMP_DA）→ 0xC0 → 握手（**2×status** + SYNC 回读）
    //        → bring-up 四步（**12×status + 2 回包**）→ EMI 两笔 → boot_to 三笔。**一帧 = 两笔**
    // （SYNC 只发不读；四个查询的尾部 status 见 T4 的逐查询表 —— GET_PARTITION_TBL_CATA 是唯一例外）
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
    QCOMPARE(m->reads.size(), 0);      // 读队列必须**正好**清空（漏读/多读都是后续错位的根源）

    // 顺序断言：在**字节流**上比首次出现下标（0x434E5953 / 0x010100 / 0x010101 / 0x01000A / 0x010008 都是本链独占值）
    const QByteArray &stream = m->writes;
    const int iSync = stream.indexOf(le32(0x434E5953));
    const int iEnv  = stream.indexOf(le32(mtkbrom::X_CMD_SETUP_ENV));
    const int iHw   = stream.indexOf(le32(mtkbrom::X_CMD_SETUP_HW_INIT));
    const int iEmi  = stream.indexOf(le32(mtkbrom::X_CMD_INIT_EXT_RAM));
    const int iBoot = stream.indexOf(le32(mtkbrom::X_CMD_BOOT_TO));
    QVERIFY2(iSync >= 0, "缺 XFlash SYNC 帧 —— 七步握手没跑");
    QVERIFY2(iSync < iEnv && iEnv < iHw, "七步握手顺序：SYNC → SETUP_ENV → SETUP_HW_INIT");
    QVERIFY2(iEmi > iHw, "INIT_EXT_RAM 必须在七步握手之后");
    QVERIFY2(iBoot > iEmi, "BOOT_TO 必须在 EMI 之后");
    QVERIFY2(stream.contains(preBytes), "EMI 必须原样发出（XFlash 整块切片）");
    QVERIFY2(m->writeFrames.contains(le32(0x68)), "bring-up 第二步必须发 set_reset_key(0x68)");

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
    QCOMPARE(m->reads.size(), 0);
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
    QCOMPARE(m->reads.size(), 0);
    QVERIFY2(m->writes.indexOf(le32(mtkbrom::X_CMD_INIT_EXT_RAM)) == -1, "无 preloader → 无 EMI 可发");
    QVERIFY2(log.join(QLatin1Char('\n')).contains(QStringLiteral("无 preloader")),
             qPrintable(log.join(QLatin1Char('\n'))));
    QVERIFY2(log.join(QLatin1Char('\n')).contains(QStringLiteral("未提供 preloader 路径")),
             "skipReason 必须转述进日志");
}

// ---- D3-T12: XML 链整合（引导 + 分区表 + 逐分区写 + REBOOT）----

// XML 链的引导段：sendDa1（BROM 级，**含 JUMP_DA**）→ 等 CMD:START → setup_env / hw_init / host_info
// 四条命令；**全程不发任何 DRAM/EMI 命令**（铁律 16：XML 的 DRAM 初始化是 setup_env 里
// `<initialize_dram>YES</initialize_dram>` 那一条参数，XL:184 / XC:120-135）。
// 夹具按 XML 的真实节奏排（**3 帧/命令**，XL:188-219；见本文件 D3-T12 夹具段头的 ⚠️）。
void TestMtkPayload::xmlChainSendsCmdStartHandshakeWithoutEmi()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    mtkbrom::DaSelection sel;
    QString err;
    QVERIFY2(mtktest::makeSelection(0x0907, sel, &err), qPrintable(err));   // 表内 XML 芯片（0x0907 = MT6983）

    m->reads = da1UploadReads(sel);                                        // 与 LEGACY/XFlash 同一份 DA1 上传夹具
    m->reads << xmlDeviceStartReads()
             << xmlCommandReads()                                          // SET-RUNTIME-PARAMETER
             << xmlCommandReads()                                          // HOST-SUPPORTED-COMMANDS
             << xmlCommandReads()                                          // NOTIFY-INIT-HW
             << xmlCommandReads();                                         // SET-HOST-INFO

    mtkbrom::BromSession brom(std::move(usb), mtkbrom::BromDevice{});
    mtkbrom::XmlSession x(m);
    QStringList log;
    QVERIFY2(mtkbrom::xmlBringUpDa(brom, x, sel, &log, &err), qPrintable(err));
    QCOMPARE(m->reads.size(), 0);                                          // 读队列正好清空

    QStringList sent;
    for (const QByteArray &f : std::as_const(m->writeFrames))
        if (f.startsWith("<?xml"))
            sent << QString::fromUtf8(f).remove(QChar('\0'));
    QCOMPARE(sent.size(), 4);                                              // 四条命令；CMD:START 不回 ack
    QVERIFY(sent.at(0).contains(QStringLiteral("CMD:SET-RUNTIME-PARAMETER")));
    QVERIFY(sent.at(0).contains(QStringLiteral("<initialize_dram>YES</initialize_dram>")));
    QVERIFY(sent.at(1).contains(QStringLiteral("CMD:HOST-SUPPORTED-COMMANDS")));
    QVERIFY(sent.at(2).contains(QStringLiteral("CMD:NOTIFY-INIT-HW")));
    QVERIFY(sent.at(3).contains(QStringLiteral("CMD:SET-HOST-INFO")));

    // DA1 走的是**三代共用的 BROM 级帧**（0xD7/0xD5 必须发出 —— 不是"一个字节都不发"）
    QVERIFY2(m->writes.contains(QByteArray(1, char(0xD7))), "必须发 SEND_DA(0xD7)");
    QVERIFY2(m->writes.contains(QByteArray(1, char(0xD5))), "必须发 JUMP_DA(0xD5)");
    // 不发 EMI：既无 XFlash 的 INIT_EXT_RAM 帧（0x01000A），也无 LEGACY 的 ENABLE_DRAM(0xE8) 帧
    QVERIFY2(m->writes.indexOf(le32(mtkbrom::X_CMD_INIT_EXT_RAM)) == -1, "XML 不得发 XFlash 的 INIT_EXT_RAM");
    QVERIFY2(m->writes.indexOf(mtktest::be32(0xE8)) == -1, "XML 不得发 LEGACY 的 ENABLE_DRAM");
    QVERIFY2(log.join(QLatin1Char('\n')).contains(QStringLiteral("未发 EMI")),
             qPrintable(log.join(QLatin1Char('\n'))));
}

// 设备首条命令不是 CMD:START → 明确失败（不猜、不硬着头皮往下走）。
// 判别力：XmlSession 对"具名但未列举"的命令返回 true 且只置 `out.command`（`mtk_xml_session.h`
// 的调用方契约）—— 不显式核对 `out.command` 的实现会把这台设备当成已就绪。
// 夹具用 PROGRESS-REPORT（保活命令，XL:390-407）：它会被 readCommandResult 走完保活循环并把
// **下一条**命令（CMD:END）作为结果交回 → 判据必须在 CMD:START 上（而不是"有没有读到帧"）。
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
    // 失败即止：**四条 setup 命令一条都不许发**（写流里只有 DA1 的帧）
    for (const QByteArray &f : std::as_const(m->writeFrames))
        QVERIFY2(!f.startsWith("<?xml"), "首条命令不是 CMD:START 时不得继续发 setup 命令");
}

// 分区表读回调适配器：mtkgpt::ReadFn 的 (byteOffset, len) → XML 的 READ-FLASH 区间读（裸 OK@ 数据路径）
// ——"三代共用同一个 GPT 解析器"的**唯一新缝**就在这个适配器上（解析器本身在 T1 用真样本钉死）。
// 夹具按读路径的**逐帧 ack**节奏排（XL:508-559，T10 的 readPartitionSequence 同款）：数据帧之后
// 还有一发 ack 和它的 "OK" 应答 —— 漏掉它，收尾的 CMD:START 会被当成 ack 应答 → 假红。
void TestMtkPayload::xmlSectorReaderRoundTripsDeviceBytes()
{
    auto usb = std::make_unique<MockUsbChannel>();
    MockUsbChannel *m = usb.get();
    const QByteArray block(0x200, '\x5C');
    m->reads << textReads(QStringLiteral("OK"))                            // READ-FLASH 被接受（noack）
             << xmlUploadFileReads()                                       // CMD:UPLOAD-FILE
             << textReads(QStringLiteral("OK@0x200"))                      // 裸数据路径：长度
             << textReads(QStringLiteral("OK"))                            // 长度确认
             << frameReads(1, block)                                       // 数据帧（头、载荷两笔）
             << textReads(QStringLiteral("OK"))                            // 逐帧 ack 的应答
             << xmlDeviceStartReads();                                     // 收尾 CMD:START

    mtkbrom::XmlSession x(m);                    // mock 由 unique_ptr 持有到用例结束（不需要 BromSession）
    const mtkgpt::ReadFn read = mtkbrom::xmlSectorReader(x);
    QByteArray got;
    QString err;
    QVERIFY2(read(0x1000, 0x200, &got, &err), qPrintable(err));
    QCOMPARE(got, block);
    QCOMPARE(m->reads.size(), 0);                                          // 读队列正好清空

    QString sentXml;
    for (const QByteArray &f : std::as_const(m->writeFrames))
        if (f.startsWith("<?xml"))
            sentXml += QString::fromUtf8(f).remove(QChar('\0'));
    QVERIFY2(sentXml.contains(QStringLiteral("CMD:READ-FLASH")), qPrintable(sentXml));
    // 适配器的实质：**字节偏移原样进 `<offset>`**、默认存储描述符是 "EMMC-USER"（XC:474-484，
    // 也是 `ST:216` 起 XML 分支的文本 parttype）—— 只断言"发过 READ-FLASH"抓不到偏移错位
    QVERIFY2(sentXml.contains(QStringLiteral("<partition>EMMC-USER</partition>")), qPrintable(sentXml));
    QVERIFY2(sentXml.contains(QStringLiteral("<offset>0x1000</offset>")), qPrintable(sentXml));
    QVERIFY2(sentXml.contains(QStringLiteral("<length>0x200</length>")), qPrintable(sentXml));
}

QTEST_APPLESS_MAIN(TestMtkPayload)
#include "test_mtk_payload.moc"
