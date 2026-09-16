// tests/test_mtk_xml_payload.cpp
//
// MTK XML（D3）载荷 ①（Phase D2+D3 Task 9）：DA1 后的 CMD:START 握手 + setup_env /
// setup_hw_init / set-host-info。对照 mtkclient v2.1.4-20-g71b0175（GPL-3.0，**只读参照，
// 代码文本不进仓库**）：XL = mtkclient/Library/DA/xmlflash/xml_lib.py；
// XC = .../xmlflash/xml_cmd.py；事实底稿 .superpowers/sdd/mtk-d2d3-facts-report.md §5.3/§5.4。
//
// 夹具约定（同 T8，**先读再改**）：默认 `IBromUsb::readExact` = 单次读 + 严格长度
// （`mtk_brom.cpp:219-231`），故**一帧 = 两笔**队列项（12B 帧头、载荷）；整帧塞成一笔会让
// "读头"截断载荷、其后全部错位；空队列项也会被 mock 判成 false。
//
// **计划缺陷订正 #1（T9 执行期实测，简令 Step 1 的读队列不可用）**：简令按"每条命令一帧 OK"
// 排布（握手 1 帧 + 4 条命令 × 1 帧 = 5 帧），但 `sendCommand(noack=false)`（**上游默认**，
// XL:188-219）一次调用消费**三帧**：
//     "OK"  →  `CMD:END(result=OK)`（XL:195-198）→ ack（XL:199）→ 再读一条判 `CMD:START`（XL:203-204）
// 按 5 帧排会在第二条命令处读空 → 用例假红。此处按 **3 帧/命令** 排（见 commandReads()），
// 断言内容与原简令一致。
//
// **计划缺陷订正 #2**：简令 Step 1 的第 4 个用例签名写成 `TestMtkXmlSession::logFrameIsSkippedNotFatal`
// —— 是本任务文件里不存在的类（T8 的类名），且其正文与 T8 的 `getResponseSkipsDaLogFrames`
// 逐条重复。此处改为 T9 自己的形态：DA 日志帧夹在**握手途中的命令响应流里**，握手仍须走完且
// 日志进 sink（T8 只覆盖 getResponse 一层）。
//
// ── T10（本文件下半部分）的计划缺陷订正（每一处都以上游为准） ─────────────────────────
//
// **订正 #3（写路径夹具的帧序）**：简令 Step 1 的写用例在 `CMD:END` 与 `CMD:START` 之间多排了
// 一帧 `"OK"`（注释称"CMD:END 后的 ack 响应"）。上游 `upload()` 的收尾是（XL:487-496）：
//     if raw: ack()  →  get_command_result()【读 CMD:END，**不** ack】→ ack() → get_command_result()
//     【读 CMD:START，内部 ack XL:406】→ True
// —— `ack()` 是**写**，其"响应"就是下一条命令，中间没有独立的 OK 帧。同形状的收尾见
// `check_lifecycle`（XL:987-1003：`download()` 的尾 ack XL:583 之后直接读 CMD:END）。多排一帧
// 会让 CMD:START 的读取落到 `"OK"` 上 → 用例假红。
//
// **订正 #4（写用例第 2 条断言不可用）**：简令的 `QVERIFY(cmds.at(1).startsWith("OK@0x600"))` ——
// `cmds` 只收以 `<?xml` 开头的写帧，而写路径上**只有 WRITE-FLASH 一条 XML 命令**，故 `cmds.at(1)`
// 越界。此处改为对**每一笔 write()** 取形态签名（writeSignature()），逐笔计数 + 逐笔大小：
// ⚠️ 本助手**按前缀分类**：若将来某条用例的数据块恰好是 12 字节、或以 "OK"/"<?xml" 开头，会被误判成 ack/命令帧
//    （当前载荷用 `i & 0xFF` 递增，不会触发）。新增用例时请避开这三种载荷形状，或改用显式类型标注。
// 拼接字节流断言分不清"某块载荷写了两遍"和"只写了一遍"。
//
// **订正 #5（WRITE-FLASH 少一项 `<offset>`）**：上游 `cmd_write_flash`（XC:452-462）的 `<arg>` 是
// partition / **offset** / source_file 三项（`<offset>{hex(offset)}</offset>`，writeflash 传
// `offset=addr`，整分区写时为 0）——简令的信封只有 partition + source_file。以**上游为准**，
// 本用例按三项逐字节钉死（offset 恒 0x0；本任务接口无 offset 参数，见 .cpp 注释）。
//
// **订正 #6（Step 3 的 `xmlReadPartition` 与自家 Interfaces/Step 1 自相矛盾）**：简令 Step 3 的
// `xmlReadPartition` 正文走 `readCommandResult` 的裸 `OK@` 分支（T8 = `get_command_result` XL:373-388
// 的**单次尾 ack** 节奏），既没调用它在 Interfaces 里声明的 `xmlReadDataFrames`，也读不出 Step 1
// 夹具的逐帧节奏。上游读路径是 `readflash`（XL:918-941）→ `download_raw`（XL:508-559）的
// **逐帧 ack**：收一帧 → ack → 读 "OK" → ack。以**上游为准**，本任务实现 `xmlReadDataFrames`
// 并让 `xmlReadPartition` 调它。

#include <QtTest>
#include <QByteArray>
#include <QRegularExpression>
#include <QStringList>
#include <utility>      // std::as_const

