// tests/test_mtk_xflash_payload.cpp
//
// MTK XFlash 引导层（Phase D2 Task 4）：七步握手 / bring-up 四步 / 只读查询。
//
// 夹具约定（与 T2 测试同款，**先读再改**）：默认 `IBromUsb::readExact` 是"单次读 + 严格长度"
// （`mtk_brom.cpp:219-231`），本层先读 12B 帧头、再读载荷 —— **每次 readExact 消耗一笔队列项**。
// 把整帧塞成一笔会让"读头"吃掉载荷、后续读全部错位。负向用例同理：要"短读失败"就少给字节。
//
// 读帧数（**逐条对照上游核过**，是这些用例的主要判据）：
//   • 七步握手 = 2 帧 status（两个 setup 各一次 send_param，XFL:986-994）+ 1 帧 SYNC 回包。
//     **裸 SYNC 命令没有 status**（上游 sync() 只发送，XFL:903-907）—— 多给这一帧，
//     实现会把它读成 ENV 的 status，整体错位一帧，最后的 SYNC 回包读到 HW_INIT 的 status 而判失败。
//   • 有回包的 devctrl 查询 = 2 帧 status（DEVICE_CTRL、子命令）+ 回包 + **尾部 status**
//     （上游拿到回包后再读一次：XFL:571-578 / :330-338 / :396-418 / :623-636 / :421-436）。
//     漏读尾部 status 会让后续读错位一帧：10 字节的 expire_date 文本会被当成 status 载荷
//     （readStatus 取首 u32 → 非 0）而当场判失败。
//   • GET_PARTITION_TBL_CATA 是**唯一例外**：上游不读尾部 status（XFL:612-621），本层同样不读。
//
// T6（写/读数据路径）的读帧数，**逐条对照上游核过**（XFL:670-704 的 cmd_write_data/cmd_read_data、
// XFL:706-806 的 readflash、XFL:835-901 的 writeflash）：
//   • 写 = 命令 status + 56B 参数帧的 status + **每块一次**（send_param）+ **循环后一次收尾 status**
//     （XFL:883）+ CC_OPTIONAL_DOWNLOAD_ACT 的 devctrl 二连 + 空回包。收尾 status 与 CC 两处都是
//     计划期更正（原稿漏掉），漏读会让这两帧留在设备侧。
//   • 读 = 命令 status + 参数帧的 status + **参数帧之后的第二个 status**（XFL:698-702）+ 数据帧/flag 帧…
//     + **收尾帧**（XFL:770-776）。第二个 status 漏读会让之后每个数据帧整体错位一帧。
//   • 数据帧（slength > 4）要收下并 `ack(rstatus=False)`（XFL:750-757）；**flag 帧（slength == 4）
//     不回 ack**（XFL:761-765）。**数据帧的"读完"判据是字节数**（bytestoread），不是"见到 flag 就停"：
//     中途出现的 flag==0 帧只是继续（XFL:730-768 的 while 只以 bytestoread 计），收尾那一帧才结束本次读。
#include <QtTest>
#include <QByteArray>
#include <QStringList>

#include "core/modes/mtk_xflash_payload.h"
#include "core/modes/mtk_xflash_session.h"
#include "core/modes/mtk_brom.h"

// mock 与 T2 同款（从 T2 的测试文件复制一份到本文件；两个测试文件各自独立，符合既有惯例）
class MockUsbChannel : public mtkbrom::IBromUsb
{
public:
    QByteArray writes;              // 全部写入字节（拼接）
    QList<QByteArray> writeFrames;  // 逐笔（每次 write() 一笔）—— 帧级断言用
    QList<QByteArray> reads;        // 按序弹出的读取响应；空队列 → read 返回 false
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
// ⚠️ **一帧应答 = 两笔队列项**：12B 帧头 + 载荷（见文件头夹具约定）。
QList<QByteArray> frameReads(quint32 dt, const QByteArray &payload)
{
    return {le32(0xFEEEEEEF) + le32(dt) + le32(quint32(payload.size())), payload};
}
QList<QByteArray> statusReads(quint32 code) { return frameReads(1, le32(code)); }   // length==4 → <I
// 日志断言：存在**同一行**同时含两个 token 的行（避免"只打标签不打值"也算过）
bool logHas(const QStringList &log, const QString &a, const QString &b)
{
    for (const QString &line : log) {
        if (line.contains(a) && line.contains(b))
            return true;
    }
    return false;
}
} // namespace

class TestMtkXflashPayload : public QObject
{
    Q_OBJECT
private slots:
    void handshakeOrderAndBytes();
    void handshakeRejectsNonSync();
    void handshakeRejectsSetupStatusError();
    void bringUpStepsOrder();
    void getChipIdParsesFiveShorts();
    void getPacketLengthParsesTwoU32();
    void getPartitionCataMapsGptAndPmt();
    void getRamInfoAccepts24And48Bytes();
    void queryRejectsNonZeroTrailingStatus();
    void devCtrlQuerySkipsTrailingStatusWhenReplyEmpty();
    void chipIdWarnsOnOverlongReply();
    void sendEmiSequence();
    void sendEmiRejectsNonZeroStatus();
    void bootToAcceptsZeroOrSyncStatus();
    void bootToRejectsBadStatus();
    void shutdownParameterLayout();
    void shutdownRejectsNonZeroTrailingStatus();
    void emptyPayloadsRejectedBeforeAnyWrite();
    void storageParamLayout();
    void writeDataChunkingAndChecksum();
    void writeDataRejectsZeroPacketLength();
    void writeDataRejectsNonZeroTrailingStatus();
    void writeDataWarnsWhenOptionalDownloadActFails();
    void writeDataAnnounces512AlignedLength();
    void writeDataRejectsUnalignedPacketLength();
    void writeDataZeroLengthSendsZeroLengthParam();
    void readDataCollectsFramesAndAcks();
    void readDataRejectsNonZeroFlag();
    void readDataRejectsUnknownFrameLength();
    void readDataRejectsNonZeroFlagAfterDataFrame();
    void readDataRejectsNonZeroFinalFlag();
    void readDataAbortsOnFlagFrameFlood();
    void readDataZeroLengthReadsFinalFrameOnly();
};

// 七步握手：SYNC 帧 → SETUP_ENV（命令帧 + 20B）→ SETUP_HW_INIT（命令帧 + 4B）→ 读回 "SYNC"
void TestMtkXflashPayload::handshakeOrderAndBytes()
{
    MockUsbChannel m;
    // 读队列：SETUP_ENV 的 status → SETUP_HW_INIT 的 status → 最后的 SYNC 回包（共 3 帧，无 SYNC status）
    m.reads << statusReads(0) << statusReads(0) << frameReads(1, le32(0x434E5953));
    mtkbrom::XFlashSession x(&m, 0x6765);
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::xflashDa1Handshake(x, &log, &err), qPrintable(err));

    // 写帧顺序（每条命令一个**独立帧**：xsend(cmd) 两笔 + send_param 帧头/载荷两笔，XFL:911/921-922 / :928-930）：
    //   SYNC(头+值) → SETUP_ENV(命令头+命令) + (载荷头+载荷) → SETUP_HW_INIT(命令头+命令) + (载荷头+载荷) = 10 笔
    QCOMPARE(m.writeFrames.size(), 10);
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));   // SYNC 帧头
    QCOMPARE(m.writeFrames.at(1), le32(0x434E5953));                       // SYNC 值
    QCOMPARE(m.writeFrames.at(2), le32(0xFEEEEEEF) + le32(1) + le32(4));   // SETUP_ENV 命令帧头
    QCOMPARE(m.writeFrames.at(3), le32(quint32(mtkbrom::X_CMD_SETUP_ENV)));          // 命令 0x010100
    QCOMPARE(m.writeFrames.at(4), le32(0xFEEEEEEF) + le32(1) + le32(20));  // SETUP_ENV 载荷帧头
    QCOMPARE(m.writeFrames.at(5).size(), 20);                              // 载荷 = 20 字节
    QCOMPARE(m.writeFrames.at(6), le32(0xFEEEEEEF) + le32(1) + le32(4));   // SETUP_HW_INIT 命令帧头
    QCOMPARE(m.writeFrames.at(7), le32(quint32(mtkbrom::X_CMD_SETUP_HW_INIT)));      // 命令 0x010101
    QCOMPARE(m.writeFrames.at(8), le32(0xFEEEEEEF) + le32(1) + le32(4));   // SETUP_HW_INIT 载荷帧头
    QCOMPARE(m.writeFrames.at(9), le32(0));                                // 参数 0

    // SETUP_ENV 载荷字段（小端，5×u32 = 20B；XFP:83-85 的 OS_LINUX=1、上游默认 logchannel="UART"→1）
    const QByteArray env = m.writeFrames.at(5);
    QCOMPARE(env.size(), 20);
    QCOMPARE(env.mid(0, 4), le32(0));                                      // da_log_level
    QCOMPARE(env.mid(4, 4), le32(1));                                      // log_channel = UART
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

