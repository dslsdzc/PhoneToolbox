// tests/test_odin_libusb_transport.cpp
//
// Phase C Task 7：libusb 真机传输（src/core/odin/odin_libusb_transport.cpp）的**离线**用例。
//
// 诚实边界（写在用例头上，免得读者误以为真机行为已被验证）：USB 枚举/claim/端点发现/短写/
// ZLP/超时的**真机时序**无法离线验证（本阶段无真机）。本文件只钉四类不碰 USB 栈的事：
//   ① 设备身份判据（**纯函数** `isOdinDevice`/`fallbackPids`）—— 与 Task 8 的设备检测共用同一
//      判据，判据本身三方对照见 odin_libusb_transport.h 头注释；
//   ② `noDeviceError` 的中文文案（要能直接落 UI 诊断"插的是不是别的模式"）；
//   ③ `effectiveTimeoutMs` 的换算（libusb 的 timeout=0 是无限等待，绝不能透传）；
//   ④ 未打开句柄时 write/read/close 的行为（构造/close 不触碰 USB 栈，故无需真机、也无需 mock）。
// 不测：open()/枚举/候选接口选择/claim/短写拒绝的真机路径 —— 见 phaseC-task-7-report.md §边界。
#include <QtTest>
#include <QList>

#include "core/odin/odin_libusb_transport.h"

class TestOdinLibusbTransport : public QObject
{
    Q_OBJECT
private slots:
    void matchesByInterfaceClass();
    void fallsBackToLegacyPids();
    void rejectsOtherVendorsAndMtpOnly();
    void noDeviceErrorMentionsVidAndClasses();
    void zeroTimeoutBecomesPollTimeout();
    void closedTransportFailsWithoutTouchingDevice();
};

using namespace odin;

void TestOdinLibusbTransport::matchesByInterfaceClass()
{
    // 现代机型：VID 0x04E8 + CDC_DATA 接口 + 批量端点 → 认（与 PID 无关）
    QVERIFY(LibusbOdinTransport::isOdinDevice(0x04E8, 0x6860, {0x0A}, true));
    QVERIFY(LibusbOdinTransport::isOdinDevice(0x04E8, 0x1234, {0x03, 0x0A}, true));
    // 类对但没有批量端点 → 不认（CDC_DATA 控制接口不是下载模式）
    QVERIFY(!LibusbOdinTransport::isOdinDevice(0x04E8, 0x6860, {0x0A}, false));
    // 类不对且 PID 不在兜底表 → 不认
    QVERIFY(!LibusbOdinTransport::isOdinDevice(0x04E8, 0x6860, {0x06, 0xFF}, true));
}

void TestOdinLibusbTransport::fallsBackToLegacyPids()
{
    // 描述符读不到（classes 为空）时靠老 PID 兜底（Heimdall BridgeManager.h:71-78）
    for (quint16 pid : LibusbOdinTransport::fallbackPids())
        QVERIFY(LibusbOdinTransport::isOdinDevice(0x04E8, pid, {}, false));
    QCOMPARE(LibusbOdinTransport::fallbackPids().size(), 3);
    QVERIFY(LibusbOdinTransport::fallbackPids().contains(0x685D));
    // 老 PID + 类不对（例如被系统当成 MTP 枚举）仍认 —— 兜底表的语义就是"认这个 PID"
    QVERIFY(LibusbOdinTransport::isOdinDevice(0x04E8, 0x6601, {0x06}, true));
}

void TestOdinLibusbTransport::rejectsOtherVendorsAndMtpOnly()
{
    QVERIFY(!LibusbOdinTransport::isOdinDevice(0x05C6, 0x9008, {0x0A}, true));   // 高通 EDL 不是三星
    QVERIFY(!LibusbOdinTransport::isOdinDevice(0x18D1, 0x4EE7, {0x0A}, true));
    // 三星手机的 MTP 模式（VID 0x04E8 / 类 0x06 / PID 不在兜底表）→ 不认
    QVERIFY(!LibusbOdinTransport::isOdinDevice(0x04E8, 0x6860, {0x06}, true));
}

void TestOdinLibusbTransport::noDeviceErrorMentionsVidAndClasses()
{
    const QString msg = LibusbOdinTransport::noDeviceError();
    QVERIFY(msg.contains(QStringLiteral("04e8")));
    QVERIFY(msg.contains(QStringLiteral("CDC")) || msg.contains(QStringLiteral("0a")));
}

void TestOdinLibusbTransport::zeroTimeoutBecomesPollTimeout()
{
    // ⚠️ libusb 的 timeout=0 是**无限等待**（libusb sync.c "For an unlimited timeout, use value 0"），
    // 而 IOdinTransport 的 0 = 非阻塞轮询 —— 直接透传会让会话的 drain 在真机上永久阻塞。
    // 与 Phase B 同款红线（src/core/edl/edl_libusb_transport.cpp 的 effectiveTimeoutMs）。
    QCOMPARE(LibusbOdinTransport::effectiveTimeoutMs(0), 1);
    QCOMPARE(LibusbOdinTransport::effectiveTimeoutMs(-5), 1);
    QCOMPARE(LibusbOdinTransport::effectiveTimeoutMs(3000), 3000);
}

// 任务书 5 例之外的**唯一**新增（reason 写在这里，防审查以为是漏读任务书）：本实现里有"未打开
// 直接拒"的分支（EDL 同款：write/read 不得静默失败），不留成无测试覆盖的行为；且它同时钉住
// "构造/close 不触碰 USB 栈"（未打开时是纯内存对象，可无真机运行）。对照 Phase B 的
// tests/test_edl_libusb_transport.cpp::closedTransportFailsWithoutTouchingDevice。
void TestOdinLibusbTransport::closedTransportFailsWithoutTouchingDevice()
{
    LibusbOdinTransport t;                          // 构造只初始化成员，不做 libusb_init

    QString err;
    QVERIFY(!t.write(QByteArray("x"), &err));
    QVERIFY2(err.contains(QStringLiteral("未打开")), qPrintable(err));

    err.clear();
    QVERIFY(t.read(16, 0, &err).isEmpty());         // 轮询：空返回……
    QVERIFY2(err.contains(QStringLiteral("未打开")), qPrintable(err));   // ……但"未打开"是错误

    t.close();                                      // 幂等：未打开时反复调用不崩
    t.close();
}

QTEST_APPLESS_MAIN(TestOdinLibusbTransport)
#include "test_odin_libusb_transport.moc"