#include "core/modes/mtk_brom.h"
#include "core/modes/mtk_xml_payload.h"
#include "core/modes/mtk_xml_session.h"

class MockUsbChannel : public mtkbrom::IBromUsb   // 与 T8 同款
{
public:
    QByteArray writes;              // 全部写入字节（拼接）
    QList<QByteArray> writeFrames;  // 逐笔（每次 write() 一笔）—— 帧级断言用
    QList<QByteArray> reads;        // 按序弹出的读取响应；空队列 → read 返回 false
    int pktSize = 0x400;            // XML 层不使用（无分块写），保留接口语义

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

// 文本/日志帧载荷 = utf8 字节 + **NUL**（XL:146-153：str 载荷 length = len+1）
QByteArray textBody(const QString &s) { return s.toUtf8() + QByteArray(1, '\0'); }

// 文本帧（DT_PROTOCOL_FLOW）读队列：**两笔**（12B 头 declare length = 可见字节 + 1，载荷）
QList<QByteArray> textReads(const QString &s)
{
    const QByteArray body = textBody(s);
    return {le32(0xFEEEEEEF) + le32(1) + le32(quint32(body.size())), body};
}

// DA 日志帧（DT_MESSAGE）：**三笔** —— 12B 头（宣布长度 = 载荷 + 4）、priority:u32、载荷（XL:124-127）
QList<QByteArray> logReads(const QString &s, quint32 priority = 7)
{
    const QByteArray body = textBody(s);
    return {le32(0xFEEEEEEF) + le32(2) + le32(quint32(body.size()) + 4), le32(priority), body};
}

// `sendCommand(noack=false)` **一次调用**消费的三帧（XL:188-219）：见文件头"订正 #1"
QList<QByteArray> commandReads(const QString &result = QStringLiteral("OK"))
{
    return textReads(QStringLiteral("OK"))
         + textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>%1</result></arg></host>")
                         .arg(result))
         + textReads(QStringLiteral("<host><command>CMD:START</command></host>"));
}

// 设备发来的 CMD:START 文本消息（DA1 起来的同步信号，**不是** XFlash 的 0xC0；XL:309-310）
QList<QByteArray> deviceStartReads()
{
    return textReads(QStringLiteral("<host><command>CMD:START</command></host>"));
}

// 写帧里的 XML 命令载荷（跳过 ack 的 `OK\0\0` 等非 XML 写入）
QStringList sentXml(const MockUsbChannel &m)
{
    QStringList out;
    for (const QByteArray &f : std::as_const(m.writeFrames))
        if (f.startsWith("<?xml"))
            out << QString::fromUtf8(f).remove(QChar('\0'));
    return out;
}

// ── T10 追加的夹具 ──────────────────────────────────────────────────────────

quint32 le32At(const QByteArray &b, int off)
{
    return quint32(quint8(b.at(off))) | (quint32(quint8(b.at(off + 1))) << 8)
         | (quint32(quint8(b.at(off + 2))) << 16) | (quint32(quint8(b.at(off + 3))) << 24);
}

// 数据帧（DT_PROTOCOL_FLOW，**字节**载荷）：两笔（12B 头宣布 = 字节数，载荷；铁律 17 —— 字节
// 载荷不追加 NUL，与 ack 类的文本载荷不同）
QList<QByteArray> frameReads(quint32 dt, const QByteArray &payload)
{
    return {le32(0xFEEEEEEF) + le32(dt) + le32(quint32(payload.size())), payload};
}

// **逐笔** write() 的形态签名（按调用序）。12B 魔法帧头 → `hdr[<宣布长度>]`；XML → `xml:<命令>`；
// ackValue → `ok@<hex>`；ack → `ack`；其余按字节数 → `data[<n>]`。
// 用途（见文件头订正 #4）：钉"哪一笔写了多少字节、按什么顺序"——少发/多发一笔 ack、同一块
// 载荷写两遍、或用 xsendText 而不是 xsendBytes 发数据块（会多一个 NUL、宣布长度 +1），都能抓到。
QStringList writeSignature(const MockUsbChannel &m)
{
    QStringList out;
    for (const QByteArray &f : std::as_const(m.writeFrames)) {
        if (f.size() == 12 && f.startsWith(le32(0xFEEEEEEF)))
            out << QStringLiteral("hdr[%1]").arg(le32At(f, 8));
        else if (f.startsWith("<?xml"))
            out << QStringLiteral("xml:%1")
                       .arg(mtkbrom::XmlSession::field(QString::fromUtf8(f), QStringLiteral("command")));
        else if (f.startsWith("OK@"))
            out << QStringLiteral("ok@%1").arg(QString::fromUtf8(f).remove(QChar('\0')).mid(3));
        else if (f.startsWith("OK"))
            out << QStringLiteral("ack");
        else
            out << QStringLiteral("data[%1]").arg(f.size());
    }
    return out;
}

// 设备发来的 FileSysOp（写路径的②，`XL:432-443`）：key 决定是否放行（`XL:967-968` 只认 FILE-SIZE）
QList<QByteArray> fileSysOpReads(const QString &key, quint32 length)
{
    return textReads(QStringLiteral("<host><command>CMD:FILE-SYS-OPERATION</command><arg>"
                                    "<key>%1</key><file_path>MEM://0x8000000:0x%2</file_path></arg></host>")
                         .arg(key, QString::number(length, 16)));
}