// 本层比上游更严的一处：两个 setup 的 status 非 0 必须失败
// （上游 XFL:989-990 不检查返回值、失败也继续；静默继续会让后续帧全部错位 —— 见文件头纪律）
void TestMtkXflashPayload::handshakeRejectsSetupStatusError()
{
    {
        MockUsbChannel m;
        m.reads << statusReads(0xC0020053);        // SETUP_ENV 的 status = anti-rollback 硬错误码
        mtkbrom::XFlashSession x(&m, 0x6765);
        QString err;
        QVERIFY(!mtkbrom::xflashDa1Handshake(x, nullptr, &err));
        QVERIFY2(err.contains(QStringLiteral("0xC0020053")), qPrintable(err));
    }
    {
        MockUsbChannel m;
        m.reads << statusReads(0) << statusReads(0xC0040050);   // SETUP_HW_INIT 的 status
        mtkbrom::XFlashSession x(&m, 0x6765);
        QString err;
        QVERIFY(!mtkbrom::xflashDa1Handshake(x, nullptr, &err));
        QVERIFY2(err.contains(QStringLiteral("0xC0040050")), qPrintable(err));
    }
}

// bring-up 四步顺序（XFL:1103-1107）：expire_date → reset_key(0x68) → checksum_level(0) → connection_agent
void TestMtkXflashPayload::bringUpStepsOrder()
{
    MockUsbChannel m;
    // 四步的读帧（**上游逐条核过**，多一帧/少一帧都会让后续错位）：
    //   get_expire_date    : 2×status + 回包 + 尾部 status（XFL:571-578）
    //   set_reset_key      : 3×status（DEVICE_CTRL、子命令、send_param 各一次；XFL:206-209）
    //   set_checksum_level : 3×status（XFL:241-244）
    //   get_connection_agent: 2×status + 回包 + 尾部 status（XFL:330-338）
    m.reads << statusReads(0) << statusReads(0) << frameReads(1, QByteArray("0x20240101")) << statusReads(0)
            << statusReads(0) << statusReads(0) << statusReads(0)
            << statusReads(0) << statusReads(0) << statusReads(0)
            << statusReads(0) << statusReads(0) << frameReads(1, QByteArray("brom")) << statusReads(0);
    mtkbrom::XFlashSession x(&m, 0x6765);
    QByteArray agent;
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::xflashBringUpSteps(x, &agent, &log, &err), qPrintable(err));
    QCOMPARE(agent, QByteArray("brom"));
    QCOMPARE(m.reads.size(), 0);                                 // 读帧数与上游逐帧对齐（多一帧/少一帧都红）
    // 日志断言：两条都必须带**解析出的值**（"只打标签不打值"或整行丢失都会红）
    QVERIFY2(logHas(log, QStringLiteral("expire_date"), QStringLiteral("0x20240101")),
             qPrintable(log.join(QLatin1Char('|'))));
    QVERIFY2(logHas(log, QStringLiteral("connection_agent"), QStringLiteral("brom")),
             qPrintable(log.join(QLatin1Char('|'))));

    // 顺序断言：四步的子命令号都是本链独占值 —— 记**首次出现的下标**再比大小（顺序错即红）
    int iExpire = -1, iResetKey = -1, iChecksum = -1, iAgent = -1, iResetKeyParam = -1;
    for (int i = 0; i < m.writeFrames.size(); ++i) {
        const QByteArray &f = m.writeFrames.at(i);
        if (iExpire < 0 && f == le32(quint32(mtkbrom::X_CTRL_GET_EXPIRE_DATE)))       iExpire = i;
        if (iResetKey < 0 && f == le32(quint32(mtkbrom::X_CTRL_SET_RESET_KEY)))       iResetKey = i;
        if (iChecksum < 0 && f == le32(quint32(mtkbrom::X_CTRL_SET_CHECKSUM_LEVEL)))  iChecksum = i;
        if (iAgent < 0 && f == le32(quint32(mtkbrom::X_CTRL_GET_CONNECTION_AGENT)))   iAgent = i;
        if (iResetKeyParam < 0 && f == le32(0x68))                                    iResetKeyParam = i;
    }
    QVERIFY2(iExpire >= 0, "必须发 GET_EXPIRE_DATE 子命令");
    QVERIFY2(iResetKey >= 0, "必须发 SET_RESET_KEY 子命令");
    QVERIFY2(iChecksum >= 0, "必须发 SET_CHECKSUM_LEVEL 子命令");
    QVERIFY2(iAgent >= 0, "必须发 GET_CONNECTION_AGENT 子命令");
    QVERIFY2(iResetKeyParam > iResetKey, "set_reset_key 的参数 0x68 必须跟在它的子命令之后");
    QVERIFY(iExpire < iResetKey);
    QVERIFY(iResetKey < iChecksum);
    QVERIFY(iChecksum < iAgent);
}

// GET_CHIP_ID：回包 5×u16（XFL:396-418）
void TestMtkXflashPayload::getChipIdParsesFiveShorts()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0)
            << frameReads(1, le16(0x6765) + le16(0x8A00) + le16(0xCA00) + le16(0x0000) + le16(1))
            << statusReads(0);                                   // 尾部 status（XFL:409）
    mtkbrom::XFlashSession x(&m, 0x6765);
    mtkbrom::XChipId id;
    QString err;
    QVERIFY2(mtkbrom::xflashGetChipId(x, id, &err), qPrintable(err));
    QCOMPARE(id.hwCode, quint16(0x6765));
    QCOMPARE(id.hwSubCode, quint16(0x8A00));
    QCOMPARE(id.hwVersion, quint16(0xCA00));
    QCOMPARE(id.swVersion, quint16(0x0000));
    QCOMPARE(id.chipEvolution, quint16(1));
    QCOMPARE(m.reads.size(), 0);                                 // 尾部 status 已被消费（读帧数精确）
}

// GET_PACKET_LENGTH：回包 <II（XFL:623-636）
void TestMtkXflashPayload::getPacketLengthParsesTwoU32()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0) << frameReads(1, le32(0x10000) + le32(0x20000))
            << statusReads(0);                                   // 尾部 status（XFL:626）
    mtkbrom::XFlashSession x(&m, 0x6765);
    mtkbrom::XPacketLength pl;
    QString err;
    QVERIFY2(mtkbrom::xflashGetPacketLength(x, pl, &err), qPrintable(err));
    QCOMPARE(pl.writeLength, quint32(0x10000));
    QCOMPARE(pl.readLength, quint32(0x20000));
    QCOMPARE(m.reads.size(), 0);
}

