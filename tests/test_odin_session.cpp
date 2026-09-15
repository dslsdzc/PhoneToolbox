// tests/test_odin_session.cpp
//
// OdinSession 编排：断言**写出去的完整字节序列**（命令帧 + 数据分片 + 唯一一处空写）与调用序列。
// 无真机可验证的核心手段（与 test_edl_session.cpp 同款）。
#include <QtTest>
#include <utility>          // std::as_const（项目约定：不得用 qAsConst）

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "core/odin/odin_session.h"
#include "core/odin/odin_protocol.h"
#include "mock_odin_transport.h"
#include "odin_test_helpers.h"

class TestOdinSession : public QObject
{
    Q_OBJECT
private slots:
    void fullSessionByteSequence();
    void stopsOnPartIndexMismatchWithoutEndSession();
    void refusesWhenDevicePitLacksPartition();
    void fallsBackToPackagePitWhenDumpFails();
    void refusesEmptyPlanWithoutTouchingDevice();
    void refusesZeroSizedImage();
    void negotiatesOnlyForVersion2();
    void reportsProgressToHundred();
    void failsWhenImageFileMissing();
    void pitDumpStreamInterruptionDoesNotFallBack();
    void flashesMultiSequenceImage();
    void transportFailuresAbortAndClose();
    void refusesWhenDevicePartitionSmallerThanImage();
    void reportsZeroLengthPacketDiagnostic();
};

using namespace odin;
using namespace odintest;

// 造一个镜像文件 + 一个单条目计划（默认 1 个分区 BOOT/spl.img）
struct Fixture {
    QTemporaryDir dir;
    QByteArray image;
    SamsungPlan plan;
    QString imagePath;
};

static bool makeFixture(Fixture &fx, const QByteArray &image, const QString &partition = QStringLiteral("BOOT"),
                        const QString &imageName = QStringLiteral("spl.img"), quint32 identifier = 80)
{
    fx.image = image;
    const QString tarPath = fx.dir.path() + QStringLiteral("/BL.tar.md5");
    {
        QFile f(tarPath);
        if (!f.open(QIODevice::WriteOnly) || f.write(image) != image.size())
            return false;
    }
    SamsungPlanFile file;
    file.path = tarPath;
    file.sizeBytes = quint64(image.size());
    file.md5HasFooter = false;
    file.verifyOk = false;
    file.entryNames << imageName;
    fx.plan.files.append(file);
    SamsungPlanEntry e;
    e.partition = partition;
    e.imageFile = imageName;
    e.sizeBytes = quint64(image.size());
    e.sourceOffset = 0;                       // 夹具文件里镜像从 0 开始
    e.fileIndex = 0;
    e.matchRule = QStringLiteral("文件名精确匹配");
    e.pit.partitionName = partition;
    e.pit.identifier = identifier;
    e.pit.deviceType = 2;
    e.pit.binaryType = 0;
    e.pit.blockCount = 1024;
    fx.plan.entries.append(e);
    fx.plan.totalBytes = quint64(image.size());
    fx.imagePath = tarPath;
    return true;
}

// 8 字节应答：id + code
static QByteArray ackFrame(quint32 id, quint32 code)
{
    QByteArray a(8, '\0');
    odintest::putU32(a, 0, id);
    odintest::putU32(a, 4, code);
    return a;
}

// 起会话应答：id=0x64，code = (version<<16) | (compressed? 0x80000000:0)
static QByteArray beginAck(quint32 version, bool compressed = false)
{
    const quint32 code = (version << 16) | (compressed ? 0x80000000u : 0u);
    return ackFrame(0x64, code);
}

// 常规起会话读脚本：握手 → 起会话 →（version≥2）片大小协商 → 机型查询（12B）→ 总字节
static void queueSessionSetup(MockOdinTransport &t, quint32 version = 2, bool compressed = true)
{
    t.reads << QByteArray("LOKE") << beginAck(version, compressed);
    if (version >= 2)
        t.reads << ackFrame(0x64, 0);
    QByteArray dt(12, '\0');
    dt[0] = char(0x64);
    odintest::putU32(dt, 8, 0x1234);
    t.reads << dt;
    t.reads << ackFrame(0x64, 0);
}