// 设备发来的 DwnFile（写路径的④，`XL:408-424`）：packet_length 用**十六进制**（`XL:422`）
QList<QByteArray> dwnFileReads(quint32 packetLength, quint32 length)
{
    return textReads(QStringLiteral("<host><command>CMD:DOWNLOAD-FILE</command><arg>"
                                    "<checksum>CHK_NO</checksum><info>2nd-DA</info>"
                                    "<source_file>MEM://0x8000000:0x%1</source_file>"
                                    "<packet_length>0x%2</packet_length></arg></host>")
                         .arg(QString::number(length, 16), QString::number(packetLength, 16)));
}

// 写路径的收尾两帧（`XL:489-496`）：CMD:END(OK) → CMD:START
QList<QByteArray> writeTailReads()
{
    return textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>OK</result></arg></host>"))
         + textReads(QStringLiteral("<host><command>CMD:START</command></host>"));
}

// 读路径的 UPLOAD-FILE 应答（设备→host；`XL:425-431`）
QList<QByteArray> uploadFileReads()
{
    return textReads(QStringLiteral("<host><command>CMD:UPLOAD-FILE</command><arg>"
                                    "<checksum>CHK_NO</checksum><info>ROM_0</info>"
                                    "<target_file>ROM_0</target_file></arg></host>"));
}
} // namespace

class TestMtkXmlPayload : public QObject
{
    Q_OBJECT
private slots:
    void handshakeSequence();
    void handshakeRejectsUnexpectedFirstCommand();
    void handshakeRejectsUnlistedFirstCommand();
    void handshakeFailsWhenSetupEnvRejected();
    void handshakeFailsWhenHostSupportedCommandsRejected();
    void handshakeFailsWhenNotifyInitHwRejected();
    void handshakeFailsWhenSetHostInfoRejected();
    void handshakeSurvivesDaLogFramesInCommandStream();
    void setupEnvPayloadFields();
    // ── T10 ──
    void writePartitionSequence();
    // 设备对 WRITE-FLASH 答非所问（直接 CMD:END，既非 FileSysOp 也非 DwnFile）→ 失败且文案点名实收命令（审查 Minor 9）
    void writePartitionRejectsUnexpectedDeviceReply()
    {
        MockUsbChannel m;
        m.reads << textReads(QStringLiteral("OK"))                       // WRITE-FLASH 被接受
                << textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>OK</result></arg></host>"));
        mtkbrom::XmlSession x(&m);
        QString err;
        QVERIFY(!mtkbrom::xmlWritePartition(x, QStringLiteral("EMMC-USER"), QByteArray(0x200, '\x11'), nullptr, &err));
        QVERIFY2(err.contains(QStringLiteral("CMD:END")), qPrintable(err));      // 报出**实收**命令
        QVERIFY(!err.contains(QStringLiteral("(空)")));                          // 不是"收到空"
    }

    // 补零分支：0x500 → 补到 0x600，**宣布长度必须是补零后的值**（审查 Minor 6）
    void writePartitionAnnouncesPaddedLength()
    {
        MockUsbChannel m;
        QByteArray data(0x500, '\x33');                       // 非 512 整数倍
        m.reads << textReads(QStringLiteral("OK"))
                << fileSysOpReads(QStringLiteral("FILE-SIZE"), 0x600)   // 设备按**补零后**的长度索要
                << dwnFileReads(0x400, 0x600)
                << textReads(QStringLiteral("OK"))                      // 第二次 ackValue(length)
                << textReads(QStringLiteral("OK"))                      // 第一包 ack(0)
                << textReads(QStringLiteral("OK"))
                << textReads(QStringLiteral("OK"))                      // 第二包 ack(0)
                << textReads(QStringLiteral("OK"))
                << writeTailReads();
        mtkbrom::XmlSession x(&m);
        QString err;
        QVERIFY2(mtkbrom::xmlWritePartition(x, QStringLiteral("EMMC-USER"), data, nullptr, &err), qPrintable(err));
        QCOMPARE(m.reads.size(), 0);
        QList<QByteArray> blocks;
        for (const QByteArray &f : std::as_const(m.writeFrames))
            if (f.size() == 0x400 || f.size() == 0x200)
                blocks << f;
        QCOMPARE(blocks.size(), 2);
        QCOMPARE(blocks.at(0).size(), 0x400);
        QCOMPARE(blocks.at(1).size(), 0x200);
        QCOMPARE(blocks.at(0).left(0x400), data.mid(0, 0x400));          // 前段原样
        QCOMPARE(blocks.at(1).left(0x100), data.mid(0x400, 0x100));      // 余下 0x100 原样
        QCOMPARE(blocks.at(1).mid(0x100), QByteArray(0x100, '\0'));     // **尾部补零**
    }

