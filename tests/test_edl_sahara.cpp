// tests/test_edl_sahara.cpp
//
// Sahara 协议层（src/core/edl/sahara.cpp）单元测试：用 MockEdlTransport 扮演"假设备"
// （预置读队列 = 设备要发的帧），断言**实际发出去的字节**（应答帧 + programmer 切片）。
#include <QtTest>
#include "core/edl/sahara.h"
#include "mock_edl_transport.h"
#include "edl_test_helpers.h"   // saharaFrame()（Task 6 的测试也用它，不许各写一份）

// 小端读回 4 字节（断言应答帧字段用）
static quint32 le32(const QByteArray &b, int off)
{
    return quint32(quint8(b.at(off))) |
           (quint32(quint8(b.at(off + 1))) << 8) |
           (quint32(quint8(b.at(off + 2))) << 16) |
           (quint32(quint8(b.at(off + 3))) << 24);
}

class TestEdlSahara : public QObject
{
    Q_OBJECT
private slots:
    void servesProgrammerInRequestedSlices();
    void sendsWellFormedHelloResponse();
    void failsWhenDeviceNeverRequests();
    void failsWhenDeviceGoesSilentAfterHello();
    void failsOnEndOfImageErrorStatus();
    void handlesHelloSplitAcrossReads();
    void doesNotTruncate64BitOffset();
    void doesNotTruncate64BitLength();
    void failsWhenHelloResponseWriteFails();
    void failsWhenProgrammerWriteFails();
    void failsWhenDoneReqWriteFails();
    void parsesSecondFrameFromSameRead();
    void stopsOnDeviceDeclaredCompletion();
    void echoesDeviceVersionWithFallback();
};

void TestEdlSahara::servesProgrammerInRequestedSlices()
{
    edl::MockEdlTransport t;
    const QByteArray prog = QByteArray("0123456789ABCDEF");   // 16 字节
    t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0})          // mode=2, ver=2
            << saharaFrame(edl::SAHARA_READ_DATA, {0, 0, 4, 0})          // image=0 off=0 len=4
            << saharaFrame(edl::SAHARA_READ_DATA, {0, 4, 8, 0})          // off=4 len=8
            // 64 位：off=12 len=4。3×u64 载荷 = {0,0, 12,0, 4,0}（低位字在前）——
            // 手写该字列表极易漏高位字（brief 原稿 {0,12,0,0,4,0} 实际编码的是
            // image_id=0xC00000000/off=0/len=4，任何标准 64 位解析都会回吐 prog[0..4)），
            // 故一律经共享夹具 saharaReadData64Frame() 构造。
            << saharaReadData64Frame(0, 12, 4)
            << saharaFrame(edl::SAHARA_END_OF_IMAGE, {0, 0})
            << saharaFrame(edl::SAHARA_DONE_RSP, {});   // ← DONE 这对是 **host 主动**：host 发 DONE_REQ，
                                                        //   设备回 DONE_RSP（bkerler sahara.py:453-459 +
                                                        //   edl_handler.cpp（重写前 515cc57）:566-584 两源一致；与 HELLO_REQ/
                                                        //   READ_DATA/END_OF_IMAGE 的"设备主动"方向相反，最易记反）

    QString err;
    QVERIFY2(edl::saharaLoadProgrammer(t, prog, &err), qPrintable(err));
    QCOMPARE(t.writes.size(), 5);                                  // HELLO_RSP + 3 段数据 + DONE_REQ
    QCOMPARE(t.writes[1], QByteArray("0123"));                     // 第一段切片
    QCOMPARE(t.writes[2], QByteArray("456789AB"));                 // 第二段切片
    QCOMPARE(t.writes[3], QByteArray("CDEF"));                     // 64 位偏移段（未截断）
    const QByteArray helloRsp = t.writes[0];
    QCOMPARE(int(helloRsp.at(0)), 0x02);                           // cmd = HELLO_RSP
    QCOMPARE(int(t.writes[4].at(0)), 0x05);                        // 最后一帧 = DONE_REQ（host 主动）
    QCOMPARE(int(t.writes[4].at(4)), 8);                           // 长度字段 = 8（无 payload）
}

