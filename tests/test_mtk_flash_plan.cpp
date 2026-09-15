// tests/test_mtk_flash_plan.cpp
//
// D1 Task 7：MTK 刷写计划层（匹配 / 告警 / 大小 + scatter 解析 + 预览渲染）。
// 本模块**不碰设备、不读 USB**（只依赖 Qt Core）—— 参照表从哪来由调用方决定：
// 预览期用 scatter（可选）或空表推导，写入期用设备实读分区表。
// 真样本 reference/mtk-samples/（gitignored，只读）：MTK_SAMPLES_REQUIRED=ON 时缺失即 FAIL。
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <utility>   // std::as_const（遍历 Qt 容器，不得用 qAsConst）

#include "core/bytes_format.h"
#include "core/mtk_flash_plan.h"
#include "mtk_test_helpers.h"   // samplesDir() / sampleFileAvailable() / MTK_SAMPLES_REQUIRED

using mtkplan::PartitionRef;
using mtkplan::MtkFlashPlan;

namespace {
// 造一个指定字节数的稀疏文件（只占元数据；大小真实）
QString writeImage(const QDir &dir, const QString &name, quint64 size)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    if (!f.resize(qsizetype(size))) return QString();
    f.close();
    return path;
}
bool anyContains(const QStringList &l, const QString &needle)
{
    for (const QString &s : std::as_const(l)) if (s.contains(needle)) return true;
    return false;
}
} // namespace

class TestMtkFlashPlan : public QObject
{
    Q_OBJECT
private slots:
    // 参照表顺序 = 计划顺序（不按镜像选择顺序）；totalBytes = 匹配镜像字节和
    void planFollowsReferenceOrderAndSumsBytes()
    {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        QDir d(tmp.path());
        const QString bootImg = writeImage(d, QStringLiteral("boot.img"), 1024);
        const QString preImg  = writeImage(d, QStringLiteral("preloader.bin"), 4096);
        QVERIFY(!bootImg.isEmpty() && !preImg.isEmpty());

        const QList<PartitionRef> parts{{QStringLiteral("preloader"), 8192},
                                        {QStringLiteral("boot"), 2048},
                                        {QStringLiteral("vbmeta"), 16384}};
        MtkFlashPlan plan; QString err;
        QVERIFY2(mtkplan::buildMtkPlan(parts, {bootImg, preImg}, plan, &err), qPrintable(err));
        QCOMPARE(plan.entries.size(), 2);
        QCOMPARE(plan.entries.at(0).partition, QStringLiteral("preloader"));   // 参照表顺序
        QCOMPARE(plan.entries.at(1).partition, QStringLiteral("boot"));
        QCOMPARE(plan.entries.at(0).matchRule, QStringLiteral("exact"));
        QCOMPARE(plan.entries.at(0).partitionSize, quint64(8192));             // 分区大小随条目带出
        QCOMPARE(plan.totalBytes, quint64(4096 + 1024));
        QCOMPARE(plan.skippedOversize, 0);
    }

    // preloader_k65v1_64_bsp.bin → 前缀命中 preloader（并如实告警用了哪条规则）
    void prefixRuleMatchesRealPreloaderNaming()
    {
        QTemporaryDir tmp; QDir d(tmp.path());
        const QString img = writeImage(d, QStringLiteral("preloader_k65v1_64_bsp.bin"), 512);
        QVERIFY(!img.isEmpty());
        const QList<PartitionRef> parts{{QStringLiteral("preloader"), 8192}};
        MtkFlashPlan plan; QString err;
        QVERIFY2(mtkplan::buildMtkPlan(parts, {img}, plan, &err), qPrintable(err));
        QCOMPARE(plan.entries.size(), 1);
        QCOMPARE(plan.entries.at(0).matchRule, QStringLiteral("prefix"));
        QVERIFY2(anyContains(plan.warnings, QStringLiteral("前缀")), qPrintable(plan.warnings.join('\n')));
    }

