// tests/test_mtk_xml_session.cpp
//
// MTK XML（D3）帧层（Phase D2+D3 Task 8）：文本帧 / OK / OK@0x<len> / OK!EOT 保活 /
// get_command_result 分派。对照 mtkclient v2.1.4-20-g71b0175（GPL-3.0，**只读参照，代码文本不进仓库**）：
//   XL = mtkclient/Library/DA/xmlflash/xml_lib.py；XC = .../xmlflash/xml_cmd.py
//
// 夹具约定（**先读再改**，同 T2/T4）：默认 `IBromUsb::readExact` 是"单次读 + 严格长度"
// （`mtk_brom.cpp:219-231`），本层每次 readExact 消耗**一笔**队列项 ——
//   • 文本帧（DT_PROTOCOL_FLOW）= **两笔**：12B 帧头、载荷；
//   • DA 日志帧（DT_MESSAGE）= **三笔**：12B 帧头、`priority:u32`、日志载荷
//     （`XL:124-127` 的 16B 头 = 本层"12B 头 + 独立读 4B"）。
// 把整帧塞成一笔会让"读头"截断载荷、后续读全部错位；空队列项也会被 mock 判成 false。

#include <QtTest>
#include <QByteArray>
#include <QStringList>
#include <utility>      // std::as_const

#include "core/modes/mtk_xml_session.h"
#include "core/modes/mtk_brom.h"

class MockUsbChannel : public mtkbrom::IBromUsb   // 与 T2/T4 同款
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

// 原始数据帧（裸 OK@ 路径；datatype 默认走协议流）：**两笔**
QList<QByteArray> dataFrameReads(const QByteArray &payload, quint32 datatype = 1)
{
    return {le32(0xFEEEEEEF) + le32(datatype) + le32(quint32(payload.size())), payload};
}

// DA 日志帧（DT_MESSAGE）：**三笔** —— 12B 头（宣布长度 = 载荷 + 4）、priority:u32、载荷（XL:124-127）
QList<QByteArray> logReads(const QString &s, quint32 priority = 7)
{
    const QByteArray body = textBody(s);
    return {le32(0xFEEEEEEF) + le32(2) + le32(quint32(body.size()) + 4), le32(priority), body};
}
} // namespace

class TestMtkXmlSession : public QObject
{
    Q_OBJECT
private slots:
    void envelopeAndFieldHelpers();
    void xsendTextAddsNulAndLengthPlusOne();
    void ackValueSendsLowercaseHexWith0xPrefix();
    void ackWritesFourByteDoubleNulFrame();
    void getResponseStripsNulAndDecodes();
    void getResponseSkipsDaLogFrames();
    void sendCommandRequiresOkAndEndsWithEndThenStart();
    void sendCommandDrainsUnsupportedHandshakeAndFails();
    void readCommandResultParsesDownloadFilePacketLengthHex();
    void readCommandResultHandlesProgressReportKeepAlive();
    void readCommandResultHandlesBareOkAtDataPath();
    void dataPathSkipsLogFramesMidRead();
    void dataPathBoundsSkippedLogFrames();
    void dataPathRejectsUnknownDatatype();
    void readCommandResultRejectsFrameWithoutCommandOrOkAt();
    void readCommandResultRejectsMalformedOkAtLength();
    void sendCommandFailsOnEmptyResult();
    void sendCommandFailsOnUnparseableFollowUp();
    void readCommandResultKeepsUnknownNamedCommandForCaller();
};

// XC:18-25 create_cmd：`<da><version>v</version><command>CMD:<name></command>[<arg>…</arg>]</da>`；
// 字段取值 = 上游 get_field（XL:35-43）
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

// 铁律 17：str 载荷 length = len+1 且带 NUL（XL:146-153）
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

