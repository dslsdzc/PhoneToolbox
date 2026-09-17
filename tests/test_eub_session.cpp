// tests/test_eub_session.cpp
//
// 会话编排：段序、每段重开、重试、失败语义、进度单调 —— 全部经 MockEubTransport（无真机，facts §F1）。
// 断言对象只有 mock 的记录（帧字节 / 调用序列 / 睡眠参数）与错误、进度文案 —— 不碰真设备，
// 也不依赖真实时间（sleep 一律注入，见 fastOptions）。
#include <QtTest>

#include "core/eub/eub_session.h"
// 只为常量锚点：本头**只前向声明** libusb 类型（不含 libusb.h、不拖链接），故不破坏
// "test_eub_session 无 libusb 依赖"的契约（CMakeLists.txt:567-576）；本文件也不构造其对象。
#include "core/eub/eub_libusb_transport.h"
#include "mock_eub_transport.h"

namespace {

// 合成 sboot：够 9610 全表（最大段到 0x1DA000 + 0x40000 = 0x21A000）
// 非周期填充（xorshift32）：`(i * k) & 0xFF` 是 256 周期图案，会让"偏移错 256 的整数倍"漏检
//（T4 实现者的变异证据：brief 原夹具下 offset 错 0x100 时 split 槽仍全绿）
QByteArray syntheticSboot(quint64 bytes = 0x220000)
{
    QByteArray b(int(bytes), '\0');
    quint32 x = 0x12345678u;
    for (int i = 0; i < b.size(); ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b[i] = char(x & 0xFF); }
    return b;
}

eub::EubOptions fastOptions()
{
    eub::EubOptions o;
    o.segmentGapMs = 0;         // 用例里不等真时间
    o.revolvePollMs = 0;
    o.revolveAttempts = 3;
    o.sleepFn = [](int) {};     // 注入空实现
    return o;
}

// 逐段调用序列的期望值（空串 = 一致，否则返回差异描述，供 QVERIFY2 打印）。
// 契约（eub_session.h / brief 行为契约 §2）：每段都是 open →（**仅第 1 段** info）→ write →
//（responseSupport 表项读一次 read）→ close，段与段之间无其它调用。
// 写成"返回差异"而不是在助手函数里直接 QCOMPARE：QCOMPARE 的失败分支只从**助手函数** return，
// 槽函数会继续往下跑（假绿风险）。
QString callSequenceDiff(const eub::MockEubTransport &t, int segmentCount,
                         bool expectInfo, bool expectRead)
{
    QStringList expect;
    for (int i = 0; i < segmentCount; ++i) {
        expect << QStringLiteral("open");
        if (i == 0 && expectInfo)
            expect << QStringLiteral("info");
        expect << QStringLiteral("write");
        if (expectRead)
            expect << QStringLiteral("read");
        expect << QStringLiteral("close");
    }
    if (t.calls == expect)
        return QString();
    return QStringLiteral("调用序列不符\n  期望：%1\n  实际：%2")
        .arg(expect.join(QStringLiteral(",")), t.calls.join(QStringLiteral(",")));
}

// details 里含 needle 的条数（notes 转发的**次数**要用它：contains 只回答"转没转过"）。
int countDetails(const QStringList &details, const QString &needle)
{
    int n = 0;
    for (const QString &d : details)
        if (d.contains(needle))
            ++n;
    return n;
}

} // namespace

class TestEubSession : public QObject
{
    Q_OBJECT

private:
    static eub::EubLoadout loadout9610()
    {
        eub::EubLoadout lo; QString err;
        if (!eub::eubLoadoutFor(QStringLiteral("Exynos9610"), lo, &err)) qFatal("查表失败");
        return lo;
    }
    static eub::EubLoadout loadout9830()
    {
        eub::EubLoadout lo; QString err;
        if (!eub::eubLoadoutFor(QStringLiteral("Exynos9830"), lo, &err)) qFatal("查表失败");
        return lo;
    }
    // 9830 原表带 extraFiles（ldfw.img/tzsw.img）→ run 会 fail-closed 拒绝执行（见
    // extraFilesTableIsRejectedWithoutTouchingDevice）。回显接线与 extraFiles 无关，故本助手把该
    // 字段清掉：那个槽钉的是"回显读在写之后、读几次"，不是 9830 整表的可执行性。
    static eub::EubLoadout runnable9830ForEchoTest()
    {
        eub::EubLoadout lo = loadout9830();
        lo.extraFiles.clear();
        return lo;
    }
    static eub::EubDeviceInfo info9610()
    {
        eub::EubDeviceInfo i;
        i.socName = QStringLiteral("Exynos9610");
        i.socId = QStringLiteral("0123456789abcde");
        i.chipId = QStringLiteral("0123456789abcdef");
        i.usbBootVersion = QStringLiteral("1.0");
        return i;
    }

private slots:
    void sendsEverySegmentAsOneFrameInOrder()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        eub::EubSession s(t, fastOptions());
        const QByteArray sboot = syntheticSboot();
        QString err;
        QVERIFY2(s.run(loadout9610(), sboot, &err), qPrintable(err));