    // 镜像 > 分区 → 跳过 + 逐条告警 + 不计入 totalBytes（裁决 2）
    void oversizeImageIsSkippedNotWritten()
    {
        QTemporaryDir tmp; QDir d(tmp.path());
        const QString img = writeImage(d, QStringLiteral("boot.img"), 4096);
        QVERIFY(!img.isEmpty());
        const QList<PartitionRef> parts{{QStringLiteral("boot"), 2048}};
        MtkFlashPlan plan; QString err;
        QVERIFY2(mtkplan::buildMtkPlan(parts, {img}, plan, &err), qPrintable(err));
        QVERIFY(plan.entries.isEmpty());
        QCOMPARE(plan.skippedOversize, 1);
        QCOMPARE(plan.totalBytes, quint64(0));
        QVERIFY2(anyContains(plan.warnings, QStringLiteral("放不下")), qPrintable(plan.warnings.join('\n')));
        QVERIFY2(anyContains(plan.warnings, QStringLiteral("没有任何可写入")), qPrintable(plan.warnings.join('\n')));
    }

    // 分区大小未知（scatter 没给 partition_size）→ 不做大小校验，照常入计划
    void unknownPartitionSizeSkipsSizeCheck()
    {
        QTemporaryDir tmp; QDir d(tmp.path());
        const QString img = writeImage(d, QStringLiteral("boot.img"), 8192);
        QVERIFY(!img.isEmpty());
        const QList<PartitionRef> parts{{QStringLiteral("boot"), 0}};   // 0 = 未知
        MtkFlashPlan plan; QString err;
        QVERIFY2(mtkplan::buildMtkPlan(parts, {img}, plan, &err), qPrintable(err));
        QCOMPARE(plan.entries.size(), 1);
        QCOMPARE(plan.entries.at(0).partitionSize, quint64(0));
        QCOMPARE(plan.skippedOversize, 0);
        QCOMPARE(plan.totalBytes, quint64(8192));
    }

    // 镜像在参照表里没有对应分区 → 逐条告警 + 跳过
    void unknownImageIsSkippedWithWarning()
    {
        QTemporaryDir tmp; QDir d(tmp.path());
        const QString img = writeImage(d, QStringLiteral("nosuchpart.img"), 256);
        QVERIFY(!img.isEmpty());
        const QList<PartitionRef> parts{{QStringLiteral("boot"), 2048}};
        MtkFlashPlan plan; QString err;
        QVERIFY2(mtkplan::buildMtkPlan(parts, {img}, plan, &err), qPrintable(err));
        QVERIFY(plan.entries.isEmpty());
        QVERIFY2(anyContains(plan.warnings, QStringLiteral("没有对应分区")), qPrintable(plan.warnings.join('\n')));
    }

    // 同一分区多个前缀候选 → 不猜（跳过 + 告警列出候选）
    void multiplePrefixCandidatesAreNotGuessed()
    {
        QTemporaryDir tmp; QDir d(tmp.path());
        const QString a = writeImage(d, QStringLiteral("preloader_a.bin"), 128);
        const QString b = writeImage(d, QStringLiteral("preloader_b.bin"), 128);
        QVERIFY(!a.isEmpty() && !b.isEmpty());
        const QList<PartitionRef> parts{{QStringLiteral("preloader"), 8192}};
        MtkFlashPlan plan; QString err;
        QVERIFY2(mtkplan::buildMtkPlan(parts, {a, b}, plan, &err), qPrintable(err));
        QVERIFY(plan.entries.isEmpty());
        QVERIFY2(anyContains(plan.warnings, QStringLiteral("候选镜像")), qPrintable(plan.warnings.join('\n')));
    }

    // 无参照表（无 scatter）→ 按文件名推导目标分区名，如实告警，全部镜像计入
    void emptyReferenceTableDerivesNamesFromImages()
    {
        QTemporaryDir tmp; QDir d(tmp.path());
        const QString a = writeImage(d, QStringLiteral("boot.img"), 100);
        const QString b = writeImage(d, QStringLiteral("super.img"), 200);
        QVERIFY(!a.isEmpty() && !b.isEmpty());
        MtkFlashPlan plan; QString err;
        QVERIFY2(mtkplan::buildMtkPlan({}, {a, b}, plan, &err), qPrintable(err));
        QCOMPARE(plan.entries.size(), 2);
        QCOMPARE(plan.entries.at(0).partition, QStringLiteral("boot"));
        QCOMPARE(plan.entries.at(1).partition, QStringLiteral("super"));
        QCOMPARE(plan.entries.at(0).matchRule, QStringLiteral("derived"));
        QCOMPARE(plan.totalBytes, quint64(300));
        QVERIFY2(anyContains(plan.warnings, QStringLiteral("scatter")), qPrintable(plan.warnings.join('\n')));
    }