// XL:161-163 `f"OK@{hex(length)}\0"`：Python hex() = **小写、0x 前缀、不补零**；
// **逐字节复刻上游**（2026-09-17 控制方裁决）：该 str 自带 NUL + xsend 追加 NUL → 实写 `<text>\0\0`，
// 宣布长度 = 字符数 + 2（不是 +1）。（本用例由简令的实现者注记要求补上。）
void TestMtkXmlSession::ackValueSendsLowercaseHexWith0xPrefix()
{
    MockUsbChannel m;
    mtkbrom::XmlSession s(&m);
    QString err;
    QVERIFY2(s.ackValue(0x1000, &err), qPrintable(err));
    QCOMPARE(m.writeFrames.size(), 2);
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(11));   // "OK@0x1000" 9 字符 + 2 NUL
    QCOMPARE(m.writeFrames.at(1), QByteArray("OK@0x1000\0\0", 11));
    QString err2;
    QVERIFY2(s.ackValue(0xFF, &err2), qPrintable(err2));                    // 不补零：0xff 而非 0x00ff
    QCOMPARE(m.writeFrames.at(2), le32(0xFEEEEEEF) + le32(1) + le32(9));    // "OK@0xff" 7 字符 + 2 NUL
    QCOMPARE(m.writeFrames.at(3), QByteArray("OK@0xff\0\0", 9));
}

// ack() 的**线上的字节**（控制方 2026-09-17 裁决）：上游 str "OK\0" 自带 NUL + xsend 追加 NUL
// = `4F 4B 00 00`、宣布 4 字节（不是 3）。这条是本次裁决的**主判别器**。
void TestMtkXmlSession::ackWritesFourByteDoubleNulFrame()
{
    MockUsbChannel m;
    mtkbrom::XmlSession s(&m);
    QString err;
    QVERIFY2(s.ack(&err), qPrintable(err));
    QCOMPARE(m.writeFrames.size(), 2);
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));
    QCOMPARE(m.writeFrames.at(1), QByteArray("OK\0\0", 4));
}

// XL:221-232 get_response：读一帧 → rstrip NUL → utf-8
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

// 计划期更正（控制方预核对 XL:107-132）：上游 xread() 是**循环** —— DT_MESSAGE（DA 日志帧）的载荷被读掉、
// 追加进 UART log，然后**继续读下一帧**；只有 DT_PROTOCOL_FLOW 才返回给调用方。
// 故日志帧**不打断协议**（本层把文本交给 logSink），且不能被当成"响应"。
void TestMtkXmlSession::getResponseSkipsDaLogFrames()
{
    MockUsbChannel m;
    m.reads << logReads(QStringLiteral("[DA] boot stage 1"))
            << logReads(QStringLiteral("[DA] dram init ok"), 3)
            << textReads(QStringLiteral("OK"));
    mtkbrom::XmlSession s(&m);
    QStringList logged;
    s.setLogSink([&logged](const QString &line) { logged << line; });
    QString text;
    QString err;
    QVERIFY2(s.getResponse(text, &err), qPrintable(err));
    QCOMPARE(text, QStringLiteral("OK"));
    QCOMPARE(logged.size(), 2);
    QCOMPARE(logged.at(0), QStringLiteral("[DA] boot stage 1"));
    QCOMPARE(logged.at(1), QStringLiteral("[DA] dram init ok"));
}

// sendCommand（XL:188-219）：xsend → 响应必须 OK → 非 noack 时等 CMD:END(result OK) → ack → CMD:START
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
    QVERIFY(m.reads.isEmpty());     // 三帧都被消费
}

// XL:212-217：`ERR!UNSUPPORTED` 特判 —— 上游读一条结果 → ack → **再读一条**（期望 CMD:START 收尾）。
// 本层同样把流读干净（否则下一条命令的 "OK" 会被残留的 CMD:START 顶掉），但**返回 false + 中文文案**
// —— 上游此处 `return False` 一致；上游**未复刻**的是"`ERR!*` 当成功用"那类调用方缺陷（铁律 18）。
void TestMtkXmlSession::sendCommandDrainsUnsupportedHandshakeAndFails()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("ERR!UNSUPPORTED"))
            << textReads(QStringLiteral("<host><command>CMD:END</command><arg><result>ERR</result>"
                                        "<message>unsupported</message></arg></host>"))
            << textReads(QStringLiteral("<host><command>CMD:START</command></host>"));
    mtkbrom::XmlSession s(&m);
    QString err;
    QVERIFY(!s.sendCommand(QStringLiteral("<da><command>CMD:BOOT-TO</command></da>"), nullptr, false, &err));
    QVERIFY2(err.contains(QStringLiteral("ERR!UNSUPPORTED")), qPrintable(err));
    QVERIFY(m.reads.isEmpty());     // 前导帧 + CMD:START 都被吃掉：流干净
}

