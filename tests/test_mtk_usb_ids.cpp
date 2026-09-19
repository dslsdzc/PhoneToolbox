// tests/test_mtk_usb_ids.cpp
//
// W6-T1：MTK preloader 直连 —— 检测接线的判据与结构钉。
//
// 缺口的形状（侦察结论：.superpowers/sdd/w6-d-class-recon.md §2）：协议层**早就认** preloader
// （src/core/modes/mtk_brom.cpp 的 kUsbIds 含 0x0E8D:0x6000 一族；isBromPid() 的 0xA0 预发与
// GET_BL_VER 的 0xFE 判据都已实现），但检测层只认 {0x0E8D, 0x0003} —— 设备以 preloader 身份插入时
// 本仓**看不见它**：不是能力缺失，是能力不可达。修法：判据单点放在协议层
// （mtkbrom::isKnownMtkUsbId，与 kUsbIds 同源），检测层调它、不再列第二份 PID 表。
//
// 本文件两段：
//  ① 判据（纯函数，行为断言）：BROM PID / preloader 一族命中；**同 PID 数字配别家 VID** 不命中。
//  ② 结构钉（读生产源文件，文本断言）：检测层确实调这个判据、没有第二份 MTK PID 表、模式文案
//     没有把 preloader 说成 BROM。这三处**无法**用行为用例覆盖：detectProtocolDevices 要真实
//     libusb 设备（仓内无注入点），getModeDisplayName 是非静态成员（为它链接 device_detector
//     的依赖闭包不划算）。取材边界见各用例内注释。
#include <QtTest>

#include <QFile>
#include <QRegularExpression>

#include "core/modes/mtk_brom.h"

namespace {

// 判据的**独立副本**：逐条手抄自 src/core/modes/mtk_brom.cpp 的 kUsbIds（= 上游 config/usb_ids.py
// default_ids 的白名单本体）。有意重复一份 —— 实现表改了而这里没跟着改，本用例即红。
struct UsbId {
    quint16 vid;
    quint16 pid;
    const char *note;   // 失败信息里点名"这一条是哪一族的"
};

const UsbId kKnownIds[] = {
    { 0x0E8D, 0x0003, "BROM（mtkclient brom_pids 的 0x3）" },
    { 0x0E8D, 0x6000, "preloader" },
    { 0x0E8D, 0x2000, "preloader" },
    { 0x0E8D, 0x2001, "preloader" },
    { 0x0E8D, 0x20FF, "preloader" },
    { 0x0E8D, 0x3000, "preloader" },
    { 0x1004, 0x6000, "preloader（LG VID）" },
    { 0x22D9, 0x0006, "preloader（OPPO VID）" },
    { 0x0FCE, 0xF200, "BROM（Sony VID，mtkclient brom_pids 的 0xF200）" },
    { 0x0FCE, 0xD1E9, "BROM（Sony VID）" },
    { 0x0FCE, 0xD1E2, "BROM（Sony VID）" },
    { 0x0FCE, 0xD1EC, "BROM（Sony VID）" },
    { 0x0FCE, 0xD1DD, "BROM（Sony VID）" },
};

// 不命中：VID 在表内、PID 不在（含紧邻 0x0003 的 0x0002/0x0004 —— 判据必须是**精确**匹配）
const UsbId kUnknownPids[] = {
    { 0x0E8D, 0x0000, "同 VID 的未知 PID" },
    { 0x0E8D, 0x0002, "同 VID 的未知 PID（紧邻 BROM）" },
    { 0x0E8D, 0x0004, "同 VID 的未知 PID（紧邻 BROM）" },
    { 0x0E8D, 0x2002, "同 VID 的未知 PID（紧邻 preloader 族）" },
    { 0x0E8D, 0xFFFF, "同 VID 的未知 PID" },
};

// 不命中：**PID 数字相同、VID 是别家** —— 判据必须查 VID，不能只查 PID。
// 三个 VID 都是本仓另一条检测判据的地盘（三星 Odin/EUB、华为 USB Update、展锐 SPD）：
// 只按 PID 认就会把这些厂商的设备抢成 MTK。
const UsbId kOtherVendors[] = {
    { 0x04E8, 0x6000, "三星 VID + preloader PID 数字" },
    { 0x04E8, 0x0003, "三星 VID + BROM PID 数字" },
    { 0x12D1, 0x6000, "华为 VID + preloader PID 数字" },
    { 0x1782, 0x6000, "展锐 VID + preloader PID 数字" },
    { 0x05C6, 0x9008, "高通 EDL 9008" },
    { 0x1004, 0x2000, "表内 VID（LG）配表内别的 PID 数字" },
};

QString hex4(quint16 v)
{
    return QStringLiteral("0x") + QString::number(v, 16).rightJustified(4, QLatin1Char('0')).toUpper();
}

QString describe(const UsbId &id)
{
    // note 是源码里的 UTF-8 字面量（含中文）—— 用 Latin-1 解释会把注释显示成乱码
    return QStringLiteral("%1:%2（%3）").arg(hex4(id.vid), hex4(id.pid), QString::fromUtf8(id.note));
}

} // namespace

