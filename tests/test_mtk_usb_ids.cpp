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
//     甄别力**只在删/改方向**：实现表多了行不违反任何一条（那条方向见 ② 的第二条）。
//  ② 结构钉（读生产源文件，文本断言）：实现对表的两道钉（表内容镜像 + 串口芯片黑名单）、
//     检测层确实调这个判据且没有第二份 MTK PID 表、模式文案没有把 preloader 说成 BROM。
//     这几处**无法**用行为用例覆盖：detectProtocolDevices 要真实 libusb 设备（仓内无注入点），
//     getModeDisplayName 是非静态成员（为它链接 device_detector 的依赖闭包不划算）。
//     取材边界见各用例内注释。
#include <QtTest>

#include <QFile>
#include <QRegularExpression>

#include "core/modes/mtk_brom.h"

namespace {

// 判据的**独立副本**：逐条手抄自 src/core/modes/mtk_brom.cpp 的 kUsbIds（= 上游 config/usb_ids.py
// default_ids 里**本仓有意保留的那部分**；同一份上游表里的 8 行 USB 串口芯片没带，见下）。
//
// 这份副本的甄别力**只在"删行/改行"方向**：下面逐条 QVERIFY 会因实现表少了某行而红。
// **"加行"方向它不红**（多出来的行不违反任何一条断言）—— 那条方向由
// implementationTableMirrorsThisList() 的两道钉守着（表内容镜像 + 串口芯片黑名单）。
// 早先这里写"实现表改了这里没跟着改即红"是**不准确**的（复审 I-1 实测：往 kUsbIds 加一行，本文件全绿）。
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

// 三张表的条数（**防空表空断言**）：逐条 QVERIFY 的循环在表为空时是"全绿"的 —— 故先把表本身钉住条数，
// 免得将来把整表删空之后这三条用例静默退化成空断言。
// 13 = 本文件 kKnownIds 的条数，**必须与实现表 kUsbIds 一致**（由 implementationTableMirrorsThisList()
// 的字面量集比对强制）：往实现表加行 → 那条钉先红 → 把行抄到这里、再改这个数，是有意的两道手续。
constexpr int kKnownIdsExpected = 13;
constexpr int kUnknownPidsExpected = 5;
constexpr int kOtherVendorsExpected = 6;

// 上游 config/usb_ids.py 同一份表里带、而本仓**有意不带**的 8 行 USB 串口芯片（FTDI / CH34x / CH340 / CP2102）。
// 它们是 kUsbIds 的自动认领语义下最可能的误入者：谁为"忠实对照上游"补上，检测层就会把每一根
// CH340 数据线认成「MTK BROM / Preloader」并引到刷写面板。逐行取自上游（2026-09-19 读 mtkclient 子模块）。
const char *const kSerialChipIds[] = {
    "0x0403:0x6001",   // FTDI
    "0x1A86:0x55D3",   // QinHeng CH343G
    "0x1A86:0x7523",   // QinHeng CH340C
    "0x1A86:0x5523",   // QinHeng CH341
    "0x1A86:0x5512",   // QinHeng CH341A
    "0x4348:0x5523",   // WCH CH340
    "0x10C4:0xEA60",   // CP2102
    "0x11CA:0x0211",   // CP2102
};

QString hexDigits(quint16 v)
{
    return QString::number(v, 16).rightJustified(4, QLatin1Char('0')).toUpper();
}

QString hex4(quint16 v)
{
    return QStringLiteral("0x") + hexDigits(v);
}

// 读生产源文件（相对仓库根，路径在 CMake 里编成 PT_SRC_ROOT）。读不到返回空串，
// 调用方一律 QVERIFY2 硬 FAIL —— 钉在缺文件时静默失效，正是本仓反复吃亏的形态。
QString readProductionFile(const QString &relPath)
{
    QFile f(QStringLiteral(PT_SRC_ROOT) + relPath);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    return QString::fromUtf8(f.readAll());
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

    // ② 实现对表的文本钉：kUsbIds 现在**是自动认领表** —— 表内容与用例表互为镜像，
    //    且不得出现已知 USB 串口芯片（FTDI / CH34x / CH340 / CP2102）
    void implementationTableMirrorsThisList();

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
    QCOMPARE(int(sizeof(kKnownIds) / sizeof(kKnownIds[0])), kKnownIdsExpected);
    for (const UsbId &id : kKnownIds)
        QVERIFY2(mtkbrom::isKnownMtkUsbId(id.vid, id.pid),
                 qPrintable(QStringLiteral("白名单应含 %1").arg(describe(id))));
}

void TestMtkUsbIds::unknownPidOfKnownVidIsRejected()
{
    QCOMPARE(int(sizeof(kUnknownPids) / sizeof(kUnknownPids[0])), kUnknownPidsExpected);
    for (const UsbId &id : kUnknownPids)
        QVERIFY2(!mtkbrom::isKnownMtkUsbId(id.vid, id.pid),
                 qPrintable(QStringLiteral("白名单不应含 %1").arg(describe(id))));
}

void TestMtkUsbIds::samePidUnderAnotherVendorIsRejected()
{
    // 这条是"判据查了 VID 而不只是 PID"的甄别力所在：把 isKnownMtkUsbId 改成只比 PID，
    // 本用例即红（变异留档 M2，见 .superpowers/sdd/w6-task-1-logs/）——
    // 同一处改动也会让三星/华为/展锐三家的设备被 MTK 抢走（检测层不得抢既有厂商判据）。
    QCOMPARE(int(sizeof(kOtherVendors) / sizeof(kOtherVendors[0])), kOtherVendorsExpected);
    for (const UsbId &id : kOtherVendors)
        QVERIFY2(!mtkbrom::isKnownMtkUsbId(id.vid, id.pid),
                 qPrintable(QStringLiteral("白名单不应含 %1").arg(describe(id))));
}

// ---------------------------------------------------------------------------
// ② 结构钉（读生产源文件）
// ---------------------------------------------------------------------------

// 实现对表的文本钉（复审 I-1）：kUsbIds 现在**同时是自动认领表**（检测层按它把设备认成
// MODE_MTK_BROM 并引到刷写面板），误入一行的后果从"多试开一个设备"升级成"对所有用户亮错模式"。
// 两道：
//   ① 表内容镜像 —— 实现表与本文件 kKnownIds 的字面量集必须逐对相同（顺序无关）。
//      往实现表加行即红（上面那三条逐条断言的甄别力只在删/改方向，加行不红）。
//   ② 串口芯片黑名单 —— 上游 usb_ids.py 同一份表里的 8 行串口芯片本仓有意不带。
//      这是 ① 堵不住的方向：有人把串口行**同时**抄进两侧时 ① 仍绿，② 才拦得住。
//
// ⚠️ 取材边界：断的是**文本**（解析 kUsbIds 块里的 `{0xXXXX, 0xYYYY}` 字面量对），不是行为 ——
// 表运行时怎么被消费由 enumerateUsb / isKnownMtkUsbId 决定，那两条已各有行为用例。
// 正则只认这一种写法：若将来表的写法换形态（如改用常量引用），本钉会以"解析到的行数不对"红，
// 那是**要人来更新这个解析器**，不是放宽。
void TestMtkUsbIds::implementationTableMirrorsThisList()
{
    const QString src = readProductionFile(QStringLiteral("/src/core/modes/mtk_brom.cpp"));
    QVERIFY2(!src.isEmpty(),
             qPrintable(QStringLiteral("读不到生产源文件：%1").arg(QStringLiteral(PT_SRC_ROOT))));

    // 只在 kUsbIds 块内找字面量（从声明处到其后的第一个 "};"）—— 免得把文件里别的表/别处的
    // 十六进制字面量算进来
    const qsizetype begin = src.indexOf(QStringLiteral("kUsbIds[] = {"));
    QVERIFY(begin >= 0);
    const qsizetype end = src.indexOf(QStringLiteral("};"), begin);
    QVERIFY(end > begin);
    const QString block = src.mid(begin, end - begin);

    // ① 表内容镜像
    //    ⚠️ 两侧一律经 hex4() 归一（"0xXXXX:0xYYYY"）—— 早先这里直接用正则捕获串，与下面黑名单的
    //    "0x…" 写法不同形，黑名单**恒真**（本仓变异实测：把串口行同时写进两侧时，镜像钉仍绿、黑名单也不响）；经归一后黑名单才有效。
    QStringList implIds;
    const QRegularExpression re(
        QStringLiteral("\\{\\s*0[xX]([0-9a-fA-F]+)\\s*,\\s*0[xX]([0-9a-fA-F]+)\\s*\\}"));
    QRegularExpressionMatchIterator it = re.globalMatch(block);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        implIds << QStringLiteral("%1:%2")
                       .arg(hex4(quint16(m.captured(1).toUShort(nullptr, 16))),
                            hex4(quint16(m.captured(2).toUShort(nullptr, 16))));
    }
    QCOMPARE(int(implIds.size()), kKnownIdsExpected);   // 解析器本身也要受检（写法变了先在这里红）

