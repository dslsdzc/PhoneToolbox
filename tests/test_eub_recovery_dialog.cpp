// tests/test_eub_recovery_dialog.cpp
//
// 对话框的**离线可测面**：段表/摘要两个纯函数，以及"注入 mock 传输后的识别 + 载荷预检 + 门控"路径。
// 真机流程（选择文件对话框 / 真机发送）本机无设备，facts §F1。
//
// 断言强度的取舍（本目标易恒真点已在 T7 brief 里点名"文案回显"与"门控"两类）：
//   * 段表槽不只查"名字出现过"，而是查 **同一行** 同时含该段的 offset 与 length（逐段一行的契约）
//     + 出现次数 == segments.size()（漏掉"重发段"的实现在这里挂）；
//   * statusText 槽查的是**由镜像字节推导出的 sha1 前 8 位**（不是回显表里的常量）——
//     只有真的走了"读镜像 → sha1 → 填标签"这条链才可能有这个值；
//   * 门控槽查**真实按钮控件**的 enabled（findChild 拿到的那个），并覆盖"先勾选后 prepare"的
//     负向序（只按 isChecked() 计算门控的实现会在这里挂）；
//   * extraFiles（9830）槽（backlog Task 2）：字节数断言用**两份长度不同的合成载荷**（12,345 /
//     17,185）钉住"数字来自实际取出的载荷"，末尾的 gated 真样本槽再用真包的 6,291,456 / 1,572,864
//     复核 —— 单靠真样本槽无法分辨"数字来自包"与"数字被硬编"（两者恰好相同）。
#include <QtTest>
#include <QCheckBox>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTemporaryDir>

#include "core/eub/eub_payload.h"   // loadSbootBytes（gated 真样本槽按 onPickSource 的同一条入口载入）
#include "eub_test_helpers.h"       // 真样本 gating（共享）：EUB_SAMPLES_DIR / EUB_SKIP_OR_FAIL / eubtest::*
#include "image_engine/compression/lz4_wrapper.h"
#include "image_engine/tar_image.h"
#include "mock_eub_transport.h"
#include "ui/eub_recovery_dialog.h"

namespace {

// 合成 sboot：够 9610 全表（最大段到 0x1DA000 + 0x40000 = 0x21A000）。
// 非周期填充（xorshift32，与 test_eub_session.cpp:19-25 同款）：`(i * k) & 0xFF` 是 256 周期图案，
// 会让"偏移错 256 的整数倍"漏检（T4 实现者的变异证据）。
QByteArray syntheticSboot(quint64 bytes = 0x220000)
{
    QByteArray b(int(bytes), '\0');
    quint32 x = 0x12345678u;
    for (int i = 0; i < b.size(); ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b[i] = char(x & 0xFF); }
    return b;
}

// 在镜像里写入 ASCII 标记，供 facts §A4 的退化识别路径（detectSocFromImage）用。
// 标记后强制一个 '\0'：正则 `EXYNOS[0-9]+` 是贪婪的，标记后若正好是随机字节里的数字会被续接。
QByteArray sbootWithMarker(const QByteArray &marker, quint64 bytes = 0x220000)
{
    QByteArray b = syntheticSboot(bytes);
    const int at = 0x1000;
    if (at + marker.size() + 1 <= b.size()) {
        b.replace(at, marker.size(), marker);
        b[at + marker.size()] = '\0';
    }
    return b;
}

int countOccurrences(const QString &hay, const QString &needle)
{
    int n = 0;
    for (int from = hay.indexOf(needle); from != -1; from = hay.indexOf(needle, from + needle.size()))
        ++n;
    return n;
}

// 该段是否有一条"名字 + 自己的 offset + 自己的 length"同行的展示行（逐段一行，brief 的格式契约）。
bool segmentLinePaired(const QString &text, const eub::EubSegment &s)
{
    const QString off = QStringLiteral("0x") + QString::number(s.offset, 16);
    const QString len = QStringLiteral("0x") + QString::number(s.length, 16);
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.contains(s.name) && line.contains(off) && line.contains(len))
            return true;
    }
    return false;
}

} // namespace

class TestEubRecoveryDialog : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    // 裸文件落盘（裸 sboot.bin 的源约束槽要一个**真的不是 tar** 的路径：判据在对话框里，
    // 拿合成路径骗不过去）
    QString writeFile(const QString &name, const QByteArray &bytes)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            qFatal("无法写临时文件");
        f.write(bytes);
        f.close();
        return path;
    }

    // 合成的 BL 包落盘（用例自造，不依赖真样本）
    QString writeTar(const QString &name, const QList<imgtar::TarEntry> &entries)
    {
        const QString path = m_dir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            qFatal("无法写临时包");
        f.write(imgtar::buildTar(entries));
        f.close();
        return path;
    }

    // 段表控件的**展示文本**（用户在界面上读到的那份，不是另拼一份）
    static QString segmentViewText(const EubRecoveryDialog &dlg)
    {
        QPlainTextEdit *view = dlg.findChild<QPlainTextEdit *>(QStringLiteral("eubSegmentView"));
        return view ? view->toPlainText() : QString();
    }

