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
#include <QFile>       // A-40 结构钉：读生产源文件
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
    void switchesAltSettingOnlyWhenNonZero();
    void altSettingFailureNoteNamesInterfaceAndSetting();
    void pinsAltSettingWiringInProductionSource();
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

// A-40：端点取自 `bAlternateSetting != 0` 的那一份时，claim 之后必须显式
// `libusb_set_interface_alt_setting` —— `libusb_claim_interface` 只按**接口号**认领，不会替你
// 选中端点所属的设置；不切则首次 bulk 打在默认设置上（真机表现为设备无响应）。真机上的切换
// 调用要真句柄、离线执行不了，但"何时该切"这条判定可以离线钉住（与 effectiveTimeoutMs 同款）。
void TestOdinLibusbTransport::switchesAltSettingOnlyWhenNonZero()
{
    QVERIFY(!LibusbOdinTransport::needsAltSettingSwitch(0));    // 默认设置：claim 之后即是它
    QVERIFY(LibusbOdinTransport::needsAltSettingSwitch(1));
    QVERIFY(LibusbOdinTransport::needsAltSettingSwitch(2));
    QVERIFY(LibusbOdinTransport::needsAltSettingSwitch(255));   // bAlternateSetting 是 quint8
}

// 切换失败的说明要能直接落日志：点名接口号、目标 altsetting 与底层错误 —— 真机上"传输打在别的
// 设置上"只表现为设备无响应，这条说明是唯一线索（失败**不判失败**，故它必须自带上下文）。
void TestOdinLibusbTransport::altSettingFailureNoteNamesInterfaceAndSetting()
{
    const QString note = LibusbOdinTransport::altSettingSwitchNote(2, 3, QStringLiteral("BUSY"));
    QVERIFY2(note.contains(QStringLiteral("接口 2")), qPrintable(note));
    QVERIFY2(note.contains(QStringLiteral("altsetting 3")), qPrintable(note));
    QVERIFY2(note.contains(QStringLiteral("BUSY")), qPrintable(note));
}

// A-40 结构钉（复审要求）：离线用例只钉得住"判定 + 文案"，而真正干活的三处在**真机路径**上 ——
// 巡检记录 altsetting、claim 后的切换调用、句柄收尾的复位；删掉它们不会有行为用例变红。这里读
// **生产源文件**、按字面把这三处钉住（任一处消失、或复制成多份，用例即红）。
//
// ⚠️ 取材边界（如实标注）：断的是**文本计数**，不是行为 —— 把整块包进 `if (false)`、或换成恒假
// 的开关，本用例仍绿（那要靠真机用例，本阶段无真机）。计数口径**有意收窄**：在 .cpp 的注释里
// 再写一次 `libusb_set_interface_alt_setting(` 也会让计数变 2 而变红 —— 宁可逼注释换措辞，
// 也不把口径放宽成"至少出现一次"。
void TestOdinLibusbTransport::pinsAltSettingWiringInProductionSource()
{
    const auto readAll = [](const QString &path, QString *out) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return false;
        *out = QString::fromUtf8(f.readAll());
        return !out->isEmpty();
    };

    QString src;
    QVERIFY2(readAll(QStringLiteral(PT_SRC_ROOT "/src/core/odin/odin_libusb_transport.cpp"), &src),
             qPrintable(QStringLiteral("读不到生产源文件：%1").arg(QStringLiteral(PT_SRC_ROOT))));

    // ① 巡检结果带上端点所属的 altsetting：**两条候选选择路径都记录**（缺一条 = 那条路径丢失）
    QCOMPARE(int(src.count(QStringLiteral("out.altSetting = alt->bAlternateSetting;"))), 2);
    // ② claim 成功后的显式切换：守卫与调用各一处，且调用紧跟在守卫之后（同一语句块内）
    QCOMPARE(int(src.count(QStringLiteral("needsAltSettingSwitch(m_altSetting)"))), 1);
    QCOMPARE(int(src.count(QStringLiteral("libusb_set_interface_alt_setting("))), 1);
    const qsizetype guard = src.indexOf(QStringLiteral("needsAltSettingSwitch(m_altSetting)"));
    QVERIFY(guard >= 0);
    QVERIFY2(src.mid(guard, 200).contains(QStringLiteral("libusb_set_interface_alt_setting(")),
             qPrintable(src.mid(guard, 200)));
    // ③ 记入与复位：open() 从巡检结果记一次、close() 复位一次（不留上次的 altsetting）
    QCOMPARE(int(src.count(QStringLiteral("m_altSetting = insp.altSetting;"))), 1);
    QCOMPARE(int(src.count(QStringLiteral("m_altSetting = 0;"))), 1);
    // ④ 顺序：切换调用必须在**首次 claim 之后**（libusb 契约：接口未认领时切换不生效，真机症状
    //    与 A-40 要修的完全一样）。这条钉的是"文件内出现顺序"。
    //    ⚠️ 取材边界：把整块**原样搬**到 claim 之前会被这条挡下，但"挪进一个在 claim 之前调用的
    //    helper"仍能绕过（那要真机/行为用例）。
    QVERIFY2(src.indexOf(QStringLiteral("libusb_set_interface_alt_setting("))
                 > src.indexOf(QStringLiteral("libusb_claim_interface(")),
             "切换调用出现在首次 claim 之前：libusb 要求接口已认领，否则切换不生效");

    QString hdr;
    QVERIFY2(readAll(QStringLiteral(PT_SRC_ROOT "/src/core/odin/odin_libusb_transport.h"), &hdr),
             "读不到生产头文件");
    QCOMPARE(int(hdr.count(QStringLiteral("int m_altSetting = 0;"))), 1);   // 成员声明只有一处
}

// 任务书 5 例之外的新增（A-40 的三格见上；本格的 reason 写在这里，防审查以为是漏读任务书）：本实现里有"未打开
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
