// tests/test_edl_session.cpp
//
// Phase B Task 6：EdlSession 编排 + 数据面（分块 / ZLP / 扇区补零 / sparse 展开）+ 中止语义。
//
// 无真机可验证的核心手段与 Task 4/5 一致：**断言写出去的字节**（命令线帧逐字节 + 数据切片），
// 以及调用序列（失败路径不发 reset、成功路径 reset 在最后、写前 drain 残留）。
// 参照行号：reference/qdl/src/firehose.c:1096-1097（补零）、:1132-1137（数据发完只等一个 ACK）、
// reference/qdl/src/usb.c:548-553（ZLP）、:249-252（不 drain 后续写会超时）。
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>

#include "core/edl/edl_session.h"
#include "core/edl/firehose.h"          // 逐字节断言用真实命令构造器（xmlProgram/xmlErase/…）
#include "mock_edl_transport.h"
#include "edl_test_helpers.h"           // saharaFrame()（共享夹具，勿在本文件另写一份）

// 设备方向包裹：`<?xml …?><data>…</data>`—— firehose.cpp 的 firehoseFrame（唯一发送出口补的形态）。
// 测试侧照抄是为对**完整线帧**逐字节断言；命令元素本身仍取自 firehose 的构造器。
static QByteArray framed(const QByteArray &element)
{
    return QByteArray("<?xml version=\"1.0\" encoding=\"UTF-8\" ?><data>") + element + "</data>";
}

static const char *kAckXml = "<response value=\"ACK\" />";
static const char *kStorageInfoXml =
    "<log value=\"{&quot;storage_info&quot;:{&quot;total_blocks&quot;:100000,"
    "&quot;block_size&quot;:4096}}\" /><response value=\"ACK\" />";

// 会话引导段的读队列：HELLO → READ_DATA(整块 programmer) → END_OF_IMAGE → DONE_RSP
// → configure → **逐 LUN** 的 getstorageinfo（会话只查计划里出现过的 LUN）。
// ⚠️ DONE 这一对方向是 **host 主动**：host 发 DONE_REQ(0x05)，设备回 **DONE_RSP(0x06)**
//    （src/core/edl/sahara.cpp:174-207；tests/test_edl_sahara.cpp:50 同）——队列里放 DONE_REQ
//    会被判成"未收到 DONE_RSP（收到 0x05）"（brief 的示例队列此处是错的，已按实现修正）。
// configureSupported != 0 时首轮响应带 MaxPayloadSizeToTargetInBytesSupported →
// firehoseConfigure 会重发一次（reference/qdl/src/firehose.c:534-548），故队列里放两条。
static void queueBringUp(edl::MockEdlTransport &t, const QByteArray &programmer,
                         const QList<quint32> &luns, quint32 configureSupported = 0)
{
    t.reads << saharaFrame(edl::SAHARA_HELLO_REQ, {2, 1, 0, 0})
            << saharaFrame(edl::SAHARA_READ_DATA, {0, 0, quint32(programmer.size()), 0})
            << saharaFrame(edl::SAHARA_END_OF_IMAGE, {0, 0})
            << saharaFrame(edl::SAHARA_DONE_RSP, {});
    if (configureSupported == 0) {
        t.reads << QByteArray(kAckXml);      // 不带 Supported → 会话取保守默认（不得当无上限）
    } else {
        t.reads << QByteArray("<response value=\"ACK\" MaxPayloadSizeToTargetInBytesSupported=\"")
                       + QByteArray::number(configureSupported) + "\" />"
                << QByteArray(kAckXml);
    }
    for (int i = 0; i < luns.size(); ++i)
        t.reads << QByteArray(kStorageInfoXml);
}

// 造一个 Program 条目并写盘一个测试镜像；image 为 nullptr 时条目不含 sha256
static edl::PlanEntry makeProgram(const QString &dir, const QByteArray &image,
                                  quint32 lun = 0, quint64 startSector = 100, quint64 numSectors = 2,
                                  bool sparse = false, const QString &sha256 = QString())
{
    QFile f(dir + QStringLiteral("/boot.img"));
    if (!f.open(QIODevice::WriteOnly) || f.write(image) != image.size()) {
        qFatal("测试镜像写入失败");
    }
    f.close();
    edl::PlanEntry e;
    e.action = edl::PlanEntry::Action::Program;
    e.partitionName = QStringLiteral("boot");
    e.imageFile = dir + QStringLiteral("/boot.img");
    e.lun = lun;
    e.startSector = startSector;
    e.numSectors = numSectors;
    e.sectorSize = 4096;
    e.sparse = sparse;
    e.rawBytes = sparse ? 0 : quint64(image.size());
    e.sha256 = sha256;
    return e;
}