    // 空镜像列表 / 读不到的镜像 = 明确失败（不是空计划静默成功）
    void emptyOrUnreadableImagesFailLoudly()
    {
        MtkFlashPlan plan; QString err;
        QVERIFY(!mtkplan::buildMtkPlan({}, {}, plan, &err));
        QVERIFY(!err.isEmpty());
        err.clear();
        QVERIFY(!mtkplan::buildMtkPlan({}, {QStringLiteral("/nonexistent/x.img")}, plan, &err));
        QVERIFY(!err.isEmpty());
    }

    // 重复选择同一镜像 → 只取首个 + 告警
    void duplicateImageKeepsFirstWithWarning()
    {
        QTemporaryDir tmp; QDir d(tmp.path());
        const QString img = writeImage(d, QStringLiteral("boot.img"), 64);
        QVERIFY(!img.isEmpty());
        const QList<PartitionRef> parts{{QStringLiteral("boot"), 1024}};
        MtkFlashPlan plan; QString err;
        QVERIFY2(mtkplan::buildMtkPlan(parts, {img, img}, plan, &err), qPrintable(err));
        QCOMPARE(plan.entries.size(), 1);
        QVERIFY2(anyContains(plan.warnings, QStringLiteral("重复")), qPrintable(plan.warnings.join('\n')));
    }

    // scatter 解析：坏输入明确失败
    void scatterParseRejectsGarbage()
    {
        QList<PartitionRef> parts; QString err;
        QVERIFY(!mtkplan::parseScatter(QStringLiteral("hello world\n"), parts, &err));
        QVERIFY(!err.isEmpty());
    }

    // scatter 解析：真文件的分区名/大小行**不带前导 '-'**（只有 partition_index 带）——
    // **两种拼法各自钉住**：分区 1 用真 scatter 的缩进裸键，分区 2/3 用带 '-' 的拼法。
    // 去掉"剥前导 '-'"→ 带横线的两个分区解析不出来（本用例红）；改成"必须有 '-'"→ 分区 1 丢（本用例红）。
    void scatterParsesIndentedKeysWithoutLeadingDash()
    {
        const QString text = QStringLiteral(
            "- partition_index: SYS0\n"
            "  partition_name: boot\n"
            "  partition_size: 0x2000000\n"
            "\n"
            "- partition_index: SYS1\n"
            "- partition_name: recovery\n"
            "- partition_size: 0x4000000\n"
            "\n"
            "- partition_name: userdata\n"
            "- partition_size: 0x1000\n");
        QList<PartitionRef> parts; QString err;
        QVERIFY2(mtkplan::parseScatter(text, parts, &err), qPrintable(err));
        QCOMPARE(parts.size(), 3);
        QCOMPARE(parts.at(0).name, QStringLiteral("boot"));
        QCOMPARE(parts.at(0).sizeBytes, quint64(0x2000000));
        QCOMPARE(parts.at(1).name, QStringLiteral("recovery"));
        QCOMPARE(parts.at(1).sizeBytes, quint64(0x4000000));
        QCOMPARE(parts.at(2).name, QStringLiteral("userdata"));
        QCOMPARE(parts.at(2).sizeBytes, quint64(0x1000));
        // 十进制写法也认（老 scatter 有十进制分区大小）
        QList<PartitionRef> dec; QString derr;
        QVERIFY2(mtkplan::parseScatter(QStringLiteral("  partition_name: para\n  partition_size: 1048576\n"),
                                       dec, &derr), qPrintable(derr));
        QCOMPARE(dec.size(), 1);
        QCOMPARE(dec.at(0).sizeBytes, quint64(1048576));
    }