// HELLO_RSP 的字节形状（spec §6：HELLO 应答字节）——DONE_REQ 帧的字节形状由
// servesProgrammerInRequestedSlices() 末尾两条断言覆盖，此处不重复
void TestEdlSahara::sendsWellFormedHelloResponse()
{
    edl::MockEdlTransport t;
    t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0})
            << saharaFrame(edl::SAHARA_END_OF_IMAGE, {0, 0})
            << saharaFrame(edl::SAHARA_DONE_RSP, {});

    QString err;
    QVERIFY2(edl::saharaLoadProgrammer(t, QByteArray("0123456789ABCDEF"), &err), qPrintable(err));
    QCOMPARE(t.writes.size(), 2);              // HELLO_RSP + DONE_REQ

    const QByteArray hello = t.writes.at(0);
    QCOMPARE(hello.size(), 0x30);              // 8 头 + 10×u32（bkerler sahara.py:105-109）
    QCOMPARE(le32(hello, 0), quint32(0x02));   // cmd = HELLO_RSP
    QCOMPARE(le32(hello, 4), quint32(0x30));   // 总长字段
    QCOMPARE(le32(hello, 8), quint32(2));      // version = 设备上报版本（v2）
    QCOMPARE(le32(hello, 12), quint32(1));     // version_supported = 兼容到 v1
    QCOMPARE(le32(hello, 16), quint32(0));     // cmd_packet_length
    QCOMPARE(le32(hello, 20), quint32(0));     // mode = SAHARA_MODE_IMAGE_TX_PENDING
    QCOMPARE(le32(hello, 24), quint32(0));     // reserved[0]
}

void TestEdlSahara::failsWhenDeviceNeverRequests()
{
    edl::MockEdlTransport t;                                       // 空队列 = 读超时
    QString err;
    QVERIFY(!edl::saharaLoadProgrammer(t, QByteArray("x"), &err, 10));
    QVERIFY(!err.isEmpty());                                        // 中文文案，含"未收到"
    QVERIFY2(err.contains(QStringLiteral("未收到")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("HELLO")), qPrintable(err));  // 带阶段名
    QCOMPARE(t.writes.size(), 0);                                   // 握手未成：一个字节都不发
}

// 数据面超时（握手后再无请求）：错误带阶段名，且未收到 END_OF_IMAGE 就不许发 DONE_REQ
void TestEdlSahara::failsWhenDeviceGoesSilentAfterHello()
{
    edl::MockEdlTransport t;
    t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0});   // 之后队列空 = 读超时
    QString err;
    QVERIFY(!edl::saharaLoadProgrammer(t, QByteArray("0123456789ABCDEF"), &err));
    QVERIFY2(err.contains(QStringLiteral("Sahara 阶段读取设备请求失败")), qPrintable(err));
    QCOMPARE(t.writes.size(), 1);                                   // 只有 HELLO_RSP
}

// END_OF_IMAGE 带非 SUCCESS 状态 → 失败（edl_handler.cpp（重写前 515cc57）:394-408 同判定）
void TestEdlSahara::failsOnEndOfImageErrorStatus()
{
    edl::MockEdlTransport t;
    t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0})
            << saharaFrame(edl::SAHARA_END_OF_IMAGE, {0, 0x0B});    // status=0x0B（NAK）
    QString err;
    QVERIFY(!edl::saharaLoadProgrammer(t, QByteArray("0123456789ABCDEF"), &err));
    QVERIFY2(err.contains(QStringLiteral("programmer 传输失败")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("0x0b")), qPrintable(err));
    QCOMPARE(t.writes.size(), 1);                                   // 失败路径不发 DONE_REQ
}