    QStringList testIds;
    for (const UsbId &id : kKnownIds)
        testIds << QStringLiteral("%1:%2").arg(hex4(id.vid), hex4(id.pid));
    implIds.sort();
    testIds.sort();
    QCOMPARE(implIds, testIds);   // 两侧互为副本：任一侧加/删/改都要另一侧跟上

    // ② 串口芯片黑名单
    for (const char *const chip : kSerialChipIds) {
        const QString id = QString::fromLatin1(chip);
        QVERIFY2(!implIds.contains(id),
                 qPrintable(QStringLiteral("kUsbIds 里出现了 USB 串口芯片 %1 —— 该表是**自动认领表**"
                                           "（认成 MTK BROM / Preloader 并引到刷写面板），串口芯片不得入内")
                                .arg(id)));
    }
}

// 为什么钉在文本上：preloader 进门这件事的**行为**面在 detectProtocolDevices 里，而它要一份真实
// libusb 设备列表（仓内无注入点，且本仓 CI 无 MTK 真机）——"preloader PID 会被认成 MODE_MTK_BROM"
// 这条端到端行为**离线不可达**。能离线钉住的是它的两个必要前提：判据确实来自协议层单点、检测层
// 没有第二份会漂移的表。判据本身的行为由上面三条用例覆盖。
//
// ⚠️ 取材边界（如实标注）：断的是**文本**，不是行为 —— 把调用包进 `if (false)` 本用例仍绿
// （那要靠真机）。计数口径**有意收紧**：检测层里 `0x0E8D` 必须一次都不出现（连注释也不行），
// **大小写不敏感**（`0x0e8d` 照样算 —— 本仓本就混用大小写，复审 M-1 实测原先的大小写敏感版可被绕）；
// 逼任何新增的 MTK 字面量表去协议层找判据；宁可让人改注释措辞，也不放宽成"至多一次"。
void TestMtkUsbIds::detectorRoutesThroughTheSingleSource()
{
    const QString src = readProductionFile(QStringLiteral("/src/core/device_detector.cpp"));
    QVERIFY2(!src.isEmpty(),
             qPrintable(QStringLiteral("读不到生产源文件：%1").arg(QStringLiteral(PT_SRC_ROOT))));

    // ① 判据调用点：有且仅有一处
    QCOMPARE(int(src.count(QStringLiteral("mtkbrom::isKnownMtkUsbId("))), 1);
    // ② 认领点：MTK 的认领走的是那段共用代码（claim(MODE_MTK_BROM) 字面一处）
    QCOMPARE(int(src.count(QStringLiteral("claim(MODE_MTK_BROM);"))), 1);
    // ③ 没有第二份 MTK PID 表：MTK 的 VID 字面量在检测层一次都不该出现
    //    （原实现是 ids[] 里的一行 MTK VID:PID —— 那正是本项要拆掉的第二份表）
    //    大小写不敏感：本仓本就混用（edl_9008.cpp 写 0x05c6），0x0e8d 同样是第二份表（复审 M-1）
    QCOMPARE(int(src.toLower().count(QStringLiteral("0x0e8d"))), 0);
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
    const QString src = readProductionFile(QStringLiteral("/src/core/device_detector.cpp"));
    QVERIFY2(!src.isEmpty(),
             qPrintable(QStringLiteral("读不到生产源文件：%1").arg(QStringLiteral(PT_SRC_ROOT))));

    QCOMPARE(int(src.count(QStringLiteral("QStringLiteral(\"MTK BROM / Preloader\")"))), 1);
    // 回归面：裸的 "MTK BROM" 字面量不许再出现（只写 BROM = 对 preloader 用户说假话）
    QCOMPARE(int(src.count(QStringLiteral("QStringLiteral(\"MTK BROM\")"))), 0);
}

QTEST_APPLESS_MAIN(TestMtkUsbIds)

#include "test_mtk_usb_ids.moc"