class TestEdlSession : public QObject
{
    Q_OBJECT
private slots:
    void writesImageDataAndResetsOnSuccess();
    void stopsWithoutResetOnNak();
    void chunksByMaxPayloadAndSendsZlp();
    void padsTailToDeclaredSectors();
    void verifiesSha256AfterWrite();
    void failsWithoutResetOnShaMismatch();
    void fullVerifyBeforeWriteChecksWholeImage();
    void drainsResidualBeforeNextWrite();
    void queriesOnlyLunsInPlan();
    void setsBootableStorageDriveForSbl1();
    void stopsWithoutResetWhenDataWriteFails();
    void refusesWhenValidatePlanFails();
    void refusesEmptyPlan();
    void refusesProgramEntryWithoutSectors();
    void stopsWithoutResetWhenReenumerateFails();
    void passesReenumerateBudgetFromOptions();
    void reportsUnacknowledgedResetInsteadOfFailing();
    void reportsProgressToHundred();
    void expandsSparseChunksIntoDataStream();
    void readsBackWithoutReset();
    void readBackRejectsRangeOutsideDevice();
};

// 主路径：Erase(整 LUN) → Program(2 扇区) → Patch(DISK) → reset 收尾。
// 断言**完整写序列**：命令线帧逐字节 + 数据 8192 字节原样（不带任何帧头/长度前缀）+ ZLP +
// reset 在最后。
void TestEdlSession::writesImageDataAndResetsOnSuccess()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');                  // 2 扇区
    const edl::PlanEntry prog = makeProgram(dir.path(), image);

    edl::PlanEntry erase;
    erase.action = edl::PlanEntry::Action::Erase;
    erase.partitionName = QStringLiteral("userdata");
    erase.lun = 0;
    erase.sectorSize = 4096;                               // startSector/numSectors 全 0 = 整 LUN 擦

    edl::PlanEntry patch;
    patch.action = edl::PlanEntry::Action::Patch;
    patch.partitionName = QStringLiteral("gpt_main0.bin");
    patch.imageFile = QStringLiteral("DISK");              // 下发设备（不打本地文件）
    patch.lun = 0;
    patch.startSector = 0;
    patch.sectorSize = 4096;
    patch.byteOffset = 0;
    patch.sizeInBytes = 512;
    patch.value = QStringLiteral("0x11223344");

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {erase, prog, patch};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    t.maxPacket = 512;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});
    t.reads << QByteArray(kAckXml)                          // erase
            << QByteArray(kAckXml)                          // program 声明
            << QByteArray(kAckXml)                          // 数据发完
            << QByteArray(kAckXml);                         // patch

    QString err;
    edl::EdlSession s(t);
    QVERIFY2(s.run(plan, programmer, edl::FlashOptions{}, &err), qPrintable(err));

    // ① 写序列逐字节：命令 → 数据 → ZLP → patch → reset
    // （引导段还有 configure/getstorageinfo 的写，故按"program 命令"定位下标，不硬编码）
    const int atProgram = t.writes.indexOf(framed(edl::xmlProgram(prog)));
    QVERIFY(atProgram > 0);
    QCOMPARE(t.writes[atProgram - 1], framed(edl::xmlErase(erase)));   // program 前一条命令 = erase
    QCOMPARE(t.writes[atProgram + 1], image);              // 镜像字节**原样**推送（无长度前缀/帧头）
    QVERIFY(t.writes[atProgram + 2].isEmpty());            // 8192 % 512 == 0 → ZLP（usb.c:548-553）
    QCOMPARE(t.writes[atProgram + 3], framed(edl::xmlPatch(patch)));
    QCOMPARE(t.writes[atProgram + 4], framed(edl::xmlReset()));
    QCOMPARE(t.writes.size(), atProgram + 5);
    QVERIFY(t.writes.last().contains(QByteArray("reset")));

    // ② 调用序列：复位在所有写完成之后（且只发一次），close 收尾；全程不得有残留未 drain
    QCOMPARE(t.calls.count(QStringLiteral("reset")), 1);
    QVERIFY(t.calls.indexOf(QStringLiteral("reset")) > t.calls.lastIndexOf(QStringLiteral("write")));
    QCOMPARE(t.calls.last(), QStringLiteral("close"));
    QVERIFY(t.residual.isEmpty());

    // ③ 重枚举契约（edl_transport.h 顶部 / Task 6 控制方裁定）：
    //    close 必须在 waitReenumerate 之前；返回 true 时设备已重新 open() —— 会话自己只在最开始
    //    open 一次，重枚举后的 open 由传输负责（mock 在 waitReenumerate 成功时记录一次 "open"）
    QVERIFY(t.calls.indexOf(QStringLiteral("close")) < t.calls.indexOf(QStringLiteral("waitReenumerate")));
    QCOMPARE(t.calls.count(QStringLiteral("open")), 2);
    QVERIFY(t.calls.indexOf(QStringLiteral("open")) < t.calls.indexOf(QStringLiteral("close")));
    QVERIFY(t.calls.lastIndexOf(QStringLiteral("open")) > t.calls.indexOf(QStringLiteral("waitReenumerate")));
    // 预算来自 FlashOptions（默认 45000 = 既有 3s×15）
    QCOMPARE(t.lastReenumTimeoutMs, 45000);
}