// 半帧重组：真机 bulk IN 可能一次只回半帧（PacketReader 累积到帧头声明的总长才解析）
void TestEdlSahara::handlesHelloSplitAcrossReads()
{
    edl::MockEdlTransport t;
    const QByteArray hello = saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0});
    t.reads << hello.left(5) << hello.mid(5)                        // 先 5 字节：连 8 字节帧头都不够
            << saharaFrame(edl::SAHARA_END_OF_IMAGE, {0, 0})
            << saharaFrame(edl::SAHARA_DONE_RSP, {});
    QString err;
    QVERIFY2(edl::saharaLoadProgrammer(t, QByteArray("0123456789ABCDEF"), &err), qPrintable(err));
    QCOMPARE(t.writes.size(), 2);                                   // HELLO_RSP + DONE_REQ
    QCOMPARE(le32(t.writes.at(0), 0), quint32(edl::SAHARA_HELLO_RSP));
}

// 既有缺陷回归（edl_handler.cpp（重写前 515cc57）:302-312 把 READ_DATA_64 的 offset 截成 quint32）：
// 偏移 4 GiB 截断后折回 0 —— 截断实现会"成功"回吐 prog[0..4)="0123"，正确实现必须判越界。
// 正因为折回值落在有效区间内，两条路径才可区分（无需 4 GiB 缓冲）。
void TestEdlSahara::doesNotTruncate64BitOffset()
{
    edl::MockEdlTransport t;
    const QByteArray prog("0123456789ABCDEF");
    const quint64 hugeOffset = 0x1'0000'0000ull;   // 4 GiB = 4294967296（lead 指定值）
    t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0})
            << saharaReadData64Frame(0, hugeOffset, 4)
            << saharaFrame(edl::SAHARA_END_OF_IMAGE, {0, 0})
            << saharaFrame(edl::SAHARA_DONE_RSP, {});

    QString err;
    QVERIFY(!edl::saharaLoadProgrammer(t, prog, &err));
    QVERIFY2(err.contains(QStringLiteral("越界")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("4294967296")), qPrintable(err));   // 偏移按 64 位完整打印
    QCOMPARE(t.writes.size(), 1);                // 只有 HELLO_RSP：越界请求一个字节都不回吐
    QCOMPARE(int(t.writes.at(0).at(0)), 0x02);
}

// 同上，长度字段的 64 位截断（4 GiB 截断后为 0，截断实现会"成功"回吐空切片并继续跑完 DONE）
void TestEdlSahara::doesNotTruncate64BitLength()
{
    edl::MockEdlTransport t;
    const QByteArray prog("0123456789ABCDEF");
    const quint64 hugeLength = 0x100000000ull;   // 4 GiB = 4294967296
    t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0})
            << saharaReadData64Frame(0, 0, hugeLength)
            << saharaFrame(edl::SAHARA_END_OF_IMAGE, {0, 0})
            << saharaFrame(edl::SAHARA_DONE_RSP, {});

    QString err;
    QVERIFY(!edl::saharaLoadProgrammer(t, prog, &err));
    QVERIFY2(err.contains(QStringLiteral("越界")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("4294967296")), qPrintable(err));
    QCOMPARE(t.writes.size(), 1);
}

// ---- 写失败分支（PB-A2）：4 个写调用点各自的失败文案 ----
// 写路径有 4 个**可独立失败**的调用点：HELLO_RSP（sendHelloResponse）／programmer 数据
// （serveProgrammerChunk，READ_DATA 与 READ_DATA_64 两条分支共用）／DONE_REQ（sendDoneAndWait）。
// 三者都经 IEdlTransport::write 上报，失败文案必须点明**是哪一段**发的（否则用户只看到
// "注入的写失败"，无法定位卡在握手/数据/收尾的哪一步）。mock 的 failWriteAt 此前只被会话层用例
// 用过（test_edl_session.cpp），Sahara 侧一处未用 ⇒ 这几条分支没有回归保护。