    // 设备给出**超大** packet_length（0x80000000，≥2^31）→ 必须夹取到数据长度后**单块**写完
    // （审查 Important：不夹取时 int(packet) 为负 → 步进不前进 = 刷写线程挂死）
    void writePartitionClampsOversizedPacketLength()
    {
        MockUsbChannel m;
        const QByteArray data(0x400, '\x21');
        m.reads << textReads(QStringLiteral("OK"))
                << fileSysOpReads(QStringLiteral("FILE-SIZE"), 0x400)
                << dwnFileReads(0x80000000u, 0x400)      // 荒谬的 packet_length
                << textReads(QStringLiteral("OK"))       // 第二次 ackValue(length)
                << textReads(QStringLiteral("OK"))       // 单包：ack(0) 的应答
                << textReads(QStringLiteral("OK"))       // 单包：数据后的应答
                << writeTailReads();
        mtkbrom::XmlSession x(&m);
        QString err;
        QVERIFY2(mtkbrom::xmlWritePartition(x, QStringLiteral("EMMC-USER"), data, nullptr, &err), qPrintable(err));
        QCOMPARE(m.reads.size(), 0);
        int dataBlocks = 0;
        for (const QByteArray &f : std::as_const(m.writeFrames))
            if (f.size() == 0x400)
                ++dataBlocks;
        QCOMPARE(dataBlocks, 1);                     // **单块**（夹取后 packet == 数据长度）
    }

    // 空数据：任何写之前就失败（审查 Minor 6）
    void writePartitionRejectsEmptyData()
    {
        MockUsbChannel m;
        mtkbrom::XmlSession x(&m);
        QString err;
        QVERIFY(!mtkbrom::xmlWritePartition(x, QStringLiteral("EMMC-USER"), QByteArray(), nullptr, &err));
        QVERIFY(!err.isEmpty());
        QCOMPARE(m.writeFrames.size(), 0);        // 一个字节都没发
        QCOMPARE(m.reads.size(), 0);              // 也没读
    }

    void writePartitionRejectsWrongFileSysOpKey();
    void writePartitionFailsWhenPacketRejected();
    void readPartitionSequence();
    void readPartitionRejectsAnnouncedLengthMismatch();
    void rebootRequestsDisconnectOrImmediate();
};

// 握手：等 CMD:START → setup_env → setup_hw_init → set_host_info（四条命令各自走完）
void TestMtkXmlPayload::handshakeSequence()
{
    MockUsbChannel m;
    m.reads << deviceStartReads()   // 设备先发 CMD:START
            << commandReads()       // SET-RUNTIME-PARAMETER
            << commandReads()       // HOST-SUPPORTED-COMMANDS
            << commandReads()       // NOTIFY-INIT-HW
            << commandReads();      // SET-HOST-INFO
    mtkbrom::XmlSession x(&m);
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::xmlDa1Handshake(x, &log, &err), qPrintable(err));

    // 四条命令的文本（按写帧顺序）：SET-RUNTIME-PARAMETER(1.1/initialize_dram YES) →
    // HOST-SUPPORTED-COMMANDS → NOTIFY-INIT-HW → SET-HOST-INFO
    const QStringList sent = sentXml(m);
    QCOMPARE(sent.size(), 4);   // **四条**命令（CMD:START 本身不占写帧；XL:271-321）

    // 逐字节对齐上游 XC:98-136 的 create_cmd 产物（紧凑 XML、无换行、<adv> 独立块）
    QCOMPARE(sent.at(0), QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><da><version>1.1</version>"
        "<command>CMD:SET-RUNTIME-PARAMETER</command><arg>"
        "<checksum_level>NONE</checksum_level>"
        "<battery_exist>AUTO-DETECT</battery_exist>"
        "<da_log_level>INFO</da_log_level>"
        "<log_channel>UART</log_channel>"
        "<system_os>LINUX</system_os>"
        "</arg><adv><initialize_dram>YES</initialize_dram></adv></da>"));

    QVERIFY(sent.at(1).contains(QStringLiteral("CMD:HOST-SUPPORTED-COMMANDS")));
    QVERIFY(sent.at(1).contains(QStringLiteral(
        "CMD:DOWNLOAD-FILE^1@CMD:FILE-SYS-OPERATION^1@CMD:PROGRESS-REPORT^1@CMD:UPLOAD-FILE^1@")));

    // NOTIFY-INIT-HW：上游 `cmd_notify_init_hw` 调 `create_cmd("NOTIFY-INIT-HW")` —— content=None
    // → create_cmd **不写 <arg>**（XC:18-25 的 `if content is not None`，XC:32-42）。
    // 该函数的 docstring 里画的 `<arg></arg>` 与实现不符（实现是从），故此处钉**没有 <arg>** 的形态。
    QCOMPARE(sent.at(2), QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><da><version>1.0</version>"
        "<command>CMD:NOTIFY-INIT-HW</command></da>"));

    // SET-HOST-INFO：<info>%Y%m%dT%H%M%S</info>（XL:329-331 + XC:600-612，空参时本地生成）
    QVERIFY(sent.at(3).startsWith(QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?><da><version>1.0</version>"
        "<command>CMD:SET-HOST-INFO</command><arg>")));
    QVERIFY2(sent.at(3).contains(QRegularExpression(QStringLiteral("<info>\\d{8}T\\d{6}</info>"))),
             qPrintable(sent.at(3)));

    QVERIFY2(log.join(QLatin1Char('\n')).contains(QStringLiteral("CMD:START")),
             qPrintable(log.join(QLatin1Char('\n'))));
    QVERIFY(m.reads.isEmpty());     // 13 帧全被消费，无残留
}

// 设备没发 CMD:START（先发别的命令）→ 明确失败（XL:309-313 只认 CMD:START）
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
    QVERIFY2(err.contains(QStringLiteral("CMD:END")), qPrintable(err));   // 文案须报出实收命令
    QCOMPARE(sentXml(m).size(), 0);     // 首条不是 CMD:START → 一条命令都不许发
}

