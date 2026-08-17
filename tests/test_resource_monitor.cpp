#include <QtTest>
#include <QByteArray>
#include "core/resource_monitor.h"

// 任务 H1：ResourceMonitor 解析与滞回逻辑测试
// 覆盖：
//   parseProcStat —— 8 字段聚合行解析、10 字段（guest/guest_nice 忽略）等价、
//                    多行样本取聚合 "cpu " 行、字段缺失/非数字/溢出/无聚合行
//                    /空数据拒绝、制表符容忍
//   percentBetween —— 正常计算、零间隔、计数回绕
//   nextHighState —— >80 触发 / <70 恢复 / 滞回带保持 / 边界（80 不触发、70 不恢复）
//   采样序列集成 —— 注入两次采样的 /proc/stat 快照序列模拟 high 触发与恢复
class TestResourceMonitor : public QObject
{
    Q_OBJECT

private slots:
    void parseProcStat_valid8Fields();
    void parseProcStat_valid10Fields_guestIgnored();
    void parseProcStat_multiline_picksAggregateCpuLine();
    void parseProcStat_tabsTolerated();
    void parseProcStat_missingField_rejected();
    void parseProcStat_nonNumeric_rejected();
    void parseProcStat_overflow_rejected();
    void parseProcStat_noAggregateLine_rejected();
    void parseProcStat_emptyData_rejected();
    void percentBetween_normal();
    void percentBetween_zeroInterval_rejected();
    void percentBetween_counterWrap_rejected();
    void nextHighState_triggerAndRecover();
    void nextHighState_hysteresisBand();
    void nextHighState_boundaryValues();
    void sequence_highTriggerAndRecover();
};

void TestResourceMonitor::parseProcStat_valid8Fields()
{
    // cpu 行 8 字段：user nice system idle iowait irq softirq steal
    // total = 100+20+30+400+10+5+3+0 = 568；idle = idle(400)+iowait(10) = 410
    const QByteArray data("cpu  100 20 30 400 10 5 3 0\n");
    quint64 total = 0, idle = 0;
    QVERIFY(ResourceMonitor::parseProcStat(data, total, idle));
    QCOMPARE(total, quint64(568));
    QCOMPARE(idle, quint64(410));
}

void TestResourceMonitor::parseProcStat_valid10Fields_guestIgnored()
{
    // 内核在 8 字段后还会输出 guest/guest_nice（已含在 user/nice 内，忽略）。
    // 与 8 字段版本结果完全一致。
    const QByteArray data("cpu 100 20 30 400 10 5 3 0 55 66\n");
    quint64 total = 0, idle = 0;
    QVERIFY(ResourceMonitor::parseProcStat(data, total, idle));
    QCOMPARE(total, quint64(568));
    QCOMPARE(idle, quint64(410));
}

void TestResourceMonitor::parseProcStat_multiline_picksAggregateCpuLine()
{
    // 真实 /proc/stat 形状：聚合行在前，cpuN 行与其它行不干扰匹配
    const QByteArray data(
        "cpu  100 20 30 400 10 5 3 0 0 0\n"
        "cpu0 60 10 15 200 5 2 1 0 0 0\n"
        "cpu1 40 10 15 200 5 3 2 0 0 0\n"
        "intr 8688370575 8 3373 0\n"
        "ctxt 22848221062\n"
        "btime 1605316999\n");
    quint64 total = 0, idle = 0;
    QVERIFY(ResourceMonitor::parseProcStat(data, total, idle));
    QCOMPARE(total, quint64(568));
    QCOMPARE(idle, quint64(410));
}

void TestResourceMonitor::parseProcStat_tabsTolerated()
{
    const QByteArray data("cpu\t100\t20\t30\t400\t10\t5\t3\t0\n");
    quint64 total = 0, idle = 0;
    QVERIFY(ResourceMonitor::parseProcStat(data, total, idle));
    QCOMPARE(total, quint64(568));
    QCOMPARE(idle, quint64(410));
}

void TestResourceMonitor::parseProcStat_missingField_rejected()
{
    // 仅 7 个字段（缺 steal）→ 拒绝
    const QByteArray data("cpu 100 20 30 400 10 5 3\n");
    quint64 total = 123, idle = 456;
    QVERIFY(!ResourceMonitor::parseProcStat(data, total, idle));
    QCOMPARE(total, quint64(123)); // 失败不改写输出
    QCOMPARE(idle, quint64(456));
}

void TestResourceMonitor::parseProcStat_nonNumeric_rejected()
{
    const QByteArray data("cpu 100 20 x 400 10 5 3 0\n");
    quint64 total = 0, idle = 0;
    QVERIFY(!ResourceMonitor::parseProcStat(data, total, idle));
}

void TestResourceMonitor::parseProcStat_overflow_rejected()
{
    // 2^64 = 18446744073709551616 > quint64 上限 → 拒绝
    const QByteArray data("cpu 18446744073709551616 20 30 400 10 5 3 0\n");
    quint64 total = 0, idle = 0;
    QVERIFY(!ResourceMonitor::parseProcStat(data, total, idle));

    // quint64 上限值本身合法（不溢出）
    const QByteArray dataMax("cpu 18446744073709551615 0 0 0 0 0 0 0\n");
    QVERIFY(ResourceMonitor::parseProcStat(dataMax, total, idle));
}