// PIT dump 读脚本：请求应答（含大小）+ 每 500 字节一片的**原始数据** + 尾空包 + 结束应答
static void queuePitDump(MockOdinTransport &t, const QByteArray &pitBytes)
{
    t.reads << ackFrame(0x65, quint32(pitBytes.size()));
    for (int off = 0; off < pitBytes.size(); off += 500)
        t.reads << pitBytes.mid(off, 500);
    t.reads << QByteArray();                       // 最后一片后设备的空包（Heimdall kEmptyTransferAfter 位）
    t.reads << ackFrame(0x65, 0);                  // 结束应答
}

// 单条目写入段的读脚本：申请文件传输 / 申请序列 / 每片 / 结束序列
static void queueEntryWrites(MockOdinTransport &t, int partCount)
{
    t.reads << ackFrame(0x66, 0) << ackFrame(0x66, 0);
    for (int i = 0; i < partCount; ++i)
        t.reads << ackFrame(0x00, quint32(i));
    t.reads << ackFrame(0x66, 0);
}

static void queueEndSession(MockOdinTransport &t)
{
    t.reads << ackFrame(0x67, 0) << ackFrame(0x67, 0);
}

// 设备侧 PIT（单条 BOOT → spl.img，与夹具计划同源）
// blockCount 可覆盖：终审 I-2 的用例要造"设备分区比镜像小"（deviceType=2 → 512 B/扇区）。
static QByteArray devicePitBytes(const QByteArray &partition = QByteArray("BOOT"),
                                 const QByteArray &flashName = QByteArray("spl.img"),
                                 quint32 blockCount = 1024)
{
    PitSpec s;
    s.name = partition;
    s.deviceType = 2;
    s.identifier = 80;
    s.blockCount = blockCount;
    s.flashFilename = flashName;
    return buildPit({s});                          // 默认带 1024 字节尾部 → 3 个 500 字节分片
}

// 完整会话：逐条核对**写出去的字节**（命令帧 + 数据片 + 唯一一处空写）与调用序列
void TestOdinSession::fullSessionByteSequence()
{
    Fixture fx;
    QVERIFY(fx.dir.isValid());
    const QByteArray image(3000, '\x5A');          // 一个片内（1 MiB 片大小）
    QVERIFY(makeFixture(fx, image));

    MockOdinTransport t;
    queueSessionSetup(t);
    queuePitDump(t, devicePitBytes());             // 1184 字节 → 3 片（顺带钉住 PIT 分片拼接）
    queueEntryWrites(t, 1);
    queueEndSession(t);

    OdinSession s(t);
    QString err;
    QVERIFY2(s.run(fx.plan, OdinOptions{}, &err), qPrintable(err));

    const TransferProfile prof = profileForVersion(2);
    const quint32 aligned = alignedSequenceBytes(quint32(image.size()), prof.packetSize);
    QCOMPARE(aligned, prof.packetSize);            // 3000 字节 → 一个整片
    QCOMPARE(t.writes.size(), 17);
    QCOMPARE(t.writes[0], QByteArray("ODIN", 4));
    QCOMPARE(t.writes[1], frameBeginSession());
    QCOMPARE(t.writes[2], frameFilePartSize(prof.packetSize));
    QCOMPARE(t.writes[3], frameDeviceTypeQuery());
    QCOMPARE(t.writes[4], frameTotalBytes(quint64(image.size())));
    QCOMPARE(t.writes[5], framePitDumpRequest());
    QCOMPARE(t.writes[6], framePitPartRequest(0)); // 3 片 PIT：逐片请求，数据是**裸分片**（无 8 字节头）
    QCOMPARE(t.writes[7], framePitPartRequest(1));
    QCOMPARE(t.writes[8], framePitPartRequest(2));
    QCOMPARE(t.writes[9], framePitEndRequest());
    QCOMPARE(t.writes[10], frameRequestFlash());
    QCOMPARE(t.writes[11], frameRequestSequence(aligned));
    QCOMPARE(t.writes[12].size(), int(prof.packetSize));
    QCOMPARE(t.writes[12].left(image.size()), image);
    QCOMPARE(t.writes[12].mid(image.size()), QByteArray(int(prof.packetSize) - image.size(), '\0')); // D9 零填充
    QCOMPARE(t.writes[13], QByteArray());          // D7：结束序列**前**的空写（全序列唯一一处空写）
    QCOMPARE(t.writes[14], frameEndSequence(fx.plan.entries[0].pit, quint32(image.size()), true));
    QCOMPARE(t.writes[15], frameEndSession(false));
    QCOMPARE(t.writes[16], frameEndSession(true));
    QCOMPARE(t.calls.first(), QStringLiteral("open"));
    QCOMPARE(t.calls.last(), QStringLiteral("close"));
    QVERIFY(t.readTimeouts.contains(0));           // 握手后的轮询必须是 timeout 0（否则真机阻塞）
}

