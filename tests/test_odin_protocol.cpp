// tests/test_odin_protocol.cpp
//
// Odin 协议帧构造与应答判定（纯函数，逐字节断言）。
// 三方出处：reference/heimdall/heimdall/source/*.h（1024 控制包 / 结束序列布局）、
// reference/odin4-llucs/src/usb/odin_protocol.cpp（应答判定 / 会话协商 / 结束序列）、
// reference/thor/TheAirBlow.Thor.Library/Protocols/Odin.cs + Extensions.cs（分片档位 / 负码文案）。
// 裁定依据：docs/superpowers/specs/samsung-odin-facts.md（D1/D3/D4/D5/D10/D11/D12/D14）。
#include <QtTest>
#include <QByteArray>

#include "core/odin/odin_protocol.h"

class TestOdinProtocol : public QObject
{
    Q_OBJECT
private slots:
    void framesAre1024ZeroPadded();
    void beginSessionPutsMaxProto();
    void totalBytesIsEightBytesLittleEndian();
    void pitFrames();
    void endSequenceLayoutPhone();
    void endSequenceLayoutModem();
    void alignedSequenceBytesRoundsUp();
    void profileByVersion();
    void parsesAcksAndFailures();
    void beginSessionAckKeepsHighVersionBits();
    void sessionLayerFrames();
};

using namespace odin;

static quint32 leAt(const QByteArray &d, int off)
{
    // 测试侧独立实现（不复用被测模块的原语）：若模块端写反了字节序，这里必须能看出来
    return quint32(quint8(d.at(off))) | (quint32(quint8(d.at(off + 1))) << 8)
         | (quint32(quint8(d.at(off + 2))) << 16) | (quint32(quint8(d.at(off + 3))) << 24);
}

void TestOdinProtocol::framesAre1024ZeroPadded()
{
    // D1：三方一致 —— 控制包恒 1024 字节、前 8 字节为 type+request，其余全零
    const QByteArray f = frameControl(kControlPitFile, kPitRequestDump);
    QCOMPARE(f.size(), 1024);
    QCOMPARE(leAt(f, 0), quint32(0x65));
    QCOMPARE(leAt(f, 4), quint32(0x01));
    QCOMPARE(f.mid(8), QByteArray(1016, '\0'));

    // 显式 payload 自帧偏移 8 起逐字节写入（odin4 odin_command()：`memcpy(buf + 8, payload, size)`，
    // 恒定 1024 字节、type@0/request@4 —— 与 frameBeginSession 的偏移 8 同一约定）。
    // ⚠️ 本条用例的 payload 字面量已修正两处（原文只钉住"值 0x7FFFFFFF"：
    //    `QByteArray(4,'\0') + QByteArray("\x7f\xff\xff\xff", 4)`）：
    //    1) 字节序 —— 断言用 leAt()（LE），而 `"\x7f\xff\xff\xff"` 是 0x7FFFFFFF 的**大端**写法；
    //       内存里是 7F FF FF FF，LE 读出 0xFFFFFF7F。LE 值应为 `"\xff\xff\xff\x7f"`。
    //       （odin4 发的是 h_to_le32(0x7FFFFFFF) + memcpy(buf+8)；frameBeginSession() 同款。）
    //    2) 位置 —— 原文的 4 个前导零会把值顶到偏移 12，与断言读的偏移 8、以及与
    //       beginSessionPutsMaxProto() 钉住的偏移 8 互相矛盾。
    //    现为 [ff ff ff 7f 00 00 00 00]（4 字节 LE 值 + 4 字节零尾），并补一条"零尾确实落在偏移 12"。
    const QByteArray g = frameControl(kControlSession, kSessionBegin,
                                      QByteArray("\xff\xff\xff\x7f", 4) + QByteArray(4, '\0'));
    QCOMPARE(g.size(), 1024);
    QCOMPARE(leAt(g, 8), quint32(0x7FFFFFFF));
    QCOMPARE(leAt(g, 12), quint32(0));
}

void TestOdinProtocol::beginSessionPutsMaxProto()
{
    // D3：payload@8 = 0x7FFFFFFF（odin4:377-379 / Thor:44-45）；Heimdall 发全零（1:2 未被采纳）
    const QByteArray f = frameBeginSession();
    QCOMPARE(f.size(), 1024);
    QCOMPARE(leAt(f, 0), quint32(0x64));
    QCOMPARE(leAt(f, 4), quint32(0x00));
    QCOMPARE(leAt(f, 8), quint32(0x7FFFFFFF));
    QCOMPARE(f.mid(12), QByteArray(1012, '\0'));
    // 片大小协商帧（D4：只有 version>=2 才发）
    const QByteArray s = frameFilePartSize(1048576);
    QCOMPARE(leAt(s, 0), quint32(0x64));
    QCOMPARE(leAt(s, 4), quint32(0x05));
    QCOMPARE(leAt(s, 8), quint32(1048576));
}

