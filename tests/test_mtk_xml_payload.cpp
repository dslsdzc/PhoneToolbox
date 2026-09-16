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

QTEST_APPLESS_MAIN(TestMtkXmlPayload)
#include "test_mtk_xml_payload.moc"