// 调用点 ①：第 0 次写（HELLO_RSP）失败 → 带阶段名，且一个字节都没发出去
void TestEdlSahara::failsWhenHelloResponseWriteFails()
{
    edl::MockEdlTransport t;
    t.failWriteAt = 0;                                 // 第一次写就失败
    t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0});
    QString err;
    QVERIFY(!edl::saharaLoadProgrammer(t, QByteArray("0123456789ABCDEF"), &err));
    QVERIFY2(err.contains(QStringLiteral("HELLO_RSP 发送失败")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("注入的写失败")), qPrintable(err));   // 传输层原文保留
    QCOMPARE(t.writes.size(), 0);
}

// 调用点 ②/③：programmer 数据写失败（READ_DATA 与 READ_DATA_64 两条分支各一次；
// 同一条文案，但调用点不同 —— 只测其一无法排除"另一条分支没接错误"）
void TestEdlSahara::failsWhenProgrammerWriteFails()
{
    const QByteArray prog("0123456789ABCDEF");
    {   // READ_DATA（32 位）分支
        edl::MockEdlTransport t;
        t.failWriteAt = 1;                             // 第 0 次(HELLO_RSP)成功，数据段失败
        t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0})
                << saharaFrame(edl::SAHARA_READ_DATA, {0, 0, 4, 0});
        QString err;
        QVERIFY(!edl::saharaLoadProgrammer(t, prog, &err));
        QVERIFY2(err.contains(QStringLiteral("发送 programmer 数据失败")), qPrintable(err));
        QVERIFY2(err.contains(QStringLiteral("注入的写失败")), qPrintable(err));
        QCOMPARE(t.writes.size(), 1);                  // 只有 HELLO_RSP
        QCOMPARE(int(t.writes.at(0).at(0)), 0x02);
    }
    {   // READ_DATA_64 分支（同一 helper，但走 64 位取值路径）
        edl::MockEdlTransport t;
        t.failWriteAt = 1;
        t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0})
                << saharaReadData64Frame(0, 4, 4);
        QString err;
        QVERIFY(!edl::saharaLoadProgrammer(t, prog, &err));
        QVERIFY2(err.contains(QStringLiteral("发送 programmer 数据失败")), qPrintable(err));
        QCOMPARE(t.writes.size(), 1);
    }
}

// 调用点 ④：DONE_REQ 写失败（数据都发完了，收尾帧发不出去）—— 必须报 DONE_REQ 而不是前面任一段
void TestEdlSahara::failsWhenDoneReqWriteFails()
{
    edl::MockEdlTransport t;
    t.failWriteAt = 2;                                 // HELLO_RSP + 1 段数据之后
    t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0})
            << saharaFrame(edl::SAHARA_READ_DATA, {0, 0, 4, 0})
            << saharaFrame(edl::SAHARA_END_OF_IMAGE, {0, 0});
    QString err;
    QVERIFY(!edl::saharaLoadProgrammer(t, QByteArray("0123456789ABCDEF"), &err));
    QVERIFY2(err.contains(QStringLiteral("DONE_REQ 发送失败")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("注入的写失败")), qPrintable(err));
    QCOMPARE(t.writes.size(), 2);                      // HELLO_RSP + 数据（DONE_REQ 未记入）
    QCOMPARE(t.writes.at(1), QByteArray("0123"));
}

