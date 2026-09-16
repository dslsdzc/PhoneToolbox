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
        QCOMPARE(c, mtkbrom::PartitionCata::Unknown);            // 其它值 → Unknown（调用方按"两者都试"处理）
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
QTEST_APPLESS_MAIN(TestMtkXflashPayload)
#include "test_mtk_xflash_payload.moc"