// DOWNLOAD-FILE 的 packet_length 是**十六进制**（XL:422 `int(get_field(...), 16)`）
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
    // ack() 实写 **4 字节** `OK\0\0`（上游 str 自带 NUL + xsend 追加；XL:158-159，控制方 2026-09-17 裁决）
    int acks = 0;
    for (const QByteArray &f : std::as_const(m.writeFrames))
        if (f == QByteArray("OK\0\0", 4)) ++acks;
    QVERIFY2(acks >= 2, "PROGRESS-REPORT 期间必须持续 ack");
}

// 裸 "OK@0x<len>" 数据路径（无 <command>）：解析长度 → ack → 收数据（XL:373-388）
void TestMtkXmlSession::readCommandResultHandlesBareOkAtDataPath()
{
    MockUsbChannel m;
    const QByteArray payload(0x40, '\x99');
    m.reads << textReads(QStringLiteral("OK@0x40"))     // 宣布长度
            << textReads(QStringLiteral("OK"))          // ack 后的确认
            << dataFrameReads(payload);                 // 数据帧（**两笔**：头、载荷）
    mtkbrom::XmlSession s(&m);
    mtkbrom::XmlSession::Result r;
    QString err;
    QVERIFY2(s.readCommandResult(r, nullptr, &err), qPrintable(err));
    QCOMPARE(r.bytes, payload);
    QVERIFY(r.command.isEmpty());
}

// 2026-09-17 审查更正 #1/#2：数据路径同样经上游 `xread`（`get_response_data` XL:234-246 → `xread` XL:112-135），
// **DT_MESSAGE 日志帧在数据读取途中也被跳过**（头 + priority + 载荷 → uartlog），**不计入数据长度**。
// 旧实现遇到日志帧会直接把日志载荷当数据收（长度对不上/读偏），上游能正常读完。
void TestMtkXmlSession::dataPathSkipsLogFramesMidRead()
{
    MockUsbChannel m;
    const QByteArray part1(0x20, '\x11');
    const QByteArray part2(0x10, '\x22');
    m.reads << textReads(QStringLiteral("OK@0x30"))                 // 宣布 0x30 = 0x20 + 0x10
            << textReads(QStringLiteral("OK"))
            << dataFrameReads(part1)
            << logReads(QStringLiteral("[DA] erase progress 50%"))   // 中途插一条 DA 日志
            << dataFrameReads(part2);
    mtkbrom::XmlSession s(&m);
    QStringList logged;
    s.setLogSink([&logged](const QString &line) { logged << line; });
    mtkbrom::XmlSession::Result r;
    QString err;
    QVERIFY2(s.readCommandResult(r, nullptr, &err), qPrintable(err));
    QCOMPARE(r.bytes, part1 + part2);                               // 日志帧不计入数据
    QCOMPARE(logged.size(), 1);
    QCOMPARE(logged.at(0), QStringLiteral("[DA] erase progress 50%"));
}

// 跳帧有上限（kMaxLogFramesToSkip = 64，private 故此处写死 65）：设备刷屏不能把我们钉死
void TestMtkXmlSession::dataPathBoundsSkippedLogFrames()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("OK@0x10"))
            << textReads(QStringLiteral("OK"));
    for (int i = 0; i <= 64; ++i)                                   // 65 连续日志帧 > 上限 64
        m.reads << logReads(QStringLiteral("spam %1").arg(i));
    mtkbrom::XmlSession s(&m);
    mtkbrom::XmlSession::Result r;
    QString err;
    QVERIFY(!s.readCommandResult(r, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("连续跳过")), qPrintable(err));
}

// 数据路径遇**未知 datatype**：上游 `xread` 的 `while True:` 对其它类型什么都不做（= 空转），本层 fail-closed
void TestMtkXmlSession::dataPathRejectsUnknownDatatype()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("OK@0x10"))
            << textReads(QStringLiteral("OK"))
            << dataFrameReads(QByteArray(0x10, '\x33'), 7);         // datatype=7：既非协议流也非日志
    mtkbrom::XmlSession s(&m);
    mtkbrom::XmlSession::Result r;
    QString err;
    QVERIFY(!s.readCommandResult(r, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("datatype=7")), qPrintable(err));
}