class TestMtkUsbIds : public QObject
{
    Q_OBJECT

private slots:
    // ① 判据：BROM 与 preloader 一族**都**命中（preloader 过去进不了门，就是这道判据缺了它们）
    void knownIdsIncludeBromAndPreloader();

    // ① 判据的负控制（甄别力）：同 VID 的未知 PID 不命中、别家 VID 的相同 PID 数字不命中
    void unknownPidOfKnownVidIsRejected();
    void samePidUnderAnotherVendorIsRejected();

    // ② 结构钉：检测层调的是这个判据（且只有一处）、检测层没有第二份 MTK PID 表、
    //    模式文案没把 preloader 说成 BROM
    void detectorRoutesThroughTheSingleSource();
    void modeTextDoesNotCallPreloaderBrom();
};

// ---------------------------------------------------------------------------
// ① 判据
// ---------------------------------------------------------------------------

void TestMtkUsbIds::knownIdsIncludeBromAndPreloader()
{
    for (const UsbId &id : kKnownIds)
        QVERIFY2(mtkbrom::isKnownMtkUsbId(id.vid, id.pid),
                 qPrintable(QStringLiteral("白名单应含 %1").arg(describe(id))));

    // 反向自检（防止上表被整表删空/写错成同一对 VID:PID）：命中的**组数**必须等于表长 ——
    // 逐条 QVERIFY 的循环在表为空时"全绿"，这条把它堵住
    int hits = 0;
    for (const UsbId &id : kKnownIds) {
        if (mtkbrom::isKnownMtkUsbId(id.vid, id.pid))
            ++hits;
    }
    QCOMPARE(hits, int(sizeof(kKnownIds) / sizeof(kKnownIds[0])));
}

void TestMtkUsbIds::unknownPidOfKnownVidIsRejected()
{
    for (const UsbId &id : kUnknownPids)
        QVERIFY2(!mtkbrom::isKnownMtkUsbId(id.vid, id.pid),
                 qPrintable(QStringLiteral("白名单不应含 %1").arg(describe(id))));
}

void TestMtkUsbIds::samePidUnderAnotherVendorIsRejected()
{
    // 这条是"判据查了 VID 而不只是 PID"的甄别力所在：把 isKnownMtkUsbId 改成只比 PID，
    // 本用例即红（变异留档 M2，见 .superpowers/sdd/w6-task-1-logs/）——
    // 同一处改动也会让三星/华为/展锐三家的设备被 MTK 抢走（检测层不得抢既有厂商判据）。
    for (const UsbId &id : kOtherVendors)
        QVERIFY2(!mtkbrom::isKnownMtkUsbId(id.vid, id.pid),
                 qPrintable(QStringLiteral("白名单不应含 %1").arg(describe(id))));
}

// ---------------------------------------------------------------------------
// ② 结构钉（读生产源文件）
// ---------------------------------------------------------------------------