// T8 契约的**字面形态**（`mtk_xml_session.h`）：**具名但未列举**的命令（如扩展命令 CMD:CUSTOMX）
// 让 readCommandResult 返回 true 而只置 out.command —— 题面风险 3 的原形。首条即这种帧时，
// 握手必须靠"核对 out.command"拒掉（去掉核对就会把它当同步信号放行、继续发 setup_env）。
void TestMtkXmlPayload::handshakeRejectsUnlistedFirstCommand()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("<host><command>CMD:CUSTOMX</command></host>"));
    mtkbrom::XmlSession x(&m);
    QString err;
    QVERIFY(!mtkbrom::xmlDa1Handshake(x, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("CMD:START")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("CMD:CUSTOMX")), qPrintable(err));   // 文案须报出实收命令
    QCOMPARE(sentXml(m).size(), 0);     // 同样一条命令都不许发
}

// 铁律 18 的姿态：上游 upload_da1 对三步的返回值**一概不检查**（XL:311-313），本层任一失败即
// 中止 —— 本用例钉 setup_env 被设备拒绝时整个握手失败（若退回"忽略返回值"则会假绿）。
// 同时钉失败文案**点名到本步**（单前缀 + 具体命令，reviewer Minor 2）。
void TestMtkXmlPayload::handshakeFailsWhenSetupEnvRejected()
{
    MockUsbChannel m;
    m.reads << deviceStartReads()
            << textReads(QStringLiteral("ERR!INVALID-PARAM"));   // XL:218-219：ERR! → 失败
    mtkbrom::XmlSession x(&m);
    QString err;
    QVERIFY(!mtkbrom::xmlDa1Handshake(x, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("setup_env")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("SET-RUNTIME-PARAMETER")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("ERR!INVALID-PARAM")), qPrintable(err));   // 内层措辞保留
    QCOMPARE(sentXml(m).size(), 1);     // 只发了 setup_env 的命令就中止（不再发后两条）
}

// 第 2 步 setup_hw_init 的**第一条**命令被拒 → 中止，且文案点名到它（不是笼统的"设备返回错误"；
// 去掉该处返回值检查、或只报通用文案，本用例都会红）
void TestMtkXmlPayload::handshakeFailsWhenHostSupportedCommandsRejected()
{
    MockUsbChannel m;
    m.reads << deviceStartReads()
            << commandReads()                                     // SET-RUNTIME-PARAMETER 成功
            << textReads(QStringLiteral("ERR!NO-CAP"));           // HOST-SUPPORTED-COMMANDS 被拒
    mtkbrom::XmlSession x(&m);
    QString err;
    QVERIFY(!mtkbrom::xmlDa1Handshake(x, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("HOST-SUPPORTED-COMMANDS")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("ERR!NO-CAP")), qPrintable(err));
    QCOMPARE(sentXml(m).size(), 2);     // NOTIFY-INIT-HW 不许发（第一条就失败）
}

// 第 2 步的**第二条**命令（NOTIFY-INIT-HW）被拒 → 中止（两条命令的返回值都要看，只检查第一条不够）
void TestMtkXmlPayload::handshakeFailsWhenNotifyInitHwRejected()
{
    MockUsbChannel m;
    m.reads << deviceStartReads()
            << commandReads()                                     // SET-RUNTIME-PARAMETER
            << commandReads()                                     // HOST-SUPPORTED-COMMANDS 成功
            << textReads(QStringLiteral("ERR!NO-HW"));            // NOTIFY-INIT-HW 被拒
    mtkbrom::XmlSession x(&m);
    QString err;
    QVERIFY(!mtkbrom::xmlDa1Handshake(x, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("NOTIFY-INIT-HW")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("ERR!NO-HW")), qPrintable(err));
    QCOMPARE(sentXml(m).size(), 3);     // 走到第三条就中止（set_host_info 不许发）
}

// 第 3 步 SET-HOST-INFO 被拒 → 整条握手失败（上游此处**不看返回值**，退回上游姿态就会假绿）
void TestMtkXmlPayload::handshakeFailsWhenSetHostInfoRejected()
{
    MockUsbChannel m;
    m.reads << deviceStartReads()
            << commandReads()                                     // SET-RUNTIME-PARAMETER
            << commandReads()                                     // HOST-SUPPORTED-COMMANDS
            << commandReads()                                     // NOTIFY-INIT-HW
            << textReads(QStringLiteral("ERR!NO-INFO"));          // SET-HOST-INFO 被拒
    mtkbrom::XmlSession x(&m);
    QString err;
    QVERIFY(!mtkbrom::xmlDa1Handshake(x, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("set_host_info")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("SET-HOST-INFO")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("ERR!NO-INFO")), qPrintable(err));
    QCOMPARE(sentXml(m).size(), 4);
}

