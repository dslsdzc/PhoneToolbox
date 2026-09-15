#include <QtTest>
#include <memory>
#include <utility>

#include "core/modes/mtk_brom.h"
#include "core/modes/mtk_da_file.h"
#include "core/modes/mtk_emmc.h"
#include "core/modes/mtk_payload.h"
#include "core/modes/mtk_preloader_fetch.h"
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
    int idxDa1Payload = -1, idxEmi = -1, idxBootHeader = -1, idxStorageAck = -1;
    for (int i = 0; i < m->writeFrames.size(); ++i) {
        if (m->writeFrames.at(i) == sel.da1Bytes) idxDa1Payload = i;
        if (m->writeFrames.at(i) == QByteArray("\xE8", 1)) idxEmi = i;
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
    QVERIFY2(idxStorageAck > idxDa1Payload, "存储信息交换必须在 DA1 之后");
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

QTEST_APPLESS_MAIN(TestMtkPayload)
#include "test_mtk_payload.moc"