void TestEdlSession::stopsWithoutResetOnNak()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    const edl::PlanEntry prog = makeProgram(dir.path(), image);

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    t.maxPacket = 512;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});
    t.reads << QByteArray("<response value=\"NAK\" />")     // program 声明被拒
            << QByteArray(kAckXml);

    QString err;
    edl::EdlSession s(t);
    QVERIFY(!s.run(plan, programmer, edl::FlashOptions{}, &err));
    QVERIFY(err.contains(QStringLiteral("boot")));          // 文案点名失败条目
    for (const QByteArray &w : t.writes)
        QVERIFY(!w.contains(QByteArray("reset")));          // 失败路径不得复位设备
    QVERIFY(!t.calls.contains(QStringLiteral("reset")));
    QVERIFY(t.calls.contains(QStringLiteral("close")));      // 但必须释放句柄
    for (const QByteArray &w : t.writes)
        QVERIFY(!w.contains(QByteArray("<patch")));          // 后续条目不得继续
}

// 分块：configure 协商出 4096 → 每块 1 扇区（4096 字节）；每块都是 maxPacket(512) 的整数倍 → 每块
// 补一次 ZLP；**块间不额外等 ACK**（整条 program 只有数据发完那一个 ACK）。
void TestEdlSession::chunksByMaxPayloadAndSendsZlp()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');                   // 2 扇区
    const edl::PlanEntry prog = makeProgram(dir.path(), image);

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    t.maxPacket = 512;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0}, 4096);                 // 协商上限 = 4096 = 1 扇区
    t.reads << QByteArray(kAckXml) << QByteArray(kAckXml);

    // 协商会把 configure 发两次（首轮 0 → 设备回报 4096 → 重发一次，firehose.c:534-548），
    // 故按 program 命令定位；失败时打印各笔写的长度便于定位
    QString err;
    edl::EdlSession s(t);
    QVERIFY2(s.run(plan, programmer, edl::FlashOptions{}, &err), qPrintable(err));

    QString sizes;
    for (const QByteArray &w : t.writes) sizes += QString::number(w.size()) + QLatin1Char(',');
    const int at = t.writes.indexOf(framed(edl::xmlProgram(prog)));
    QVERIFY2(at > 0, qPrintable(sizes));
    QVERIFY2(t.writes.at(at + 1) == image.left(4096), qPrintable(sizes));  // 第 1 块 = 第 1 扇区
    QVERIFY(t.writes.at(at + 2).isEmpty());                 // ZLP（4096 % 512 == 0）
    QVERIFY2(t.writes.at(at + 3) == image.mid(4096), qPrintable(sizes));   // 第 2 块 = 第 2 扇区
    QVERIFY(t.writes.at(at + 4).isEmpty());                 // ZLP
    QCOMPARE(t.writes.size(), at + 1 + 4 + 1);              // 命令 + 2 块 + 2 ZLP + reset
    QVERIFY(t.writes.last().contains(QByteArray("reset")));
}