// GET_PARTITION_TBL_CATA：0x64=GPT / 0x65=PMT / 其它=Unknown（XFL:612-621）
void TestMtkXflashPayload::getPartitionCataMapsGptAndPmt()
{
    {
        MockUsbChannel m;
        // 唯一的"不读尾部 status"查询：多塞一帧**毒药**（status 0xDEAD）——实现若读它就会判失败，
        // 读走则队列不再剩两笔。本层照上游不读（XFL:612-621 拿到回包即返回），故毒药必须原封不动。
        m.reads << statusReads(0) << statusReads(0) << frameReads(1, le32(0x64)) << statusReads(0xDEAD);
        mtkbrom::XFlashSession x(&m, 0x6765);
        mtkbrom::PartitionCata c = mtkbrom::PartitionCata::Unknown;
        QVERIFY(mtkbrom::xflashGetPartitionCata(x, c, nullptr));
        QCOMPARE(c, mtkbrom::PartitionCata::Gpt);
        QCOMPARE(m.reads.size(), 2);                             // 毒药帧（头+载荷）没被读走
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
        QCOMPARE(c, mtkbrom::PartitionCata::Unknown);            // 其它值 → Unknown（调用方按"两者都试"处理）（⚠️ 契约已更正：Unknown **不得**两者都试 —— 集成层明确拒绝，见 mtk_xflash_payload.h:99-103）
    }
}

// GET_RAM_INFO：回包 24B（32 位）或 48B（64 位）**原样**返回；其它长度明确失败（XFL:421-436）
void TestMtkXflashPayload::getRamInfoAccepts24And48Bytes()
{
    {
        MockUsbChannel m;
        const QByteArray ram24 = le32(1) + le32(0x40000000) + le32(0x8000)
                               + le32(2) + le32(0x80000000) + le32(0x40000000);
        m.reads << statusReads(0) << statusReads(0) << frameReads(1, ram24) << statusReads(0);
        mtkbrom::XFlashSession x(&m, 0x6765);
        QByteArray raw;
        QString err;
        QVERIFY2(mtkbrom::xflashGetRamInfo(x, &raw, &err), qPrintable(err));
        QCOMPARE(raw, ram24);
        QCOMPARE(m.reads.size(), 0);                             // 尾部 status 已被消费（读帧数精确）
    }
    {
        MockUsbChannel m;
        QByteArray ram48;
        for (int i = 0; i < 6; ++i)                              // 6×u64 = 48B：(sram, dram) 三元组 ×2
            ram48 += le32(quint32(0x1000 + i)) + le32(quint32(0x2000 + i));
        m.reads << statusReads(0) << statusReads(0) << frameReads(1, ram48) << statusReads(0);
        mtkbrom::XFlashSession x(&m, 0x6765);
        QByteArray raw;
        QVERIFY(mtkbrom::xflashGetRamInfo(x, &raw, nullptr));
        QCOMPARE(raw, ram48);
        QCOMPARE(m.reads.size(), 0);
    }
    {
        MockUsbChannel m;
        const QByteArray bad(20, '\x33');                        // 20B：上游只认 24/48，本层明确报错（更严）
        m.reads << statusReads(0) << statusReads(0) << frameReads(1, bad) << statusReads(0);
        mtkbrom::XFlashSession x(&m, 0x6765);
        QByteArray raw;
        QString err;
        QVERIFY(!mtkbrom::xflashGetRamInfo(x, &raw, &err));
        QVERIFY2(err.contains(QStringLiteral("24")), qPrintable(err));   // 文案给出期望长度
        QCOMPARE(m.reads.size(), 0);                             // 长度不符也要先把尾部 status 读掉（先对齐、后校验）
    }
}
// 尾部 status 是**判据**，不只是"排空一帧"：非 0 必须让查询失败并给出可诊断文案
// （契约见 devCtrlQuery 注释；上游拿到非 0 只是"取不到值"继续走，本层中止 —— 比上游更严）
void TestMtkXflashPayload::queryRejectsNonZeroTrailingStatus()
{
    {
        MockUsbChannel m;
        m.reads << statusReads(0) << statusReads(0)
                << frameReads(1, le16(0x6765) + le16(0x8A00) + le16(0xCA00) + le16(0x0000) + le16(1))
                << statusReads(0xDEADBEEF);                       // 回包之后的 status 非 0
        mtkbrom::XFlashSession x(&m, 0x6765);
        mtkbrom::XChipId id;
        QString err;
        QVERIFY(!mtkbrom::xflashGetChipId(x, id, &err));
        QVERIFY2(err.contains(QStringLiteral("0xDEADBEEF")), qPrintable(err));   // 文案带码值
        QCOMPARE(m.reads.size(), 0);                              // 错帧仍被排空，不留给下一次读
    }
    {
        // bring-up：expire_date 的尾部 status 非 0 → 整链中止（后续三步一帧都不发）
        MockUsbChannel m;
        m.reads << statusReads(0) << statusReads(0) << frameReads(1, QByteArray("0x20240101"))
                << statusReads(0xDEADBEEF);
        mtkbrom::XFlashSession x(&m, 0x6765);
        QByteArray agent;
        QString err;
        QVERIFY(!mtkbrom::xflashBringUpSteps(x, &agent, nullptr, &err));
        QVERIFY2(err.contains(QStringLiteral("0xDEADBEEF")), qPrintable(err));
        QVERIFY(agent.isEmpty());                                 // 连 agent 都没解析出来
        int resetKeyFrames = 0;
        for (const QByteArray &f : std::as_const(m.writeFrames)) {
            if (f == le32(quint32(mtkbrom::X_CTRL_SET_RESET_KEY)))
                ++resetKeyFrames;
        }
        QCOMPARE(resetKeyFrames, 0);                              // 中止后没再发任何子命令
    }
}

// 空回包**不读**尾部 status（上游同样按 `回包非空` 前置判断跳过，XFL:573）：
// 若多读一帧，就会把下一步的 status 吃掉、后续全部后移一帧 —— 本用例用"后三步仍逐帧对齐"钉住
void TestMtkXflashPayload::devCtrlQuerySkipsTrailingStatusWhenReplyEmpty()
{
    MockUsbChannel m;
    // 0 长度回包帧 = **只有帧头一笔**：T2 的 xread 在 len==0 时不读载荷（mtk_xflash_session.cpp:83），
    // 故这里不能用 frameReads(1, QByteArray())（那会多留一笔空载荷，末行的队列断言会当场红）
    m.reads << statusReads(0) << statusReads(0)                          // DEVICE_CTRL / 子命令的 status
            << QList<QByteArray>{le32(0xFEEEEEEF) + le32(1) + le32(0)}   // expire 的 0 长度回包（只一笔）
            << statusReads(0) << statusReads(0) << statusReads(0)            // set_reset_key
            << statusReads(0) << statusReads(0) << statusReads(0)            // set_checksum_level
            << statusReads(0) << statusReads(0) << frameReads(1, QByteArray("brom")) << statusReads(0);
    mtkbrom::XFlashSession x(&m, 0x6765);
    QByteArray agent;
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::xflashBringUpSteps(x, &agent, &log, &err), qPrintable(err));
    QCOMPARE(agent, QByteArray("brom"));
    QCOMPARE(m.reads.size(), 0);                                 // 一帧不多、一帧不少
}

