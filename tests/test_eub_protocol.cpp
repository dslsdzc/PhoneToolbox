// tests/test_eub_protocol.cpp
//
// 帧格式的**逐字节**断言。数值全部来自 facts §B3/§B4/§B5：
//   [4B 头字段][u32 LE = 数据长 + 10][数据][2B 尾]
//   ZeroStyle 头 00 00 00 00 / 尾 00 00（exynos-usbdl）；DnwStyle 头 1B 44 4E 57 / 尾 FF FF（hubble）
// 头/尾两字段的**语义未定**（facts §B6：三实现互相矛盾且设备很可能不校验）—— 本文件只断言
// "照抄自哪个实现"，不断言含义。
#include <QtTest>

#include "core/eub/eub_protocol.h"
#include "mock_eub_transport.h"

// 测试侧独立解码（不复用被测模块的原语）：把字节序读反的实现必须能在这里现形（照
// test_odin_protocol.cpp 的 leAt 先例）。
static quint32 le32At(const QByteArray &d, int off)
{
    return quint32(quint8(d.at(off)))
         | (quint32(quint8(d.at(off + 1))) << 8)
         | (quint32(quint8(d.at(off + 2))) << 16)
         | (quint32(quint8(d.at(off + 3))) << 24);
}

class TestEubProtocol : public QObject
{
    Q_OBJECT

private slots:
    void zeroStyleFrameBytes()
    {
        QString err;
        const QByteArray f = eub::buildEubFrame(QByteArray("AB"), eub::zeroStyle(), &err);
        QVERIFY2(!f.isEmpty(), qPrintable(err));
        QCOMPARE(f.size(), 12);                                   // 4 + 4 + 2 + 2
        QCOMPARE(f.left(4), QByteArray::fromHex("00000000"));     // facts §B4 的 exynos-usbdl 值
        QCOMPARE(f.mid(4, 4), QByteArray::fromHex("0c000000"));   // 12 = n(2) + 10，小端
        QCOMPARE(f.mid(8, 2), QByteArray("AB"));
        QCOMPARE(f.right(2), QByteArray::fromHex("0000"));        // facts §B5
    }

    void dnwStyleFrameBytes()
    {
        QString err;
        const QByteArray f = eub::buildEubFrame(QByteArray("AB"), eub::dnwStyle(), &err);
        QVERIFY2(!f.isEmpty(), qPrintable(err));
        QCOMPARE(f.left(4), QByteArray::fromHex("1b444e57"));     // facts §B4 的 hubble 值（ESC "DNW"）
        QCOMPARE(f.mid(4, 4), QByteArray::fromHex("0c000000"));
        QCOMPARE(f.right(2), QByteArray::fromHex("ffff"));        // facts §B5
    }

    void lengthFieldIsPayloadPlusOverheadForLargePayload()
    {
        QString err;
        const QByteArray payload(2 * 1024 * 1024, 'x');
        const QByteArray f = eub::buildEubFrame(payload, eub::dnwStyle(), &err);
        QCOMPARE(f.size(), 4 + 4 + payload.size() + 2);
        const quint32 expect = quint32(payload.size()) + 10u;     // kFrameOverhead
        QByteArray le(4, Qt::Uninitialized);
        for (int i = 0; i < 4; ++i) le[i] = char((expect >> (8 * i)) & 0xFF);
        QCOMPARE(f.mid(4, 4), le);
    }

    // 加固（约束 9；变异证据见报告）：上面三条把 10 写死在自己算的期望里，若实现把
    // kFrameOverhead 改成别的值、同时在 buildEubFrame 里**硬编码** 10，则全部既有 slot
    // 依旧全绿 —— 导出的常量与线上字节脱钩。本条把常量钉回帧字节，并覆盖 1 字节这侧
    // 的边界（空载荷判据的另一侧）。
    void frameLengthFieldTracksExportedOverhead()
    {
        QString err;
        const QByteArray payload(1, 'A');
        const QByteArray f = eub::buildEubFrame(payload, eub::zeroStyle(), &err);
        QVERIFY2(!f.isEmpty(), qPrintable(err));
        QCOMPARE(eub::kFrameOverhead, 10);                        // facts §B3：4 + 4 + 2
        QCOMPARE(le32At(f, 4), quint32(payload.size()) + quint32(eub::kFrameOverhead));
        QCOMPARE(f.mid(8, 1), QByteArray("A"));
        QCOMPARE(f.size(), 4 + 4 + payload.size() + 2);
    }

    void emptyPayloadFails()
    {
        QString err;
        QVERIFY(eub::buildEubFrame(QByteArray(), eub::zeroStyle(), &err).isEmpty());
        QVERIFY(!err.isEmpty());
        // 光查"有 error"钉不住这条判据（T1 审查同款）：若空载荷检查被**风格/尺寸检查**顶掉，
        // 空帧会带着"风格非法"文案返回。必须断言命中的是载荷这条分支。
        QVERIFY(err.contains(QStringLiteral("载荷")));
    }

