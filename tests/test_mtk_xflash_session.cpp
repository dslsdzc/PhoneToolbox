// tests/test_mtk_xflash_session.cpp
//
// MTK XFlash 帧层（Phase D2 Task 2）：12B 帧 / status 判定 / send_param 0x200 分块 /
// 0x6781 一次 16 字节 ack / send_data 按 wMaxPacketSize 分块。
//
// 夹具约定（**先读再改**）：默认 `IBromUsb::readExact` 是"单次读 + 严格长度"（`mtk_brom.cpp:219-231`），
// 本层先读 12B 帧头、再读载荷 —— **每次 readExact 消耗一笔队列项**。把整帧塞成一笔会让"读头"吃掉载荷、
// 后续读全部错位（D1 的 LEGACY 通道是逐字节 read()，不受此约束）。负向用例同理：要"短读失败"就少给字节。
#include <QtTest>
#include <QByteArray>

#include "core/modes/mtk_xflash_session.h"
#include "core/modes/mtk_brom.h"

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
// ⚠️ **一帧应答 = 两笔队列项**：12B 帧头 + 载荷（见文件头夹具约定）。负向用例要"短读失败"就少给字节。
QList<QByteArray> frameReads(quint32 dt, const QByteArray &payload)
{
    return {le32(0xFEEEEEEF) + le32(dt) + le32(quint32(payload.size())), payload};
}
QList<QByteArray> statusReads(quint32 code)   // length==4 → <I
{
    return frameReads(1, le32(code));
}
} // namespace

class TestMtkXflashSession : public QObject
{
    Q_OBJECT
private slots:
    void xsendWritesHeaderThenPayload();
    void ackUsesSixteenByteWriteFor6781Only();
    void statusParsesByLength();
    void statusTreatsMagicAsSuccess();
    void sendParamChunksAt0x200AndFailsOnEmiVersionMismatch();
    void sendParamRejectsHardErrorCodes();
    void sendDataChunksByMaxPacketSize();
    void readStatusRejectsShortPayloadAndBadMagic();
};

// 帧头一次写、载荷第二次写（铁律 3；xflash_lib.py:112-115）
void TestMtkXflashSession::xsendWritesHeaderThenPayload()
{
    MockUsbChannel m;
    mtkbrom::XFlashSession s(&m, 0x6765);
    QString err;
    QVERIFY2(s.xsend(QByteArray("\x01\x02\x03\x04", 4), &err), qPrintable(err));
    QCOMPARE(m.writeFrames.size(), 2);
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));
    QCOMPARE(m.writeFrames.at(1), QByteArray("\x01\x02\x03\x04", 4));
}

// ack：0x6781 一次 16 字节；其他芯片 12B 头 + 4B 载荷（铁律 4；xflash_lib.py:85-100）
void TestMtkXflashSession::ackUsesSixteenByteWriteFor6781Only()
{
    {
        MockUsbChannel m;
        mtkbrom::XFlashSession s(&m, 0x6781);
        QString err;
        QVERIFY2(s.ack(&err), qPrintable(err));
        QCOMPARE(m.writeFrames.size(), 1);
        QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4) + le32(0));
        QCOMPARE(m.writeFrames.at(0).size(), 16);
    }
    {
        MockUsbChannel m;
        mtkbrom::XFlashSession s(&m, 0x6765);
        QString err;
        QVERIFY2(s.ack(&err), qPrintable(err));
        QCOMPARE(m.writeFrames.size(), 2);
        QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));
        QCOMPARE(m.writeFrames.at(1), le32(0));
    }
}