// 文件尾补零到声明的扇区数（firehose.c:1096-1097）：5000 字节文件 + 声明 2 扇区 → 推 8192 字节。
void TestEdlSession::padsTailToDeclaredSectors()
{
    QTemporaryDir dir;
    const QByteArray image(5000, '\x5A');
    const edl::PlanEntry prog = makeProgram(dir.path(), image);

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    t.maxPacket = 512;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});
    t.reads << QByteArray(kAckXml) << QByteArray(kAckXml);

    QString err;
    edl::EdlSession s(t);
    QVERIFY2(s.run(plan, programmer, edl::FlashOptions{}, &err), qPrintable(err));

    const int at = t.writes.indexOf(framed(edl::xmlProgram(prog)));
    QVERIFY(at > 0);
    const QByteArray pushed = t.writes[at + 1];
    QCOMPARE(pushed.size(), 8192);                          // 声明 2 扇区 = 8192 字节，一个字节不少
    QCOMPARE(pushed.left(5000), image);
    QCOMPARE(pushed.mid(5000), QByteArray(3192, '\0'));     // 文件尾补零（firehose.c:1089-1097）
}

void TestEdlSession::verifiesSha256AfterWrite()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    const QByteArray hash = QCryptographicHash::hash(image, QCryptographicHash::Sha256).toHex();
    const edl::PlanEntry prog = makeProgram(dir.path(), image, 0, 100, 2, false, QString::fromLatin1(hash));

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});
    t.reads << QByteArray(kAckXml) << QByteArray(kAckXml);

    QString err;
    edl::EdlSession s(t);
    QVERIFY2(s.run(plan, programmer, edl::FlashOptions{}, &err), qPrintable(err));
    QVERIFY(t.writes.last().contains(QByteArray("reset")));
}

void TestEdlSession::failsWithoutResetOnShaMismatch()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    const edl::PlanEntry prog = makeProgram(dir.path(), image, 0, 100, 2, false,
                                            QString(64, QLatin1Char('0')));   // 故意错的 sha256

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});
    t.reads << QByteArray(kAckXml) << QByteArray(kAckXml);

    QString err;
    edl::EdlSession s(t);
    QVERIFY(!s.run(plan, programmer, edl::FlashOptions{}, &err));
    QVERIFY(err.contains(QStringLiteral("boot")));
    QVERIFY(err.contains(QStringLiteral("sha256")));
    QVERIFY(err.contains(QStringLiteral("不回滚")));          // 数据已写入，不做回滚
    QVERIFY(!err.isEmpty());
    QVERIFY(!t.calls.contains(QStringLiteral("reset")));
}

// 刷前完整校验在**写入之前**：hash 不符时一条 program 命令都不该发出去。
void TestEdlSession::fullVerifyBeforeWriteChecksWholeImage()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    const edl::PlanEntry prog = makeProgram(dir.path(), image, 0, 100, 2, false,
                                            QString(64, QLatin1Char('0')));

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});
    t.reads << QByteArray(kAckXml) << QByteArray(kAckXml);

    edl::FlashOptions opt;
    opt.fullVerifyBeforeWrite = true;
    QString err;
    edl::EdlSession s(t);
    QVERIFY(!s.run(plan, programmer, opt, &err));
    QVERIFY(err.contains(QStringLiteral("刷前")));
    for (const QByteArray &w : t.writes)
        QVERIFY(!w.contains(QByteArray("<program")));        // 未进入写入
    QVERIFY(!t.calls.contains(QStringLiteral("reset")));

    // 对照组：hash 正确时能过
    edl::MockEdlTransport t2;
    const QByteArray good = QCryptographicHash::hash(image, QCryptographicHash::Sha256).toHex();
    const edl::PlanEntry prog2 = makeProgram(dir.path(), image, 0, 100, 2, false, QString::fromLatin1(good));
    edl::FlashPlan plan2;
    plan2.storageType = QStringLiteral("ufs");
    plan2.entries = {prog2};
    plan2.totalBytes = 8192;
    queueBringUp(t2, programmer, {0});
    t2.reads << QByteArray(kAckXml) << QByteArray(kAckXml);
    edl::EdlSession s2(t2);
    QString err2;
    QVERIFY2(s2.run(plan2, programmer, opt, &err2), qPrintable(err2));
}