// GET_CHIP_ID 回包 **> 10 字节**：照上游截断（只取前 5×u16、**不判失败** —— 未知硬件可能多带填充），
// 但截断要落到日志里，不再静默（T4 审查 Minor 7 的处置：改判据会误伤未知硬件，改"可见性"不会）
void TestMtkXflashPayload::chipIdWarnsOnOverlongReply()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0)
            << frameReads(1, le16(0x6765) + le16(0x8A00) + le16(0xCA00) + le16(0x0000) + le16(1)
                            + QByteArray("\xAA\xBB", 2))         // 12B：多出 2 字节
            << statusReads(0);
    mtkbrom::XFlashSession x(&m, 0x6765);
    mtkbrom::XChipId id;
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::xflashGetChipId(x, id, &err, &log), qPrintable(err));
    QCOMPARE(id.hwCode, quint16(0x6765));                        // 前 10 字节照常解析
    QCOMPARE(id.chipEvolution, quint16(1));
    QVERIFY2(logHas(log, QStringLiteral("GET_CHIP_ID"), QStringLiteral("12")),
             qPrintable(log.join(QLatin1Char('|'))));            // 截断不再静默
    QCOMPARE(m.reads.size(), 0);
}

// EMI（DRAM 初始化，XFL:251-270）：INIT_EXT_RAM → status → sleep → **长度单独一帧** → 数据分块（0x200）→ 一次 status
void TestMtkXflashPayload::sendEmiSequence()
{
    MockUsbChannel m;
    m.reads << statusReads(0)          // INIT_EXT_RAM 的 status
            << statusReads(0);         // send_param 的 status
    mtkbrom::XFlashSession x(&m, 0x6765);
    const QByteArray emi(912, '\xA5');
    QString err;
    QVERIFY2(mtkbrom::xflashSendEmi(x, emi, &err), qPrintable(err));
    // 写调用**逐笔**核对（先钉次数再取下标）：912 = 0x200 + 400 两笔 —— 只对字节流求和
    // 抓不到"整段写一遍再分块写一遍"的双写，笔数与每笔长度都要对
    QCOMPARE(m.writeFrames.size(), 7);
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));   // INIT_EXT_RAM 帧头
    QCOMPARE(m.writeFrames.at(1), le32(0x01000A));                         // 命令值
    QCOMPARE(m.writeFrames.at(2), le32(0xFEEEEEEF) + le32(1) + le32(4));   // 长度帧头
    QCOMPARE(m.writeFrames.at(3), le32(912));                              // **长度单独一帧**
    QCOMPARE(m.writeFrames.at(4), le32(0xFEEEEEEF) + le32(1) + le32(912)); // EMI 帧头
    QCOMPARE(m.writeFrames.at(5), emi.left(0x200));                        // 第一块 = 512
    QCOMPARE(m.writeFrames.at(6), emi.mid(0x200));                         // 第二块 = 剩余 400
    int dataBytes = 0;
    for (int i = 5; i < m.writeFrames.size(); ++i) dataBytes += m.writeFrames.at(i).size();
    QCOMPARE(dataBytes, 912);                                              // 载荷分块（0x200）合计
    QCOMPARE(m.reads.size(), 0);                                           // 恰读 2 帧 status
}

// EMI 的 INIT_EXT_RAM status 非 0 → 立即失败，且**其后一帧都不发**：长度帧与 EMI 本体都不得出现。
// 毒药帧（预置的 send_param status）没被读走 = 恰读 1 帧 status，失败点确实在第一步。
void TestMtkXflashPayload::sendEmiRejectsNonZeroStatus()
{
    MockUsbChannel m;
    m.reads << statusReads(0xC0020053)      // INIT_EXT_RAM 的 status = anti-rollback（硬错误码）
            << statusReads(0xDEAD);         // 毒药
    mtkbrom::XFlashSession x(&m, 0x6765);
    QString err;
    QVERIFY(!mtkbrom::xflashSendEmi(x, QByteArray(912, '\xA5'), &err));
    QVERIFY2(err.contains(QStringLiteral("0xC0020053")), qPrintable(err));   // session 层码值文案
    QCOMPARE(m.writeFrames.size(), 2);      // 只到 INIT_EXT_RAM 的命令帧（头+值）为止
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));
    QCOMPARE(m.writeFrames.at(1), le32(0x01000A));
    QCOMPARE(m.reads.size(), 2);            // 毒药帧（头+载荷）没被读走
}

// boot_to：BOOT_TO 帧 → <QQ addr,len> → 数据 → sleep → status（0 或 SYNC 都算成功）
// **整段读 3 帧 status**：BOOT_TO / send_data（XFL:282 自带一次）/ 终判（XFL:307）。
// 只给 2 帧（原稿的写法）会让终判读到空队列 → 正确实现也被判红。
void TestMtkXflashPayload::bootToAcceptsZeroOrSyncStatus()
{
    const QByteArray da2(0x300, '\x11');         // 已剥签名的 DA2
    for (quint32 st : {0u, 0x434E5953u}) {
        MockUsbChannel m;
        m.reads << statusReads(0) << statusReads(0) << statusReads(st);
        mtkbrom::XFlashSession x(&m, 0x6765);
        QString err;
        QVERIFY2(mtkbrom::xflashBootTo(x, 0x40000000ull, da2, &err), qPrintable(err));
        QCOMPARE(m.writeFrames.size(), 6);                                     // 命令 + 16B 参数 + DA2 单块
        QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));    // BOOT_TO 帧头
        QCOMPARE(m.writeFrames.at(1), le32(0x010008));                         // 命令值
        QCOMPARE(m.writeFrames.at(2), le32(0xFEEEEEEF) + le32(1) + le32(16));   // 参数帧头（16B）
        QCOMPARE(m.writeFrames.at(3), le32(quint32(0x40000000)) + le32(0) + le32(quint32(da2.size())) + le32(0));
        QCOMPARE(m.writeFrames.at(4), le32(0xFEEEEEEF) + le32(1) + le32(quint32(da2.size())));
        QCOMPARE(m.writeFrames.at(5), da2);                                    // **不剥**：原样送出（0x300 < 0x400 单块）
        QCOMPARE(m.reads.size(), 0);                                           // 恰读 3 帧 status
    }
}

// boot_to：最终 status 既非 0 也非 SYNC → 明确失败（前两帧正常，确保失败**来自终判**）
void TestMtkXflashPayload::bootToRejectsBadStatus()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0) << statusReads(0xDEAD);
    mtkbrom::XFlashSession x(&m, 0x6765);
    QString err;
    QVERIFY(!mtkbrom::xflashBootTo(x, 0x40000000ull, QByteArray(16, '\x11'), &err));
    QVERIFY2(err.contains(QStringLiteral("boot_to")), qPrintable(err));
    // 文案带码值，且**大小写与 session 层一致**（hexCode 的大写零填充；按字面断言，
    // 换回小写 arg(...,16,...) 会当场红 —— 用户按码值 grep 时全仓只有一种拼法）
    QVERIFY2(err.contains(QStringLiteral("0x0000DEAD")), qPrintable(err));
    QCOMPARE(m.reads.size(), 0);                                           // 终判那帧也读掉了，不留给下一次读
}