void TestResourceMonitor::parseProcStat_noAggregateLine_rejected()
{
    // 只有 cpuN 行、没有聚合 "cpu " 行 → 拒绝
    const QByteArray data(
        "cpu0 100 20 30 400 10 5 3 0\n"
        "cpu1 100 20 30 400 10 5 3 0\n");
    quint64 total = 0, idle = 0;
    QVERIFY(!ResourceMonitor::parseProcStat(data, total, idle));
}

void TestResourceMonitor::parseProcStat_emptyData_rejected()
{
    quint64 total = 0, idle = 0;
    QVERIFY(!ResourceMonitor::parseProcStat(QByteArray(), total, idle));
    QVERIFY(!ResourceMonitor::parseProcStat("garbage\n", total, idle));
    QVERIFY(!ResourceMonitor::parseProcStat("cpu\n", total, idle)); // 无字段
}

void TestResourceMonitor::percentBetween_normal()
{
    // (1000,200) → (2000,300)：Δtotal=1000，Δidle=100，busy=900 → 90%
    QCOMPARE(ResourceMonitor::percentBetween(1000, 200, 2000, 300), 90);
    // 完全空闲：busy=0 → 0%
    QCOMPARE(ResourceMonitor::percentBetween(1000, 900, 2000, 1900), 0);
    // 完全忙：busy=1000 → 100%
    QCOMPARE(ResourceMonitor::percentBetween(1000, 0, 2000, 0), 100);
}

void TestResourceMonitor::percentBetween_zeroInterval_rejected()
{
    QCOMPARE(ResourceMonitor::percentBetween(1000, 200, 1000, 200), -1);
}

void TestResourceMonitor::percentBetween_counterWrap_rejected()
{
    // total 回绕
    QCOMPARE(ResourceMonitor::percentBetween(2000, 300, 1000, 200), -1);
    // idle 回绕
    QCOMPARE(ResourceMonitor::percentBetween(1000, 300, 2000, 100), -1);
}

void TestResourceMonitor::nextHighState_triggerAndRecover()
{
    // >80 触发 high
    QVERIFY(ResourceMonitor::nextHighState(false, 81));
    QVERIFY(ResourceMonitor::nextHighState(false, 100));
    // <70 恢复
    QVERIFY(!ResourceMonitor::nextHighState(true, 69));
    QVERIFY(!ResourceMonitor::nextHighState(true, 0));
}

void TestResourceMonitor::nextHighState_hysteresisBand()
{
    // 滞回带 70~80：保持原状态
    QVERIFY(!ResourceMonitor::nextHighState(false, 75)); // 低态不误触发
    QVERIFY(ResourceMonitor::nextHighState(true, 75));   // 高态不误恢复
}

void TestResourceMonitor::nextHighState_boundaryValues()
{
    // 边界严格对照约束：触发是 ">80"（80 不触发），恢复是 "<70"（70 不恢复）
    QVERIFY(!ResourceMonitor::nextHighState(false, 80));
    QVERIFY(ResourceMonitor::nextHighState(true, 70));
}

void TestResourceMonitor::sequence_highTriggerAndRecover()
{
    // 模拟连续采样：注入 /proc/stat 快照 → parseProcStat → percentBetween →
    // nextHighState，验证 high 触发（>80）与恢复（<70）完整闭环。
    // 快照序列（Δtotal 均 1000，idle 增量控制使用率）：
    //   S1: 20%  busy → S2: 99% busy（触发 high）→ S3: 99.5% busy（保持）
    //   → S4: 40% busy（恢复）
    quint64 t1 = 0, i1 = 0, t2 = 0, i2 = 0;
    // user 列按 total = user+idle+iowait 凑整（其余列 0），保证每步 Δtotal=1000
    QVERIFY(ResourceMonitor::parseProcStat(
        QByteArray("cpu 200 0 0 780 20 0 0 0\n"), t1, i1)); // total 1000, idle 800
    QVERIFY(ResourceMonitor::parseProcStat(
        QByteArray("cpu 1190 0 0 785 25 0 0 0\n"), t2, i2)); // total 2000, idle 810

    bool high = false;
    int pct = ResourceMonitor::percentBetween(t1, i1, t2, i2);
    QCOMPARE(pct, 99);
    high = ResourceMonitor::nextHighState(high, pct);
    QVERIFY(high); // >80 → 触发降级

    // S3：保持 high（99.5 → 100%）
    quint64 t3 = 0, i3 = 0;
    QVERIFY(ResourceMonitor::parseProcStat(
        QByteArray("cpu 2185 0 0 786 29 0 0 0\n"), t3, i3)); // total 3000, idle 815
    pct = ResourceMonitor::percentBetween(t2, i2, t3, i3);
    QCOMPARE(pct, 100);
    QVERIFY(ResourceMonitor::nextHighState(high, pct)); // 仍 >80 → 保持 high

    // S4：idle 大幅增加 → 40% busy → 恢复
    quint64 t4 = 0, i4 = 0;
    QVERIFY(ResourceMonitor::parseProcStat(
        QByteArray("cpu 2585 0 0 1390 25 0 0 0\n"), t4, i4)); // total 4000, idle 1415
    pct = ResourceMonitor::percentBetween(t3, i3, t4, i4);
    QCOMPARE(pct, 40);
    QVERIFY(!ResourceMonitor::nextHighState(high, pct)); // <70 → 恢复
}

QTEST_APPLESS_MAIN(TestResourceMonitor)
#include "test_resource_monitor.moc"