// ---- 一次 read 含多帧：余量留给下一帧（PB-A3）----
// PacketReader 的跨读缓冲（sahara.cpp:97-104）在"一次 bulk IN 同时给了两帧"时**不再向传输层要数据**，
// 直接从 m_buf 切出第二帧。既有用例只覆盖了"半帧跨两次 read"的反方向（handlesHelloSplitAcrossReads），
// 多帧同读这条路径没有测试 —— 而真机 bulk IN 一次给多帧是常态（帧都很小，见 kReadChunkBytes 注释）。
// 断言两层：a) 第二帧被正确解析（否则会去读传输层、把 DONE_RSP 当 END_OF_IMAGE 用 → 流程失败）；
// b) 传输层**只被读了 2 次**（HELLO+END_OF_IMAGE 一读、DONE_RSP 一读），即第二帧确实取自缓冲。
void TestEdlSahara::parsesSecondFrameFromSameRead()
{
    edl::MockEdlTransport t;
    t.reads << (saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0})
                + saharaFrame(edl::SAHARA_END_OF_IMAGE, {0, 0}))    // ← 一次 read 两帧
            << saharaFrame(edl::SAHARA_DONE_RSP, {});

    QString err;
    QVERIFY2(edl::saharaLoadProgrammer(t, QByteArray("0123456789ABCDEF"), &err), qPrintable(err));
    QCOMPARE(t.writes.size(), 2);                                   // HELLO_RSP + DONE_REQ
    QCOMPARE(int(t.writes.at(0).at(0)), int(edl::SAHARA_HELLO_RSP));
    QCOMPARE(int(t.writes.at(1).at(0)), int(edl::SAHARA_DONE_REQ));
    QCOMPARE(t.calls.count(QStringLiteral("read")), 2);              // 第二帧来自缓冲，不再读传输层
}

// 设备直接宣告完成（sahara.cpp:280-284 的 CMD_READY / DONE_RSP 分支）：多阶段/XML 配置流程里设备
// 不需要我们的 DONE_REQ 就返回成功（bkerler sahara.py:741；既有 edl_handler.cpp:409-415 同）。
// 这条 `return true` 此前无用例经过 —— 若被误改成"继续等下一帧"，真机会卡到超时。
void TestEdlSahara::stopsOnDeviceDeclaredCompletion()
{
    for (const quint32 cmd : {quint32(edl::SAHARA_CMD_READY), quint32(edl::SAHARA_DONE_RSP)}) {
        edl::MockEdlTransport t;
        t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0})
                << saharaFrame(cmd, {});
        QString err;
        QVERIFY2(edl::saharaLoadProgrammer(t, QByteArray("0123456789ABCDEF"), &err), qPrintable(err));
        QCOMPARE(t.writes.size(), 1);            // 只有 HELLO_RSP：设备已宣告完成，不再发 DONE_REQ
        QCOMPARE(int(t.writes.at(0).at(0)), int(edl::SAHARA_HELLO_RSP));
    }
}

// HELLO_RSP 的 version 字段 = 设备上报值；设备报 0（异常/空字段）才退回主机 v2
// （sahara.cpp:236-237 的三元；既有 edl_handler.cpp:549 与 bkerler sahara.py:656 都传设备版本）。
// 两个方向都要钉：3（非默认值，证明是"回设备值"而不是写死 2）与 0（证明兜底生效）。
void TestEdlSahara::echoesDeviceVersionWithFallback()
{
    const auto helloVersionField = [](quint32 deviceVersion) {
        edl::MockEdlTransport t;
        t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {deviceVersion, 1, 0, 0})
                << saharaFrame(edl::SAHARA_CMD_READY, {});
        QString err;
        if (!edl::saharaLoadProgrammer(t, QByteArray("0123456789ABCDEF"), &err))
            return quint32(0xFFFFFFFFu);         // 失败哨兵：调用点断言会立刻看出来
        return le32(t.writes.at(0), 8);          // HELLO_RSP 的 version 字段（+8）
    };
    QCOMPARE(helloVersionField(3), quint32(3));  // ≥1：回设备上报值
    QCOMPARE(helloVersionField(0), quint32(2));  // 0：退回主机 kHostVersion
}

QTEST_APPLESS_MAIN(TestEdlSahara)
#include "test_edl_sahara.moc"