void TestOdinSession::stopsOnPartIndexMismatchWithoutEndSession()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image));
    MockOdinTransport t;
    queueSessionSetup(t);
    OdinOptions opt;
    opt.dumpDevicePit = false;                     // 本例只需写到数据面
    t.reads << ackFrame(0x66, 0) << ackFrame(0x66, 0)
            << ackFrame(0x00, 5);                  // 设备回的分片序号是 5，期望 0
    OdinSession s(t);
    QString err;
    QVERIFY(!s.run(fx.plan, opt, &err));
    QVERIFY(err.contains(QStringLiteral("BOOT")));
    QVERIFY(err.contains(QStringLiteral("分片")));
    for (const QByteArray &w : std::as_const(t.writes))
        QVERIFY(w != frameEndSession(false));      // 失败路径**不发**结束会话
    QCOMPARE(t.calls.last(), QStringLiteral("close"));
}

void TestOdinSession::refusesWhenDevicePitLacksPartition()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image, QStringLiteral("BOOT")));
    MockOdinTransport t;
    queueSessionSetup(t);
    queuePitDump(t, devicePitBytes(QByteArray("SBOOT"), QByteArray("sboot.bin")));   // 设备 PIT 里没有 BOOT
    OdinSession s(t);
    QString err;
    QVERIFY(!s.run(fx.plan, OdinOptions{}, &err));
    QVERIFY(err.contains(QStringLiteral("BOOT")));
    for (const QByteArray &w : std::as_const(t.writes))
        QVERIFY(w != frameRequestFlash());         // 拒刷：**一条数据都不发**
    QVERIFY(!t.writes.contains(frameEndSession(false)));   // 失败路径不发结束会话（红线；审查 Minor 3）
    QCOMPARE(t.calls.last(), QStringLiteral("close"));
}

void TestOdinSession::fallsBackToPackagePitWhenDumpFails()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image));
    MockOdinTransport t;
    queueSessionSetup(t);
    t.reads << ackFrame(0xFFFFFFFFu, 4);           // PIT dump 请求被判 BOOTLOADER_FAIL(-4 Write)
    queueEntryWrites(t, 1);
    queueEndSession(t);
    QList<QString> details;
    OdinSession s(t, [&details](const OdinProgress &p) { details << p.detail; });
    QString err;
    QVERIFY2(s.run(fx.plan, OdinOptions{}, &err), qPrintable(err));   // 成功（回退包内 PIT）
    bool warned = false;
    for (const QString &d : std::as_const(details))
        warned = warned || d.contains(QStringLiteral("设备 PIT"));
    QVERIFY(warned);
    bool flashed = false;
    for (const QByteArray &w : std::as_const(t.writes))
        flashed = flashed || w == frameRequestFlash();
    QVERIFY(flashed);
}