private slots:
    void segmentTableListsEverySegmentWithHexRanges()
    {
        eub::EubLoadout lo; QString err;
        QVERIFY(eub::eubLoadoutFor(QStringLiteral("Exynos9610"), lo, &err));
        const QString text = EubRecoveryDialog::segmentTableText(lo);

        for (const eub::EubSegment &s : lo.segments)
            QVERIFY2(segmentLinePaired(text, s), qPrintable(s.name));

        // 逐段一行：行数必须等于段数。9610 表第 4 段是**重发 fwbl1**（facts §C5）——
        // "按名字去重再列"的实现会少一行，在这里挂。
        QCOMPARE(countOccurrences(text, QStringLiteral("offset 0x")), lo.segments.size());
        QVERIFY(text.contains(QStringLiteral("0x2000")));       // fwbl1 的长度
        QVERIFY(text.contains(QStringLiteral("offset 0x5a000")));// u-boot 的偏移（小写十六进制）
        QVERIFY(text.contains(lo.evidence));                    // 证据等级随表展示（facts §F3）
        QVERIFY(text.contains(lo.sourceNote));
    }

    void segmentTableStatesFrameStyleOrigin()
    {
        // facts §B6：UI 必须说明"照抄自哪个实现"，不得声称理解头/尾两字段的语义。
        eub::EubLoadout dnw; QString err;
        QVERIFY(eub::eubLoadoutFor(QStringLiteral("Exynos9610"), dnw, &err));
        const QString dnwText = EubRecoveryDialog::segmentTableText(dnw);
        QVERIFY(dnwText.contains(QStringLiteral("照抄 hubble")));
        QVERIFY(dnwText.contains(QStringLiteral("1B 44 4E 57")));
        QVERIFY(dnwText.contains(QStringLiteral("FF FF")));
        QVERIFY(dnwText.contains(QStringLiteral("语义未定")));
        QVERIFY(!dnwText.contains(QStringLiteral("照抄 exynos-usbdl")));

        eub::EubLoadout zero;
        QVERIFY(eub::eubLoadoutFor(QStringLiteral("Exynos8890"), zero, &err));
        const QString zeroText = EubRecoveryDialog::segmentTableText(zero);
        QVERIFY(zeroText.contains(QStringLiteral("照抄 exynos-usbdl")));
        QVERIFY(zeroText.contains(QStringLiteral("00 00 00 00")));
        QVERIFY(zeroText.contains(QStringLiteral("00 00")));
        QVERIFY(zeroText.contains(QStringLiteral("语义未定")));
        QVERIFY(!zeroText.contains(QStringLiteral("照抄 hubble")));

        // 第三种：既不是 zeroStyle 也不是 dnwStyle（EubFrameStyle 是公开结构，调用方可自造）。
        // 此时**不能**冒用某个实现的名义 —— 必须自报"自定义"。
        eub::EubLoadout custom;
        custom.soc = QStringLiteral("Exynos0000");
        custom.style.header = QByteArray::fromHex("deadbeef");
        custom.style.trailer = QByteArray::fromHex("cafe");
        custom.segments = {{QStringLiteral("fwbl1"), 0x0, 0x2000}};
        const QString customText = EubRecoveryDialog::segmentTableText(custom);
        QVERIFY(customText.contains(QStringLiteral("DE AD BE EF")));
        QVERIFY(customText.contains(QStringLiteral("CA FE")));
        QVERIFY(customText.contains(QStringLiteral("自定义")));
        QVERIFY(!customText.contains(QStringLiteral("照抄 hubble")));
        QVERIFY(!customText.contains(QStringLiteral("照抄 exynos-usbdl")));
    }

    void segmentTableListsExtraFilesAndResponseSupport()
    {
        // facts §C7：9830 在 sboot 各段之后还要另发 BL 包内的 ldfw.img / tzsw.img，且设备会回显。
        // backlog Task 2：这两个文件**已实现**（从同一个 BL 包取出、段后按序另发）—— 段表不得再写
        // "本仓本期不发送"（I1 的旧口径）；也不得只写"各段发完后另发"而不说清**从哪来**。
        eub::EubLoadout lo; QString err;
        QVERIFY(eub::eubLoadoutFor(QStringLiteral("Exynos9830"), lo, &err));
        QVERIFY(lo.responseSupport);            // 前置：该表确实带这两项（否则下面的断言无意义）
        QCOMPARE(lo.extraFiles.size(), 2);
        const QString text = EubRecoveryDialog::segmentTableText(lo);
        QVERIFY(text.contains(QStringLiteral("ldfw.img")));
        QVERIFY(text.contains(QStringLiteral("tzsw.img")));
        QVERIFY(text.contains(QStringLiteral("回显")));
        QVERIFY2(text.contains(QStringLiteral("段后另发")), qPrintable(text));      // 什么时候发
        QVERIFY2(text.contains(QStringLiteral("同一个 BL 包")), qPrintable(text));  // 从哪来
        QVERIFY2(!text.contains(QStringLiteral("不发送")), qPrintable(text));        // 旧口径必须消失
        // 反向：旧口径"各段发完后另发"（不说清谁发）也必须消失 —— 只补新句、留旧句同样误导
        QVERIFY2(!text.contains(QStringLiteral("各段发完后另发")), qPrintable(text));

        // 传了 extras（= 预检成功后的展示形态）→ 每个文件带上**实际取出的**字节数。
        // 两份长度刻意不同（12,345 / 17,185）：互换顺序或拿固定数字顶替都会在这里挂。
        const QList<QByteArray> extras{QByteArray(12345, '\x37'), QByteArray(0x4321, '\x9C')};
        const QString sized = EubRecoveryDialog::segmentTableText(lo, extras);
        QVERIFY2(sized.contains(QStringLiteral("12,345")), qPrintable(sized));
        QVERIFY2(sized.contains(QStringLiteral("17,185")), qPrintable(sized));
        // 证据等级声明仍在，且**不得**越界成"真机已验证"（facts §H：真样本≠真机）
        QVERIFY2(sized.contains(QStringLiteral("单源")), qPrintable(sized));
        QVERIFY2(sized.contains(QStringLiteral("真样本")), qPrintable(sized));
        QVERIFY2(!sized.contains(QStringLiteral("真机已验证")), qPrintable(sized));

        // 反向：9610 的 responseSupport=false 且无 extraFiles → 段表不得凭空说"会回显"。
        eub::EubLoadout plain;
        QVERIFY(eub::eubLoadoutFor(QStringLiteral("Exynos9610"), plain, &err));
        QVERIFY(!plain.responseSupport);
        QVERIFY(plain.extraFiles.isEmpty());
        const QString plainText = EubRecoveryDialog::segmentTableText(plain);
        QVERIFY(!plainText.contains(QStringLiteral("ldfw.img")));
        QVERIFY(!plainText.contains(QStringLiteral("会回显")));
    }

    void sha1CompareTextDistinguishesMatchAndMismatch()
    {
        const QByteArray a = QByteArray("a9993e364706816aba3e25717850c26c9cd0d89d");
        const QByteArray b = QByteArray("b1d5781111d84f7b3fe45a0852e59758cd7a87e5");
        const QString same = EubRecoveryDialog::sha1CompareText(a, a);
        QVERIFY(same.contains(QStringLiteral("一致")));
        // 反向：一致分支不得同时出现"不一致"（无脑拼"不一致"的实现会在这里挂）
        QVERIFY(!same.contains(QStringLiteral("不一致")));
        QVERIFY(same.contains(QStringLiteral("a9993e36")));      // 一致时也给前 8 位便于人工核对

        const QString diff = EubRecoveryDialog::sha1CompareText(a, b);
        QVERIFY(diff.contains(QStringLiteral("不一致")));
        QVERIFY(diff.contains(QStringLiteral("a9993e36")));      // 给出前 8 位便于人工核对（spec §D6）
        QVERIFY(diff.contains(QStringLiteral("b1d57811")));      // **两边**都要给，只给一边无法核对
        // 表未记录修订（hubble 系）→ 明说不知道，而不是假装一致
        const QString unknown = EubRecoveryDialog::sha1CompareText(QByteArray(), b);
        QVERIFY(unknown.contains(QStringLiteral("未记录")));
        QVERIFY(unknown.contains(QStringLiteral("b1d57811")));   // 仍要给出用户文件的前 8 位
        QVERIFY(!unknown.contains(QStringLiteral("一致")));
    }

    void prepareIdentifiesDeviceAndPreviewsSegments()
    {
        eub::MockEubTransport mock;
        mock.info.socName = QStringLiteral("Exynos9610");
        mock.info.socId = QStringLiteral("0123456789abcde");
        EubRecoveryDialog dlg(mock, nullptr);

        QStringList sink;
        dlg.setLogSink([&sink](const QString &m, bool) { sink << m; });

        const QByteArray sboot = syntheticSboot();
        QString err;
        QVERIFY2(dlg.prepare(sboot, QString() /* 无来源路径：该 SoC 无额外文件，本参数用不到 */,
                             QStringLiteral("合成 sboot（用例）"), &err), qPrintable(err));
        QCOMPARE(dlg.loadout().soc, QStringLiteral("Exynos9610"));
        QVERIFY(dlg.statusText().contains(QStringLiteral("Exynos9610")));
        QVERIFY(dlg.statusText().contains(QStringLiteral("双源一致")));
        QVERIFY(dlg.statusText().contains(QStringLiteral("6 段")));   // 9610 六段（含重发段，facts §C5）

        // 非回显式断言：这里查的是**由镜像字节算出的** sha1 前 8 位 —— 只有真的走了
        // "读 m_sboot → sha1Hex → 填标签"这条链才可能出现这个值。
        const QString filePrefix = eub::sha1Hex(sboot).left(8);
        QVERIFY2(dlg.statusText().contains(filePrefix), qPrintable(dlg.statusText()));
        // 9610 表**未记录** sboot 修订（hubble 系 JSON 无修订字段；终审 I2 后只有 8890/8895 记了
        // sha1，7580 也留空了）→ spec §D6 要求此时明说"该表未记录"，不得假报一致。
        QVERIFY(dlg.loadout().sbootSha1.isEmpty());            // 前置：本表确实没有修订记录
        QVERIFY2(dlg.statusText().contains(QStringLiteral("未记录")), qPrintable(dlg.statusText()));
        // 反向断言要**精确到 sha1 那句**：设备行里的证据等级 "双源一致" 本身含 "一致" 二字，
        // 裸查 "一致" 会恒真（本槽第一版就是这么挂的 —— 见报告 RED/GREEN 记录）。
        QVERIFY(!dlg.statusText().contains(QStringLiteral("sha1 一致")));

        // 进度回调必须接到日志汇（brief：把会话 progress 接到窗口日志才能看到真机线索）
        QVERIFY2(!sink.isEmpty(), "progress 回调没有接到 setLogSink");
        QVERIFY(sink.join(QLatin1Char('\n')).contains(QStringLiteral("已识别")));
    }

    void prepareShowsSha1MismatchWithBothPrefixes()
    {
        // 换一张**记了修订**的表（8890，eub_loadout.cpp:39 的 9322ccb4…）走不一致分支：
        // 两边的前 8 位都要出现 —— 只给一边用户没法核对（spec §D6）。
        eub::MockEubTransport mock;
        mock.info.socName = QStringLiteral("Exynos8890");
        EubRecoveryDialog dlg(mock, nullptr);
        const QByteArray sboot = syntheticSboot();
        QString err;
        QVERIFY2(dlg.prepare(sboot, QString() /* 无来源路径：该 SoC 无额外文件，本参数用不到 */,
                             QStringLiteral("合成 sboot（用例）"), &err), qPrintable(err));
        QCOMPARE(dlg.loadout().soc, QStringLiteral("Exynos8890"));
        QVERIFY(!dlg.loadout().sbootSha1.isEmpty());           // 前置：本表记了修订
        QVERIFY(dlg.statusText().contains(QStringLiteral("不一致")));
        QVERIFY2(dlg.statusText().contains(QStringLiteral("9322ccb4")), qPrintable(dlg.statusText()));
        QVERIFY2(dlg.statusText().contains(eub::sha1Hex(sboot).left(8)), qPrintable(dlg.statusText()));
    }

    void tableRevisionFormatIsComparableWithFileSha1()
    {
        // 跨模块**格式契约**：表里存的是 hex **文本**（eub_loadout.cpp:39 的 "9322ccb4…"），
        // 而对话框喂给 sha1CompareText 的是 sha1Hex() 产出的 hex 文本。两者必须同格式，否则
        // "用户文件恰好等于表修订"这个唯一可以放心的情形会被永远报成不一致 ——
        // 假阴性比假阳性更坏（用户会去换一个本来正确的固件）。
        // 离线拿不到"sha1 恰好等于表修订"的真 sboot（sha1 单向），故这里钉的是**格式可比值**。
        eub::EubLoadout lo; QString err;
        QVERIFY(eub::eubLoadoutFor(QStringLiteral("Exynos8890"), lo, &err));
        QCOMPARE(lo.sbootSha1.size(), 40);
        QVERIFY(QRegularExpression(QStringLiteral("^[0-9a-f]{40}$"))
                    .match(QString::fromLatin1(lo.sbootSha1)).hasMatch());
        const QString text = EubRecoveryDialog::sha1CompareText(lo.sbootSha1, lo.sbootSha1);
        QVERIFY2(text.contains(QStringLiteral("一致")), qPrintable(text));
        QVERIFY(!text.contains(QStringLiteral("不一致")));
    }

    void prepareShowsUnrecordedRevisionWhenTableHasNoSha1()
    {
        // 终审 I2：7580 的 sbootSha1 已置空（唯一记了修订的那一源正被本表否决，见 eub_loadout.cpp）
        // → 必须走"该表未记录固件修订"分支（明说不知道），**不得**报成一致 —— 报一致会让用户
        // 以为"我的固件与该表所用修订相同"（方向相反的保证）。
        eub::MockEubTransport mock;
        mock.info.socName = QStringLiteral("Exynos7580");
        EubRecoveryDialog dlg(mock, nullptr);
        QString err;
        const QByteArray sboot = syntheticSboot();          // 7580 表最大段到 0x10B000，够用
        QVERIFY2(dlg.prepare(sboot, QString() /* 无来源路径：该 SoC 无额外文件，本参数用不到 */,
                             QStringLiteral("合成 sboot（用例）"), &err), qPrintable(err));
        QCOMPARE(dlg.loadout().soc, QStringLiteral("Exynos7580"));
        QVERIFY(dlg.loadout().sbootSha1.isEmpty());         // 前置：本表确实没有修订记录
        QVERIFY2(dlg.statusText().contains(QStringLiteral("未记录")), qPrintable(dlg.statusText()));
        QVERIFY2(!dlg.statusText().contains(QStringLiteral("sha1 一致")), qPrintable(dlg.statusText()));
        // 仍要给出用户文件自己的前 8 位（不给就无从人工比对）
        QVERIFY2(dlg.statusText().contains(eub::sha1Hex(sboot).left(8)), qPrintable(dlg.statusText()));
    }

    // ---- extraFiles（9830）：源约束 + 包内取件 + 解禁门控（backlog Task 2）----
    // 旧口径（终审 I1）是"预检照常通过但禁用开始"；Task 2 起改为**来源必须是 BL 包、预检时就把
    // 额外文件取出来**：取不出来即预检失败（连表都不给），取出来才放行 —— 两头的失败都在
    // **不碰设备**的预检阶段收口，用户不会点了"开始"才发现少文件。

    void prepareRejectsBareSourceForSocWithExtraFiles()
    {
        // 源约束：9830 的额外文件只存在于 BL_*.tar.md5 里（facts §H2）—— 选裸 sboot.bin 时必须
        // **失败并指引改选 BL 包**（本仓不另开第二个文件选择器，spec §D8 的既定取舍）。
        // 勾选确认不能救活它（门控是"预检通过 AND 勾选"，不是"勾选"）。
        eub::MockEubTransport mock;
        mock.info.socName = QStringLiteral("Exynos9830");
        EubRecoveryDialog dlg(mock, nullptr);
        QCheckBox *box = dlg.findChild<QCheckBox *>(QStringLiteral("eubConfirmCheck"));
        QVERIFY(box != nullptr);
        box->setChecked(true);
        const QByteArray sboot = syntheticSboot(0x400000);   // 9830 表最大段到 0x39B000，够长
        // 真的落一个**裸** sboot.bin（不是拿空路径骗：判据在对话框里，要对真文件成立）
        const QString bare = writeFile(QStringLiteral("sboot.bin"), sboot);
        QString err;
        QVERIFY2(!dlg.prepare(sboot, bare, QStringLiteral("sboot.bin（裸镜像）"), &err),
                 "裸来源 + 带 extraFiles 的表必须预检失败");
        QVERIFY2(err.contains(QStringLiteral("BL_*.tar.md5")), qPrintable(err));   // 可行动：改选什么
        QVERIFY2(err.contains(QStringLiteral("ldfw.img")), qPrintable(err));       // 缺哪几个（列名）
        QVERIFY2(err.contains(QStringLiteral("tzsw.img")), qPrintable(err));
        QVERIFY2(err.contains(QStringLiteral("不是 tar")), qPrintable(err));       // 成因如实（不是"包坏了"）
        QVERIFY(!dlg.isStartEnabledForTest());
        QVERIFY(dlg.loadout().soc.isEmpty());          // fail-closed：不留半张表
    }

    void prepareRejectsTarMissingExtraEntry()
    {
        // 合成了 sboot 与 tzsw、**缺 ldfw** 的 9830 包 → 预检失败，文案点名缺哪个（用户据此换包，
        // 而不是只看到一句"预检失败"）。名称要落到 err（来自 loadNamedEntriesFromTar 的缺失清单）。
        eub::MockEubTransport mock;
        mock.info.socName = QStringLiteral("Exynos9830");
        EubRecoveryDialog dlg(mock, nullptr);
        const QByteArray sboot = syntheticSboot(0x400000);
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry s; s.name = QStringLiteral("sboot.bin"); s.data = sboot;
        imgtar::TarEntry t; t.name = QStringLiteral("tzsw.img.lz4");
        t.data = imgcomp::lz4Compress(QByteArray(0x2000, '\x5A'));
        entries << s << t;
        const QString pkg = writeTar(QStringLiteral("BL_SM-G980F_PART.tar.md5"), entries);

        QCheckBox *box = dlg.findChild<QCheckBox *>(QStringLiteral("eubConfirmCheck"));
        QVERIFY(box != nullptr);
        box->setChecked(true);
        QString err;
        QVERIFY2(!dlg.prepare(sboot, pkg, QStringLiteral("BL_SM-G980F_PART.tar.md5 内的 sboot.bin"), &err),
                 "缺 ldfw 的包必须预检失败（否则会把设备留在半完成状态）");
        QVERIFY2(err.contains(QStringLiteral("ldfw.img")), qPrintable(err));
        QVERIFY(!dlg.isStartEnabledForTest());
        QVERIFY(dlg.loadout().soc.isEmpty());
    }

    void prepareAcceptsTarWithExtraFilesAndUnblocksStart()
    {
        // 完整的合成 9830 包：sboot + ldfw.img（**裸**条目）+ tzsw.img.lz4（压缩条目）——
        // 一次覆盖两条名字优先级（先裸名、再 .lz4），且字节数断言用的是**实际取出的**长度。
        eub::MockEubTransport mock;
        mock.info.socName = QStringLiteral("Exynos9830");
        EubRecoveryDialog dlg(mock, nullptr);
        QStringList sink;
        dlg.setLogSink([&sink](const QString &m, bool) { sink << m; });

        const QByteArray sboot = syntheticSboot(0x400000);
        const QByteArray ldfw(12345, '\x37');          // 与 tzsw 长度刻意不同：互换/顶替都能看出来
        const QByteArray tzsw(0x4321, '\x9C');
        QList<imgtar::TarEntry> entries;
        imgtar::TarEntry s; s.name = QStringLiteral("sboot.bin"); s.data = sboot;
        imgtar::TarEntry l; l.name = QStringLiteral("ldfw.img"); l.data = ldfw;
        imgtar::TarEntry t; t.name = QStringLiteral("tzsw.img.lz4"); t.data = imgcomp::lz4Compress(tzsw);
        entries << s << l << t;
        const QString pkg = writeTar(QStringLiteral("BL_SM-G980F_FULL.tar.md5"), entries);

        QCheckBox *box = dlg.findChild<QCheckBox *>(QStringLiteral("eubConfirmCheck"));
        QVERIFY(box != nullptr);
        QString err;
        QVERIFY2(dlg.prepare(sboot, pkg, QStringLiteral("BL_SM-G980F_FULL.tar.md5 内的 sboot.bin"), &err),
                 qPrintable(err));
        QCOMPARE(dlg.loadout().soc, QStringLiteral("Exynos9830"));

        // 段表（用户在界面上读到的那份）：两个文件名 + 实际字节数；旧口径必须一个不剩
        const QString table = segmentViewText(dlg);
        QVERIFY2(table.contains(QStringLiteral("ldfw.img")), qPrintable(table));
        QVERIFY2(table.contains(QStringLiteral("tzsw.img")), qPrintable(table));
        QVERIFY2(table.contains(QStringLiteral("12,345")), qPrintable(table));    // ldfw 的实际大小
        QVERIFY2(table.contains(QStringLiteral("17,185")), qPrintable(table));    // tzsw 解压后的实际大小
        QVERIFY2(!table.contains(QStringLiteral("不发送")), qPrintable(table));
        QVERIFY2(table.contains(QStringLiteral("段后另发")), qPrintable(table));

        // 门控：勾选前不许开始、勾选后**必须放行**（撤掉 I1 的一票否决是本任务的目的）
        QVERIFY2(!dlg.isStartEnabledForTest(), "未勾选时仍须关闸");
        box->setChecked(true);
        QVERIFY2(dlg.isStartEnabledForTest(), "extras 齐备 + 已勾选 → 开始按钮必须可用");
        // 日志要说明发送计划（用户据此知道设备接下来会发生什么）
        const QString logText = sink.join(QLatin1Char('\n'));
        QVERIFY2(logText.contains(QStringLiteral("ldfw.img")), qPrintable(logText));
    }

    void prepareRejectsUnsupportedSoc()
    {
        eub::MockEubTransport mock;
        mock.info.socName = QStringLiteral("Exynos9999");
        EubRecoveryDialog dlg(mock, nullptr);
        QCheckBox *box = dlg.findChild<QCheckBox *>(QStringLiteral("eubConfirmCheck"));
        QVERIFY(box != nullptr);
        box->setChecked(true);                 // 勾选不能救活一个失败的预检
        QString err;
        QVERIFY(!dlg.prepare(syntheticSboot(), QString(), QString(), &err));
        QVERIFY(!err.isEmpty());
        // 文案必须点名是哪个 SoC 没有表（可行动：用户据此判断是设备不对还是型号不支持）
        QVERIFY2(err.contains(QStringLiteral("Exynos9999")), qPrintable(err));
        QVERIFY(dlg.loadout().soc.isEmpty());  // fail-closed：不得留上一次的表项
        QVERIFY(!dlg.isStartEnabledForTest());
    }

    void prepareRejectsTooShortImage()
    {
        eub::MockEubTransport mock;
        mock.info.socName = QStringLiteral("Exynos9610");
        EubRecoveryDialog dlg(mock, nullptr);
        QCheckBox *box = dlg.findChild<QCheckBox *>(QStringLiteral("eubConfirmCheck"));
        QVERIFY(box != nullptr);
        box->setChecked(true);
        QString err;
        QVERIFY(!dlg.prepare(QByteArray(0x1000, '\xAB'), QString(), QString(), &err));
        QVERIFY(!err.isEmpty());
        // 文案要说清"太短"（而不是笼统失败）——用户据此知道要换与表匹配的固件修订
        QVERIFY2(err.contains(QStringLiteral("太短")), qPrintable(err));
        QVERIFY(!dlg.isStartEnabledForTest());  // 预检失败 → 勾了也不许开始
        // 设备行要说清"设备认出来了，是载荷不匹配"——一律写"识别失败"会把用户的排查方向带偏
        // （该换固件 vs 该查设备，是两条路）。
        QLabel *devLine = dlg.findChild<QLabel *>(QStringLiteral("eubDeviceLabel"));
        QVERIFY(devLine != nullptr);
        QVERIFY2(devLine->text().contains(QStringLiteral("Exynos9610")), qPrintable(devLine->text()));
        QVERIFY(!devLine->text().contains(QStringLiteral("识别失败")));
    }

    void prepareFallsBackToImageSocDetection()
    {
        // facts §A4：极老 SoC 的 iProduct 不报 SoC 名 → 从镜像内容反推（hubble.py:192-202）。
        eub::MockEubTransport mock;
        mock.info.socName = QString();          // 设备没报名字
        EubRecoveryDialog dlg(mock, nullptr);
        QStringList sink;
        dlg.setLogSink([&sink](const QString &m, bool) { sink << m; });
        QString err;
        const QByteArray sboot = sbootWithMarker(QByteArray("EXYNOS9610"));
        QVERIFY2(dlg.prepare(sboot, QString(), QStringLiteral("合成 sboot（镜像内含 EXYNOS9610）"), &err),
                 qPrintable(err));
        QCOMPARE(dlg.loadout().soc, QStringLiteral("Exynos9610"));
        // SoC 名的来源必须写在明处：不写，用户会以为设备自报了型号。断言两半：说"镜像"
        // 且**不声称**"设备自报"（后者是设备行上唯一会误导人的说法）。
        QVERIFY2(dlg.statusText().contains(QStringLiteral("镜像")), qPrintable(dlg.statusText()));
        QVERIFY2(!dlg.statusText().contains(QStringLiteral("设备自报")), qPrintable(dlg.statusText()));
        // 日志要同时给出"镜像"与反推出的 SoC 名 —— 用户据此核对选的 BL 包对不对
        const QString logText = sink.join(QLatin1Char('\n'));
        QVERIFY2(logText.contains(QStringLiteral("镜像")), qPrintable(logText));
        QVERIFY2(logText.contains(QStringLiteral("Exynos9610")), qPrintable(logText));
    }

    void fallbackMessageDoesNotBlameTheDeviceWhenItNeverAppeared()
    {
        // 兜底对**任意** identify 失败生效 —— 设备根本没连上时同样走到这里。此时文案若一律说
        // "设备未自报 SoC 名"就是假话（设备压根没被读到），把用户的排查方向从"为什么连不上"
        // 带偏到"为什么不报名字"。真实原因要原样带出来。
        // 注：对话框的 EubOptions 是构造时固定的默认值（revolveAttempts=20 / poll=500ms，且无
        // 注入点）—— 本槽的"设备未出现"要真的走完重试，代价 ≈ 19×500ms = 9.5s。
        eub::MockEubTransport mock;
        mock.info.socName = QString();
        mock.openFailures = 99;                 // 设备一直没出现
        EubRecoveryDialog dlg(mock, nullptr);
        QStringList sink;
        dlg.setLogSink([&sink](const QString &m, bool) { sink << m; });
        QString err;
        const QByteArray sboot = sbootWithMarker(QByteArray("EXYNOS9610"));
        // 镜像能反推出 SoC → 预检**照样通过**（这正是危险处：设备没连上也能"预检通过"）
        QVERIFY2(dlg.prepare(sboot, QString(), QStringLiteral("合成 sboot（镜像内含 EXYNOS9610）"), &err),
                 qPrintable(err));
        QCOMPARE(dlg.loadout().soc, QStringLiteral("Exynos9610"));
        const QString logText = sink.join(QLatin1Char('\n'));
        QVERIFY2(logText.contains(QStringLiteral("注入的打开失败")), qPrintable(logText));  // 真实原因带出来了
        QVERIFY2(!logText.contains(QStringLiteral("设备未自报")), qPrintable(logText));
        QVERIFY2(!dlg.statusText().contains(QStringLiteral("设备自报")), qPrintable(dlg.statusText()));
    }

    void prepareRejectsWhenNeitherDeviceNorImageNamesTheSoc()
    {
        eub::MockEubTransport mock;
        mock.info.socName = QString();          // 设备没报名字
        EubRecoveryDialog dlg(mock, nullptr);
        QString err;
        // 镜像里没有 EXYNOS<型号> 字样（合成随机字节）→ 兜底也失败
        QVERIFY(!dlg.prepare(syntheticSboot(), QString(), QString(), &err));
        QVERIFY(!err.isEmpty());
        QVERIFY2(err.contains(QStringLiteral("无法从镜像识别")), qPrintable(err));
        QVERIFY2(err.contains(QStringLiteral("自报 SoC 名")), qPrintable(err));  // 兜底的存在理由
        QVERIFY(dlg.loadout().soc.isEmpty());
    }

    void startButtonIsGatedByConfirmationCheckbox()
    {
        eub::MockEubTransport mock;
        mock.info.socName = QStringLiteral("Exynos9610");
        EubRecoveryDialog dlg(mock, nullptr);
        QString err;

        QCheckBox *box  = dlg.findChild<QCheckBox *>(QStringLiteral("eubConfirmCheck"));
        QPushButton *btn = dlg.findChild<QPushButton *>(QStringLiteral("eubStartButton"));
        QVERIFY2(box != nullptr, "确认勾选框未命名（eubConfirmCheck）");
        QVERIFY2(btn != nullptr, "开始按钮未命名（eubStartButton）");

        // 只勾选、还没预检 → 不许开始（门控是"预检 AND 勾选"，不是"勾选"）
        box->setChecked(true);
        QVERIFY(!dlg.isStartEnabledForTest());

        QVERIFY(dlg.prepare(syntheticSboot(), QString(), QString(), &err));
        QVERIFY(dlg.isStartEnabledForTest());
        // 断言对象是**真实控件**：isStartEnabledForTest 必须反映它，不能自己另算一份
        QVERIFY(btn->isEnabled());

        box->setChecked(false);
        QVERIFY(!dlg.isStartEnabledForTest());  // 取消勾选要立即关闸（toggled 接线）
        QVERIFY(!btn->isEnabled());
    }

    void dialogShowsUnverifiedNotice()
    {
        // facts §F1 + spec §11：不得让界面暗示"已验证"。这条提示是用户判断"要不要拿它救砖"的
        // 依据，且是本相位"无真机"边界在 UI 上的唯一落点 —— 抽成具名控件以便钉住。
        eub::MockEubTransport mock;
        EubRecoveryDialog dlg(mock, nullptr);
        QLabel *note = dlg.findChild<QLabel *>(QStringLiteral("eubUnverifiedNote"));
        QVERIFY2(note != nullptr, "未验证提示缺失（或未命名 eubUnverifiedNote）");
        QVERIFY2(note->text().contains(QStringLiteral("未在真机上验证")), qPrintable(note->text()));
        QVERIFY(note->text().contains(QStringLiteral("照抄")));   // 说清这些数字从哪来
    }

    void rescueSuccessTextStatesNoStorageWasWritten()
    {
        // facts §F5 / §D1：只发 RAM 镜像、不写存储，bootloader 仍处于被擦状态，必须继续刷写。
        // 这段文案是**成功路径**唯一的告知面，抽成纯函数以便离线钉住（否则只能靠模态弹窗人工看）。
        const QString t = EubRecoveryDialog::rescueSuccessText();
        QVERIFY2(t.contains(QStringLiteral("未写")), qPrintable(t));
        QVERIFY2(t.contains(QStringLiteral("仍需正常刷写")), qPrintable(t));
        QVERIFY(t.contains(QStringLiteral("Download")));          // 交接目标：Download 模式（§D1）
        QVERIFY(t.contains(QStringLiteral("请继续")));            // 可行动：下一步做什么（§D10）
        // Task 2：9830 会在分段之后另发额外文件 —— 成功文案不得**少报**已发生的动作
        const QString withExtras = EubRecoveryDialog::rescueSuccessText(true);
        QVERIFY2(withExtras.contains(QStringLiteral("额外文件")), qPrintable(withExtras));
        QVERIFY2(withExtras.contains(QStringLiteral("未写")), qPrintable(withExtras));   // 未写存储这句不能丢
    }

    // ---- 真样本槽（reference/eub-samples/，gitignored；缺失时 QSKIP/FAIL，见 CMakeLists 的
    //      EUB_SAMPLES_DIR / EUB_SAMPLES_REQUIRED=ON）----
    // 事实出处：docs/superpowers/specs/exynos-eub-facts.md §H2 与 .superpowers/sdd 的独立取值报告。
    // 只读样本、绝不写回；样本内容不进仓库（CMake 只把**目录路径**编进本目标）。

    void real9830BlPackageUnblocksStartWithRealExtraSizes()
    {
        // 9830 的**真** BL 包走完整预检（与 onPickSource 同一条入口：loadSbootBytes → prepare）：
        // 段表里的字节数必须是**从真包解压出来的** 6,291,456 / 1,572,864（facts §H2）。
        // 与合成槽（12,345 / 17,185）合起来才钉得住"数字来自包"：单看本槽，硬编这两个数字也全绿。
        const QString pkg = QStringLiteral("BL_SM-G980F_G980FXXSNHYB1.tar.md5");   // SM-G980F = Exynos9830
        if (!eubtest::sampleAvailable(pkg))
            EUB_SKIP_OR_FAIL(pkg);
        const QString tar = eubtest::samplePath(pkg);

        eub::MockEubTransport mock;
        mock.info.socName = QStringLiteral("Exynos9830");
        EubRecoveryDialog dlg(mock, nullptr);
        eub::SbootSource src;
        QByteArray sboot;
        QString err;
        QVERIFY2(eub::loadSbootBytes(tar, sboot, &src, &err), qPrintable(err));   // 真包里的 sboot.bin.lz4
        QVERIFY2(dlg.prepare(sboot, tar, src.description, &err), qPrintable(err));
        QCOMPARE(dlg.loadout().soc, QStringLiteral("Exynos9830"));

        const QString table = segmentViewText(dlg);
        QVERIFY2(table.contains(QStringLiteral("6,291,456")), qPrintable(table));   // ldfw 解压后 0x600000
        QVERIFY2(table.contains(QStringLiteral("1,572,864")), qPrintable(table));   // tzsw 解压后 0x180000
        QVERIFY2(!table.contains(QStringLiteral("不发送")), qPrintable(table));

        QCheckBox *box = dlg.findChild<QCheckBox *>(QStringLiteral("eubConfirmCheck"));
        QVERIFY(box != nullptr);
        QVERIFY(!dlg.isStartEnabledForTest());
        box->setChecked(true);
        QVERIFY2(dlg.isStartEnabledForTest(), "真包齐备 + 已勾选 → 开始按钮必须可用");
    }
};

QTEST_MAIN(TestEubRecoveryDialog)
#include "test_eub_recovery_dialog.moc"