// DA 日志帧（DT_MESSAGE）夹在命令响应流里（DA 起环境时日志最密）→ 必须**跳过**并把文本交给
// sink，命令照常走完三步（XL:112-135：xread 是循环，日志帧不打断协议）
void TestMtkXmlPayload::handshakeSurvivesDaLogFramesInCommandStream()
{
    MockUsbChannel m;
    m.reads << deviceStartReads()
            << logReads(QStringLiteral("[DA] dram init ok"))       // setup_env 的 OK 之前
            << commandReads()                                      // SET-RUNTIME-PARAMETER
            << textReads(QStringLiteral("OK"))                     // HOST-SUPPORTED-COMMANDS
            << logReads(QStringLiteral("[DA] hw init done"), 3)    // 夹在 CMD:END 之前
            << textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>OK</result></arg></host>"))
            << textReads(QStringLiteral("<host><command>CMD:START</command></host>"))
            << commandReads()                                      // NOTIFY-INIT-HW
            << commandReads();                                     // SET-HOST-INFO
    mtkbrom::XmlSession x(&m);
    QStringList logs;
    x.setLogSink([&logs](const QString &s) { logs << s; });
    QString err;
    QVERIFY2(mtkbrom::xmlDa1Handshake(x, nullptr, &err), qPrintable(err));
    QCOMPARE(logs.size(), 2);
    QVERIFY2(logs.at(0).contains(QStringLiteral("dram init ok")), qPrintable(logs.at(0)));
    QVERIFY2(logs.at(1).contains(QStringLiteral("hw init done")), qPrintable(logs.at(1)));
    QVERIFY(m.reads.isEmpty());     // 日志帧没被留在队列里
}

// setup_env 的命令文本要素（版本 1.1 / initialize_dram / checksum_level / system_os / log_channel）
void TestMtkXmlPayload::setupEnvPayloadFields()
{
    MockUsbChannel m;
    m.reads << commandReads();
    mtkbrom::XmlSession x(&m);
    QString err;
    QVERIFY2(mtkbrom::xmlSetupEnv(x, &err), qPrintable(err));
    const QString sent = QString::fromUtf8(m.writeFrames.at(1)).remove(QChar('\0'));   // [0] 是 12B 帧头
    QVERIFY(sent.contains(QStringLiteral("<version>1.1</version>")));
    QVERIFY(sent.contains(QStringLiteral("<battery_exist>AUTO-DETECT</battery_exist>")));
    QVERIFY(sent.contains(QStringLiteral("<checksum_level>NONE</checksum_level>")));
    QVERIFY(sent.contains(QStringLiteral("<system_os>LINUX</system_os>")));
    QVERIFY(sent.contains(QStringLiteral("<log_channel>UART</log_channel>")));
    QVERIFY(sent.contains(QStringLiteral("<da_log_level>INFO</da_log_level>")));   // **字符串**，非数字
    QVERIFY(sent.contains(QStringLiteral("<initialize_dram>YES</initialize_dram>")));
    QVERIFY(sent.contains(QStringLiteral("CMD:SET-RUNTIME-PARAMETER")));
}

// ── T10：写分区（XL:943-983 writeflash + XL:451-506 upload/raw=True） ─────────────────────
// 完整节拍：① WRITE-FLASH（noack）→ ② FileSysOp(key 必须 FILE-SIZE) → ③ ackValue(length)
//   → ④ DwnFile（拿 packet_length）→ ⑤ **再** ackValue(length) + 读 "OK"（upload 自己又发一次，
//   `XL:463-464`；漏掉这一发一读后面全部错位）→ ⑥ 数据补零到 512 → 逐包 {ackValue(0) → OK →
//   发块 → OK} → ⑦ ack → CMD:END(OK) → ack → CMD:START。
void TestMtkXmlPayload::writePartitionSequence()
{
    MockUsbChannel m;
    const QByteArray data = [] {                     // 逐字节可辨：切片错位/重复发送都会现形
        QByteArray d(0x600, '\0');
        for (int i = 0; i < d.size(); ++i)
            d[i] = char(i & 0xFF);
        return d;
    }();
    m.reads << textReads(QStringLiteral("OK"))                    // ① WRITE-FLASH 被接受（noack）
            << fileSysOpReads(QStringLiteral("FILE-SIZE"), 0x600)  // ② 设备索要文件大小
            << dwnFileReads(0x400, 0x600)                          // ④ 应答 ③ 的 ackValue(length)
            << textReads(QStringLiteral("OK"))                     // ⑤ 第二次 ackValue(length) 的应答
            << textReads(QStringLiteral("OK"))                     // ⑥ 第一包前的 ackValue(0) 应答
            << textReads(QStringLiteral("OK"))                     // 第一包数据后的应答
            << textReads(QStringLiteral("OK"))                     // 第二包前的 ackValue(0) 应答
            << textReads(QStringLiteral("OK"))                     // 第二包数据后的应答
            << writeTailReads();                                   // ⑦ CMD:END(OK) → CMD:START
    mtkbrom::XmlSession x(&m);
    QStringList log;
    QString err;
    QVERIFY2(mtkbrom::xmlWritePartition(x, QStringLiteral("EMMC-USER"), data, &log, &err), qPrintable(err));

    // ① 的**逐字节**命令文本（订正 #5：XC:452-462 是 partition / offset / source_file **三项**）
    const QString expectedCmd = mtkbrom::XmlSession::envelope(
        QStringLiteral("WRITE-FLASH"),
        {QStringLiteral("<partition>EMMC-USER</partition>"),
         QStringLiteral("<offset>0x0</offset>"),
         QStringLiteral("<source_file>MEM://0x8000000:0x600</source_file>")});
    QCOMPARE(m.writeFrames.at(1), expectedCmd.toUtf8() + QByteArray(1, '\0'));   // [0] 是 12B 帧头

    // **逐笔**形态（12 次 xsend × 2 = 24 笔）。数据块宣布长度 = 字节数（铁律 17：字节载荷不加 NUL），
    // 文本 ack 宣布 = 字符数 + 1。
    const QStringList expectedSig = {
        QStringLiteral("hdr[%1]").arg(expectedCmd.toUtf8().size() + 1),
        QStringLiteral("xml:CMD:WRITE-FLASH"),
        QStringLiteral("hdr[4]"), QStringLiteral("ack"),        // FileSysOp 的应答 ack（XL:442）
        QStringLiteral("hdr[10]"), QStringLiteral("ok@0x600"),  // ③ ackValue(length)（XL:969）
        QStringLiteral("hdr[4]"), QStringLiteral("ack"),        // DwnFile 的应答 ack（XL:423）
        QStringLiteral("hdr[10]"), QStringLiteral("ok@0x600"),  // ⑤ ackValue(length)（XL:463）
        QStringLiteral("hdr[8]"), QStringLiteral("ok@0x0"),     // ⑥ 第一包 ackValue(0)
        QStringLiteral("hdr[1024]"), QStringLiteral("data[1024]"),
        QStringLiteral("hdr[8]"), QStringLiteral("ok@0x0"),     // 第二包 ackValue(0)
        QStringLiteral("hdr[512]"), QStringLiteral("data[512]"),
        QStringLiteral("hdr[4]"), QStringLiteral("ack"),        // ⑦ 收尾 raw ack（XL:487-488）
        QStringLiteral("hdr[4]"), QStringLiteral("ack"),        // CMD:END 后的 ack（XL:490）
        QStringLiteral("hdr[4]"), QStringLiteral("ack"),        // CMD:START 的 ack（XL:406）
    };
    QCOMPARE(writeSignature(m), expectedSig);

    // 两包 = 原数据的两个切片（0x600 = 0x400 + 0x200，已 512 对齐 → **不补零**）
    QList<QByteArray> blocks;
    for (const QByteArray &f : std::as_const(m.writeFrames))
        if (f.size() == 0x400 || f.size() == 0x200)
            blocks << f;
    QCOMPARE(blocks.size(), 2);
    QCOMPARE(blocks.at(0), data.mid(0, 0x400));
    QCOMPARE(blocks.at(1), data.mid(0x400, 0x200));

    QVERIFY(m.reads.isEmpty());     // 应答链精确耗尽
    QVERIFY2(log.join(QLatin1Char('\n')).contains(QStringLiteral("EMMC-USER")),
             qPrintable(log.join(QLatin1Char('\n'))));
}