void TestOdinSession::refusesEmptyPlanWithoutTouchingDevice()
{
    MockOdinTransport t;
    SamsungPlan plan;                              // 空计划
    OdinSession s(t);
    QString err;
    QVERIFY(!s.run(plan, OdinOptions{}, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(t.calls.isEmpty());                    // 连 open 都不发
    QVERIFY(t.writes.isEmpty());
}

void TestOdinSession::refusesZeroSizedImage()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image));
    fx.plan.entries[0].sizeBytes = 0;
    MockOdinTransport t;
    queueSessionSetup(t);
    OdinOptions opt;
    opt.dumpDevicePit = false;
    OdinSession s(t);
    QString err;
    QVERIFY(!s.run(fx.plan, opt, &err));
    QVERIFY(!err.isEmpty());
    for (const QByteArray &w : std::as_const(t.writes))
        QVERIFY(w != frameRequestFlash());
    QVERIFY(!t.writes.contains(frameEndSession(false)));   // 失败路径不发结束会话（红线；审查 Minor 3）
}

void TestOdinSession::negotiatesOnlyForVersion2()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image));
    MockOdinTransport t;
    queueSessionSetup(t, 1, false);                // version 1：不发片大小协商（D4）
    OdinOptions opt;
    opt.dumpDevicePit = false;
    queueEntryWrites(t, 1);
    queueEndSession(t);
    OdinSession s(t);
    QString err;
    QVERIFY2(s.run(fx.plan, opt, &err), qPrintable(err));
    bool negotiated = false;
    for (const QByteArray &w : std::as_const(t.writes))
        negotiated = negotiated || w == frameFilePartSize(1048576) || w == frameFilePartSize(131072);
    QVERIFY(!negotiated);
    // 片大小 = 128 KiB（profileForVersion(1)），序列声明值同款
    QCOMPARE(t.writes[2], frameDeviceTypeQuery());  // 索引 1 = 起会话，索引 2 直接是机型查询
    bool seq = false;
    for (const QByteArray &w : std::as_const(t.writes))
        seq = seq || w == frameRequestSequence(131072);
    QVERIFY(seq);
}

void TestOdinSession::reportsProgressToHundred()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image));
    MockOdinTransport t;
    queueSessionSetup(t);
    OdinOptions opt;
    opt.dumpDevicePit = false;
    queueEntryWrites(t, 1);
    queueEndSession(t);
    QList<OdinProgress> seen;
    OdinSession s(t, [&seen](const OdinProgress &p) { seen << p; });
    QString err;
    QVERIFY2(s.run(fx.plan, opt, &err), qPrintable(err));
    QVERIFY(!seen.isEmpty());
    QCOMPARE(seen.last().percent, 100);
    QStringList stages;
    for (const OdinProgress &p : std::as_const(seen))
        if (!stages.contains(p.stage))
            stages << p.stage;
    QVERIFY(stages.contains(QStringLiteral("handshake")));
    QVERIFY(stages.contains(QStringLiteral("session")));
    QVERIFY(stages.contains(QStringLiteral("write")));
    QVERIFY(stages.contains(QStringLiteral("done")));
    // 进度单调不减
    int prev = -1;
    for (const OdinProgress &p : std::as_const(seen)) {
        QVERIFY(p.percent >= prev);
        prev = p.percent;
    }
}

void TestOdinSession::failsWhenImageFileMissing()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image));
    QVERIFY(QFile::remove(fx.imagePath));          // 计划构建后文件消失
    MockOdinTransport t;
    queueSessionSetup(t);
    OdinOptions opt;
    opt.dumpDevicePit = false;
    OdinSession s(t);
    QString err;
    QVERIFY(!s.run(fx.plan, opt, &err));
    QVERIFY(err.contains(QStringLiteral("BOOT")));
    // 打开镜像失败必须在**发任何 0x66 命令之前**（不把设备带进半途状态）
    for (const QByteArray &w : std::as_const(t.writes))
        QVERIFY(w != frameRequestFlash());
    QVERIFY(!t.writes.contains(frameEndSession(false)));   // 失败路径不发结束会话（红线；审查 Minor 3）
    QCOMPARE(t.calls.last(), QStringLiteral("close"));
}

