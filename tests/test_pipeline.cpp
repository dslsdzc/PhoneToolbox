#include <QtTest>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "core/device_detector.h"
#include "core/flash_tool.h"

class TestPipeline : public QObject {
    Q_OBJECT
private slots:
    void channelMapping();
    void channelMappingUnknown();
    void samsungOdinChannel();
    void edlKeepsPartitionFlashPath();
    void edlChannelRejectsMissingPlanDir();
    void resolveProgrammerPicksFirstCandidate();
    void bromParamsRejectMissingDaAndImages();
    void bromParamsNormalizeDefaults();
    void bromChannelRejectsMissingDaBeforeUsb();
};

void TestPipeline::channelMapping()
{
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_MTK_BROM),
             QStringLiteral("mtk-brom"));
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_HUAWEI_USB_UPDATE),
             QStringLiteral("huawei-usb-update"));
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_SPD),
             QStringLiteral("spd"));
    // Phase B Task 8：9008 走 oppo-edl 通道（计划目录 + programmer → EdlSession::run）
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_EDL_9008),
             QStringLiteral("oppo-edl"));
}

void TestPipeline::channelMappingUnknown()
{
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_ADB), QString());
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_FASTBOOT), QString());
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_UNKNOWN), QString());
}

// Phase C：三星 Odin 通道 —— 通道名 + 是否整包通道（不含 EDL 的"分区刷写"例外）
void TestPipeline::samsungOdinChannel()
{
    QCOMPARE(FlashTool::flashChannelForMode(DeviceDetector::MODE_SAMSUNG_ODIN),
             QStringLiteral("samsung-odin"));
    QVERIFY(FlashTool::isPackageChannelMode(DeviceDetector::MODE_SAMSUNG_ODIN));
    // 缺 tarMd5Files 时必须**明确报错**（不能让通道分支静默 return 掉）
    FlashTool tool;
    QString err;
    QVERIFY(!tool.flashFullPackage(QStringLiteral("usb-1-2"), DeviceDetector::MODE_SAMSUNG_ODIN,
                                  QVariantMap(), &err));
    QVERIFY(err.contains(QStringLiteral("tarMd5Files")));
}

// Task 8 审查 Important 的回归守卫：EDL（9008）的「刷入」**不得**被整包协议通道接管 ——
// 否则 params 无 planDir → 通道早退，`onFlashClicked` 的 EDL 分区写分支永久不可达
// （单选分区刷写失效）。这条断言的就是"面板里那个 if 的判据"。
void TestPipeline::edlKeepsPartitionFlashPath()
{
    QVERIFY(!FlashTool::isPackageChannelMode(DeviceDetector::MODE_EDL_9008));
    // 三个协议通道仍按整包接管（与 F5 行为一致）
    QVERIFY(FlashTool::isPackageChannelMode(DeviceDetector::MODE_MTK_BROM));
    QVERIFY(FlashTool::isPackageChannelMode(DeviceDetector::MODE_HUAWEI_USB_UPDATE));
    QVERIFY(FlashTool::isPackageChannelMode(DeviceDetector::MODE_SPD));
    // 非协议模式既不接管也无通道
    QVERIFY(!FlashTool::isPackageChannelMode(DeviceDetector::MODE_ADB));
    QVERIFY(!FlashTool::isPackageChannelMode(DeviceDetector::MODE_FASTBOOT));
    QVERIFY(!FlashTool::isPackageChannelMode(DeviceDetector::MODE_UNKNOWN));
}

// oppo-edl 通道：params 缺 planDir → **在碰设备之前**就拒（中文文案指向 UI 入口「EDL 刷写计划…」），
// 且不产生任何日志/进度。这条同时是"刷入按钮不会静默走进通道"的守门用例（Task 8 审查 Minor 5）。
void TestPipeline::edlChannelRejectsMissingPlanDir()
{
    FlashTool tool;
    QList<QString> logged;
    connect(&tool, &FlashTool::outputMessage, this,
            [&logged](const QString &msg, bool) { logged << msg; });

    QString err;
    // 空 params（= 不带 planDir）。注意：该分支在 buildPlanFromDir/USB 之前返回，
    // 故本用例不会碰真机（有 9008 设备插着也安全）。
    QVERIFY(!tool.flashFullPackage(QString(), DeviceDetector::MODE_EDL_9008, QVariantMap(), &err));
    QVERIFY2(err.contains(QStringLiteral("planDir")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("EDL 刷写计划")), qPrintable(err));
    QVERIFY(logged.isEmpty());
}

