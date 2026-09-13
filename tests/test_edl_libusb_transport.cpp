// tests/test_edl_libusb_transport.cpp
//
// Phase B Task 7：libusb 传输层（src/core/edl/edl_libusb_transport.cpp）的**离线**用例。
//
// 诚实边界（写在用例头上，免得读者误以为真机行为已被验证）：真机的端点/时序/重枚举/ZLP
// **无法离线验证**。本文件只钉三件不碰 USB 栈的事：
//   ① VID/PID 匹配表（哪个 PID 属哪个阶段）——搬自 src/core/modes/edl_handler.cpp（重写前 515cc57）:10-21 的既有经验值；
//   ② 每阶段的接口号/端点号（同一处搬来）——重构中最容易被悄悄改错、真机上表现为"设备没反应"；
//   ③ 中文错误文案 + **未打开句柄**时 7 个方法的行为（构造/close/read/write 不得触碰 USB 栈，
//      故无需真机、也无需 mock）。
// 不测：open()/枚举/claim/重枚举轮询节奏/maxPacketSize 的真机取值 —— 见 task-7-report.md。
#include <QtTest>
#include "core/edl/edl_libusb_transport.h"

class TestEdlLibusbTransport : public QObject
{
    Q_OBJECT
private slots:
    void recognizesKnownEdlIds();
    void rejectsNonEdlIds();
    void matchesPidPerStage();
    void pinsStageInterfaceAndEndpoints();
    void buildsChineseNoDeviceError();
    void mapsZeroTimeoutToPollInsteadOfInfinite();
    void closedTransportFailsWithoutTouchingDevice();
};

// brief 点名的三条：0x05C6 家族的 9008/900E/9025 都是 EDL 身份（9008 = Sahara、9025 = Firehose、
// 900E 两种阶段都出现）
void TestEdlLibusbTransport::recognizesKnownEdlIds()
{
    QVERIFY(edl::LibusbEdlTransport::isEdlId(0x05C6, 0x9008));
    QVERIFY(edl::LibusbEdlTransport::isEdlId(0x05C6, 0x900E));
    QVERIFY(edl::LibusbEdlTransport::isEdlId(0x05C6, 0x9025));
}

// 反例：Google fastboot（0x18D1:0x4EE0）同 PID 不同 VID、同 VID 未知 PID —— 都不得命中，
// 否则 open() 会把非 EDL 设备当 9008 打开（真机上会 claim 错设备）
void TestEdlLibusbTransport::rejectsNonEdlIds()
{
    QVERIFY(!edl::LibusbEdlTransport::isEdlId(0x18D1, 0x4EE0));
    QVERIFY(!edl::LibusbEdlTransport::isEdlId(0x18D1, 0x9008));
    QVERIFY(!edl::LibusbEdlTransport::isEdlId(0x05C6, 0x1234));
    QVERIFY(!edl::LibusbEdlTransport::isEdlId(0x0000, 0x0000));
}

// 阶段相关的 PID 表**不对称**（既有实现即如此）：Sahara 阶段不认 0x9025（那是 programmer 载入后
// 的身份），Firehose 阶段不认 0x9008；0x900E 两阶段都认（部分机型不换 PID，原地切换模式）
void TestEdlLibusbTransport::matchesPidPerStage()
{
    using T = edl::LibusbEdlTransport;
    QVERIFY(T::matchesStage(edl::EdlUsbStage::Sahara, 0x05C6, 0x9008));
    QVERIFY(T::matchesStage(edl::EdlUsbStage::Sahara, 0x05C6, 0x900E));
    QVERIFY(!T::matchesStage(edl::EdlUsbStage::Sahara, 0x05C6, 0x9025));
    QVERIFY(T::matchesStage(edl::EdlUsbStage::Firehose, 0x05C6, 0x9025));
    QVERIFY(T::matchesStage(edl::EdlUsbStage::Firehose, 0x05C6, 0x900E));
    QVERIFY(!T::matchesStage(edl::EdlUsbStage::Firehose, 0x05C6, 0x9008));
    QVERIFY(!T::matchesStage(edl::EdlUsbStage::Sahara, 0x18D1, 0x9008));   // VID 不符
    QVERIFY(!T::matchesStage(edl::EdlUsbStage::Firehose, 0x18D1, 0x9025));
}