// 审查修复②：设备 PIT 读取**在数据阶段中断**（请求已发出、设备不再回片）时**不得**回退包内 PIT
// —— 设备可能仍在把余下的片发进 IN 端点，改道继续刷写会把设备留在半开会话。
// 对照组是同文件里的 fallsBackToPackagePitWhenDumpFails（**请求阶段**被拒 → 回退成功，保持原样）。
void TestOdinSession::pitDumpStreamInterruptionDoesNotFallBack()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image));
    MockOdinTransport t;
    queueSessionSetup(t);
    t.reads << ackFrame(0x65, 1184);               // ① 大小应答正常
    t.reads << QByteArray();                       // ② 数据阶段：设备对第 0 片**什么都没回**
    OdinSession s(t);
    QString err;
    QVERIFY(!s.run(fx.plan, OdinOptions{}, &err));                 // 不可回退 → run 直接失败
    QVERIFY2(err.contains(QStringLiteral("数据阶段")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("重试")), qPrintable(err));
    QVERIFY(!t.writes.contains(frameRequestFlash()));              // 未下发任何分区数据
    QVERIFY(!t.writes.contains(frameEndSession(false)));           // 也不发结束会话
    QVERIFY(!t.writes.contains(framePitEndRequest()));             // 中断即止，不补结束 PIT 请求
    QCOMPARE(t.calls.last(), QStringLiteral("close"));
}

// 审查修复④：**多序列**分支（v2 profile = 1 MiB × 30 片 = 30 MiB/序列）—— 真机固件几乎必走。
// 31 MiB 镜像 = 序列1（30 片）+ 序列2（1 片）；断言两条序列声明值、两次结束序列的
// realSize/isLast、分片总数与**拼接结果逐字节等于镜像**（既证明无零填充、也证明无重复/丢字节）。
void TestOdinSession::flashesMultiSequenceImage()
{
    Fixture fx;
    const int mib = 1024 * 1024;
    const QByteArray image(31 * mib, '\x5A');      // 31 MiB
    QVERIFY(makeFixture(fx, image));
    MockOdinTransport t;
    queueSessionSetup(t);                          // version 2 → 1 MiB / 30 片
    OdinOptions opt;
    opt.dumpDevicePit = false;
    t.reads << ackFrame(0x66, 0);                  // 申请文件传输
    t.reads << ackFrame(0x66, 0);                  // 申请序列 1
    for (int i = 0; i < 30; ++i)
        t.reads << ackFrame(0x00, quint32(i));     // 序列 1 的 30 片
    t.reads << ackFrame(0x66, 0);                  // 序列 1 的结束序列应答
    t.reads << ackFrame(0x66, 0);                  // 申请序列 2
    t.reads << ackFrame(0x00, 0);                  // 序列 2 的唯一一片（片序号从 0 重新计）
    t.reads << ackFrame(0x66, 0);                  // 序列 2 的结束序列应答
    queueEndSession(t);

    QList<OdinProgress> seen;
    OdinSession s(t, [&seen](const OdinProgress &p) { seen << p; });
    QString err;
    QVERIFY2(s.run(fx.plan, opt, &err), qPrintable(err));

    const quint32 seq1 = quint32(30 * mib);
    QCOMPARE(int(t.writes.count(frameRequestSequence(seq1))), 1);   // 30 MiB 序列声明一次
    QCOMPARE(int(t.writes.count(frameRequestSequence(quint32(mib)))), 1);  // 1 MiB 序列声明一次
    // 两次结束序列：realSize 分别是 30 MiB / 1 MiB，isLast 分别是 false / true
    QCOMPARE(int(t.writes.count(frameEndSequence(fx.plan.entries[0].pit, seq1, false))), 1);
    QCOMPARE(int(t.writes.count(frameEndSequence(fx.plan.entries[0].pit, quint32(mib), true))), 1);

    QByteArray joined;
    int parts = 0;
    for (const QByteArray &w : std::as_const(t.writes)) {
        if (w.size() == mib) { joined.append(w); ++parts; }   // 数据片恒为整片 1 MiB
    }
    QCOMPARE(parts, 31);
    QCOMPARE(joined.size(), image.size());
    QCOMPARE(joined, image);                       // 无额外零填充、无重复、无丢字节
    QCOMPARE(seen.last().percent, 100);
    int prev = -1;
    for (const OdinProgress &p : std::as_const(seen)) {
        QVERIFY(p.percent >= prev);                // 进度单调不减
        prev = p.percent;
    }
    QCOMPARE(t.calls.last(), QStringLiteral("close"));
}