// oppo-edl 通道：programmer 解析（纯函数，不碰 USB）——目录内多个候选 → 取字典序首个 + 一条
// "多候选"提示（通道把它落日志）；显式路径优先、不探测；无候选 → 空串 + 中文错误。
void TestPipeline::resolveProgrammerPicksFirstCandidate()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto touch = [](const QString &path) {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
    };
    // 干扰项：非 prog 前缀 / 不含 firehose / 非 elf-mbn-bin 后缀 —— 都不该进候选
    touch(dir.filePath(QStringLiteral("firehose_x.elf")));
    touch(dir.filePath(QStringLiteral("prog_other.elf")));
    touch(dir.filePath(QStringLiteral("prog_firehose_note.txt")));
    // 候选（首个按排序取）：三个候选只在 a/b/c 上不同，任何比较口径（ASCII / 大小写不敏感 /
    // locale 排序）都给出同一个"首个" —— 免得用例被 QDir 的排序口径绑死；第三个同时覆盖
    // "文件名含大写"的大小写不敏感命中。
    touch(dir.filePath(QStringLiteral("prog_a_firehose.elf")));
    touch(dir.filePath(QStringLiteral("prog_b_firehose.mbn")));
    touch(dir.filePath(QStringLiteral("prog_c_FIREHOSE.ELF")));

    QStringList messages;
    QString err;
    const QString chosen = FlashTool::resolveProgrammer(dir.path(), QString(), &messages, &err);
    QVERIFY2(err.isEmpty(), qPrintable(err));
    QCOMPARE(chosen, dir.filePath(QStringLiteral("prog_a_firehose.elf")));
    QCOMPARE(messages.size(), 1);
    QVERIFY2(messages.first().contains(QStringLiteral("3 个 programmer 候选")),
             qPrintable(messages.first()));
    QVERIFY2(messages.first().contains(QStringLiteral("prog_a_firehose.elf")),
             qPrintable(messages.first()));

    // 显式路径优先：目录里有多少候选都不探测、不提示
    QStringList explicitMsgs;
    QCOMPARE(FlashTool::resolveProgrammer(dir.path(), QStringLiteral("/x/y.elf"),
                                          &explicitMsgs, &err),
             QStringLiteral("/x/y.elf"));
    QVERIFY(explicitMsgs.isEmpty());

    // 无候选：空串 + 中文错误（文案给出 glob 与显式 programmerPath 两条出路）
    QTemporaryDir empty;
    QString emptyErr;
    QStringList emptyMsgs;
    QVERIFY(FlashTool::resolveProgrammer(empty.path(), QString(), &emptyMsgs, &emptyErr).isEmpty());
    QVERIFY2(emptyErr.contains(QStringLiteral("prog_*firehose*")), qPrintable(emptyErr));
    QVERIFY(emptyMsgs.isEmpty());
}

// mtk-brom 通道参数：缺 DA / 缺镜像都必须**明确失败**（不让空计划走到 USB 才炸）
void TestPipeline::bromParamsRejectMissingDaAndImages()
{
    QString da, err;
    QStringList images, dirs;
    bool net = true;
    QVERIFY(!FlashTool::parseBromParams({{QStringLiteral("imagePaths"), QStringList{QStringLiteral("/a/boot.img")}}},
                                        &da, &images, &dirs, &net, &err));
    QVERIFY2(err.contains(QStringLiteral("DA")), qPrintable(err));

    err.clear();
    QVERIFY(!FlashTool::parseBromParams({{QStringLiteral("daPath"), QStringLiteral("/a/da.bin")}},
                                        &da, &images, &dirs, &net, &err));
    QVERIFY2(err.contains(QStringLiteral("镜像")), qPrintable(err));
}

// 参数齐全：字段规范化正确（allowNetwork 缺省 false —— 网络默认关闭）
void TestPipeline::bromParamsNormalizeDefaults()
{
    QString da, err;
    QStringList images, dirs;
    bool net = true;                       // 故意给 true，验证缺省会被覆盖成 false
    const QVariantMap params{{QStringLiteral("daPath"), QStringLiteral("/a/da.bin")},
                             {QStringLiteral("imagePaths"),
                              QStringList{QStringLiteral("/a/boot.img"), QStringLiteral("/a/super.img")}},
                             {QStringLiteral("firmwareDirs"), QStringList{QStringLiteral("/a/fw")}}};
    QVERIFY2(FlashTool::parseBromParams(params, &da, &images, &dirs, &net, &err), qPrintable(err));
    QCOMPARE(da, QStringLiteral("/a/da.bin"));
    QCOMPARE(images.size(), 2);
    QCOMPARE(dirs, QStringList{QStringLiteral("/a/fw")});
    QVERIFY(!net);
}

// mtk-brom 通道：params 缺 DA → **在碰 USB 之前**就拒（与上面 edlChannelRejectsMissingPlanDir
// 同款守门用例，覆盖 T10 接线的通道体早退路径 —— 这条**离线可测**，通道体其余部分才不可）。
// 断言"不产生任何日志"= 没有走到通道日志/多设备警告/USB：真机段（枚举→握手）在
// runBromFlash 里，本用例用空 params 在 parseBromParams 处返回。
void TestPipeline::bromChannelRejectsMissingDaBeforeUsb()
{
    FlashTool tool;
    QList<QString> logged;
    connect(&tool, &FlashTool::outputMessage, this,
            [&logged](const QString &msg, bool) { logged << msg; });

    QString err;
    QVERIFY(!tool.flashFullPackage(QString(), DeviceDetector::MODE_MTK_BROM, QVariantMap(), &err));
    QVERIFY2(err.contains(QStringLiteral("DA")), qPrintable(err));
    QVERIFY(logged.isEmpty());
}

QTEST_APPLESS_MAIN(TestPipeline)
#include "test_pipeline.moc"