// SHUTDOWN：32B 参数（hasflags 依 async_mode/dl_bit/bootmode 推导）
void TestMtkXflashPayload::shutdownParameterLayout()
{
    {
        MockUsbChannel m;
        m.reads << statusReads(0) << statusReads(0);        // SHUTDOWN 的两处 status
        mtkbrom::XFlashSession x(&m, 0x6765);
        QString err;
        QVERIFY2(mtkbrom::xflashShutdown(x, /*bootmode=*/0, &err), qPrintable(err));
        QCOMPARE(m.writeFrames.size(), 4);                  // 命令帧（头+值）+ 参数帧（头+32B）
        QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));
        QCOMPARE(m.writeFrames.at(1), le32(0x010007));      // SHUTDOWN
        QCOMPARE(m.writeFrames.at(2), le32(0xFEEEEEEF) + le32(1) + le32(32));
        const QByteArray p = m.writeFrames.at(3);
        QCOMPARE(p.size(), 32);
        QCOMPARE(p.mid(0, 4), le32(0));                     // hasflags = 0（NORMAL、未异步）
        QCOMPARE(p.mid(4, 4), le32(0));                     // enablewdt = 0（禁用看门狗）
        QCOMPARE(p.mid(8, 4), le32(0));                     // async_mode
        QCOMPARE(p.mid(12, 4), le32(0));                    // bootmode = NORMAL
        QCOMPARE(p.mid(16), le32(0) + le32(0) + le32(0) + le32(0));   // dl_bit/dont_resetrtc/leaveusb/保留
        QCOMPARE(m.reads.size(), 0);                        // 恰读 2 帧 status
    }
    {
        // bootmode = FASTBOOT(2)：hasflags 必须推导为 1（XFL:817-825），bootmode 字段原样带入。
        // 少了这个子用例，"hasflags 恒 0"的实现在上面也能过。
        MockUsbChannel m;
        m.reads << statusReads(0) << statusReads(0);
        mtkbrom::XFlashSession x(&m, 0x6765);
        QString err;
        QVERIFY2(mtkbrom::xflashShutdown(x, /*bootmode=*/2, &err), qPrintable(err));
        QCOMPARE(m.writeFrames.size(), 4);                  // 同上：先钉笔数再取下标（空队列取下标会 Q_ASSERT 崩）
        const QByteArray p = m.writeFrames.at(3);
        QCOMPARE(p.size(), 32);
        QCOMPARE(p.mid(0, 4), le32(1));                     // hasflags = 1（bootmode != NORMAL）
        QCOMPARE(p.mid(12, 4), le32(2));                    // bootmode = FASTBOOT
        QCOMPARE(m.reads.size(), 0);
    }
}

// SHUTDOWN 的**尾部** status 非 0 → 失败：命令帧与 32B 参数帧照发（失败点在第二处 status），
// 文案既要带 session 层的码值、也要点名是哪条命令（其它两条函数都点名）
void TestMtkXflashPayload::shutdownRejectsNonZeroTrailingStatus()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0xDEADBEEF);   // 命令后 0；参数后非 0
    mtkbrom::XFlashSession x(&m, 0x6765);
    QString err;
    QVERIFY(!mtkbrom::xflashShutdown(x, /*bootmode=*/0, &err));
    // 整串按字面断言：单前缀（`XFlash：` 只出现一次）+ 点名 SHUTDOWN + session 措辞与码值原样
    // —— 双前缀、漏点名、码值大小写不符都当场红
    QCOMPARE(err, QStringLiteral("XFlash：SHUTDOWN 收尾失败（设备返回错误码 0xDEADBEEF）"));
    QCOMPARE(m.writeFrames.size(), 4);      // 命令帧与参数帧都已发出（失败不早于第二处 status）
    QCOMPARE(m.reads.size(), 0);            // 两帧 status 都读满，不留给下一次读
}

// 空载荷**一个字节都不发**就拒绝（同 D1/F5"空 DA 早拒"的纪律）：漏过校验会把空帧发到设备上
void TestMtkXflashPayload::emptyPayloadsRejectedBeforeAnyWrite()
{
    {
        MockUsbChannel m;
        mtkbrom::XFlashSession x(&m, 0x6765);
        QString err;
        QVERIFY(!mtkbrom::xflashSendEmi(x, QByteArray(), &err));
        QVERIFY(!err.isEmpty());
        QCOMPARE(m.writeFrames.size(), 0);                  // 早拒：一帧未发
    }
    {
        MockUsbChannel m;
        mtkbrom::XFlashSession x(&m, 0x6765);
        QString err;
        QVERIFY(!mtkbrom::xflashBootTo(x, 0x40000000ull, QByteArray(), &err));
        QVERIFY2(err.contains(QStringLiteral("签名")), qPrintable(err));   // 文案点出"已剥签名"是调用方义务
        QCOMPARE(m.writeFrames.size(), 0);
    }
}
// ---- T6：写/读数据路径 ----

// 56B 存储参数布局：pack("<IIQQ", …) = 24B + NandExtension 8×u32（全 0）= 32B → 共 **56**。
// 上游 NandExtension 类有 9 个属性，pack 里跳过 operation_type（XFL:677-680）——
// 少 8 字节设备会把后 8 字节读成垃圾（铁律 15）。
void TestMtkXflashPayload::storageParamLayout()
{
    const QByteArray p = mtkbrom::xflashStorageParam(0x1, 0x8, 0x100000, 0x2000);
    QCOMPARE(p.size(), 56);
    QCOMPARE(p.mid(0, 4), le32(0x1));                 // storage = EMMC
    QCOMPARE(p.mid(4, 4), le32(0x8));                 // partType = USER
    QCOMPARE(p.mid(8, 8), le32(0x100000) + le32(0));  // addr（<Q 小端）
    QCOMPARE(p.mid(16, 8), le32(0x2000) + le32(0));   // length
    QCOMPARE(p.mid(24), QByteArray(32, '\0'));        // NandExtension 8×u32 = 32B
    // 大地址/大长度要落到高 32 位（写成 <II 的实现会在这里红）
    const QByteArray q = mtkbrom::xflashStorageParam(0x4, 0x1, 0x1234567890abcdefull, 0x100000000ull);
    QCOMPARE(q.size(), 56);
    QCOMPARE(q.mid(8, 8), le32(0x90abcdef) + le32(0x12345678));
    QCOMPARE(q.mid(16, 8), le32(0) + le32(1));
}