// 为什么钉在文本上：preloader 进门这件事的**行为**面在 detectProtocolDevices 里，而它要一份真实
// libusb 设备列表（仓内无注入点，且本仓 CI 无 MTK 真机）——"preloader PID 会被认成 MODE_MTK_BROM"
// 这条端到端行为**离线不可达**。能离线钉住的是它的两个必要前提：判据确实来自协议层单点、检测层
// 没有第二份会漂移的表。判据本身的行为由上面三条用例覆盖。
//
// ⚠️ 取材边界（如实标注）：断的是**文本**，不是行为 —— 把调用包进 `if (false)` 本用例仍绿
// （那要靠真机）。计数口径**有意收紧**：检测层里 `0x0E8D` 必须一次都不出现（连注释也不行），
// 逼任何新增的 MTK 字面量表去协议层找判据；宁可让人改注释措辞，也不放宽成"至多一次"。
void TestMtkUsbIds::detectorRoutesThroughTheSingleSource()
{
    QFile f(QStringLiteral(PT_SRC_ROOT "/src/core/device_detector.cpp"));
    QVERIFY2(f.open(QIODevice::ReadOnly),
             qPrintable(QStringLiteral("读不到生产源文件：%1").arg(QStringLiteral(PT_SRC_ROOT))));
    const QString src = QString::fromUtf8(f.readAll());
    QVERIFY(!src.isEmpty());

    // ① 判据调用点：有且仅有一处
    QCOMPARE(int(src.count(QStringLiteral("mtkbrom::isKnownMtkUsbId("))), 1);
    // ② 认领点：MTK 的认领走的是那段共用代码（claim(MODE_MTK_BROM) 字面一处）
    QCOMPARE(int(src.count(QStringLiteral("claim(MODE_MTK_BROM);"))), 1);
    // ③ 没有第二份 MTK PID 表：MTK 的 VID 字面量在检测层一次都不该出现
    //    （原实现是 ids[] 里的一行 MTK VID:PID —— 那正是本项要拆掉的第二份表）
    QCOMPARE(int(src.count(QStringLiteral("0x0E8D"))), 0);
    // ④ 其余两条厂商判据原样在位（本项不碰它们；比对前先压掉空白，免得对齐改动就误红）
    QString dense = src;
    dense.remove(QRegularExpression(QStringLiteral("\\s+")));
    QCOMPARE(int(dense.count(QStringLiteral("{0x12D1,-1,MODE_HUAWEI_USB_UPDATE},"))), 1);
    QCOMPARE(int(dense.count(QStringLiteral("{0x1782,-1,MODE_SPD},"))), 1);
}

// 模式文案：claim 进 MODE_MTK_BROM 的设备可能是 BROM、也可能是 preloader（身份握手后由
// GET_BL_VER 判定），故显示名不许只写 "MTK BROM" —— 那是把 preloader 说成 BROM。
// getModeDisplayName 是非静态成员（链接整个 DeviceDetector 只为调它不划算），故同样钉在文本上。
void TestMtkUsbIds::modeTextDoesNotCallPreloaderBrom()
{
    QFile f(QStringLiteral(PT_SRC_ROOT "/src/core/device_detector.cpp"));
    QVERIFY2(f.open(QIODevice::ReadOnly),
             qPrintable(QStringLiteral("读不到生产源文件：%1").arg(QStringLiteral(PT_SRC_ROOT))));
    const QString src = QString::fromUtf8(f.readAll());

    QCOMPARE(int(src.count(QStringLiteral("QStringLiteral(\"MTK BROM / Preloader\")"))), 1);
    // 回归面：裸的 "MTK BROM" 字面量不许再出现（只写 BROM = 对 preloader 用户说假话）
    QCOMPARE(int(src.count(QStringLiteral("QStringLiteral(\"MTK BROM\")"))), 0);
}

QTEST_APPLESS_MAIN(TestMtkUsbIds)

#include "test_mtk_usb_ids.moc"
