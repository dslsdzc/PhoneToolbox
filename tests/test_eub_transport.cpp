// tests/test_eub_transport.cpp
//
// EUB 传输层的**离线可测面**：只有纯函数（设备判据、超时换算、文案）。
// 真机路径（open/描述符解析/claim/bulk 读写）本机无设备，**未验证**（facts §F1）。
#include <QtTest>
#include "core/eub/eub_libusb_transport.h"

using eub::LibusbEubTransport;

class TestEubTransport : public QObject
{
    Q_OBJECT

private slots:
    // facts §A1：VID 0x04E8 / PID 0x1234，全 SoC 一致
    void isEubDeviceMatchesExactIds()
    {
        QVERIFY(LibusbEubTransport::isEubDevice(0x04E8, 0x1234));
    }

    void isEubDeviceRejectsNeighbours()
    {
        QVERIFY(!LibusbEubTransport::isEubDevice(0x04E8, 0x1233));
        // Heimdall 的三个老 Odin PID 是**下载模式**，不是 EUB（facts §E2 的邻接风险）
        QVERIFY(!LibusbEubTransport::isEubDevice(0x04E8, 0x6601));
        QVERIFY(!LibusbEubTransport::isEubDevice(0x04E8, 0x685D));
    }

    void isEubDeviceRejectsOtherVendors()
    {
        QVERIFY(!LibusbEubTransport::isEubDevice(0x18D1, 0x1234));  // Google VID、同 PID
        QVERIFY(!LibusbEubTransport::isEubDevice(0x0E8D, 0x0003));  // MTK BROM
    }

    void effectiveTimeoutNeverZero()   // libusb 的 0 = 无限等待，见 odin 同款红线
    {
        QCOMPARE(LibusbEubTransport::effectiveTimeoutMs(0), 1);
        QCOMPARE(LibusbEubTransport::effectiveTimeoutMs(-7), 1);
        QCOMPARE(LibusbEubTransport::effectiveTimeoutMs(50), 50);
    }

    void noDeviceErrorMentionsIds()
    {
        const QString msg = LibusbEubTransport::noDeviceError();
        QVERIFY(msg.contains(QStringLiteral("04e8")) || msg.contains(QStringLiteral("0x04e8")));
        QVERIFY(msg.contains(QStringLiteral("1234")));
    }

    void emptyWriteIsRejectedEvenWhenClosed()   // EUB 无 ZLP 语义：空帧只可能是调用方 bug（fail-closed）
    {
        LibusbEubTransport t;                   // 默认构造：未 open，且**不触碰 USB 栈**
        QString err;
        QVERIFY(!t.writeBulk(QByteArray(), &err));
        QVERIFY(!err.isEmpty());
        // 光查"有 error"钉不住这条判据：把空帧检查放回 !m_dev 守卫**之后**，本用例照样全绿
        // （两条路都是 false + 非空 error，离线复现过）。必须断言命中的是**空帧**分支。
        QVERIFY(err.contains(QStringLiteral("空帧")));
    }
};

QTEST_APPLESS_MAIN(TestEubTransport)
#include "test_eub_transport.moc"