// 写（XFL:670-685 + :835-901）：命令 status → 56B 参数（长度取 **512 对齐后**的总长，XFL:847-849 → :861）
// → 每块 [<I 0>、<I checksum>、data（补零到 512 整数倍）] 各一次 status → 收尾 status → CC_OPTIONAL_DOWNLOAD_ACT。
// 数据 0x700 / packet 0x400 → 两块：0x400（不用补）+ 0x300（补到 0x400）。
// 字节值 0xFF 让校验和必须**按 16 位回绕**：0x400×0xFF = 0x3FC00 → 0xFC00（漏 & 0xFFFF 会红）。
void TestMtkXflashPayload::writeDataChunkingAndChecksum()
{
    MockUsbChannel m;
    // 读帧：命令 status、参数 status、两块各一次、**收尾 status**（XFL:883）、CC 的 devctrl 二连 + 回包
    m.reads << statusReads(0) << statusReads(0)
            << statusReads(0) << statusReads(0)
            << statusReads(0)
            << statusReads(0) << statusReads(0) << frameReads(1, le32(0));
    mtkbrom::XFlashSession x(&m, 0x6765);
    const QByteArray data(0x700, '\xFF');
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::xflashWriteData(x, 0x200000, data, 0x1, 0x8, /*writePacketLength=*/0x400, &log, &err),
             qPrintable(err));
    QCOMPARE(m.reads.size(), 0);                                  // 七帧 status + 一帧回包，一帧不多不少

    // 写调用**逐笔**核对（先钉笔数再取下标）：只对拼接字节流断言抓不到"整段写一遍再分块写一遍"的双写。
    // 2（命令）+ 2（参数）+ 7（第一块：三段参数的头/值各一笔，数据 0x400 再分两个 0x200）
    // + 7（第二块：数据结构相同）+ 4（CC devctrl 的命令与子命令各两笔）= 22
    QCOMPARE(m.writeFrames.size(), 22);
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));   // WRITE_DATA 命令帧头
    QCOMPARE(m.writeFrames.at(1), le32(0x010004));
    QCOMPARE(m.writeFrames.at(2), le32(0xFEEEEEEF) + le32(1) + le32(56));  // 参数帧头 = **56**
    const QByteArray p = m.writeFrames.at(3);
    QCOMPARE(p.size(), 56);
    QCOMPARE(p.mid(0, 4), le32(0x1));
    QCOMPARE(p.mid(4, 4), le32(0x8));
    QCOMPARE(p.mid(8, 8), le32(0x200000) + le32(0));
    QCOMPARE(p.mid(16, 8), le32(0x800) + le32(0));                // 长度 = 0x700 **补到 512 的整数倍**（XFL:847-849）
    QCOMPARE(p.mid(24), QByteArray(32, '\0'));

    // 第一块（0x400，无需补零）：三段参数
    QCOMPARE(m.writeFrames.at(4), le32(0xFEEEEEEF) + le32(1) + le32(4));
    QCOMPARE(m.writeFrames.at(5), le32(0));                       // 段1 值 0（XFL:878）
    QCOMPARE(m.writeFrames.at(6), le32(0xFEEEEEEF) + le32(1) + le32(4));
    QCOMPARE(m.writeFrames.at(7), le32(0xFC00));                  // sum(0x400×0xFF) & 0xFFFF
    QCOMPARE(m.writeFrames.at(8), le32(0xFEEEEEEF) + le32(1) + le32(0x400));
    // 数据按 0x200 分块（铁律 5）：**每一笔单独断言**（只拼起来比对会放过 0x300+0x100 这种拆分）
    QCOMPARE(m.writeFrames.at(9), data.mid(0, 0x200));
    QCOMPARE(m.writeFrames.at(10), data.mid(0x200, 0x200));

    // 第二块（0x300 → 补零到 0x400）：补的是 0，且**补零部分不进校验和**
    QCOMPARE(m.writeFrames.at(11), le32(0xFEEEEEEF) + le32(1) + le32(4));
    QCOMPARE(m.writeFrames.at(12), le32(0));
    QCOMPARE(m.writeFrames.at(13), le32(0xFEEEEEEF) + le32(1) + le32(4));
    QCOMPARE(m.writeFrames.at(14), le32(0xFD00));                 // sum(0x300×0xFF) & 0xFFFF
    QCOMPARE(m.writeFrames.at(15), le32(0xFEEEEEEF) + le32(1) + le32(0x400));
    QByteArray tail(0x400, '\0');
    tail.replace(0, 0x300, data.mid(0x400));                      // 实数据 0x300 + **补零 0x100**
    QCOMPARE(m.writeFrames.at(16), tail.left(0x200));             // 同样按 0x200 分块，逐笔断言
    QCOMPARE(m.writeFrames.at(17), tail.mid(0x200));
    QCOMPARE(tail.mid(0x300), QByteArray(0x100, '\0'));           // 补的是 0

    // 收尾：CC_OPTIONAL_DOWNLOAD_ACT 是**无参** devctrl（DEVICE_CTRL → 子命令 → 读回包，不读尾部 status）
    QCOMPARE(m.writeFrames.at(18), le32(0xFEEEEEEF) + le32(1) + le32(4));
    QCOMPARE(m.writeFrames.at(19), le32(0x010009));               // DEVICE_CTRL
    QCOMPARE(m.writeFrames.at(20), le32(0xFEEEEEEF) + le32(1) + le32(4));
    // 上游这个常量是 **0x800005**（同段邻居都是 0x0800xx，是上游自己的写法，XFP:68）—— 照发：
    // 被"顺手修正"成邻居的 0x0800xx 拼法（少一个 0 的那位补回去）就会在这里红
    QCOMPARE(m.writeFrames.at(21), le32(0x800005));
    QVERIFY2(logHas(log, QStringLiteral("XFlash 写"), QStringLiteral("0x200000")),
             qPrintable(log.join(QLatin1Char('|'))));
}

// 铁律 14：write_packet_length 拿不到（GET_PACKET_LENGTH 失败/为 0）**无回退** —— 一帧都不发就拒绝。
// 若哪天有人"顺手"给个 0x1000 的默认值，这条会红。
void TestMtkXflashPayload::writeDataRejectsZeroPacketLength()
{
    MockUsbChannel m;
    mtkbrom::XFlashSession x(&m, 0x6765);
    QString err;
    QVERIFY(!mtkbrom::xflashWriteData(x, 0x200000, QByteArray(0x200, '\x11'), 0x1, 0x8, /*writePacketLength=*/0, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("write_packet_length")), qPrintable(err));
    QCOMPARE(m.writeFrames.size(), 0);        // 早拒：命令帧都没发
    QCOMPARE(m.reads.size(), 0);              // 早拒：一帧未读
}

// 循环**之后**的收尾 status 是判据（XFL:883-893，上游在此判错并打印）：非 0 → 失败，
// 且**不再发** CC_OPTIONAL_DOWNLOAD_ACT（上游把它放在 "status == 0" 分支里）。
// 毒药帧没被读走 = 失败点确实在收尾 status，不在更早处。
void TestMtkXflashPayload::writeDataRejectsNonZeroTrailingStatus()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0) << statusReads(0)   // 命令 / 参数 / 唯一一块
            << statusReads(0xDEADBEEF)                              // 收尾 status 非 0
            << statusReads(0xDEAD);                                 // 毒药
    mtkbrom::XFlashSession x(&m, 0x6765);
    QString err;
    QVERIFY(!mtkbrom::xflashWriteData(x, 0x200000, QByteArray(0x200, '\x11'), 0x1, 0x8, 0x400, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("0xDEADBEEF")), qPrintable(err));   // 带 session 层的码值
    QVERIFY2(err.contains(QStringLiteral("XFlash 写")), qPrintable(err));    // 且点名是本操作
    QCOMPARE(m.writeFrames.size(), 10);       // 命令(2) + 参数(2) + 一块的三段(6，0x200 不补零)：失败不早于收尾 status
    int devCtrlFrames = 0;
    for (const QByteArray &f : std::as_const(m.writeFrames)) {
        if (f == le32(0x010009))
            ++devCtrlFrames;
    }
    QCOMPARE(devCtrlFrames, 0);               // 写失败 → 不发 CC_OPTIONAL_DOWNLOAD_ACT
    QCOMPARE(m.reads.size(), 2);              // 毒药帧（头+载荷）没被读走
}

// CC_OPTIONAL_DOWNLOAD_ACT 失败**不判失败**（上游 XFL:885 不检查返回值；数据此时已写入），
// 但也不能静默 —— 失败要落进日志且带码值。毒药帧没被读走 = 失败点确实在 CC 的 devctrl。
void TestMtkXflashPayload::writeDataWarnsWhenOptionalDownloadActFails()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0) << statusReads(0) << statusReads(0)   // 命令/参数/块/收尾
            << statusReads(0xDEAD)                                                   // CC 的 DEVICE_CTRL status 非 0
            << statusReads(0xDEAD);                                                  // 毒药
    mtkbrom::XFlashSession x(&m, 0x6765);
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::xflashWriteData(x, 0x200000, QByteArray(0x200, '\x11'), 0x1, 0x8, 0x400, &log, &err),
             qPrintable(err));                                  // **仍然成功**：数据已写入
    QVERIFY2(logHas(log, QStringLiteral("CC_OPTIONAL_DOWNLOAD_ACT"), QStringLiteral("0x0000DEAD")),
             qPrintable(log.join(QLatin1Char('|'))));           // 不静默，且带码值
    QCOMPARE(m.writeFrames.size(), 12);                         // 命令(2)+参数(2)+块(6)+CC 命令(2)：CC 确实发了
    QCOMPARE(m.writeFrames.at(10), le32(0xFEEEEEEF) + le32(1) + le32(4));
    QCOMPARE(m.writeFrames.at(11), le32(0x010009));             // DEVICE_CTRL
    QCOMPARE(m.reads.size(), 2);                                // 毒药帧（头+载荷）没被读走
}