    // scatter 解析：size 的归属 —— 分区只在"name 之后紧跟 size"时成立。
    // **有判别力**：若去掉 name 分支的 size/haveSize 重置，"name 之前出现的 size"就会被当成
    // boot 的大小从而凭空造出一个分区（本用例红）。
    void sizeMustFollowItsOwnName()
    {
        // 前导 size 不属于任何分区，也不能被后来的 name 认领 → boot 缺 size → 一个分区都解析不出
        QList<PartitionRef> parts; QString err;
        QVERIFY(!mtkplan::parseScatter(QStringLiteral("partition_size: 0x400\n"
                                                      "partition_name: boot\n"),
                                       parts, &err));
        QVERIFY(!err.isEmpty());
        QVERIFY(parts.isEmpty());

        // 正常顺序（name 后跟 size）不受影响；中间夹一个前导 size 也无害
        QList<PartitionRef> ok; QString oerr;
        QVERIFY2(mtkplan::parseScatter(QStringLiteral("partition_size: 0x400\n"
                                                      "partition_name: boot\n"
                                                      "partition_size: 0x800\n"),
                                       ok, &oerr), qPrintable(oerr));
        QCOMPARE(ok.size(), 1);
        QCOMPARE(ok.at(0).name, QStringLiteral("boot"));
        QCOMPARE(ok.at(0).sizeBytes, quint64(0x800));   // 认领 name 之后那个，不是前导那个

        // 只有 name 没有 size 的分区不成立（不能凭相邻分区的 size 蒙一个）
        QList<PartitionRef> mixed; QString merr;
        QVERIFY2(mtkplan::parseScatter(QStringLiteral("partition_name: boot\n"
                                                      "partition_name: vbmeta\n"
                                                      "partition_size: 0x800\n"),
                                       mixed, &merr), qPrintable(merr));
        QCOMPARE(mixed.size(), 1);
        QCOMPARE(mixed.at(0).name, QStringLiteral("vbmeta"));
        QCOMPARE(mixed.at(0).sizeBytes, quint64(0x800));
    }

    // 预览渲染：表头 5 列、逐行映射（文件名/分区/humanBytes/未知/规则中文名）、摘要含跳过数
    void renderingMapsEntriesToRowsAndSummary()
    {
        MtkFlashPlan plan;
        mtkplan::PlanEntry e;
        e.partition = QStringLiteral("boot");
        e.imagePath = QStringLiteral("/tmp/does/not/matter/boot.img");
        e.imageSize = 0x200000;              // 2 MiB
        e.partitionSize = 0;                 // 未知
        e.matchRule = QStringLiteral("exact");
        plan.entries << e;
        mtkplan::PlanEntry d;
        d.partition = QStringLiteral("super");
        d.imagePath = QStringLiteral("/tmp/does/not/matter/super.img");
        d.imageSize = 1024;
        d.partitionSize = 0x1000;
        d.matchRule = QStringLiteral("derived");
        plan.entries << d;
        plan.totalBytes = e.imageSize + d.imageSize;
        plan.skippedOversize = 1;

        QCOMPARE(mtkplan::planHeaders().size(), 5);
        const QList<QStringList> rows = mtkplan::planRows(plan);
        QCOMPARE(rows.size(), 2);
        QCOMPARE(rows.at(0).size(), 5);
        QCOMPARE(rows.at(0).at(0), QStringLiteral("boot.img"));   // 显示文件名，不是全路径
        QCOMPARE(rows.at(0).at(1), QStringLiteral("boot"));
        QCOMPARE(rows.at(0).at(2), QStringLiteral("2.0 MiB"));
        QCOMPARE(rows.at(0).at(3), QStringLiteral("未知"));
        QCOMPARE(rows.at(0).at(4), QStringLiteral("精确"));
        QCOMPARE(rows.at(1).at(3), QStringLiteral("4 KiB"));
        QCOMPARE(rows.at(1).at(4), QStringLiteral("推导"));
        // 未知规则（将来 D2/D3 新增）**原样输出**，不回落成"推导"（可见即正确，别猜）
        plan.entries.last().matchRule = QStringLiteral("future_rule");
        QCOMPARE(mtkplan::planRows(plan).at(1).at(4), QStringLiteral("future_rule"));
        QCOMPARE(mtkplan::planRows(plan).at(0).at(4), QStringLiteral("精确"));   // 已知规则不受影响

        const QString html = mtkplan::planSummaryHtml(plan);
        QVERIFY2(html.contains(QStringLiteral("2 个分区")), qPrintable(html));
        QVERIFY2(html.contains(QStringLiteral("跳过")), qPrintable(html));
        // 无跳过时不提跳过（摘要不吓人）
        plan.skippedOversize = 0;
        QVERIFY(!mtkplan::planSummaryHtml(plan).contains(QStringLiteral("跳过")));
    }