// 每阶段的接口号/端点号是**搬来的经验值**（src/core/modes/edl_handler.cpp（重写前 515cc57）:16-21 的
// EDP_OUT/EDP_IN/FH_OUT/FH_IN + claimInterface 的 iface 0/1）。真机正确性离线无法验证，
// 但"不得在重构里悄悄改掉"是可离线钉住的 —— 改错只会表现为真机没反应。
void TestEdlLibusbTransport::pinsStageInterfaceAndEndpoints()
{
    using T = edl::LibusbEdlTransport;
    QCOMPARE(T::interfaceNumber(edl::EdlUsbStage::Sahara), 0);
    QCOMPARE(T::outEndpoint(edl::EdlUsbStage::Sahara), 0x01);
    QCOMPARE(T::inEndpoint(edl::EdlUsbStage::Sahara), 0x82);
    QCOMPARE(T::interfaceNumber(edl::EdlUsbStage::Firehose), 1);
    QCOMPARE(T::outEndpoint(edl::EdlUsbStage::Firehose), 0x02);
    QCOMPARE(T::inEndpoint(edl::EdlUsbStage::Firehose), 0x83);
}

// 找不到设备时的中文文案要能直接落到 UI：带阶段名 + 该阶段期望的 PID（诊断"插的是不是别的模式"）
void TestEdlLibusbTransport::buildsChineseNoDeviceError()
{
    using T = edl::LibusbEdlTransport;
    const QString sahara = T::noDeviceError(edl::EdlUsbStage::Sahara);
    QVERIFY2(sahara.contains(QStringLiteral("未找到")), qPrintable(sahara));
    QVERIFY2(sahara.contains(QStringLiteral("Sahara")), qPrintable(sahara));
    QVERIFY2(sahara.contains(QStringLiteral("9008")), qPrintable(sahara));
    QVERIFY2(!sahara.contains(QStringLiteral("9025")), qPrintable(sahara));

    const QString firehose = T::noDeviceError(edl::EdlUsbStage::Firehose);
    QVERIFY2(firehose.contains(QStringLiteral("Firehose")), qPrintable(firehose));
    QVERIFY2(firehose.contains(QStringLiteral("9025")), qPrintable(firehose));
    QVERIFY2(!firehose.contains(QStringLiteral("9008")), qPrintable(firehose));
}

// ⚠️ 本轮修的真缺陷（离线唯一能钉住的部分）：libusb 的 `timeout=0` 是**无限等待**
// （libusb sync.c "For an unlimited timeout, use value 0"；`libusb_transfer` 结构体字段注释同款），
// 而 IEdlTransport 的 `read(timeoutMs=0)` 契约是**非阻塞轮询**（drain）—— 两者必须换算，
// 否则会话每笔写前的 drain 会在真机上**永久阻塞**（mock 把 0 当轮询，离线用例全绿也发现不了）。
// 换算做成纯函数就是为了让这条修正在离线侧有回归保护。
void TestEdlLibusbTransport::mapsZeroTimeoutToPollInsteadOfInfinite()
{
    using T = edl::LibusbEdlTransport;
    QCOMPARE(T::effectiveTimeoutMs(0), 1);          // 轮询 → 1 ms（bkerler 同款：usblib.py:377-378）
    QCOMPARE(T::effectiveTimeoutMs(-5), 1);         // 负值同样按轮询处理（绝不能变成"无限"）
    QCOMPARE(T::effectiveTimeoutMs(1), 1);
    QCOMPARE(T::effectiveTimeoutMs(1000), 1000);    // 正值原样透传
    QCOMPARE(T::effectiveTimeoutMs(30000), 30000);
}

// 未打开句柄时：7 个方法都不得触碰 USB 栈（构造/lclose/read/write 里不能有 libusb 调用），
// 且必须给出可诊断的中文错误而不是崩/静默
void TestEdlLibusbTransport::closedTransportFailsWithoutTouchingDevice()
{
    edl::LibusbEdlTransport t;                      // 构造只初始化成员，不做 libusb_init
    QVERIFY(!t.isOpen());
    QCOMPARE(t.maxPacketSize(), 0);                 // 包长未知 → 会话据此不发 ZLP（edl_session.cpp 判据）

    QString err;
    QVERIFY(!t.write(QByteArray("x"), &err));
    QVERIFY2(err.contains(QStringLiteral("未打开")), qPrintable(err));

    err.clear();
    QVERIFY(t.read(16, 0, &err).isEmpty());         // 轮询：空返回……
    QVERIFY2(err.contains(QStringLiteral("未打开")), qPrintable(err));   // ……但"未打开"是错误

    t.close();                                      // 幂等：未打开时反复调用不崩
    t.close();
    QVERIFY(!t.isOpen());

    err.clear();
    QVERIFY(t.resetDevice(&err));                   // best-effort 契约：未打开 = 无事可做（true）
}

QTEST_APPLESS_MAIN(TestEdlLibusbTransport)
#include "test_edl_libusb_transport.moc"