// 审查 Minor 4 附带：mock 的 `failWriteAt`/`openResult` 此前在全部用例里**从未使用** ——
// 实现里约 9 处 `if (!m_t.write(...))` 失败分支与 open 失败分支零覆盖。一条用例、三个场景。
void TestOdinSession::transportFailuresAbortAndClose()
{
    {   // 场景 A：**会话建立阶段**写失败（0x64/0x00 起会话那一笔）
        Fixture fx;
        const QByteArray image(3000, '\x5A');
        QVERIFY(makeFixture(fx, image));
        MockOdinTransport t;
        queueSessionSetup(t);
        t.failWriteAt = 1;                         // 0 = "ODIN" 握手，1 = 起会话帧
        OdinSession s(t);
        QString err;
        QVERIFY(!s.run(fx.plan, OdinOptions{}, &err));
        QVERIFY2(err.contains(QStringLiteral("起会话失败")), qPrintable(err));       // 错误带阶段名
        QVERIFY2(err.contains(QStringLiteral("注入的写失败")), qPrintable(err));     // 传输层原文被带出
        QCOMPARE(t.writes.size(), 1);              // 失败的那笔不进 writes，故只剩握手
        QVERIFY(!t.writes.contains(frameEndSession(false)));                // 失败路径不发 0x67
        QCOMPARE(t.calls.last(), QStringLiteral("close"));                  // 失败也关句柄
    }
    {   // 场景 B：**数据面**写失败（第 1 片数据）
        Fixture fx;
        const QByteArray image(3000, '\x5A');
        QVERIFY(makeFixture(fx, image));
        MockOdinTransport t;
        queueSessionSetup(t);
        OdinOptions opt;
        opt.dumpDevicePit = false;
        t.reads << ackFrame(0x66, 0) << ackFrame(0x66, 0);   // 申请刷写 / 申请序列 的应答
        // 前 7 笔：ODIN / 起会话 / 片大小 / 机型 / 总字节 / 申请刷写 / 申请序列 → 第 8 笔（下标 7）= 第 1 片
        t.failWriteAt = 7;
        OdinSession s(t);
        QString err;
        QVERIFY(!s.run(fx.plan, opt, &err));
        QVERIFY2(err.contains(QStringLiteral("写入分片失败")), qPrintable(err));     // 命中**写失败**分支
        QVERIFY2(err.contains(QStringLiteral("BOOT")), qPrintable(err));            // 错误带分区名
        QVERIFY2(err.contains(QStringLiteral("已写 0 字节")), qPrintable(err));      // 与已写字节
        QVERIFY2(err.contains(QStringLiteral("注入的写失败")), qPrintable(err));     // 传输层原文被带出
        QVERIFY(!t.writes.contains(frameEndSession(false)));
        QCOMPARE(t.calls.last(), QStringLiteral("close"));
    }
    {   // 场景 C（记账用）：**open 失败** → 立即返回；此时尚未拿到句柄，故不 close
        MockOdinTransport t;
        t.openResult = false;
        SamsungPlan plan;                          // 非空计划才会走到 open（空计划在 open 前早退）
        SamsungPlanEntry e;
        e.partition = QStringLiteral("BOOT");
        e.imageFile = QStringLiteral("spl.img");
        e.sizeBytes = 1;
        e.fileIndex = 0;
        plan.files.append(SamsungPlanFile{});
        plan.entries.append(e);
        OdinSession s(t);
        QString err;
        QVERIFY(!s.run(plan, OdinOptions{}, &err));
        QVERIFY2(err.contains(QStringLiteral("打开设备失败")), qPrintable(err));
        QCOMPARE(t.calls, QStringList{QStringLiteral("open")});   // 不 close：open 未成功，无句柄可关
        QVERIFY(t.writes.isEmpty());
    }
}