    // humanBytes：单位分档与小数位（Phase C 的 planBytesText 同源实现）
    void humanBytesFormatsEachMagnitude()
    {
        QCOMPARE(humanBytes(0), QStringLiteral("0 B"));
        QCOMPARE(humanBytes(512), QStringLiteral("512 B"));
        QCOMPARE(humanBytes(1024), QStringLiteral("1 KiB"));
        QCOMPARE(humanBytes(1024ull * 1024), QStringLiteral("1.0 MiB"));
        QCOMPARE(humanBytes(1610612736ull), QStringLiteral("1.50 GiB"));   // 1.5 GiB
        QCOMPARE(humanBytes(0xC2C4F8000ull), QStringLiteral("48.69 GiB")); // 真 sample 的 userdata
    }

    // 真实 scatter（50 分区，SYS0..SYS49）：硬断言 4 个已知分区的真实大小 + 端到端匹配
    void realScatterParsesAndMatches()
    {
        if (!mtktest::sampleFileAvailable(QStringLiteral("MT6765_Android_scatter.txt"))) {
#if MTK_SAMPLES_REQUIRED
            QFAIL("真样本缺失：MT6765_Android_scatter.txt（MTK_SAMPLES_REQUIRED=ON）");
#else
            QSKIP("真样本目录/文件缺失（reference/mtk-samples/，gitignored）");
#endif
        }
        QFile f(QDir(mtktest::samplesDir()).filePath(QStringLiteral("MT6765_Android_scatter.txt")));
        QVERIFY(f.open(QIODevice::ReadOnly));
        QList<PartitionRef> parts; QString err;
        QVERIFY2(mtkplan::parseScatter(QString::fromLatin1(f.readAll()), parts, &err), qPrintable(err));
        QCOMPARE(parts.size(), 50);

        auto sizeOf = [&parts](const QString &name) -> quint64 {
            for (const PartitionRef &p : std::as_const(parts))
                if (p.name.compare(name, Qt::CaseInsensitive) == 0) return p.sizeBytes;
            return 0;
        };
        QCOMPARE(sizeOf(QStringLiteral("preloader")), quint64(0x80000));    // 实测值
        QCOMPARE(sizeOf(QStringLiteral("boot")), quint64(0x2000000));
        QCOMPARE(sizeOf(QStringLiteral("vbmeta")), quint64(0x800000));
        QCOMPARE(sizeOf(QStringLiteral("userdata")), quint64(0xC2C4F8000ull));

        // 端到端：真分区表 + 合成镜像 → 两条精确命中
        QTemporaryDir tmp; QDir d(tmp.path());
        const QString preImg = writeImage(d, QStringLiteral("preloader.bin"), 0x40000);
        const QString bootImg = writeImage(d, QStringLiteral("boot.img"), 0x100000);
        QVERIFY(!preImg.isEmpty() && !bootImg.isEmpty());
        MtkFlashPlan plan;
        QVERIFY2(mtkplan::buildMtkPlan(parts, {preImg, bootImg}, plan, &err), qPrintable(err));
        QCOMPARE(plan.entries.size(), 2);
        QCOMPARE(plan.entries.at(0).partition, QStringLiteral("preloader"));  // 真表顺序
        QCOMPARE(plan.entries.at(1).partition, QStringLiteral("boot"));
        QCOMPARE(plan.totalBytes, quint64(0x40000 + 0x100000));
        QCOMPARE(plan.skippedOversize, 0);
        // 真表命中一条告警：两个镜像都小于分区容量（boot 1 MiB < 32 MiB、preloader 256 KiB < 512 KiB），
        // 汇总成一条 —— 真表无重名/无空名，不应有其它告警
        QCOMPARE(plan.warnings.size(), 1);
        QVERIFY2(plan.warnings.first().contains(QStringLiteral("小于分区容量")),
                 qPrintable(plan.warnings.join('\n')));
    }
};
QTEST_APPLESS_MAIN(TestMtkFlashPlan)
#include "test_mtk_flash_plan.moc"