// 判别器：参数里的 length 必须是 **512 对齐**（上游 XFL:847-849 的规则），不是"补齐到 packet 的整数倍"。
// 取 data 0x600 / packet 0x400：ceil512(0x600) = **0x600**，而 ceil-to-packet = **0x800** —— 两种规则在这一组
// 输入上结果不同（0x700/0x400 那组两者都是 0x800，判别不了）。分块也一并钉住：0x400 + 0x200 两块、不加块。
void TestMtkXflashPayload::writeDataAnnounces512AlignedLength()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0) << statusReads(0) << statusReads(0)   // 命令/参数/两块
            << statusReads(0)                                                         // 收尾
            << statusReads(0) << statusReads(0) << frameReads(1, le32(0));            // CC 二连 + 回包
    mtkbrom::XFlashSession x(&m, 0x6765);
    const QByteArray data(0x600, '\x11');
    QString err;
    QVERIFY2(mtkbrom::xflashWriteData(x, 0x100000, data, 0x1, 0x8, /*writePacketLength=*/0x400, nullptr, &err),
             qPrintable(err));
    QCOMPARE(m.reads.size(), 0);
    QCOMPARE(m.writeFrames.size(), 21);                      // 2 + 2 + 7（0x400 一块）+ 6（0x200 一块）+ 4（CC）
    QCOMPARE(m.writeFrames.at(2), le32(0xFEEEEEEF) + le32(1) + le32(56));
    QCOMPARE(m.writeFrames.at(3).mid(16, 8), le32(0x600) + le32(0));   // **0x600**（ceil-to-packet 会给 0x800）
    // 第一块 0x400（无需补零；字节 0x11 → 校验和 0x400×0x11）
    QCOMPARE(m.writeFrames.at(7), le32(0x4400));
    QCOMPARE(m.writeFrames.at(8), le32(0xFEEEEEEF) + le32(1) + le32(0x400));
    QCOMPARE(m.writeFrames.at(9), data.mid(0, 0x200));
    QCOMPARE(m.writeFrames.at(10), data.mid(0x200, 0x200));
    // 第二块 0x200（正好对齐，不补零）
    QCOMPARE(m.writeFrames.at(14), le32(0x2200));
    QCOMPARE(m.writeFrames.at(15), le32(0xFEEEEEEF) + le32(1) + le32(0x200));
    QCOMPARE(m.writeFrames.at(16), data.mid(0x400));         // 0x200 字节，单笔写完
    QCOMPARE(m.writeFrames.at(20), le32(0x800005));          // CC 仍照发
}

// packet 长不是 512 的整数倍 → **拒绝写入**（控制器裁决，比上游更严）：此时循环会"切原始数据再补零"，
// 补的零落在实时数据之间，实发字节也不再等于参数里承诺的长度 —— 是"静默写坏镜像"，不是"少写几个字节"。
// 上游默认档与常见 DA 报值（0x200/0x400/0x1000）都对齐，所以这条分支实际不可达；**不可达且静默破坏** → fail-closed
// （同 GPT CRC、未知分区表、未知代际的处置）。早拒在**任何写之前**。
void TestMtkXflashPayload::writeDataRejectsUnalignedPacketLength()
{
    MockUsbChannel m;
    mtkbrom::XFlashSession x(&m, 0x6765);
    QString err;
    QVERIFY(!mtkbrom::xflashWriteData(x, 0x200000, QByteArray(0x600, '\x11'), 0x1, 0x8, /*writePacketLength=*/0x300, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("0x300")), qPrintable(err));      // 点名拿到的值
    QVERIFY2(err.contains(QStringLiteral("512")), qPrintable(err));        // 点名要求
    QCOMPARE(m.writeFrames.size(), 0);      // 早拒：命令帧都没发（不是"发到一半才停"）
    QCOMPARE(m.reads.size(), 0);            // 早拒：一帧未读
}

// 空数据（0 字节）**不特殊对待**：照上游的时序走完（参数长度 0、无块、收尾 status、CC）。
// 这是"钉住现状"的用例，不是"这是好行为"的用例 —— 要改行为得先改这条。
void TestMtkXflashPayload::writeDataZeroLengthSendsZeroLengthParam()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0) << statusReads(0)                     // 命令/参数/收尾
            << statusReads(0) << statusReads(0) << frameReads(1, le32(0));            // CC 二连 + 回包
    mtkbrom::XFlashSession x(&m, 0x6765);
    QString err;
    QVERIFY2(mtkbrom::xflashWriteData(x, 0x200000, QByteArray(), 0x1, 0x8, 0x400, nullptr, &err), qPrintable(err));
    QCOMPARE(m.reads.size(), 0);
    QCOMPARE(m.writeFrames.size(), 8);                    // 命令 2 + 参数 2 + CC 4：**一块都没有**
    const QByteArray p = m.writeFrames.at(3);
    QCOMPARE(p.size(), 56);
    QCOMPARE(p.mid(16, 8), le32(0) + le32(0));            // 参数长度 = 0
}

// 读（XFL:687-704 + :706-806）：命令 status → 56B 参数 status → **参数后的第二个 status**（XFL:698-702）
// → 数据帧（收下 + ack(rstatus=False)）… 中途 flag==0 帧只是继续（XFL:730-768 的 while 以字节数计）
// → 收尾帧（XFL:770-776）。数据 0x100 + 0x80 = 0x180 = 请求长度。
void TestMtkXflashPayload::readDataCollectsFramesAndAcks()
{
    MockUsbChannel m;
    const QByteArray blk1(0x100, '\xAB'), blk2(0x80, '\xCD');
    m.reads << statusReads(0) << statusReads(0) << statusReads(0)   // 命令 status + 参数 status + **第二个 status**
            << frameReads(1, blk1)                                   // 数据帧 1
            << frameReads(1, le32(0))                                // 中途 flag（值 0 = 继续，不是结束）
            << frameReads(1, blk2)                                   // 数据帧 2（凑满 0x180）
            << frameReads(1, le32(0));                               // 收尾 flag（值 0）
    mtkbrom::XFlashSession x(&m, 0x6765);
    QByteArray got;
    QString err;
    QVERIFY2(mtkbrom::xflashReadData(x, 0, 0x180, 0x1, 0x8, got, &err), qPrintable(err));
    QCOMPARE(got, blk1 + blk2);                                  // 只收数据帧，flag 帧不进缓冲
    QCOMPARE(m.reads.size(), 0);                                 // 七帧读满：多读/少读一帧都会红

    // 写调用逐笔核对：命令帧(2) + 参数帧(2) + **只有数据帧才 ack**（两个数据帧 → 4 笔）。
    // ack 在 0x6785 上是"两次写"（铁律 4）；若实现给 flag 帧也 ack，笔数与 acks 都会红。
    QCOMPARE(m.writeFrames.size(), 8);
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));
    QCOMPARE(m.writeFrames.at(1), le32(0x010005));               // READ_DATA
    QCOMPARE(m.writeFrames.at(2), le32(0xFEEEEEEF) + le32(1) + le32(56));
    const QByteArray p = m.writeFrames.at(3);
    QCOMPARE(p.size(), 56);
    QCOMPARE(p.mid(0, 4), le32(0x1));                            // storage = EMMC
    QCOMPARE(p.mid(4, 4), le32(0x8));                            // partType = USER
    QCOMPARE(p.mid(8, 8), le32(0) + le32(0));                    // addr = 0
    QCOMPARE(p.mid(16, 8), le32(0x180) + le32(0));               // 读长度**不做 512 对齐**（写路径才对齐）
    QCOMPARE(p.mid(24), QByteArray(32, '\0'));
    QCOMPARE(m.writeFrames.at(4), le32(0xFEEEEEEF) + le32(1) + le32(4));   // ack1 帧头
    QCOMPARE(m.writeFrames.at(5), le32(0));                      // ack 载荷 0
    QCOMPARE(m.writeFrames.at(6), le32(0xFEEEEEEF) + le32(1) + le32(4));   // ack2 帧头
    QCOMPARE(m.writeFrames.at(7), le32(0));
}