// 残留 drain：qdl 明确"不消费完 IN 端点，后续写会超时"（firehose.c:249-252）。mock 在 residual
// 非空时直接让 write() 失败 —— 会话若不在每笔写前 drain，本用例必失败。
// 注入时机用进度回调（stage="getstorageinfo" = configure 之后、getstorageinfo 命令之前），
// 模拟设备在此刻吐了一条 <log>。
void TestEdlSession::drainsResidualBeforeNextWrite()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    const edl::PlanEntry prog = makeProgram(dir.path(), image);

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    t.maxPacket = 512;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});
    t.reads << QByteArray(kAckXml) << QByteArray(kAckXml);

    bool injected = false;
    edl::ProgressFn hook = [&t, &injected](const edl::SessionProgress &p) {
        if (!injected && p.stage == QStringLiteral("getstorageinfo")) {
            t.residual << QByteArray("<log value=\"stray\" />");   // 响应之后又跟着一段字节
            injected = true;
        }
    };

    QString err;
    edl::EdlSession s(t, hook);
    QVERIFY2(s.run(plan, programmer, edl::FlashOptions{}, &err), qPrintable(err));
    QVERIFY(injected);
    QVERIFY(t.residual.isEmpty());                                    // 被 drain 掉
    QVERIFY(t.calls.indexOf(QStringLiteral("drain")) > 0);
    QVERIFY(t.calls.indexOf(QStringLiteral("drain"))
            < t.calls.lastIndexOf(QStringLiteral("write")));           // drain 发生在后续写之前
}

// getstorageinfo 只查**计划里出现过的 LUN**（去重；reference/qdl/src/program.c:259 逐条目取
// physical_partition_number，不枚举设备 LUN 总数）。
void TestEdlSession::queriesOnlyLunsInPlan()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    const edl::PlanEntry p0 = makeProgram(dir.path(), image, 0, 100);
    edl::PlanEntry p2 = makeProgram(dir.path(), image, 2, 200);
    p2.partitionName = QStringLiteral("system");

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {p0, p2};
    plan.totalBytes = 16384;

    edl::MockEdlTransport t;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0, 2});                    // 两条几何（顺序 = 去重后的计划顺序）
    t.reads << QByteArray(kAckXml) << QByteArray(kAckXml)    // 条目 p0
            << QByteArray(kAckXml) << QByteArray(kAckXml);   // 条目 p2

    QString err;
    edl::EdlSession s(t);
    QVERIFY2(s.run(plan, programmer, edl::FlashOptions{}, &err), qPrintable(err));

    QVERIFY(t.writes.contains(framed(edl::xmlGetStorageInfo(0))));
    QVERIFY(t.writes.contains(framed(edl::xmlGetStorageInfo(2))));
    QVERIFY(!t.writes.contains(framed(edl::xmlGetStorageInfo(1))));   // 计划外的 LUN 不查
}

// 计划含 sbl1 → 全部条目执行完后发 setbootablestoragedrive（值 = 该条目的 LUN），然后在 reset 之前。
void TestEdlSession::setsBootableStorageDriveForSbl1()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    edl::PlanEntry sbl = makeProgram(dir.path(), image, 2, 300);
    sbl.partitionName = QStringLiteral("sbl1");

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {sbl};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {2});
    t.reads << QByteArray(kAckXml) << QByteArray(kAckXml)    // program
            << QByteArray(kAckXml);                          // setbootablestoragedrive

    QString err;
    edl::EdlSession s(t);
    QVERIFY2(s.run(plan, programmer, edl::FlashOptions{}, &err), qPrintable(err));

    const int bootable = t.writes.indexOf(framed(edl::xmlSetBootableStorageDrive(2)));
    QVERIFY(bootable > 0);
    QVERIFY(bootable < t.writes.lastIndexOf(framed(edl::xmlReset())));   // 在 reset 之前
}