// 审查 Important #3：无 `<command>` 且无 `OK@` 的帧必须 **fail-closed**
// （原实现 `return true` + 空 Result = fail-open，sendCommand 尾部还会当成功上报）
void TestMtkXmlSession::readCommandResultRejectsFrameWithoutCommandOrOkAt()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("<host><thing>nothing-useful</thing></host>"));
    mtkbrom::XmlSession s(&m);
    mtkbrom::XmlSession::Result r;
    QString err;
    QVERIFY(!s.readCommandResult(r, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("nothing-useful")), qPrintable(err));   // 文案须含实收内容
}

// 同上：失败必须穿透到 sendCommand（不得报成功）
void TestMtkXmlSession::sendCommandFailsOnUnparseableFollowUp()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("OK"))                       // 命令被接受
            << textReads(QStringLiteral("bogus-garbage"));           // 既无 <command> 也无 OK@
    mtkbrom::XmlSession s(&m);
    QString err;
    QVERIFY(!s.sendCommand(QStringLiteral("<da><command>CMD:FOO</command></da>"), nullptr, false, &err));
    QVERIFY2(err.contains(QStringLiteral("bogus-garbage")), qPrintable(err));
}

// 畸形的 `OK@` 长度（`OK@0xZZ`）：上游 `int(tmp[2:],16)` 会当场 ValueError 崩掉；本层 fail-closed + 文案
void TestMtkXmlSession::readCommandResultRejectsMalformedOkAtLength()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("OK@0xZZ"));
    mtkbrom::XmlSession s(&m);
    mtkbrom::XmlSession::Result r;
    QString err;
    QVERIFY(!s.readCommandResult(r, nullptr, &err));
    QVERIFY2(err.contains(QStringLiteral("0xZZ")), qPrintable(err));
}

// 审查 #3 精确形态（XL:210-211）：上游 get_command_result 返回 `("", "")` → send_command 返回假值 = 失败。
// 我们的 Result 无真假值 → sendCommand 必须显式拦"三空"。本用例用 `OK@0x0`（0 长度数据帧）钉它：
// 数据路径会**成功地**返回一个全空 Result（宣布长度 0、循环不跑、长度校验 0==0），没有这道闸就会报成功。
void TestMtkXmlSession::sendCommandFailsOnEmptyResult()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("OK"))            // 命令被接受
            << textReads(QStringLiteral("OK@0x0"))        // 宣布 0 字节的数据帧 → 全空 Result
            << textReads(QStringLiteral("OK"));           // 数据路径的确认
    mtkbrom::XmlSession s(&m);
    QString err;
    QVERIFY(!s.sendCommand(QStringLiteral("<da><command>CMD:FOO</command></da>"), nullptr, false, &err));
    QVERIFY2(err.contains(QStringLiteral("空结果")), qPrintable(err));
}

// 窄化的边界：**具名但未列举**的命令（如扩展命令 CMD:CUSTOM*）不算"不可解析" ——
// 上游 get_command_result 返回 (cmd, "") 把处置交给调用方（read_register XL:357-359 即 `if cmd != '': return False`），
// 本层保留命令名（T9/T10 的调用方必须查 out.command），**不**在此失败
void TestMtkXmlSession::readCommandResultKeepsUnknownNamedCommandForCaller()
{
    MockUsbChannel m;
    m.reads << textReads(QStringLiteral("<host><command>CMD:CUSTOM</command></host>"));
    mtkbrom::XmlSession s(&m);
    mtkbrom::XmlSession::Result r;
    QString err;
    QVERIFY2(s.readCommandResult(r, nullptr, &err), qPrintable(err));
    QCOMPARE(r.command, QStringLiteral("CMD:CUSTOM"));
    QVERIFY(r.text.isEmpty());
}
QTEST_APPLESS_MAIN(TestMtkXmlSession)
#include "test_mtk_xml_session.moc"
