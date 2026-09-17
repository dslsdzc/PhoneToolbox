// tests/test_eub_session.cpp
//
// 会话编排：段序、每段重开、重试、失败语义、进度单调 —— 全部经 MockEubTransport（无真机，facts §F1）。
// 断言对象只有 mock 的记录（帧字节 / 调用序列 / 睡眠参数）与错误、进度文案 —— 不碰真设备，
// 也不依赖真实时间（sleep 一律注入，见 fastOptions）。
#include <QtTest>

#include "core/eub/eub_session.h"
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
        QVERIFY(err.contains(QStringLiteral("第 2 段")));       // 段序号（文案格式见 eub_session.h）
        QVERIFY(err.contains(QStringLiteral("注入的写失败")));   // 末位必须是**失败原因**（%5），不能吞掉
        QCOMPARE(t.calls.last(), QStringLiteral("close"));     // 句柄收干净
    }

    void socMismatchBetweenIdentifyAndRunFails()
    {
        eub::MockEubTransport t;
        t.info = info9610();
        t.info.socName = QStringLiteral("Exynos8890");         // 用户中途换了设备
        eub::EubSession s(t, fastOptions());
        QString err;
        QVERIFY(!s.run(loadout9610(), syntheticSboot(), &err));
        QVERIFY(err.contains(QStringLiteral("Exynos8890")));   // 当前设备
        QVERIFY(err.contains(QStringLiteral("Exynos9610")));   // 期望的布局表 —— 缺了它用户不知道换成了什么
        QCOMPARE(t.writes.size(), 0);
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
            const eub::EubLoadout lo = loadout9830();
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
            QVERIFY2(s.run(loadout9830(), syntheticSboot(0x400000), &err), qPrintable(err));
            QCOMPARE(t.calls.count(QStringLiteral("read")), loadout9830().segments.size());
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
