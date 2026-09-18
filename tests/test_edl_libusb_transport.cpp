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
#include <QFile>       // A-40 结构钉：读生产源文件
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
    void switchesAltSettingOnlyWhenNonZero();
    void altSettingFailureNoteNamesInterfaceAndSetting();
    void pinsAltSettingWiringInProductionSource();
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

// A-40：端点取自 `bAlternateSetting != 0` 的那一份时，claim 之后必须显式
// `libusb_set_interface_alt_setting` —— `libusb_claim_interface` 只按**接口号**认领，不会替你
// 选中端点所属的设置；不切则首次 bulk 打在默认设置上（真机表现为设备无响应）。真机上的切换
// 调用要真句柄、离线执行不了，但"何时该切"这条判定可以离线钉住（与 effectiveTimeoutMs 同款）。
void TestEdlLibusbTransport::switchesAltSettingOnlyWhenNonZero()
{
    using T = edl::LibusbEdlTransport;
    QVERIFY(!T::needsAltSettingSwitch(0));    // 默认设置：claim 之后即是它
    QVERIFY(T::needsAltSettingSwitch(1));
    QVERIFY(T::needsAltSettingSwitch(2));
    QVERIFY(T::needsAltSettingSwitch(255));   // bAlternateSetting 是 quint8
}

// 切换失败的说明要能直接落日志：点名接口号、目标 altsetting 与底层错误 —— 真机上"传输打在别的
// 设置上"只表现为设备无响应，这条说明是唯一线索（失败**不判失败**，故它必须自带上下文）。
void TestEdlLibusbTransport::altSettingFailureNoteNamesInterfaceAndSetting()
{
    using T = edl::LibusbEdlTransport;
    const QString note = T::altSettingSwitchNote(2, 3, QStringLiteral("BUSY"));
    QVERIFY2(note.contains(QStringLiteral("接口 2")), qPrintable(note));
    QVERIFY2(note.contains(QStringLiteral("altsetting 3")), qPrintable(note));
    QVERIFY2(note.contains(QStringLiteral("BUSY")), qPrintable(note));
}

// A-40 结构钉（复审要求）：离线用例只钉得住"判定 + 文案"，而真正干活的三处在**真机路径**上 ——
// 端点发现记录 altsetting、claim 后的切换调用、句柄收尾的复位；删掉它们不会有行为用例变红。这里
// 读**生产源文件**、按字面把这三处钉住（任一处消失、或复制成多份，用例即红）。
//
// ⚠️ 取材边界（如实标注）：断的是**文本计数**，不是行为 —— 把整块包进 `if (false)`、或换成恒假
// 的开关，本用例仍绿（那要靠真机用例，本阶段无真机）。计数口径**有意收窄**：在 .cpp 的注释里
// 再写一次 `libusb_set_interface_alt_setting(` 也会让计数变 2 而变红 —— 宁可逼注释换措辞，
// 也不把口径放宽成"至少出现一次"。
void TestEdlLibusbTransport::pinsAltSettingWiringInProductionSource()
{
    const auto readAll = [](const QString &path, QString *out) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return false;
        *out = QString::fromUtf8(f.readAll());
        return !out->isEmpty();
    };

    QString src;
    QVERIFY2(readAll(QStringLiteral(PT_SRC_ROOT "/src/core/edl/edl_libusb_transport.cpp"), &src),
             qPrintable(QStringLiteral("读不到生产源文件：%1").arg(QStringLiteral(PT_SRC_ROOT))));

    // ① 端点发现处记录端点取自哪一份设置（描述符读不到时退回阶段常量、不做切换的那条路径
    //    不记录 —— 常量本来就对应默认设置）
    QCOMPARE(int(src.count(QStringLiteral("m_altSetting = alt->bAlternateSetting;"))), 1);
    // ② claim 成功后的显式切换：守卫与调用各一处，且调用紧跟在守卫之后（同一语句块内）
    QCOMPARE(int(src.count(QStringLiteral("needsAltSettingSwitch(m_altSetting)"))), 1);
    QCOMPARE(int(src.count(QStringLiteral("libusb_set_interface_alt_setting("))), 1);
    const qsizetype guard = src.indexOf(QStringLiteral("needsAltSettingSwitch(m_altSetting)"));
    QVERIFY(guard >= 0);
    QVERIFY2(src.mid(guard, 200).contains(QStringLiteral("libusb_set_interface_alt_setting(")),
             qPrintable(src.mid(guard, 200)));
    // ③ 复位**两处**：进入端点发现前先置 0（描述符异常路径不清上次的值）+ close() 收尾复位
    QCOMPARE(int(src.count(QStringLiteral("m_altSetting = 0;"))), 2);

    QString hdr;
    QVERIFY2(readAll(QStringLiteral(PT_SRC_ROOT "/src/core/edl/edl_libusb_transport.h"), &hdr),
             "读不到生产头文件");
    QCOMPARE(int(hdr.count(QStringLiteral("int m_altSetting = 0;"))), 1);   // 成员声明只有一处
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