// FileSysOp 的 key 不是 FILE-SIZE → 明确失败（XL:967-968），且**一字节数据都不许发**
void TestMtkXmlPayload::writePartitionRejectsWrongFileSysOpKey()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("OK"))
            << fileSysOpReads(QStringLiteral("OTHER"), 0x100);
    mtkbrom::XmlSession x(&m);
    QString err;
    QVERIFY(!mtkbrom::xmlWritePartition(x, QStringLiteral("EMMC-USER"), QByteArray(0x100, '\x01'), nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("FILE-SIZE")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("OTHER")), qPrintable(err));      // 文案须报出实收 key
    // 只发了 WRITE-FLASH（头+体）与 FileSysOp 的应答 ack（XL:442，上游也是先 ack 后判 key）
    QCOMPARE(writeSignature(m).size(), 4);
    QVERIFY(m.reads.isEmpty());     // 失败后不再继续读（不许吞掉后续帧）
}

// ackValue(0)（每包开头，XL:469-471）被设备拒绝 → 立刻失败，且**该包数据一字节都不发**
void TestMtkXmlPayload::writePartitionFailsWhenPacketRejected()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("OK"))
            << fileSysOpReads(QStringLiteral("FILE-SIZE"), 0x600)
            << dwnFileReads(0x400, 0x600)
            << textReads(QStringLiteral("OK"))              // ⑤ 第二次 ackValue(length)
            << textReads(QStringLiteral("ERR!BUSY"));       // ⑥ 第一包 ackValue(0) 被拒
    mtkbrom::XmlSession x(&m);
    QString err;
    QVERIFY(!mtkbrom::xmlWritePartition(x, QStringLiteral("EMMC-USER"), QByteArray(0x600, '\x42'), nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("ERR!BUSY")), qPrintable(err));     // 内层措辞保留
    QVERIFY2(err.contains(QStringLiteral("0x0")), qPrintable(err));          // 点名片内偏移
    QCOMPARE(writeSignature(m).size(), 12);     // 到 ackValue(0) 为止，数据块未发
    QVERIFY(m.reads.isEmpty());
}