// 数据面写失败 → 中止 + 文案带"已写入 N 扇区、失败于偏移 M" + 不发 reset。
void TestEdlSession::stopsWithoutResetWhenDataWriteFails()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    const edl::PlanEntry prog = makeProgram(dir.path(), image);

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    t.maxPacket = 512;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});
    t.reads << QByteArray(kAckXml) << QByteArray(kAckXml);

    // 让**数据块**那一笔写失败：进度回调在 program 命令写出之前触发（stage="write"），
    // 此刻 writes 里只有 configure/getstorageinfo，故下一笔 +1 正是数据块
    edl::ProgressFn hook = [&t](const edl::SessionProgress &p) {
        if (p.stage == QStringLiteral("write") && p.detail == QStringLiteral("boot") && t.failWriteAt < 0)
            t.failWriteAt = t.writes.size() + 1;
    };

    QString err;
    edl::EdlSession s(t, hook);
    QVERIFY(!s.run(plan, programmer, edl::FlashOptions{}, &err));
    QVERIFY(t.failWriteAt > 0);
    QVERIFY(err.contains(QStringLiteral("boot")));
    QVERIFY(err.contains(QStringLiteral("已写入 0 扇区")));
    QVERIFY(err.contains(QStringLiteral("偏移 0")));
    QVERIFY(!t.calls.contains(QStringLiteral("reset")));
}

// validatePlan 不过 → 一条写入命令都不发（spec §3.5：绝不进入写入）。
void TestEdlSession::refusesWhenValidatePlanFails()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    const edl::PlanEntry prog = makeProgram(dir.path(), image, 0, 999999, 2);   // 越界

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});

    QString err;
    edl::EdlSession s(t);
    QVERIFY(!s.run(plan, programmer, edl::FlashOptions{}, &err));
    QVERIFY(err.contains(QStringLiteral("校验")));
    QVERIFY(err.contains(QStringLiteral("boot")));
    for (const QByteArray &w : t.writes)
        QVERIFY(!w.contains(QByteArray("<program")));
    QVERIFY(!t.calls.contains(QStringLiteral("reset")));
}

void TestEdlSession::refusesEmptyPlan()
{
    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    edl::MockEdlTransport t;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {});

    QString err;
    edl::EdlSession s(t);
    QVERIFY(!s.run(plan, programmer, edl::FlashOptions{}, &err));
    QVERIFY(err.contains(QStringLiteral("空计划")));
    QVERIFY(!t.calls.contains(QStringLiteral("reset")));
}

// Program 条目 numSectors==0 没有合法语义（"整 LUN" 只属 Erase）→ fail-closed，不发空 program。
void TestEdlSession::refusesProgramEntryWithoutSectors()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    const edl::PlanEntry prog = makeProgram(dir.path(), image, 0, 100, 0);   // numSectors = 0

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});
    t.reads << QByteArray(kAckXml);

    QString err;
    edl::EdlSession s(t);
    QVERIFY(!s.run(plan, programmer, edl::FlashOptions{}, &err));
    QVERIFY(err.contains(QStringLiteral("num_partition_sectors")));
    for (const QByteArray &w : t.writes)
        QVERIFY(!w.contains(QByteArray("<program")));
    QVERIFY(!t.calls.contains(QStringLiteral("reset")));
}

// waitReenumerate 失败（设备没回到 Firehose）→ 中止 + 带阶段名的中文错误 + **不发 reset**。
void TestEdlSession::stopsWithoutResetWhenReenumerateFails()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    const edl::PlanEntry prog = makeProgram(dir.path(), image);

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    t.reenumerateResult = false;                            // 设备没回来
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});                       // 后面的响应根本不该被消费

    QString err;
    edl::EdlSession s(t);
    QVERIFY(!s.run(plan, programmer, edl::FlashOptions{}, &err));
    QVERIFY(err.contains(QStringLiteral("重枚举")));          // 文案带阶段名
    QVERIFY(err.contains(QStringLiteral("Sahara")));
    QVERIFY(!t.calls.contains(QStringLiteral("reset")));
    QVERIFY(t.calls.contains(QStringLiteral("close")));      // 句柄要释放
    for (const QByteArray &w : t.writes)
        QVERIFY(!w.contains(QByteArray("<program")));        // 未进入写入
}

