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

QTEST_APPLESS_MAIN(TestEdlSahara)
#include "test_edl_sahara.moc"