void TestOdinProtocol::totalBytesIsEightBytesLittleEndian()
{
    // D5：u64（odin4:454-458 / Thor:101-106）；Heimdall 的 u32 在 <4GiB 时字节相容
    const QByteArray f = frameTotalBytes(0x123456789ABull);
    QCOMPARE(leAt(f, 8), quint32(0x456789ABu));
    QCOMPARE(leAt(f, 12), quint32(0x123u));
    QCOMPARE(f.size(), 1024);
    QCOMPARE(frameTotalBytes(0x100000000ull).mid(8, 4), QByteArray(4, '\0'));
}

void TestOdinProtocol::pitFrames()
{
    QCOMPARE(leAt(framePitDumpRequest(), 4), quint32(0x01));
    const QByteArray part = framePitPartRequest(7);
    QCOMPARE(leAt(part, 0), quint32(0x65));
    QCOMPARE(leAt(part, 4), quint32(0x02));
    QCOMPARE(leAt(part, 8), quint32(7));
    QCOMPARE(leAt(framePitEndRequest(), 4), quint32(0x03));
    // 无 payload 的帧：8 字节之后全零
    QCOMPARE(framePitEndRequest().mid(8), QByteArray(1016, '\0'));
    QCOMPARE(kControlPacketSize, 1024);
    QCOMPARE(kPitPartSize, 500);
}

void TestOdinProtocol::endSequenceLayoutPhone()
{
    // D10 phone 布局（三方一致）：8=dest(0) 12=realSize 16=binaryType 20=deviceType
    //                           24=identifier 28=isLast 32=efsClear 36=bootUpdate
    PitEntry e;
    e.binaryType = 0;          // AP
    e.deviceType = 2;
    e.identifier = 80;
    const QByteArray f = frameEndSequence(e, 0x1000, true);
    QCOMPARE(f.size(), 1024);
    QCOMPARE(leAt(f, 0), quint32(0x66));
    QCOMPARE(leAt(f, 4), quint32(0x03));
    QCOMPARE(leAt(f, 8), quint32(0));          // kDestinationPhone
    QCOMPARE(leAt(f, 12), quint32(0x1000));
    QCOMPARE(leAt(f, 16), quint32(0));         // binaryType
    QCOMPARE(leAt(f, 20), quint32(2));         // deviceType
    QCOMPARE(leAt(f, 24), quint32(80));        // identifier
    QCOMPARE(leAt(f, 28), quint32(1));         // isLast
    QCOMPARE(leAt(f, 32), quint32(0));         // efsClear（本期恒 0）
    QCOMPARE(leAt(f, 36), quint32(0));         // bootUpdate（本期恒 0）
    QCOMPARE(f.mid(40), QByteArray(1024 - 40, '\0'));
    QCOMPARE(leAt(frameEndSequence(e, 0x1000, false), 28), quint32(0));
}

void TestOdinProtocol::endSequenceLayoutModem()
{
    // D10 modem：16=binaryType(=1) 20=deviceType 24=isLast（Heimdall+Thor 2:1；odin4 写 0 并在 28 写 isLast）
    PitEntry e;
    e.binaryType = 1;          // CP（Heimdall FlashAction.cpp:253 按它选 kDestinationModem）
    e.deviceType = 2;
    e.identifier = 80;         // modem 分支**不使用** identifier（Heimdall SendFile 要求 fileIdentifier=0xFFFFFFFF）
    const QByteArray f = frameEndSequence(e, 0x2000, true);
    QCOMPARE(leAt(f, 8), quint32(1));          // kDestinationModem
    QCOMPARE(leAt(f, 12), quint32(0x2000));
    QCOMPARE(leAt(f, 16), quint32(1));         // binaryType
    QCOMPARE(leAt(f, 20), quint32(2));         // deviceType
    QCOMPARE(leAt(f, 24), quint32(1));         // isLast
    QCOMPARE(leAt(f, 28), quint32(0));
    QCOMPARE(f.mid(32), QByteArray(1024 - 32, '\0'));
}