// 终审 I-2：**落点以设备 PIT 为准**，故"装不装得下"必须用**设备**的分区字节数重核 ——
// 计划层核对用的是包内/用户 PIT；设备分区更小但三字段一致时它一条告警都不发，会话却按
// 包内尺寸下发 → 可能越界写相邻分区。本用例：包内 PIT 声明 1024 扇区（512 KiB，装得下 3000 B），
// 设备 PIT 只声明 4 扇区（2048 B）→ 必须**拒刷**。
void TestOdinSession::refusesWhenDevicePartitionSmallerThanImage()
{
    Fixture fx;
    const QByteArray image(3000, '\x5A');
    QVERIFY(makeFixture(fx, image));               // 包内 PIT：blockCount=1024 → 512 KiB
    MockOdinTransport t;
    queueSessionSetup(t);
    queuePitDump(t, devicePitBytes(QByteArray("BOOT"), QByteArray("spl.img"), 4));  // 设备：4×512 = 2048 B
    OdinSession s(t);
    QString err;
    QVERIFY(!s.run(fx.plan, OdinOptions{}, &err));
    QVERIFY2(err.contains(QStringLiteral("BOOT")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("3000")), qPrintable(err));    // 镜像字节数
    QVERIFY2(err.contains(QStringLiteral("2048")), qPrintable(err));    // **设备**分区字节数（非包内的 524288）
    QVERIFY(!t.writes.contains(frameRequestFlash()));                   // 拒刷：一条数据都不发
    QVERIFY(!t.writes.contains(frameEndSession(false)));
    QCOMPARE(t.calls.last(), QStringLiteral("close"));
}

// 终审 I-1④：readAck 的"空返回"要区分**读到 0 长度包**与**超时** —— 前者传输层不置 error。
// 本用例两个场景各钉一半：ZLP → 文案指向第 13 条且**不**带"传输层报"；超时 → 带"传输层报"。
void TestOdinSession::reportsZeroLengthPacketDiagnostic()
{
    {   // 场景 A：设备回了一个 0 长度包（mock 队列里的空 QByteArray：空返回且不置 error）
        Fixture fx;
        const QByteArray image(3000, '\x5A');
        QVERIFY(makeFixture(fx, image));
        MockOdinTransport t;
        queueSessionSetup(t);
        OdinOptions opt;
        opt.dumpDevicePit = false;
        t.reads << QByteArray();                   // ← 申请文件传输的应答：ZLP
        OdinSession s(t);
        QString err;
        QVERIFY(!s.run(fx.plan, opt, &err));
        QVERIFY2(err.contains(QStringLiteral("0 长度包")), qPrintable(err));
        QVERIFY2(err.contains(QStringLiteral("第 13 条")), qPrintable(err));
        QVERIFY2(!err.contains(QStringLiteral("传输层报")), qPrintable(err));   // 不是超时口径
        QVERIFY(!t.writes.contains(frameEndSession(false)));
        QCOMPARE(t.calls.last(), QStringLiteral("close"));
    }
    {   // 场景 B（对照）：读队列空 = 超时 → 仍是"传输层报"口径，**不**出现 0 长度包文案
        Fixture fx;
        const QByteArray image(3000, '\x5A');
        QVERIFY(makeFixture(fx, image));
        MockOdinTransport t;
        queueSessionSetup(t);
        OdinOptions opt;
        opt.dumpDevicePit = false;
        OdinSession s(t);                          // 脚本至此为止 → 下一笔应答读超时
        QString err;
        QVERIFY(!s.run(fx.plan, opt, &err));
        QVERIFY2(err.contains(QStringLiteral("传输层报")), qPrintable(err));
        QVERIFY2(!err.contains(QStringLiteral("0 长度包")), qPrintable(err));
        QCOMPARE(t.calls.last(), QStringLiteral("close"));
    }
}

QTEST_APPLESS_MAIN(TestOdinSession)
#include "test_odin_session.moc"