        const eub::EubLoadout lo = loadout9610();
        QCOMPARE(t.writes.size(), lo.segments.size());
        for (int i = 0; i < lo.segments.size(); ++i) {
            const eub::EubSegment &seg = lo.segments[i];
            QString ferr;
            const QByteArray expect = eub::buildEubFrame(sboot.mid(int(seg.offset), int(seg.length)),
                                                         lo.style, &ferr);
            QVERIFY2(!expect.isEmpty(), qPrintable(ferr));
            QCOMPARE(t.writes[i], expect);
        }
    }

    void closesAndReopensForEverySegment()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY(s.run(loadout9610(), syntheticSboot(), &err));
        QCOMPARE(t.calls.count(QStringLiteral("open")), 6);     // 9610 是 6 段
        QCOMPARE(t.calls.count(QStringLiteral("write")), 6);
        QCOMPARE(t.calls.count(QStringLiteral("close")), 6);
        QCOMPARE(t.calls.first(), QStringLiteral("open"));
        QCOMPARE(t.calls.last(), QStringLiteral("close"));
        // 加固（约束 9）：计数断言管不住**顺序** —— 上面的 count 在"先 close 再 write"这类
        // 错序下仍全绿。这里逐段核对整条序列（9610 无回显 → 无 read）。
        const QString diff = callSequenceDiff(t, 6, /*expectInfo=*/true, /*expectRead=*/false);
        QVERIFY2(diff.isEmpty(), qPrintable(diff));
    }

    void retriesOpenUntilDeviceAppears()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        t.openFailures = 2;                     // 头两次"设备还没出现"
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY2(s.run(loadout9610(), syntheticSboot(), &err), qPrintable(err));
        QCOMPARE(t.writes.size(), 6);
        // 加固（约束 9）：writes.size() 管不住重试 —— 一个"忽略 open 返回值"的实现同样写出 6 帧
        //（mock 的 writeBulk 不依赖打开状态）。第 1 段的 2 次失败 + 后续 5 段各 1 次成功 = 8 次 open。
        QCOMPARE(t.calls.count(QStringLiteral("open")), 8);
    }

    void failsWhenDeviceNeverAppears()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        t.openFailures = 99;                    // 一直没出现
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY(!s.run(loadout9610(), syntheticSboot(), &err));
        QVERIFY(!err.isEmpty());
        QCOMPARE(t.calls.count(QStringLiteral("open")), 3);     // revolveAttempts
        QCOMPARE(t.writes.size(), 0);
    }

    void writeFailureNamesTheSegmentAndStops()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        t.failWriteAt = 1;                      // 第 2 段（epbl）写失败
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY(!s.run(loadout9610(), syntheticSboot(), &err));
        QCOMPARE(t.writes.size(), 1);           // 出错即停，不再发后续段
        QVERIFY(err.contains(QStringLiteral("epbl")));
        QVERIFY(err.contains(QStringLiteral("0x2000")));       // 偏移可复盘
        QVERIFY(err.contains(QStringLiteral("0x13000")));      // 长度也要在（9610 epbl = (0x2000, 0x13000)）
        // 加固：两个 contains 单独都**管不住 .arg 互换**（互换后是 "0x13000/0x2000"，两个串都在）
        // —— 再钉一次**顺序**（偏移在前、长度在后，格式见 eub_session.cpp）。
        QVERIFY2(err.contains(QStringLiteral("0x2000/0x13000")), qPrintable(err));
        QVERIFY(err.contains(QStringLiteral("第 2 段")));       // 段序号（文案格式见 eub_session.h）
        QVERIFY(err.contains(QStringLiteral("注入的写失败")));   // 末位必须是**失败原因**（%5），不能吞掉
        QCOMPARE(t.calls.last(), QStringLiteral("close"));     // 句柄收干净
    }

    void socMismatchBetweenIdentifyAndRunFails()
    {
        // 守卫的本意是防"识别与开始之间**换了设备**"—— 故基准是**设备自己**识别时自报的串
        //（不是布局表：兜底路径下两者永不相等，见 fallbackLoadoutIsAcceptedWhenDeviceNameUnchanged）。
        // 本槽走完整序：identify 时自报 Exynos9610（成功、名字被记下）→ 换设备（现在自报 8890）
        // → run 必须拒绝、零写入；文案**两个名字都要有**（只说一个，用户无从判断"换成了什么"）。
        eub::MockEubTransport t;
        t.info = info9610();
        eub::EubSession s(t, fastOptions());
        eub::EubLoadout lo;
        QString err;
        QVERIFY2(s.identify(lo, &err), qPrintable(err));       // 此时设备自报 Exynos9610
        QCOMPARE(t.writes.size(), 0);

        t.info.socName = QStringLiteral("Exynos8890");         // 用户中途换了设备
        QVERIFY(!s.run(loadout9610(), syntheticSboot(), &err));
        QVERIFY2(err.contains(QStringLiteral("Exynos8890")), qPrintable(err));   // 现在自报的
        QVERIFY2(err.contains(QStringLiteral("Exynos9610")), qPrintable(err));   // 识别时自报的
        QCOMPARE(t.writes.size(), 0);                          // 零写入
        QCOMPARE(t.calls.last(), QStringLiteral("close"));     // 句柄照收
    }

    void fallbackLoadoutIsAcceptedWhenDeviceNameUnchanged()
    {
        // 跨任务缝合（T7 对话框兜底 ↔ T6 run 守卫）：设备不自报 SoC 名时，调用方按**镜像内容**
        // 反推后选表（facts §A4）—— 这条路径产出的 lo.soc 与设备自报串（空）永不相等。
        // 拿 lo.soc 当基准（旧契约）＝"预检能过、点开始必被拒"，整条兜底路径不可达。
        eub::MockEubTransport t;
        t.info = info9610();
        t.info.socName.clear();                     // 设备不自报名字（极老 SoC，facts §A4）
        QStringList details;
        eub::EubSession s(t, fastOptions(),
                          [&](const eub::EubProgress &p) { details << p.detail; });
        eub::EubLoadout lo;
        QString err;
        QVERIFY(!s.identify(lo, &err));             // 无表可查 → 识别失败
        QVERIFY2(err.contains(QStringLiteral("detectSocFromImage")), qPrintable(err));

        const eub::EubLoadout fallback = loadout9610();   // 调用方反推 SoC 名后查到的表
        QVERIFY2(s.run(fallback, syntheticSboot(), &err), qPrintable(err));
        QCOMPARE(t.writes.size(), fallback.segments.size());   // 兜底路径真的发完了
        // 变异③的钉子：identify 的记录点必须**在查表之前**。挪到查表之后，本会话（查表失败早退）
        // 就记不下名字 → run 走"没识别过"分支，下面这句会出现在日志里（＝换设备守卫根本没跑）。
        QVERIFY2(countDetails(details, QStringLiteral("跳过换设备核对")) == 0,
                 qPrintable(details.join(QStringLiteral(" | "))));
    }

    void runWithoutIdentifySkipsSwapCheck()
    {
        // run 是公开 API，可单独调用：此时没有可比基准 → **不阻断**（阻断会把"直接调 run"的
        // 合法用法一并打死），但必须在日志里留下"核对没跑"的痕迹 —— 静默跳过会让用户以为
        // 换设备守卫一直开着。
        eub::MockEubTransport t;
        t.info = info9610();
        QStringList details;
        eub::EubSession s(t, fastOptions(),
                          [&](const eub::EubProgress &p) { details << p.detail; });
        QString err;
        QVERIFY2(s.run(loadout9610(), syntheticSboot(), &err), qPrintable(err));
        QCOMPARE(t.writes.size(), loadout9610().segments.size());
        QCOMPARE(countDetails(details, QStringLiteral("跳过换设备核对")), 1);
    }

    void emptyNamesAreNotedAsUnverifiableIdentity()
    {
        // 空↔空（facts §A4 的极老 SoC）：比较形式上通过、实质上**没有可比的设备身份**。
        // 不在日志里点明，对话框那句"发送前仍会核对设备身份"就是空话（T7 复审意见）。
        eub::MockEubTransport t;
        t.info = info9610();
        t.info.socName.clear();
        QStringList details;
        eub::EubSession s(t, fastOptions(),
                          [&](const eub::EubProgress &p) { details << p.detail; });
        eub::EubLoadout lo;
        QString err;
        QVERIFY(!s.identify(lo, &err));            // 无自报名 → 识别失败（但已记录基准）
        QVERIFY2(s.run(loadout9610(), syntheticSboot(), &err), qPrintable(err));
        QCOMPARE(countDetails(details, QStringLiteral("无法核对设备身份")), 1);
        QCOMPARE(countDetails(details, QStringLiteral("跳过换设备核对")), 0);   // 基准在，核对跑过
    }

    void identifyAttemptedButUnreadableIsDistinguishedFromNotIdentifying()
    {
        // 两种"无基准"的成因要分开说：没识别（调用方顺序）vs 识别了但读不到自述（设备/连接）。
        // 混成一句会把后者说成前者，用户会去查自己的操作顺序（T7 复审意见）。
        eub::MockEubTransport t;
        t.info = info9610();
        t.infoResult = false;                      // identify 时读不到设备自述
        QStringList details;
        eub::EubSession s(t, fastOptions(),
                          [&](const eub::EubProgress &p) { details << p.detail; });
        eub::EubLoadout lo;
        QString err;
        QVERIFY(!s.identify(lo, &err));
        t.infoResult = true;                       // 设备恢复了
        QVERIFY2(s.run(loadout9610(), syntheticSboot(), &err), qPrintable(err));
        QCOMPARE(countDetails(details, QStringLiteral("识别时未读到设备自述")), 1);
        QCOMPARE(countDetails(details, QStringLiteral("未先识别设备")), 0);
    }

    void warnsWhenDeviceNameDiffersFromFallbackTable()
    {
        // 另一条兜底路径：设备**自报了一个表里没有的名字** → 调用方按镜像反推选表（facts §A4）。
        // 身份没变（仍是那个名字）→ 不阻断，但设备自报串与所用布局表不同名，日志必须点明
        // "表可能不匹配本机"（用户据此决定换固件还是换表），且两个名字写在**同一条**里。
        eub::MockEubTransport t;
        t.info = info9610();
        t.info.socName = QStringLiteral("Exynos9999");     // 自报了，但没有对应布局表
        QStringList details;
        eub::EubSession s(t, fastOptions(),
                          [&](const eub::EubProgress &p) { details << p.detail; });
        eub::EubLoadout lo;
        QString err;
        QVERIFY(!s.identify(lo, &err));
        QVERIFY2(err.contains(QStringLiteral("Exynos9610")), qPrintable(err));   // 文案列出支持的 SoC

        QVERIFY2(s.run(loadout9610(), syntheticSboot(), &err), qPrintable(err));
        QCOMPARE(t.writes.size(), loadout9610().segments.size());
        bool warned = false;
        for (const QString &d : details)
            if (d.contains(QStringLiteral("表可能不匹配本机")))
                warned = d.contains(QStringLiteral("Exynos9999"))
                      && d.contains(QStringLiteral("Exynos9610"));
        QVERIFY2(warned, qPrintable(details.join(QStringLiteral(" | "))));
        QVERIFY(countDetails(details, QStringLiteral("跳过换设备核对")) == 0);   // 核对确实跑了
    }

    void echoIsReadOnlyWhenResponseSupport()
    {
        // 9610：不读回显
        {
            eub::MockEubTransport t; t.info = info9610();
            eub::EubSession s(t, fastOptions());
            QString err;
            QVERIFY(s.run(loadout9610(), syntheticSboot(), &err));
            QCOMPARE(t.calls.count(QStringLiteral("read")), 0);
            const QString diff = callSequenceDiff(t, 6, true, false);
            QVERIFY2(diff.isEmpty(), qPrintable(diff));
        }
        // 9830：responseSupport = true → 每段读一次，且**读在写之后**（序列助手钉住 read 的位置）
        {
            const eub::EubLoadout lo = runnable9830ForEchoTest();
            eub::MockEubTransport t;
            t.info = info9610(); t.info.socName = QStringLiteral("Exynos9830");
            t.response = QByteArray("boot ok");
            QStringList details;
            eub::EubSession s(t, fastOptions(),
                              [&](const eub::EubProgress &p) { details << p.detail; });
            QString err;
            QVERIFY2(s.run(lo, syntheticSboot(0x400000), &err), qPrintable(err));
            QCOMPARE(t.calls.count(QStringLiteral("read")), lo.segments.size());
            const QString diff = callSequenceDiff(t, int(lo.segments.size()), true, true);
            QVERIFY2(diff.isEmpty(), qPrintable(diff));
            // 回显"原文落日志"（spec §7）：只调 readBulk 不落日志等于把设备的话丢掉。
            bool echoLogged = false;
            for (const QString &d : details)
                if (d.contains(QStringLiteral("boot ok")))
                    echoLogged = true;
            QVERIFY2(echoLogged, qPrintable(details.join(QStringLiteral(" | "))));
        }
        // 9830 但设备没回话（空回显）：**不判失败**（spec §7 best-effort）
        {
            eub::MockEubTransport t;
            t.info = info9610(); t.info.socName = QStringLiteral("Exynos9830");
            eub::EubSession s(t, fastOptions());     // response 留空 → readBulk 返回空
            QString err;
            QVERIFY2(s.run(runnable9830ForEchoTest(), syntheticSboot(0x400000), &err), qPrintable(err));
            QCOMPARE(t.calls.count(QStringLiteral("read")), runnable9830ForEchoTest().segments.size());
        }
    }

    void identifyReportsUnknownSocWithoutWriting()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        t.info.socName = QStringLiteral("Exynos9999");
        eub::EubSession s(t, fastOptions());
        eub::EubLoadout lo;
        QString err;
        QVERIFY(!s.identify(lo, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(err.contains(QStringLiteral("Exynos9610")));   // 列出支持项
        QVERIFY2(lo.soc.isEmpty(), "查表失败必须清空出参（fail-closed，见 eub_loadout.h）");
        QCOMPARE(t.writes.size(), 0);
        QCOMPARE(t.calls.last(), QStringLiteral("close"));     // identify 也不留句柄
    }

    void identifyFailsWhenDeviceInfoUnreadable()
    {
        // readDeviceInfo 的失败分支。实现者原报告称"mock 无法注入"—— **不准确**：能注入
        // （mock_eub_transport.h 的 infoResult，:26/:43）；真正无法注入的只有 readBulk 的失败
        // （:63-67 恒返回 response，没有失败注入点）。
        eub::MockEubTransport t;
        t.info = info9610();
        t.infoResult = false;
        eub::EubSession s(t, fastOptions());
        eub::EubLoadout lo = loadout9610();     // 预置非空：失败后必须被清掉（fail-closed）
        QString err;
        QVERIFY(!s.identify(lo, &err));
        QVERIFY2(err.contains(QStringLiteral("读取设备自述失败")), qPrintable(err));
        QVERIFY2(err.contains(QStringLiteral("注入的信息读取失败")), qPrintable(err));  // 原因不能吞掉
        QVERIFY2(lo.soc.isEmpty(), qPrintable(QStringLiteral("自述读失败后出参未清空：%1").arg(lo.soc)));
        QCOMPARE(t.writes.size(), 0);                                   // 识别阶段一个字节不发
        QCOMPARE(t.calls.last(), QStringLiteral("close"));              // 句柄照关（identify 不持有）
    }

    void runFailsWhenFirstSegmentDeviceInfoUnreadable()
    {
        // run 的第 1 段核对 SoC 之前的那次 readDeviceInfo 失败：**零写入** + 收干净句柄
        // （eub_session.cpp 的 close 在两个早退分支之前）。
        eub::MockEubTransport t;
        t.info = info9610();
        t.infoResult = false;
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY(!s.run(loadout9610(), syntheticSboot(), &err));
        QVERIFY2(err.contains(QStringLiteral("第 1 段")), qPrintable(err));
        QVERIFY2(err.contains(QStringLiteral("注入的信息读取失败")), qPrintable(err));
        QCOMPARE(t.writes.size(), 0);
        QCOMPARE(t.calls, QStringList({QStringLiteral("open"), QStringLiteral("info"),
                                       QStringLiteral("close")}));      // 只碰了第 1 段就收手
    }

    void identifyClearsOutOnFailurePaths()
    {
        // fail-closed（与 eub_loadout.h:44-46 同一约定）：identify 的**三条**失败路径
        //（打开失败 / 自述读失败 / SoC 名为空）都必须清出参 —— 调用方忽略返回值时不能拿到
        // 上一次的表项，否则"识别失败"会被当成"识别成功"直接进 run（救援工具最坏的假成功）。
        // 三条路径各预置一张非空表：不清就看得见。失败**收集后一次报出**（QVERIFY2 在槽内即 return：
        // 写成三个独立断言的话，只有第一条路径的残留会被看见，另外两条永远轮不到报）。
        QStringList leaked;
        // 入参是 identify 的**返回值**（成功 = true）；soc 必须由调用点**先调用后取**再传进来
        // —— 写成 check(..., s.identify(lo,&err), lo.soc) 会踩未定序求值（GCC 自右向左：读到的
        // 是调用**前**的 lo.soc），那样断言会永远绿。
        const auto check = [&leaked](const QString &path, bool ok, const QString &soc) {
            if (ok)
                leaked << QStringLiteral("%1：identify 竟然成功了").arg(path);
            else if (!soc.isEmpty())
                leaked << QStringLiteral("%1：出参残留 %2").arg(path, soc);
        };
        {
            eub::MockEubTransport t; t.info = info9610(); t.openFailures = 99;
            eub::EubSession s(t, fastOptions());
            eub::EubLoadout lo = loadout9610();
            QString err;
            const bool ok = s.identify(lo, &err);
            check(QStringLiteral("打开失败"), ok, lo.soc);
        }
        {
            eub::MockEubTransport t; t.info = info9610(); t.infoResult = false;
            eub::EubSession s(t, fastOptions());
            eub::EubLoadout lo = loadout9610();
            QString err;
            const bool ok = s.identify(lo, &err);
            check(QStringLiteral("自述读失败"), ok, lo.soc);
        }
        {
            eub::MockEubTransport t; t.info = info9610(); t.info.socName.clear();
            eub::EubSession s(t, fastOptions());
            eub::EubLoadout lo = loadout9610();
            QString err;
            const bool ok = s.identify(lo, &err);
            check(QStringLiteral("SoC 名为空"), ok, lo.soc);
        }
        QVERIFY2(leaked.isEmpty(), qPrintable(leaked.join(QStringLiteral(" | "))));
    }

    void readEchoConstantsMatchTransportHint()
    {
        // 两处常量靠人同步（本层故意不 include 传输实现：见 eub_session.h 的常量注释）——
        // 用**字面值**钉住，漂移即红。static constexpr 是常量表达式：本用例只读值、不构造传输
        // 对象，故不会碰任何 libusb 符号（头只前向声明类型）。
        // 注：这两个常量在 eub **命名空间**作用域（非 EubSession 成员）。
        QCOMPARE(eub::kReadEchoTimeoutMs, 50);                          // facts §B2（hubble.py:115）
        QCOMPARE(eub::LibusbEubTransport::kReadTimeoutMs, 50);          // 与上者同源
        QCOMPARE(eub::kReadEchoMaxBytes, 512);                          // hubble.py:115 的 read(0x81, 512, 50)
    }

    void identifyFillsLoadoutAndCloses()
    {
        // identify 的**成功路径**：此前只有失败路径被覆盖 —— 出参为空或漏 close 都无人发现。
        eub::MockEubTransport t;
        t.info = info9610();
        eub::EubSession s(t, fastOptions());
        eub::EubLoadout lo;
        QString err;
        QVERIFY2(s.identify(lo, &err), qPrintable(err));
        QCOMPARE(lo.soc, QStringLiteral("Exynos9610"));
        QCOMPARE(lo.segments.size(), 6);
        QCOMPARE(t.calls, QStringList({QStringLiteral("open"), QStringLiteral("info"),
                                       QStringLiteral("close")}));
        QCOMPARE(t.writes.size(), 0);                          // 识别阶段一个字节都不发
    }

    void identifyForwardsTransportNotes()
    {
        // 硬要求（T1/T3 审查）：open 期间的非致命说明（如"端点回退到常数 0x02/0x81"）必须转进日志，
        // 否则真机上这条线索会丢。两条路径都要转：identify() 与 run()。
        const QString note = QStringLiteral("EUB 端点回退：改用参照实现的常数 0x02/0x81");
        {
            eub::MockEubTransport t;
            t.info = info9610();
            t.noteList = {note};
            QStringList details;
            QStringList stages;
            eub::EubSession s(t, fastOptions(), [&](const eub::EubProgress &p) {
                details << p.detail; stages << p.stage;
            });
            eub::EubLoadout lo;
            QString err;
            QVERIFY2(s.identify(lo, &err), qPrintable(err));
            int hit = -1;
            for (int i = 0; i < details.size(); ++i)
                if (details[i].contains(note))
                    hit = i;
            QVERIFY2(hit >= 0, qPrintable(details.join(QStringLiteral(" | "))));
            // 落在 identify 阶段（阶段名是 UI 契约，见 eub_session.h）
            QCOMPARE(stages[hit], QStringLiteral("identify"));
        }
        {
            eub::MockEubTransport t;
            t.info = info9610();
            t.noteList = {note};
            QStringList details;
            eub::EubSession s(t, fastOptions(),
                              [&](const eub::EubProgress &p) { details << p.detail; });
            QString err;
            QVERIFY2(s.run(loadout9610(), syntheticSboot(), &err), qPrintable(err));
            bool found = false;
            for (const QString &d : details)
                if (d.contains(note))
                    found = true;
            QVERIFY2(found, qPrintable(details.join(QStringLiteral(" | "))));
        }
    }

    void runOpenFailureForwardsTransportNotes()
    {
        // 终审 M-a：run 的 open 失败分支此前**不**调 forwardNotes（identify 的同型分支调了）。
        // "设备在但打不开"时，传输层的现场说明（端点回退/补设配置）恰恰是唯一线索 —— 丢掉它，
        // 用户只剩一句"设备在 N 次尝试内未出现"，分不清是没插好还是端点不对（与 identify 同因）。
        const QString note = QStringLiteral("EUB 端点回退：改用参照实现的常数 0x02/0x81");
        eub::MockEubTransport t;
        t.info = info9610();
        t.noteList = {note};
        t.openFailures = 99;                    // 一直打不开 → 走 run 的 open 失败早退
        QStringList details;
        eub::EubSession s(t, fastOptions(),
                          [&](const eub::EubProgress &p) { details << p.detail; });
        QString err;
        QVERIFY(!s.run(loadout9610(), syntheticSboot(), &err));
        QVERIFY2(err.contains(QStringLiteral("未出现")), qPrintable(err));   // 确认走的是 open 失败那条
        // 变异证据：去掉 run open 失败分支里的 forwardNotes() → 这条从 1 变 0，本槽红
        QCOMPARE(countDetails(details, note), 1);
        QCOMPARE(t.writes.size(), 0);
    }

    void notesForwardedAgainWhenBatchChangesBetweenIdentifyAndRun()
    {
        // notes 的作用域是**单次 open**（传输层每次 open 清空、close 不清，见 eub_session.h）：
        // 自然用法"先 identify() 再 run()"里，两阶段的 open 属于**不同批次**。会话级"只转一次"
        // 的标志会让 identify 那次就把开关置位 → run 里 N 次 open 的说明**全部静默丢弃**，
        // 丢的正是 notes 存在的理由（真机线索：端点回退 / 补设配置）。
        const QString noteA = QStringLiteral("EUB 端点回退：改用参照实现的常数 0x02/0x81");
        const QString noteB = QStringLiteral("读活动配置未确认已配置，已显式补设配置 1");
        eub::MockEubTransport t;
        t.info = info9610();
        t.noteList = {noteA};
        QStringList details;
        eub::EubSession s(t, fastOptions(), [&](const eub::EubProgress &p) { details << p.detail; });
        eub::EubLoadout lo;
        QString err;
        QVERIFY2(s.identify(lo, &err), qPrintable(err));
        QCOMPARE(countDetails(details, noteA), 1);      // 批次 A 在 identify 转出

        // run 阶段换批次（mock 的 noteList 就是"下一次 open 会报什么"）：B 必须转出
        t.noteList = {noteB};
        details.clear();
        QVERIFY2(s.run(loadout9610(), syntheticSboot(), &err), qPrintable(err));
        QCOMPARE(countDetails(details, noteB), 1);      // 变异：只转一次 → 这里是 0
        QCOMPARE(countDetails(details, noteA), 0);      // 旧批次不跟着刷

        // 反向（防"干脆去掉去重"）：批次**没变**时 run 的 6 次 open 不重复刷
        details.clear();
        QVERIFY2(s.run(loadout9610(), syntheticSboot(), &err), qPrintable(err));
        QCOMPARE(countDetails(details, noteB), 0);      // 与上次相同 → 一次都不再转
    }

    void sleepsBetweenSegmentsButNotAfterLast()
    {
        // 段间等待用**次数**而非墙钟（brief 契约 3）：注入 sleepFn 后本用例不依赖真实时间。
        // gapMs 与 pollMs 取**不同值**，才能从记录里区分两类等待。
        eub::EubOptions o = fastOptions();
        o.segmentGapMs = 11;
        o.revolvePollMs = 7;
        {
            QList<int> slept;
            o.sleepFn = [&](int ms) { slept.append(ms); };
            eub::MockEubTransport t;
            t.info = info9610();
            eub::EubSession s(t, o);
            QString err;
            QVERIFY2(s.run(loadout9610(), syntheticSboot(), &err), qPrintable(err));
            // 6 段 → 段间 5 次；**最后一段后不睡**（多了就是 6 次）
            QCOMPARE(slept.size(), 5);
            for (int v : slept)
                QCOMPARE(v, 11);
        }
        {
            // open 失败后必须等 revolvePollMs 再重试（brief 契约 3：每次失败后 sleep）
            QList<int> slept;
            eub::EubOptions o2 = o;
            o2.sleepFn = [&](int ms) { slept.append(ms); };
            eub::MockEubTransport t;
            t.info = info9610();
            t.openFailures = 2;
            eub::EubSession s(t, o2);
            QString err;
            QVERIFY2(s.run(loadout9610(), syntheticSboot(), &err), qPrintable(err));
            QCOMPARE(slept.size(), 7);          // 2 次重试等待 + 5 次段间等待
            QCOMPARE(slept[0], 7);
            QCOMPARE(slept[1], 7);
            for (int i = 2; i < slept.size(); ++i)
                QCOMPARE(slept[i], 11);
        }
    }

    void progressIsMonotonicAndEndsAtDone()
    {
        QList<eub::EubProgress> seen;
        eub::MockEubTransport t;
        t.info = info9610();
        eub::EubSession s(t, fastOptions(), [&](const eub::EubProgress &p) { seen.append(p); });
        QString err;
        QVERIFY(s.run(loadout9610(), syntheticSboot(), &err));
        QVERIFY(!seen.isEmpty());
        QCOMPARE(seen.last().stage, QStringLiteral("done"));
        QCOMPARE(seen.last().percent, 100);
        for (int i = 1; i < seen.size(); ++i)
            QVERIFY(seen[i].percent >= seen[i - 1].percent);
        // 加固（约束 9）：纯"单调不减"是恒真陷阱 —— 恒报 0、末尾报 100 也能通过。
        // 逐条钉住：①阶段名只用契约里那三个（UI 依赖这组字符串）；②send 阶段每段一条且 percent
        // 严格递增（0,0,…,0,100 这种"没长进"的进度条会被判红）。
        const int n = loadout9610().segments.size();
        QList<int> sendPercents;
        for (const eub::EubProgress &p : seen) {
            QVERIFY2(p.stage == QStringLiteral("identify") || p.stage == QStringLiteral("send")
                         || p.stage == QStringLiteral("done"),
                     qPrintable(QStringLiteral("未约定的 stage：%1").arg(p.stage)));
            if (p.stage == QStringLiteral("send"))
                sendPercents.append(p.percent);
        }
        QCOMPARE(sendPercents.size(), n);
        QVERIFY(sendPercents.first() > 0);
        for (int i = 1; i < sendPercents.size(); ++i)
            QVERIFY(sendPercents[i] > sendPercents[i - 1]);
    }

    void emptySocNameFailsWithFallbackHint()
    {
        // 契约（eub_session.h）：SoC 名为空（极老 SoC 只报 "SEC S5PC210 Test B/D"，facts §A4）→
        // 失败，并指向 detectSocFromImage 兜底；句柄照关。
        eub::MockEubTransport t;
        t.info = info9610();
        t.info.socName.clear();
        eub::EubSession s(t, fastOptions());
        eub::EubLoadout lo;
        QString err;
        QVERIFY(!s.identify(lo, &err));
        QVERIFY2(err.contains(QStringLiteral("detectSocFromImage")), qPrintable(err));
        QCOMPARE(t.calls.last(), QStringLiteral("close"));
    }

    void emptySegmentTableIsRejectedWithoutWriting()
    {
        // 防御（fail-closed）：空表若"发 0 段报成功"，用户会以为刷过了 —— 对救援工具是最坏的假成功。
        eub::EubLoadout lo = loadout9610();
        lo.segments.clear();
        eub::MockEubTransport t;
        t.info = info9610();
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY(!s.run(lo, syntheticSboot(), &err));
        QVERIFY(!err.isEmpty());
        QCOMPARE(t.writes.size(), 0);
        QVERIFY2(t.calls.isEmpty(), qPrintable(t.calls.join(QStringLiteral(","))));
    }

    void extraFilesTableIsRejectedWithoutTouchingDevice()
    {
        // 终审 I1（fail-closed）：9830 的参照流程要在分段**之后另发** BL 包内文件（ldfw.img / tzsw.img，
        // hubble.py:329-341 + ExynosData/Exynos9830.json:3），而本仓本期没有该发送路径（run 只遍历
        // segments，载荷入口只找 sboot）—— 照旧发完 5 段再报"全部 N 段已发送：设备应已进入 Download
        // 模式"，对 9830 用户是**对未发生动作的断言**，且设备被留在"分段已发、文件未发"的半完成状态。
        // 故 run 必须在**切段之前**拒绝：零写入，且连设备都不碰（句柄尚未打开）。
        const eub::EubLoadout lo = loadout9830();
        QVERIFY2(!lo.extraFiles.isEmpty(), "前置：9830 表必须带 extraFiles，否则本槽恒真");
        eub::MockEubTransport t;
        t.info = info9610();
        t.info.socName = QStringLiteral("Exynos9830");
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY(!s.run(lo, syntheticSboot(0x400000), &err));
        QVERIFY2(err.contains(QStringLiteral("Exynos9830")), qPrintable(err));
        QVERIFY2(err.contains(QStringLiteral("未实现")), qPrintable(err));   // 说清"本仓做不到"
        QVERIFY2(err.contains(QStringLiteral("ldfw.img")), qPrintable(err)); // 用户要知道涉及哪些文件
        QVERIFY2(err.contains(QStringLiteral("tzsw.img")), qPrintable(err));
        QCOMPARE(t.writes.size(), 0);                                      // 零写入
        // 变异证据：去掉 run 开头的守卫 → 本槽必红（run 返回 true、err 为空、写出 5 帧、calls 非空）
        QVERIFY2(t.calls.isEmpty(), qPrintable(t.calls.join(QStringLiteral(","))));
    }

    void splitFailureStopsBeforeAnyWrite()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY(!s.run(loadout9610(), QByteArray(0x1000, '\xAB'), &err));   // 镜像太短
        QVERIFY(!err.isEmpty());
        QCOMPARE(t.writes.size(), 0);
        // 加固（约束 9）：writes.size() 只说明"没发出去"，管不住"先抢占了设备" ——
        // 契约是**先切段再碰设备**（句柄未打开即返回），故整个调用序列必须为空。
        QVERIFY2(t.calls.isEmpty(), qPrintable(t.calls.join(QStringLiteral(","))));
    }
};

QTEST_APPLESS_MAIN(TestEubSession)
#include "test_eub_session.moc"