void TestOdinProtocol::alignedSequenceBytesRoundsUp()
{
    // 三方一致：序列声明值 = 片大小向上取整后的整数倍（Heimdall SendFile 的 sequenceTotalByteCount、
    // odin4 flash_partition_stream 的 aligned_size、Thor Odin.cs:333-335）；**不做 512 对齐**（D14）
    QCOMPARE(alignedSequenceBytes(0, 1048576), quint32(0));
    QCOMPARE(alignedSequenceBytes(1, 1048576), quint32(1048576));
    QCOMPARE(alignedSequenceBytes(1048576, 1048576), quint32(1048576));
    QCOMPARE(alignedSequenceBytes(1048577, 1048576), quint32(2097152));
    QCOMPARE(alignedSequenceBytes(32768, 131072), quint32(131072));
}

void TestOdinProtocol::profileByVersion()
{
    // odin4:399-406 / Thor:58-71：version<=1 → 128 KiB / 240 片 / 30s；>=2 → 1 MiB / 30 片 / 120s
    const TransferProfile small = profileForVersion(1);
    QCOMPARE(small.packetSize, quint32(131072));
    QCOMPARE(small.sequenceCount, 240);
    QCOMPARE(small.flashTimeoutMs, 30000);
    for (quint32 v : {2u, 9u, 0x7FFFu}) {
        const TransferProfile big = profileForVersion(v);
        QCOMPARE(big.packetSize, quint32(1048576));
        QCOMPARE(big.sequenceCount, 30);
        QCOMPARE(big.flashTimeoutMs, 120000);
    }
    QCOMPARE(profileForVersion(0).packetSize, quint32(131072));
}

void TestOdinProtocol::parsesAcksAndFailures()
{
    QString err;
    QByteArray raw(8, '\0');
    raw[0] = char(0x66);
    raw[4] = char(0x03);
    Ack ack;
    QVERIFY2(parseAck(raw, ack, &err), qPrintable(err));
    QCOMPARE(ack.id, quint32(0x66));
    QCOMPARE(ack.code, quint32(3));
    // 短包 → 失败
    err.clear();
    QVERIFY(!parseAck(QByteArray(7, '\0'), ack, &err));
    QVERIFY(!err.isEmpty());

    // 正常
    QVERIFY(judgeAck(ack, 0x66, false).ok);
    // id 回显不符
    QVERIFY(!judgeAck(ack, 0x65, false).ok);
    // 0xFFFFFFFF = BOOTLOADER_FAIL（odin4 thor_protocol.h:122）
    Ack fail; fail.id = 0xFFFFFFFFu; fail.code = quint32(-4);   // -4 = Write（Thor Extensions.cs:23）
    const AckVerdict v = judgeAck(fail, 0x66, false);
    QVERIFY(!v.ok);
    QVERIFY(v.reason.contains(QStringLiteral("Write")));
    // 负码：默认失败；结束序列允许 -7..-2（odin4:363-366）
    Ack neg; neg.id = 0x66; neg.code = quint32(-3);             // -3 = Erase
    QVERIFY(!judgeAck(neg, 0x66, false).ok);
    QVERIFY(judgeAck(neg, 0x66, true).ok);
    Ack bad6; bad6.id = 0x66; bad6.code = quint32(-6);          // -6 = Size
    QVERIFY(!judgeAck(bad6, 0x66, false).ok);
    QVERIFY(judgeAck(bad6, 0x66, true).ok);
    Ack bad8; bad8.id = 0x66; bad8.code = quint32(-8);          // 表外负码 → 即便放行也失败
    QVERIFY(!judgeAck(bad8, 0x66, true).ok);
    // 正码不拒（Heimdall 要求「必须为 0」，odin4/Thor 只要求非负 —— 采用后者，记日志）
    Ack pos; pos.id = 0x64; pos.code = 3;
    QVERIFY(judgeAck(pos, 0x64, false).ok);
}