// ── T10：读分区（XL:918-941 readflash + XL:508-559 download_raw） ────────────────────────
// 完整节拍：READ-FLASH（noack）→ UpFile → 裸 `OK@0x<len>` → ack → 读 "OK" → ack
//   → 循环{ 收一帧 → ack → 读 "OK" → ack } → CMD:START。
// **逐帧 ack** 是读路径与 T8 `get_command_result` 裸 OK@ 分支（单次尾 ack）的决定性差异。
void TestMtkXmlPayload::readPartitionSequence()
{
    MockUsbChannel m;
    const QByteArray payload(0x200, '\x77');
    m.reads << textReads(QStringLiteral("OK"))          // READ-FLASH 被接受（noack）
            << uploadFileReads()                        // CMD:UPLOAD-FILE（readCommandResult 消费）
            << textReads(QStringLiteral("OK@0x200"))    // 裸数据路径：宣布长度
            << textReads(QStringLiteral("OK"))          // 首个 ack 的应答（XL:521-522）
            << frameReads(1, payload)                   // 数据帧：12B 头 + 载荷两笔
            << textReads(QStringLiteral("OK"))          // 逐帧 ack 的应答（XL:543-544）
            << textReads(QStringLiteral("<host><command>CMD:START</command></host>"));   // 收尾
    mtkbrom::XmlSession x(&m);
    QByteArray got;
    QString err;
    QVERIFY2(mtkbrom::xmlReadPartition(x, QStringLiteral("EMMC-USER"), 0x0, 0x200, got, &err), qPrintable(err));
    QCOMPARE(got, payload);

    const QString expectedCmd = mtkbrom::XmlSession::envelope(
        QStringLiteral("READ-FLASH"),
        {QStringLiteral("<partition>EMMC-USER</partition>"),
         QStringLiteral("<offset>0x0</offset>"),
         QStringLiteral("<length>0x200</length>"),
         QStringLiteral("<target_file>ROM_0</target_file>")});      // XC:474-484
    QCOMPARE(m.writeFrames.at(1), expectedCmd.toUtf8() + QByteArray(1, '\0'));

    // 6 次 ack（UpFile / 裸 OK@ / "OK" / 数据帧 / "OK" / CMD:START）+ READ-FLASH = 7 次 xsend = 14 笔。
    // 若改走 T8 的裸 OK@ 分支（单次尾 ack）则只有 5 次 ack → 本断言红。
    QCOMPARE(writeSignature(m),
             (QStringList{QStringLiteral("hdr[%1]").arg(expectedCmd.toUtf8().size() + 1),
                          QStringLiteral("xml:CMD:READ-FLASH"),
                          QStringLiteral("hdr[4]"), QStringLiteral("ack"),
                          QStringLiteral("hdr[4]"), QStringLiteral("ack"),
                          QStringLiteral("hdr[4]"), QStringLiteral("ack"),
                          QStringLiteral("hdr[4]"), QStringLiteral("ack"),
                          QStringLiteral("hdr[4]"), QStringLiteral("ack"),
                          QStringLiteral("hdr[4]"), QStringLiteral("ack")}));
    QVERIFY(m.reads.isEmpty());     // 读队列精确耗尽（CMD:START 是最后一帧）
}

// 设备宣布的长度 ≠ 请求长度 → fail-closed。上游只按**宣布值**收字节（`XL:517-518` 覆盖掉调用方
// 传的 length），调用方拿到短包也不知道；本层与 T8 的"数据长度不符"检查同姿态（.cpp 有注释）。
void TestMtkXmlPayload::readPartitionRejectsAnnouncedLengthMismatch()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("OK"))
            << uploadFileReads()
            << textReads(QStringLiteral("OK@0x100"));       // 宣布 0x100，但请求的是 0x200
    mtkbrom::XmlSession x(&m);
    QByteArray got;
    QString err;
    QVERIFY(!mtkbrom::xmlReadPartition(x, QStringLiteral("EMMC-USER"), 0x0, 0x200, got, &err));
    QVERIFY2(err.contains(QStringLiteral("0x100")), qPrintable(err));     // 宣布值
    QVERIFY2(err.contains(QStringLiteral("0x200")), qPrintable(err));     // 请求值
    QVERIFY(got.isEmpty());
}

// ── T10：收尾复位（XL:1038-1046 shutdown → XC:429-440 cmd_reboot） ───────────────────────
// `send_command` 的**默认** noack=False → 走完 OK → CMD:END → CMD:START 三步。
void TestMtkXmlPayload::rebootRequestsDisconnectOrImmediate()
{
    const QByteArray expectDisconnect =
        mtkbrom::XmlSession::envelope(QStringLiteral("REBOOT"),
                                      {QStringLiteral("<action>DISCONNECT</action>")})
            .toUtf8() + QByteArray(1, '\0');
    const QByteArray expectImmediate =
        mtkbrom::XmlSession::envelope(QStringLiteral("REBOOT"),
                                      {QStringLiteral("<action>IMMEDIATE</action>")})
            .toUtf8() + QByteArray(1, '\0');

    MockUsbChannel m;
    m.reads << commandReads();
    mtkbrom::XmlSession x(&m);
    QString err;
    QVERIFY2(mtkbrom::xmlReboot(x, true, &err), qPrintable(err));          // 默认参数即 disconnect=true
    QCOMPARE(m.writeFrames.at(1), expectDisconnect);
    QVERIFY(m.reads.isEmpty());

    MockUsbChannel m2;
    m2.reads << commandReads();
    mtkbrom::XmlSession x2(&m2);
    QVERIFY2(mtkbrom::xmlReboot(x2, false, &err), qPrintable(err));
    QCOMPARE(m2.writeFrames.at(1), expectImmediate);
    QVERIFY(m2.reads.isEmpty());

    // 设备拒绝（ERR!）→ 失败并保留内层措辞
    MockUsbChannel m3;
    m3.reads << textReads(QStringLiteral("ERR!REBOOT-DENIED"));
    mtkbrom::XmlSession x3(&m3);
    QVERIFY(!mtkbrom::xmlReboot(x3, true, &err));
    QVERIFY2(err.contains(QStringLiteral("ERR!REBOOT-DENIED")), qPrintable(err));
}

QTEST_APPLESS_MAIN(TestMtkXmlPayload)
#include "test_mtk_xml_payload.moc"