// status 按 length 分支（铁律 6；xflash_lib.py:138-158）：
// length==2 → <H，**为 0 才算成功**；readStatus 的返回值只表示"帧读取/解析成功"，非 0 状态码交 checkStatus 判定
void TestMtkXflashSession::statusParsesByLength()
{
    {
        MockUsbChannel m;
        m.reads << frameReads(1, QByteArray("\x00\x00", 2));
        mtkbrom::XFlashSession s(&m, 0x6765);
        quint32 code = 0xDEAD;
        QString err;
        QVERIFY2(s.readStatus(code, &err), qPrintable(err));
        QCOMPARE(code, quint32(0));
    }
    {
        MockUsbChannel m;
        m.reads << frameReads(1, QByteArray("\x50\x00", 2));     // <H 小端 = 0x0050
        mtkbrom::XFlashSession s(&m, 0x6765);
        quint32 code = 0;
        QVERIFY(s.readStatus(code, nullptr));
        QCOMPARE(code, quint32(0x0050));                    // 非 0 = 错误码（由 checkStatus 查表）
    }
    {
        // 4 字节载荷：readStatus 只回**原始码值**（不归一化、不判定）—— 判定是 checkStatus 的职责。
        // 0 / 0xC0040050 / 其它码 三条分支必须各自可判别，此条钉住"原始码值不被提前吞掉"。
        MockUsbChannel m;
        m.reads << statusReads(mtkbrom::kXEmitVersionMismatch);
        mtkbrom::XFlashSession s(&m, 0x6765);
        quint32 code = 0;
        QVERIFY(s.readStatus(code, nullptr));
        QCOMPARE(code, mtkbrom::kXEmitVersionMismatch);
    }
}

// length==4 且值 == magic 视为成功（铁律 6）
void TestMtkXflashSession::statusTreatsMagicAsSuccess()
{
    MockUsbChannel m;
    m.reads << statusReads(0xFEEEEEEF);
    mtkbrom::XFlashSession s(&m, 0x6765);
    quint32 code = 1;
    QString err;
    QVERIFY2(s.readStatus(code, &err), qPrintable(err));
    QCOMPARE(code, quint32(0));
}

// send_param：0x200 分块 + 最后读一次 status；0xC0040050 **判失败**
// （上游 XFL:180-188 只跳过错误打印与 sys.exit，但**仍以失败返回**；显式 preloader 路径
//   XFL:1147-1149 据此中止整链 —— 控制方裁决 2026-09-16 更正原"容忍"读法）
void TestMtkXflashSession::sendParamChunksAt0x200AndFailsOnEmiVersionMismatch()
{
    {
        MockUsbChannel m;
        m.reads << statusReads(mtkbrom::kXEmitVersionMismatch);
        mtkbrom::XFlashSession s(&m, 0x6765);
        const QByteArray big(0x300, '\x5A');                // 0x200 + 0x100 两块
        QString err;
        QVERIFY2(!s.sendParam({big}, &err), "0xC0040050 必须判为失败");
        QVERIFY(err.contains(QStringLiteral("0xC0040050")));   // 文案可诊断（上游此处不打印）
        QVERIFY(err.contains(QStringLiteral("EMI 版本不匹配")));   // 只属于本分支的措辞 token（见硬错误码用例注释）
        QCOMPARE(m.writeFrames.size(), 3);                  // 帧头 + 块1 + 块2（先写完帧再读 status）
        QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(0x300));
        QCOMPARE(m.writeFrames.at(1).size(), 0x200);
        QCOMPARE(m.writeFrames.at(2).size(), 0x100);
    }
    {
        MockUsbChannel m;
        m.reads << statusReads(0);
        mtkbrom::XFlashSession s(&m, 0x6765);
        // 两个参数**尺寸不同**：帧头长度必须各取自本参数 —— 若某变异用第一个参数的长度写所有帧
        // （或只写一帧头），下面逐帧比对就会红
        const QByteArray p1 = le32(0);                      // 4B
        const QByteArray p2(6, '\x22');                     // 6B
        QString err;
        QVERIFY2(s.sendParam({p1, p2}, &err), qPrintable(err));   // 两个参数 = 两个帧
        QCOMPARE(m.writeFrames.size(), 4);                  // 帧1头 + 帧1载荷 + 帧2头 + 帧2载荷
        QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(4));
        QCOMPARE(m.writeFrames.at(1), p1);
        QCOMPARE(m.writeFrames.at(2), le32(0xFEEEEEEF) + le32(1) + le32(6));
        QCOMPARE(m.writeFrames.at(3), p2);
    }
}