// 重枚举预算是显式旋钮：FlashOptions::reenumerateTimeoutMs 原样传给 waitReenumerate。
void TestEdlSession::passesReenumerateBudgetFromOptions()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    const edl::PlanEntry prog = makeProgram(dir.path(), image);

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});
    t.reads << QByteArray(kAckXml) << QByteArray(kAckXml);

    edl::FlashOptions opt;
    opt.reenumerateTimeoutMs = 1234;
    QString err;
    edl::EdlSession s(t);
    QVERIFY2(s.run(plan, programmer, opt, &err), qPrintable(err));
    QCOMPARE(t.lastReenumTimeoutMs, 1234);
}

// reset 是 best-effort：设备没 ACK（真机常"先 ACK 再重启"或直接掉线）**不判失败**，
// 但必须落一条日志说明实际情况（"不失败"不等于"不报告"）。
void TestEdlSession::reportsUnacknowledgedResetInsteadOfFailing()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    const edl::PlanEntry prog = makeProgram(dir.path(), image);

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});
    t.reads << QByteArray(kAckXml) << QByteArray(kAckXml);   // **没有** reset 的响应（设备已重启）

    QList<edl::SessionProgress> seen;
    QString err;
    edl::EdlSession s(t, [&seen](const edl::SessionProgress &p) { seen.append(p); });
    QVERIFY2(s.run(plan, programmer, edl::FlashOptions{}, &err), qPrintable(err));   // 仍算成功

    bool logged = false;
    for (const edl::SessionProgress &p : seen) {
        if (p.stage == QStringLiteral("reset") && p.detail.contains(QStringLiteral("未收到 ACK")))
            logged = true;
    }
    QVERIFY(logged);                                         // 不静默
    QVERIFY(t.calls.contains(QStringLiteral("reset")));      // 复位命令确实发了
}

// 进度：percent = 已写字节 / plan.totalBytes，**最后必须到 100**；条目切换时 detail = 条目名。
void TestEdlSession::reportsProgressToHundred()
{
    QTemporaryDir dir;
    const QByteArray image(8192, '\x5A');
    const edl::PlanEntry prog = makeProgram(dir.path(), image);

    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 8192;

    edl::MockEdlTransport t;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});
    t.reads << QByteArray(kAckXml) << QByteArray(kAckXml);

    QList<edl::SessionProgress> seen;
    QString err;
    edl::EdlSession s(t, [&seen](const edl::SessionProgress &p) { seen.append(p); });
    QVERIFY2(s.run(plan, programmer, edl::FlashOptions{}, &err), qPrintable(err));

    QVERIFY(!seen.isEmpty());
    QCOMPARE(seen.last().percent, 100);
    bool namedEntry = false, monotonic = true;
    int prev = 0;
    for (const edl::SessionProgress &p : seen) {
        if (p.stage == QStringLiteral("write") && p.detail == QStringLiteral("boot"))
            namedEntry = true;
        if (p.percent < prev) monotonic = false;
        prev = p.percent;
    }
    QVERIFY(namedEntry);
    QVERIFY(monotonic);
}