// 读到 flag != 0（设备报错，XFL:761-765 / :774-776）→ 明确失败，文案带码值。
void TestMtkXflashPayload::readDataRejectsNonZeroFlag()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0) << statusReads(0)
            << frameReads(1, le32(0x1234));                       // flag 非 0 = 读失败
    mtkbrom::XFlashSession x(&m, 0x6765);
    QByteArray got;
    QString err;
    QVERIFY(!mtkbrom::xflashReadData(x, 0, 0x100, 0x1, 0x8, got, &err));
    QVERIFY2(err.contains(QStringLiteral("0x00001234")), qPrintable(err));   // 码值大写零填充（全仓一种拼法）
    int acks = 0;
    for (const QByteArray &f : std::as_const(m.writeFrames)) {
        if (f == le32(0))
            ++acks;
    }
    QCOMPARE(acks, 0);                        // flag 帧不回 ack
}

// 收帧长度既不是数据帧（> 4）也不是 flag 帧（== 4）→ 协议错误，明确失败（上游 XFL:766-768 只打印后 break）。
// 用 2 字节载荷构造（**不能**用空载荷：mock 对空笔返回 false，那是"读失败"而不是"长度未知"）。
void TestMtkXflashPayload::readDataRejectsUnknownFrameLength()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0) << statusReads(0)
            << frameReads(1, QByteArray(2, '\x01'));          // 2 字节：两头都不沾
    mtkbrom::XFlashSession x(&m, 0x6765);
    QByteArray got;
    QString err;
    QVERIFY(!mtkbrom::xflashReadData(x, 0, 0x100, 0x1, 0x8, got, &err));
    QVERIFY2(err.contains(QStringLiteral("未知长度")), qPrintable(err));   // 失败原因是"长度"而不是别的
    QVERIFY2(err.contains(QStringLiteral("2")), qPrintable(err));         // 带上实际长度
    QCOMPARE(m.writeFrames.size(), 4);   // 只有命令与参数两帧（4 笔），没有 ack
    QCOMPARE(m.reads.size(), 0);         // 出错帧已读掉，不留给下一次读
}

// 循环内**靠后**的 flag 帧非 0（前面已经收过一帧数据）→ 失败，但**之前那帧数据必须先被收下并 ack**。
// 这条钉住"收数据 → ack → 继续 → 遇 flag 判错"的顺序：把 flag 判据挪到收数据之前、
// 或只在收尾帧上判 flag 的实现都会在这里红。
void TestMtkXflashPayload::readDataRejectsNonZeroFlagAfterDataFrame()
{
    MockUsbChannel m;
    const QByteArray blk(0x40, '\x5A');
    m.reads << statusReads(0) << statusReads(0) << statusReads(0)
            << frameReads(1, blk)                             // 数据帧（不足 0x100）
            << frameReads(1, le32(0x1234));                   // 循环内靠后的 flag 非 0
    mtkbrom::XFlashSession x(&m, 0x6765);
    QByteArray got;
    QString err;
    QVERIFY(!mtkbrom::xflashReadData(x, 0, 0x100, 0x1, 0x8, got, &err));
    QVERIFY2(err.contains(QStringLiteral("0x00001234")), qPrintable(err));
    QCOMPARE(got, blk);                   // 报错前那帧数据已收下（数据帧先于 flag 帧被处理）
    QCOMPARE(m.writeFrames.size(), 6);    // 命令 2 + 参数 2 + **该数据帧的一次 ack（2 笔）**
    QCOMPARE(m.writeFrames.at(4), le32(0xFEEEEEEF) + le32(1) + le32(4));
    QCOMPARE(m.writeFrames.at(5), le32(0));
    QCOMPARE(m.reads.size(), 0);
}

// **收尾帧** flag 非 0（数据已按 length 读满）→ 失败。与上一条的区别在**位置**：此处循环已因字节数收尾，
// 失败只能来自收尾帧那一读 —— 忽略收尾帧的实现会返回 true，在这里红（XFL:770-776）。
// 无进展守卫（终审 §5）：设备持续刷 flag 帧 → 必须**按文案中止**，不挂死（上游此路径会永久挂死）
void TestMtkXflashPayload::readDataAbortsOnFlagFrameFlood()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0);                    // 命令 status + 56B 参数后的 status
    for (int i = 0; i < 5000; ++i)                                  // 连续 5000 个 flag(0) 帧（上限 4096，留余量）
        m.reads << frameReads(1, le32(0));
    mtkbrom::XFlashSession x(&m, 0x6765);
    QByteArray got;
    QString err;
    QVERIFY(!mtkbrom::xflashReadData(x, 0, 0x200, 0x1, 0x8, got, &err));
    // **必须是守卫拦下的**（不是"队列读空"）—— 文案是唯一判别点
    QVERIFY2(err.contains(QStringLiteral("连续收到超过")), qPrintable(err));
}

void TestMtkXflashPayload::readDataRejectsNonZeroFinalFlag()
{
    MockUsbChannel m;
    const QByteArray blk(0x100, '\x5A');
    m.reads << statusReads(0) << statusReads(0) << statusReads(0)
            << frameReads(1, blk)                             // 数据帧恰好凑满 0x100
            << frameReads(1, le32(0xDEAD));                   // 收尾帧 flag 非 0
    mtkbrom::XFlashSession x(&m, 0x6765);
    QByteArray got;
    QString err;
    QVERIFY(!mtkbrom::xflashReadData(x, 0, 0x100, 0x1, 0x8, got, &err));
    QVERIFY2(err.contains(QStringLiteral("0x0000DEAD")), qPrintable(err));
    QCOMPARE(got, blk);                   // 数据本身收全了，失败来自收尾帧
    QCOMPARE(m.writeFrames.size(), 6);    // 命令 2 + 参数 2 + ack 2
    QCOMPARE(m.reads.size(), 0);          // 收尾帧必须被读掉，不能留在设备侧
}

// length == 0 的读**照上游形状走**：三帧 status 之后循环一帧不读，只读一帧收尾（XFL:770-776
// 在 bytestoread == 0 时同样会读）—— 钉住现状，不是"这是好行为"。
void TestMtkXflashPayload::readDataZeroLengthReadsFinalFrameOnly()
{
    MockUsbChannel m;
    m.reads << statusReads(0) << statusReads(0) << statusReads(0)
            << frameReads(1, le32(0));                        // 唯一的收尾帧
    mtkbrom::XFlashSession x(&m, 0x6765);
    QByteArray got;
    QString err;
    QVERIFY2(mtkbrom::xflashReadData(x, 0, 0, 0x1, 0x8, got, &err), qPrintable(err));
    QVERIFY(got.isEmpty());
    QCOMPARE(m.writeFrames.size(), 4);    // 命令 2 + 参数 2（0 字节没有 ack）
    QCOMPARE(m.reads.size(), 0);
}

QTEST_APPLESS_MAIN(TestMtkXflashPayload)
#include "test_mtk_xflash_payload.moc"