// send_param：硬错误码（0xC0020053 anti-rollback / 0xC0020004 DL forbidden）→ 明确失败 +
// **各自带自己的文案**（与 0xC0040050 那条分支可判别；上游这两条是 sys.exit(1)，我们不改上层语义）
void TestMtkXflashSession::sendParamRejectsHardErrorCodes()
{
    {
        MockUsbChannel m;
        m.reads << statusReads(0xC0020053);
        mtkbrom::XFlashSession s(&m, 0x6765);
        QString err;
        QVERIFY(!s.sendParam({le32(0)}, &err));
        QVERIFY(err.contains(QStringLiteral("0xC0020053")));
    }
    {
        MockUsbChannel m;
        m.reads << statusReads(0xC0020004);
        mtkbrom::XFlashSession s(&m, 0x6765);
        QString err;
        QVERIFY(!s.sendParam({le32(0)}, &err));
        QVERIFY(err.contains(QStringLiteral("0xC0020004")));
    }
    {
        // 判别力：0xC0040050 的**判定**（失败）与**措辞**各自钉住 ——
        //   判定：!sendParam；措辞：通用文案也会插值码值，故仅 contains("0xC0040050") **分不出**
        //   "独立分支"与"并入通用文案"，靠只属于该分支的 token "EMI 版本不匹配" 才能钉住措辞。
        MockUsbChannel m;
        m.reads << statusReads(mtkbrom::kXEmitVersionMismatch);
        mtkbrom::XFlashSession s(&m, 0x6765);
        QString err;
        QVERIFY(!s.sendParam({le32(0)}, &err));
        QVERIFY(err.contains(QStringLiteral("0xC0040050")));
        QVERIFY(err.contains(QStringLiteral("EMI 版本不匹配")));
        QVERIFY(!err.contains(QStringLiteral("0xC0020053")));
    }
}

// send_data：帧头 + 按 maxPacketSize 分块 + 读一次 status（xflash_lib.py:272-286）
void TestMtkXflashSession::sendDataChunksByMaxPacketSize()
{
    MockUsbChannel m;
    m.pktSize = 0x100;
    m.reads << statusReads(0);
    mtkbrom::XFlashSession s(&m, 0x6765);
    const QByteArray data(0x250, '\x7E');
    QString err;
    QVERIFY2(s.sendData(data, &err), qPrintable(err));
    QCOMPARE(m.writeFrames.size(), 4);                      // 帧头 + 0x100 + 0x100 + 0x50
    QCOMPARE(m.writeFrames.at(0), le32(0xFEEEEEEF) + le32(1) + le32(0x250));
    QCOMPARE(m.writeFrames.at(1).size(), 0x100);
    QCOMPARE(m.writeFrames.at(3).size(), 0x50);
}

// 设备输入面的两条负向路径 —— 缺任一条都会把畸形帧当成功：
//   ① 载荷长度 3（既非 2 也非 4）→ 必须拒绝，否则 `leToU32(payload, 0)` 会越界读 payload.at(3)
//   ② 头 magic 不符 → 必须拒绝（上游 xread 也是 magic 不符即错，XFL:124-126）
void TestMtkXflashSession::readStatusRejectsShortPayloadAndBadMagic()
{
    {
        MockUsbChannel m;
        m.reads << frameReads(1, QByteArray("\x11\x22\x33", 3));   // length=3
        mtkbrom::XFlashSession s(&m, 0x6765);
        quint32 code = 0xDEAD;
        QString err;
        QVERIFY(!s.readStatus(code, &err));
        QVERIFY(err.contains(QStringLiteral("状态帧长度异常")));
    }
    {
        MockUsbChannel m;
        const QByteArray badHeader = le32(0xDEADBEEF) + le32(1) + le32(4);   // magic 错、长度 4
        m.reads << badHeader << le32(0);                     // 载荷项不会被读（magic 即拒）
        mtkbrom::XFlashSession s(&m, 0x6765);
        quint32 code = 0xDEAD;
        QString err;
        QVERIFY(!s.readStatus(code, &err));
        QVERIFY(err.contains(QStringLiteral("magic 不符")));
    }
}
QTEST_APPLESS_MAIN(TestMtkXflashSession)
#include "test_mtk_xflash_session.moc"