    void malformedStyleFails()
    {
        QString err;
        eub::EubFrameStyle bad;
        bad.header  = QByteArray(3, '\0');   // 必须 4 字节
        bad.trailer = QByteArray(2, '\0');
        QVERIFY(eub::buildEubFrame(QByteArray("A"), bad, &err).isEmpty());
        QVERIFY(!err.isEmpty());
        QVERIFY(err.contains(QStringLiteral("风格")));

        eub::EubFrameStyle bad2;
        bad2.header  = QByteArray(4, '\0');
        bad2.trailer = QByteArray(1, '\0');  // 必须 2 字节
        QVERIFY(eub::buildEubFrame(QByteArray("A"), bad2, &err).isEmpty());
        QVERIFY(!err.isEmpty());
        QVERIFY(err.contains(QStringLiteral("风格")));
    }

    void sendSegmentWritesExactlyOneFrame()
    {
        eub::MockEubTransport t;
        QString err;
        QVERIFY2(eub::sendSegment(t, QByteArray("AB"), eub::dnwStyle(), &err), qPrintable(err));
        QCOMPARE(t.writes.size(), 1);
        QCOMPARE(t.writes.first(), eub::buildEubFrame(QByteArray("AB"), eub::dnwStyle(), &err));
        QCOMPARE(t.calls, QStringList{QStringLiteral("write")});
    }

    void sendSegmentPropagatesWriteFailure()
    {
        eub::MockEubTransport t;
        t.failWriteAt = 0;
        QString err;
        QVERIFY(!eub::sendSegment(t, QByteArray("AB"), eub::zeroStyle(), &err));
        QVERIFY(!err.isEmpty());
        QCOMPARE(t.writes.size(), 0);
    }

    void sendSegmentRejectsEmptyPayloadBeforeWriting()
    {
        eub::MockEubTransport t;
        QString err;
        QVERIFY(!eub::sendSegment(t, QByteArray(), eub::zeroStyle(), &err));
        QCOMPARE(t.writes.size(), 0);      // fail-closed：不发出空帧
        QCOMPARE(t.calls.size(), 0);
    }

    // 审查修复：上面这条只覆盖了"空载荷"分支；sendSegment 走**风格非法**分支此前无用例
    // （buildEubFrame 那侧有 malformedStyleFails）。
    void sendSegmentRejectsMalformedStyleBeforeWriting()
    {
        eub::MockEubTransport t;
        eub::EubFrameStyle bad;
        bad.header  = QByteArray(3, '\0');     // 必须 4 字节
        bad.trailer = QByteArray(2, '\0');
        QString err;
        QVERIFY(!eub::sendSegment(t, QByteArray("AB"), bad, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(err.contains(QStringLiteral("风格")));   // 命中风格分支，而非空载荷/写失败
        QCOMPARE(t.writes.size(), 0);          // fail-closed：不发出任何字节
        QCOMPARE(t.calls.size(), 0);
    }

    // 审查修复：钉住守卫**顺序** —— 空帧拒绝必须先于注入式失败判定。既有用例从没让两者
    // 同时命中（sendSegmentRejectsEmptyPayloadBeforeWriting 用默认 failWriteAt = -1），
    // 把空帧守卫下移到 failWriteAt 之后曾使全部 slot 仍绿。
    void mockRejectsEmptyWriteBeforeInjectedFailure()   // 钉住守卫顺序：空帧拒绝必须**先于**注入式失败判定
    {
        eub::MockEubTransport t;
        t.failWriteAt = 0;                              // 让注入式失败**同时**命中
        QString err;
        QVERIFY(!t.writeBulk(QByteArray(), &err));
        QVERIFY2(err.contains(QStringLiteral("空帧")), qPrintable(err));   // 必须是空帧拒绝，而不是"注入的写失败"
        QCOMPARE(t.writes.size(), 0);
        QCOMPARE(t.calls.size(), 0);                    // 拒绝路径不记任何东西
    }

    // T1 审查交接要求"后续 mock 同款"：mock 在"未打开 + 空帧"这一格必须与真机同判
    // （真机那条路径见 tests/test_eub_transport.cpp:49-58）。本用例把这条 mock 契约钉住，
    // 免得 Task 6/7 复用时把失败原因误读成设备行为。
    void mockRejectsEmptyFrameLikeDevice()
    {
        eub::MockEubTransport t;
        QString err;
        QVERIFY(!t.writeBulk(QByteArray(), &err));
        QVERIFY(err.contains(QStringLiteral("空帧")));   // 命中空帧分支，而非注入式失败
        QVERIFY(t.writes.isEmpty());                     // 空帧不进写入记录
        QVERIFY(t.calls.isEmpty());                      // 也不留 "write" 调用痕迹
    }
};

QTEST_APPLESS_MAIN(TestEubProtocol)
#include "test_eub_protocol.moc"