// sparse 展开在数据面：RAW 原样、FILL 按 fillValue 生成、DONT_CARE 发零（保持字节流对齐 ——
// 声明的 numSectors 就是整段 raw 大小，少发字节会让设备苦等）。落盘内容与"先 simg2img 再刷"一致。
void TestEdlSession::expandsSparseChunksIntoDataStream()
{
    QTemporaryDir dir;
    // sparse：1 个 RAW 块(4096 'R') + 2 个 FILL 块(4 字节 pattern 44 43 42 41) + 1 个 DONT_CARE 块
    // → 4 块 = 16384 字节（AOSP 头布局同 image_engine/sparse_image.cpp 的 parseSparseHeader）
    QByteArray sparse(28, '\0');
    auto put16 = [](QByteArray &b, int off, quint16 v) { b[off] = char(v); b[off + 1] = char(v >> 8); };
    auto put32 = [](QByteArray &b, int off, quint32 v) {
        b[off] = char(v); b[off + 1] = char(v >> 8);
        b[off + 2] = char(v >> 16); b[off + 3] = char(v >> 24);
    };
    put32(sparse, 0, 0xED26FF3A);
    put16(sparse, 4, 1); put16(sparse, 6, 0); put16(sparse, 8, 28); put16(sparse, 10, 12);
    put32(sparse, 12, 4096);                 // blk_sz
    put32(sparse, 16, 4);                    // total_blks
    put32(sparse, 20, 3);                    // total_chunks
    auto addChunk = [&sparse, &put16, &put32](quint16 type, quint32 blocks, quint32 totalSz,
                                              const QByteArray &payload = {}) {
        QByteArray h(12, '\0');
        put16(h, 0, type); put32(h, 4, blocks); put32(h, 8, totalSz);
        sparse += h;
        sparse += payload;
    };
    const QByteArray raw(4096, 'R');
    addChunk(0xCAC1, 1, 12 + 4096, raw);                       // RAW
    addChunk(0xCAC2, 2, 16, QByteArray("\x44\x43\x42\x41", 4)); // FILL（2 块）
    addChunk(0xCAC3, 1, 12);                                   // DONT_CARE（1 块）

    const edl::PlanEntry prog = makeProgram(dir.path(), sparse, 0, 100, 4, true);
    edl::FlashPlan plan;
    plan.storageType = QStringLiteral("ufs");
    plan.entries = {prog};
    plan.totalBytes = 16384;

    edl::MockEdlTransport t;
    t.maxPacket = 512;
    const QByteArray programmer(16, '\x11');
    queueBringUp(t, programmer, {0});
    t.reads << QByteArray(kAckXml) << QByteArray(kAckXml);

    QString err;
    edl::EdlSession s(t);
    QVERIFY2(s.run(plan, programmer, edl::FlashOptions{}, &err), qPrintable(err));

    const QByteArray pat("\x44\x43\x42\x41", 4);
    QByteArray fillBlocks;
    for (int i = 0; i < 4096 / 4; ++i) fillBlocks += pat;     // FILL：4 字节 pattern 逐字节重复
    QByteArray expected = raw;
    expected += fillBlocks;                                  // FILL 块 1
    expected += fillBlocks;                                  // FILL 块 2
    expected += QByteArray(4096, '\0');                      // DONT_CARE：发零，偏移照常推进

    const int at = t.writes.indexOf(framed(edl::xmlProgram(prog)));
    QVERIFY(at > 0);
    const QByteArray pushed = t.writes[at + 1];
    QCOMPARE(pushed.size(), expected.size());
    QCOMPARE(pushed, expected);
    QVERIFY(t.writes.last().contains(QByteArray("reset")));
}

// readBack：只发 <read>、把回读字节落盘、**不复位**（无 programmer 参数 → 不自行引导 Sahara，
// 设备须已在 Firehose 模式）。
void TestEdlSession::readsBackWithoutReset()
{
    QTemporaryDir dir;
    const QString out = dir.path() + QStringLiteral("/dump.bin");
    const QByteArray payload(8192, '\x77');

    edl::MockEdlTransport t;
    t.maxPacket = 512;
    t.reads << QByteArray(kAckXml)          // <read> 命令的响应（rawmode 数据随后）
            << payload                      // 回读数据
            << QByteArray(kAckXml);         // 读操作结束的最终响应

    edl::ReadRequest req;
    req.lun = 0;
    req.startSector = 500;
    req.numSectors = 2;
    req.sectorSize = 4096;
    req.outputPath = out;

    edl::StorageInfo si;
    si.lun = 0;
    si.totalBlocks = 100000;
    si.blockSize = 4096;

    QString err;
    edl::EdlSession s(t);
    QVERIFY2(s.readBack({req}, {si}, &err), qPrintable(err));

    QVERIFY(t.writes.contains(framed(edl::xmlRead(0, 500, 2, 4096))));
    QFile f(out);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), payload);
    QVERIFY(!t.calls.contains(QStringLiteral("reset")));      // 不复位
}

void TestEdlSession::readBackRejectsRangeOutsideDevice()
{
    edl::MockEdlTransport t;
    edl::ReadRequest req;
    req.lun = 0;
    req.startSector = 999999;
    req.numSectors = 2;
    req.outputPath = QStringLiteral("/dev/null");             // 不该被碰

    edl::StorageInfo si;
    si.lun = 0;
    si.totalBlocks = 100000;
    si.blockSize = 4096;

    QString err;
    edl::EdlSession s(t);
    QVERIFY(!s.readBack({req}, {si}, &err));
    QVERIFY(err.contains(QStringLiteral("越界")));
    QVERIFY(t.writes.isEmpty());                              // 越界 → 一条命令都不发
}

QTEST_APPLESS_MAIN(TestEdlSession)
#include "test_edl_session.moc"