void TestOdinProtocol::beginSessionAckKeepsHighVersionBits()
{
    // 0x64/0x00 的 code 字段**是版本号**，不是错误码：带压缩位（bit15 of upper half）时
    // 整个 u32 看起来是负数 —— 对它做 code<0 判定会把合法设备判成失败。
    QByteArray raw(8, '\0');
    raw[0] = char(0x64);
    raw[6] = char(0x02);                       // version = 2（高位在前）
    raw[7] = char(0x80);                       // 压缩支持位
    BeginSessionAck a;
    QString err;
    QVERIFY2(parseBeginSessionAck(raw, a, &err), qPrintable(err));
    QCOMPARE(a.version, quint32(2));
    QVERIFY(a.compressedSupported);
    QCOMPARE(a.id, quint32(0x64));
    QVERIFY(judgeAckIdOnly(Ack{a.id, a.code}, 0x64).ok);         // 只判 id → OK
    // 对照：若用通用判定（含 code<0），同一个合法应答会被判失败 —— 这正是必须分开的原因
    QVERIFY(!judgeAck(Ack{a.id, a.code}, 0x64, false).ok);

    QByteArray plain(8, '\0');
    plain[0] = char(0x64);
    // ⚠️ 本条用例的字节位置已修正：版本号是 code 的**高半字**（u32 LE 的高 2 字节）——
    // odin4 `version = (ack_word >> 16) & 0x7FFF`（odin_protocol.cpp:394-395）、Thor
    // `BitConverter.ToInt16(new[] { buf[6], buf[7] })`（Odin.cs:53-57，LE 机器上低字节 buf[6]、
    // 高字节 buf[7]）→ 二者对 buf[6]=0,buf[7]=1 都算出 0x0100 = 256，与断言的 1 不符。
    // 按参照口径把 version=1 写到 buf[6]（真实字节布局 code = 0x00010000）。
    plain[6] = char(0x01);                     // version = 1，无压缩位
    BeginSessionAck b;
    QVERIFY2(parseBeginSessionAck(plain, b, &err), qPrintable(err));
    QCOMPARE(b.version, quint32(1));
    QVERIFY(!b.compressedSupported);
    QVERIFY(judgeAckIdOnly(Ack{b.id, b.code}, 0x64).ok);

    // BOOTLOADER_FAIL 应答
    QByteArray bad(8, '\0');
    bad[0] = char(0xFF); bad[1] = char(0xFF); bad[2] = char(0xFF); bad[3] = char(0xFF);
    BeginSessionAck c;
    QVERIFY(parseBeginSessionAck(bad, c, &err));
    QVERIFY(!judgeAckIdOnly(Ack{c.id, c.code}, 0x64).ok);
}

// Task 6（会话层）要消费、而本文件其余用例未覆盖的四个帧：钉住 type/request/payload 偏移。
// 出处：机型查询 = odin4 `odin_get_device_type()`（0x64/0x01，无 payload；D13 只做一次 best-effort
// 查询、只记原始值）；请求刷写 = odin4 `odin_request_file_flash()`（0x66/0x00）；
// 序列声明 = odin4 `odin_request_sequence_flash()`（0x66/0x02 + u32@8 = aligned_size）；
// 结束会话/复位 = Heimdall EndSessionPacket.h:32-36（0/1）+ odin4 `odin_end_session()`/`odin_reboot()`
// （0x67/0x00、0x67/0x01）。
void TestOdinProtocol::sessionLayerFrames()
{
    const QByteArray q = frameDeviceTypeQuery();
    QCOMPARE(q.size(), 1024);
    QCOMPARE(leAt(q, 0), quint32(0x64));
    QCOMPARE(leAt(q, 4), quint32(0x01));
    QCOMPARE(q.mid(8), QByteArray(1016, '\0'));

    const QByteArray rf = frameRequestFlash();
    QCOMPARE(rf.size(), 1024);
    QCOMPARE(leAt(rf, 0), quint32(0x66));
    QCOMPARE(leAt(rf, 4), quint32(0x00));
    QCOMPARE(rf.mid(8), QByteArray(1016, '\0'));

    const QByteArray rs = frameRequestSequence(2097152);   // = 2 个 1 MiB 片
    QCOMPARE(rs.size(), 1024);
    QCOMPARE(leAt(rs, 0), quint32(0x66));
    QCOMPARE(leAt(rs, 4), quint32(0x02));
    QCOMPARE(leAt(rs, 8), quint32(2097152));
    QCOMPARE(rs.mid(12), QByteArray(1012, '\0'));

    const QByteArray es = frameEndSession(false);
    QCOMPARE(es.size(), 1024);
    QCOMPARE(leAt(es, 0), quint32(0x67));
    QCOMPARE(leAt(es, 4), quint32(0x00));
    QCOMPARE(es.mid(8), QByteArray(1016, '\0'));

    const QByteArray er = frameEndSession(true);
    QCOMPARE(leAt(er, 0), quint32(0x67));
    QCOMPARE(leAt(er, 4), quint32(0x01));

    // 结束序列的 modem 分支不用 identifier（Heimdall BridgeManager.cpp:1207 传 fileIdentifier 位置为
    // deviceType/id 组合外的 0；这里钉住"identifier 不泄漏到 modem 帧的 24..27"）
    PitEntry m;
    m.binaryType = 1;
    m.deviceType = 8;      // UFS
    m.identifier = 0xDEADBEEFu;
    const QByteArray em = frameEndSequence(m, 0x200, true);
    QCOMPARE(leAt(em, 24), quint32(1));          // isLast，不是 identifier
    QCOMPARE(leAt(em, 28), quint32(0));
}

QTEST_APPLESS_MAIN(TestOdinProtocol)
#include "test_odin_protocol.moc"
