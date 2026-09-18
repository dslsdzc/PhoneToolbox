#include <QtTest>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "core/modes/spd_pac.h"
#include "pac_test_helpers.h"

// 用例：spd::extractFdlsFromPac —— 从 .pac 内按名提取 FDL1/FDL2（纯解析，离线可完整验证）。
// 夹具 pactest::buildNew/buildLegacy 只写原始字段、自算数据偏移，不复用被测模块 imgpac 的
// 任何读写原语（见 tests/pac_test_helpers.h 头注释）——用例拿夹具自算的切片对拍提取结果，
// 解析/提取双向错误不会自洽通过。
// 诚实边界：真机上 FDL 是否可用属既有 SPD 刷写链的未验证范围，不在本用例内。
class TestSpdPac : public QObject
{
    Q_OBJECT
private slots:
    // ---- 成功路径 ----
    void extractNewFormat();
    void extractLegacyFormat();
    void orderIndependentNew();
    void orderIndependentLegacy();
    void duplicateFdlKeepsFirst();
    void duplicateFdlSameNameAndFileKeepsFirst();
    void zeroByteEntryThenDataEntry();
    void zeroByteEntryThenDataEntryLegacy();
    void mixedCaseFdlNames();
    // ---- 失败路径 ----
    void missingFdl2ListsEntries();
    void missingBothFdls();
    void bareFdlNameIsNotGuessed();
    void zeroByteFdlRejected();
    void notAPacFails();
    void missingFileFails();
    void outputsClearedOnFailure();
    void listingTruncatesAfterEightEntries();
    // ---- 落盘（UI 接线：提取出的字节要先落盘，通道按路径读文件）----
    void writeExtractedFdlsRoundTrip();
    void writeExtractedFdlsNameCollision();
    void writeExtractedFdlsNameCollisionCaseInsensitive();
    void writeExtractedFdlsNameTrailingDotCollision();
    void writeExtractedFdlsNameStrippedToEmptyFallsBack();
    void writeExtractedFdlsMissingDirFails();
    void writeExtractedFdlsRejectsEmptyData();
    void writeExtractedFdlsFirstWriteFailureLeavesNoFile();
    void writeExtractedFdlsPartialWriteCleansFirstPath();
    void writeExtractedFdlsSecondWriteFailureLeavesNoFile();
    void writeExtractedFdlsPartialWriteCleansSecondPath();

private:
    QString writeSample(const QTemporaryDir &dir, const QByteArray &bytes,
                        const QString &name = QStringLiteral("sample.pac"));
    // writeSample 最近一次失败的原因（磁盘满/权限…）—— 断言消息用，免得只看"路径是空的"
    QString m_sampleError;
    // 落盘失败用例的"写不完整"构造：在 linkPath 建符号链接指向 /dev/full（写入必 ENOSPC）。
    // QFile 带缓冲 —— open 成功、write() 返回满额，失败在 flush() 现形，正是 writeFileChecked
    // 里"可能已写出部分内容"要清理的那条分支。平台无 /dev/full 或建不出符号链接时返回 false。
    static bool linkToFullDevice(const QString &linkPath);
};

// 样本载荷：三段长度与内容都不同 —— 把 FDL1/FDL2 取反或串位（如按出现顺序而非按名归位）
// 会在逐字节对拍上当场红。
namespace {
const QByteArray kFdl1Bytes(1024, '\x11');
const QByteArray kFdl2Bytes(2048, '\x22');
const QByteArray kBootBytes(4096, '\x33');

pactest::Entry fdl1Entry()
{
    return {QStringLiteral("FDL1"), QStringLiteral("fdl1.bin"), kFdl1Bytes};
}
pactest::Entry fdl2Entry()
{
    return {QStringLiteral("FDL2"), QStringLiteral("fdl2.bin"), kFdl2Bytes};
}
pactest::Entry bootEntry()
{
    return {QStringLiteral("boot"), QStringLiteral("boot.img"), kBootBytes};
}

// 落盘用例的载荷：非周期填充（xorshift32，同 test_eub_session 的做法）——`(i * k) & 0xFF` 一类
// 256 周期图案会让"偏移错 256 的整数倍"漏检；逐字节对拍要能分辨错位/截断。
QByteArray patternedBytes(int n, quint32 seed)
{
    QByteArray b(n, '\0');
    quint32 x = seed;
    for (int i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b[i] = char(x & 0xFF);
    }
    return b;
}
} // namespace

QString TestSpdPac::writeSample(const QTemporaryDir &dir, const QByteArray &bytes,
                                const QString &name)
{
    m_sampleError.clear();
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        m_sampleError = QStringLiteral("无法写入样本 %1：%2").arg(path, f.errorString());
        return QString();
    }
    const qint64 written = f.write(bytes);
    if (written != qint64(bytes.size())) {
        m_sampleError = QStringLiteral("样本 %1 只写入 %2/%3 字节：%4")
                            .arg(path).arg(written).arg(bytes.size()).arg(f.errorString());
        f.close();
        return QString();
    }
    return path;
}

bool TestSpdPac::linkToFullDevice(const QString &linkPath)
{
    if (!QFileInfo::exists(QStringLiteral("/dev/full")))
        return false;
    if (!QFile::link(QStringLiteral("/dev/full"), linkPath))
        return false;
    return QFileInfo(linkPath).isSymLink();   // Windows 上 link() 造的是 .lnk，不是符号链接
}

void TestSpdPac::extractNewFormat()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    // FDL1 在第 0 条、FDL2 在第 2 条（中间夹 boot），末尾一条 0 字节操作型条目（真实包里常见）
    const pactest::PacSample s = pactest::buildNew({
        fdl1Entry(), bootEntry(), fdl2Entry(),
        {QStringLiteral("FLASH"), QString(), QByteArray()}});
    // 夹具自检：payload 确实落在构造器自算的偏移上（不经被测解析器）
    QCOMPARE(s.data.mid(qsizetype(s.offsets[0]), kFdl1Bytes.size()), kFdl1Bytes);
    QCOMPARE(s.data.mid(qsizetype(s.offsets[2]), kFdl2Bytes.size()), kFdl2Bytes);
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY2(spd::extractFdlsFromPac(path, fdl1, fdl2, &err), qPrintable(err));
    QVERIFY(err.isEmpty());
    QCOMPARE(fdl1.name, QStringLiteral("FDL1"));
    QCOMPARE(fdl1.fileName, QStringLiteral("fdl1.bin"));
    QCOMPARE(fdl2.name, QStringLiteral("FDL2"));
    QCOMPARE(fdl2.fileName, QStringLiteral("fdl2.bin"));
    // 逐字节相等：提取结果 vs 包内切片（偏移取自夹具自算值）
    QCOMPARE(fdl1.data, s.data.mid(qsizetype(s.offsets[0]), kFdl1Bytes.size()));
    QCOMPARE(fdl2.data, s.data.mid(qsizetype(s.offsets[2]), kFdl2Bytes.size()));
}

void TestSpdPac::extractLegacyFormat()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const pactest::PacSample s = pactest::buildLegacy({fdl1Entry(), bootEntry(), fdl2Entry()});
    QCOMPARE(s.data.mid(qsizetype(s.offsets[0]), kFdl1Bytes.size()), kFdl1Bytes);
    QCOMPARE(s.data.mid(qsizetype(s.offsets[2]), kFdl2Bytes.size()), kFdl2Bytes);
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY2(spd::extractFdlsFromPac(path, fdl1, fdl2, &err), qPrintable(err));
    QCOMPARE(fdl1.name, QStringLiteral("FDL1"));
    QCOMPARE(fdl1.fileName, QStringLiteral("fdl1.bin"));
    QCOMPARE(fdl2.name, QStringLiteral("FDL2"));
    QCOMPARE(fdl2.fileName, QStringLiteral("fdl2.bin"));
    QCOMPARE(fdl1.data, s.data.mid(qsizetype(s.offsets[0]), kFdl1Bytes.size()));
    QCOMPARE(fdl2.data, s.data.mid(qsizetype(s.offsets[2]), kFdl2Bytes.size()));
}

// FDL2 排在 FDL1 **前面**：按名归位，不依赖出现顺序
void TestSpdPac::orderIndependentNew()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const pactest::PacSample s = pactest::buildNew({fdl2Entry(), fdl1Entry(), bootEntry()});
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY2(spd::extractFdlsFromPac(path, fdl1, fdl2, &err), qPrintable(err));
    QCOMPARE(fdl1.name, QStringLiteral("FDL1"));
    QCOMPARE(fdl2.name, QStringLiteral("FDL2"));
    QCOMPARE(fdl1.data, kFdl1Bytes);   // 顺序反了会把 FDL2 的字节填进 fdl1 → 此槽必红
    QCOMPARE(fdl2.data, kFdl2Bytes);
}

void TestSpdPac::orderIndependentLegacy()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const pactest::PacSample s = pactest::buildLegacy({bootEntry(), fdl2Entry(), fdl1Entry()});
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY2(spd::extractFdlsFromPac(path, fdl1, fdl2, &err), qPrintable(err));
    QCOMPARE(fdl1.name, QStringLiteral("FDL1"));
    QCOMPARE(fdl2.name, QStringLiteral("FDL2"));
    QCOMPARE(fdl1.data, kFdl1Bytes);
    QCOMPARE(fdl2.data, kFdl2Bytes);
}

// 重名条目（两个 FDL1）：取先出现者（pac 内条目顺序即工具写入顺序），FDL2 不受影响
void TestSpdPac::duplicateFdlKeepsFirst()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const QByteArray second(512, '\x44');
    const pactest::PacSample s = pactest::buildNew({
        fdl1Entry(),
        {QStringLiteral("FDL1"), QStringLiteral("fdl1_alt.bin"), second},
        fdl2Entry()});
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY2(spd::extractFdlsFromPac(path, fdl1, fdl2, &err), qPrintable(err));
    QCOMPARE(fdl1.fileName, QStringLiteral("fdl1.bin"));
    QCOMPARE(fdl1.data, kFdl1Bytes);
    QCOMPARE(fdl2.data, kFdl2Bytes);
}

// 同分区名 **且** 同容器内文件名的重复条目（打包工具并列两次/重跑残迹）：规则同
// duplicateFdlKeepsFirst —— 取先出现者；fileName 不参与去重，两份同不同名都一样。
void TestSpdPac::duplicateFdlSameNameAndFileKeepsFirst()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const QByteArray second(512, '\x66');
    const pactest::PacSample s = pactest::buildNew({
        fdl1Entry(),
        {QStringLiteral("FDL1"), QStringLiteral("fdl1.bin"), second},
        fdl2Entry()});
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY2(spd::extractFdlsFromPac(path, fdl1, fdl2, &err), qPrintable(err));
    QCOMPARE(fdl1.name, QStringLiteral("FDL1"));
    QCOMPARE(fdl1.fileName, QStringLiteral("fdl1.bin"));
    QCOMPARE(fdl1.data, kFdl1Bytes);   // 取先出现者；若"后者胜"→ 此槽读到 second 必红
    QCOMPARE(fdl2.data, kFdl2Bytes);
}

// 0 字节条目不留位：先出现的是空占位、后面才有真数据时取有数据的那条
// （空条目不含数据，无法上传；被它挡住会把本该成功的提取判成失败）
void TestSpdPac::zeroByteEntryThenDataEntry()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const pactest::PacSample s = pactest::buildNew({
        {QStringLiteral("FDL1"), QStringLiteral("fdl1.bin"), QByteArray()},
        fdl1Entry(), fdl2Entry()});
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY2(spd::extractFdlsFromPac(path, fdl1, fdl2, &err), qPrintable(err));
    QCOMPARE(fdl1.fileName, QStringLiteral("fdl1.bin"));
    QCOMPARE(fdl1.data, kFdl1Bytes);
    QCOMPARE(fdl2.data, kFdl2Bytes);
}

// 0 字节条目不占位在**旧格式**同规则：提取层是同一段代码，但旧格式的 size 来自另一条解析
// 路径（32 位 partitionSize 字段，见 parseLegacyFormat）—— 这里钉住两种格式端到端一致。
void TestSpdPac::zeroByteEntryThenDataEntryLegacy()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const pactest::PacSample s = pactest::buildLegacy({
        {QStringLiteral("FDL1"), QStringLiteral("fdl1.bin"), QByteArray()},
        fdl1Entry(), fdl2Entry()});
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY2(spd::extractFdlsFromPac(path, fdl1, fdl2, &err), qPrintable(err));
    QCOMPARE(fdl1.fileName, QStringLiteral("fdl1.bin"));
    QCOMPARE(fdl1.data, kFdl1Bytes);   // 空占位被当成"找到了"→ 数据为空，此槽必红
    QCOMPARE(fdl2.data, kFdl2Bytes);
}

// 条目名混合大小写（"Fdl1"/"fDL2"）：判名与代次细分都是大小写不敏感口径，仍要提取成功。
// 两条路径各自做 toLower（isFdlPartition 判"是不是 FDL" + classifyFdl 细分代次），任一处掉了
// 这里都会"找不到 FDL"而红；出参另要保留包内原始大小写（比较用的 toLower 不得外泄到结果）。
void TestSpdPac::mixedCaseFdlNames()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const pactest::PacSample s = pactest::buildNew({
        {QStringLiteral("Fdl1"), QStringLiteral("Fdl1.BIN"), kFdl1Bytes},
        bootEntry(),
        {QStringLiteral("fDL2"), QStringLiteral("fdl2.bin"), kFdl2Bytes}});
    QCOMPARE(s.data.mid(qsizetype(s.offsets[0]), kFdl1Bytes.size()), kFdl1Bytes);
    QCOMPARE(s.data.mid(qsizetype(s.offsets[2]), kFdl2Bytes.size()), kFdl2Bytes);
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY2(spd::extractFdlsFromPac(path, fdl1, fdl2, &err), qPrintable(err));
    QCOMPARE(fdl1.name, QStringLiteral("Fdl1"));       // 原样保留，不改成小写
    QCOMPARE(fdl1.fileName, QStringLiteral("Fdl1.BIN"));
    QCOMPARE(fdl2.name, QStringLiteral("fDL2"));
    QCOMPARE(fdl1.data, kFdl1Bytes);   // 代次判错会把 FDL2 的字节填进 fdl1 → 此槽必红
    QCOMPARE(fdl2.data, kFdl2Bytes);
}

// 缺 FDL2：报缺哪个 + 包内条目（可行动 —— 用户据此判断选错包，或回退手选）
void TestSpdPac::missingFdl2ListsEntries()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const pactest::PacSample s = pactest::buildNew({fdl1Entry(), bootEntry()});
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY(!spd::extractFdlsFromPac(path, fdl1, fdl2, &err));
    QVERIFY2(err.contains(QStringLiteral("FDL2")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("boot")), qPrintable(err));      // 包内条目
    QVERIFY2(err.contains(QStringLiteral("boot.img")), qPrintable(err));  // 条目摘要带容器内文件名
    // 只缺 FDL2 —— 不该把已找到的 FDL1 也报成缺（"FDL1 与 FDL2" 是两个都缺的措辞）
    QVERIFY2(!err.contains(QStringLiteral("FDL1 与 FDL2")), qPrintable(err));
}

void TestSpdPac::missingBothFdls()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const pactest::PacSample s = pactest::buildNew({bootEntry()});
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY(!spd::extractFdlsFromPac(path, fdl1, fdl2, &err));
    QVERIFY2(err.contains(QStringLiteral("FDL1")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("FDL2")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("boot")), qPrintable(err));
}

// 裸 "FDL"（isFdlPartition 判它是 FDL 但不区分代次）：不猜 —— 猜错等于把 FDL2 的字节
// 当 FDL1 上传。文案列出包内条目（含容器内文件名），用户可据此回退手选。
void TestSpdPac::bareFdlNameIsNotGuessed()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const pactest::PacSample s = pactest::buildNew({
        {QStringLiteral("FDL"), QStringLiteral("fdl1.bin"), kFdl1Bytes}, fdl2Entry()});
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY(!spd::extractFdlsFromPac(path, fdl1, fdl2, &err));
    QVERIFY2(err.contains(QStringLiteral("FDL1")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("fdl1.bin")), qPrintable(err));  // 条目摘要可行动
}

// FDL1 条目存在但 0 字节：不算"找到了"—— 返回空载荷会让刷写链发一个空 FDL，
// 失败点被推到设备侧；这里就地报错。
void TestSpdPac::zeroByteFdlRejected()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const pactest::PacSample s = pactest::buildNew({
        {QStringLiteral("FDL1"), QStringLiteral("fdl1.bin"), QByteArray()}, fdl2Entry()});
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY(!spd::extractFdlsFromPac(path, fdl1, fdl2, &err));
    QVERIFY2(err.contains(QStringLiteral("FDL1")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("0 字节")), qPrintable(err));
}

void TestSpdPac::notAPacFails()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    // 2048 字节 'x'：长度够旧格式头，但分区数/表结构不符（同 test_pac 的 invalidInput 口径）
    const QString path = writeSample(dir, QByteArray(2048, 'x'));
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY(!spd::extractFdlsFromPac(path, fdl1, fdl2, &err));
    QVERIFY(!err.isEmpty());
    // 文案要指向"pac 解析"这一步（parsePac 的失败文案都含 PAC）—— 钉住失败来自解析，
    // 而不是"提取器自己坏了"或别的路径。
    // 脆耦合：靠的是 pac_image.cpp 里各失败文案都以 "PAC" 开头（"PAC 分区数异常" 等）——
    // 将来只改那边的文案，会以与本模块无关的原因弄红本槽（届时改这里或那边补个公共前缀）。
    QVERIFY2(err.contains(QStringLiteral("PAC")), qPrintable(err));
}

void TestSpdPac::missingFileFails()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY(!spd::extractFdlsFromPac(dir.filePath(QStringLiteral("没有这个.pac")),
                                     fdl1, fdl2, &err));
    QVERIFY2(err.contains(QStringLiteral("没有这个.pac")), qPrintable(err)); // 文案能定位到文件
}

// 失败不留半份数据：FDL1 明明找到了，但因 FDL2 缺失而整次调用失败 —— 出参必须清干净，
// 否则调用方拿到"半个结果"接着往下走。
void TestSpdPac::outputsClearedOnFailure()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const pactest::PacSample s = pactest::buildNew({fdl1Entry()});
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    fdl1.name = QStringLiteral("旧名1");
    fdl1.fileName = QStringLiteral("旧文件1");
    fdl1.data = QByteArray(8, '\x7f');
    fdl2.name = QStringLiteral("旧名2");
    fdl2.fileName = QStringLiteral("旧文件2");
    fdl2.data = QByteArray(8, '\x7f');

    QString err;
    QVERIFY(!spd::extractFdlsFromPac(path, fdl1, fdl2, &err));
    QVERIFY(fdl1.name.isEmpty());
    QVERIFY(fdl1.fileName.isEmpty());
    QVERIFY(fdl1.data.isEmpty());
    QVERIFY(fdl2.name.isEmpty());
    QVERIFY(fdl2.fileName.isEmpty());
    QVERIFY(fdl2.data.isEmpty());
}

// 条目多于 8 条（listingOf 的列举上限）时截断：只列前 8 条 + 总数的尾巴（条目多的包别把
// 错误文案撑爆）。顺带钉住条目摘要的写法：分区 ID + 容器内文件名、只有 ID（容器内文件名空）、
// 只有容器内文件名（分区 ID 空）、两者都空的占位。
void TestSpdPac::listingTruncatesAfterEightEntries()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const pactest::PacSample s = pactest::buildNew({
        {QStringLiteral("p0"), QStringLiteral("p0.img"), QByteArray()},
        {QStringLiteral("p1"), QString(), QByteArray()},
        {QString(), QStringLiteral("orphan.bin"), QByteArray()},
        {QString(), QString(), QByteArray()},
        {QStringLiteral("dup"), QStringLiteral("dup"), QByteArray()},
        {QStringLiteral("p5"), QStringLiteral("p5.img"), QByteArray()},
        {QStringLiteral("p6"), QStringLiteral("p6.img"), QByteArray()},
        {QStringLiteral("p7"), QStringLiteral("p7.img"), QByteArray()},
        {QStringLiteral("p8"), QStringLiteral("p8.img"), QByteArray()},
        {QStringLiteral("p9"), QStringLiteral("p9.img"), QByteArray()}});
    const QString path = writeSample(dir, s.data);
    QVERIFY2(!path.isEmpty(), qPrintable(m_sampleError));

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY(!spd::extractFdlsFromPac(path, fdl1, fdl2, &err));
    // 期望文案写字面量（不从被测实现推导）：前 8 条的摘要 + "等 10 个条目"
    const QString expected = QStringLiteral(
        "包内条目：p0（p0.img）、p1、orphan.bin、(无名条目)、dup、p5（p5.img）、"
        "p6（p6.img）、p7（p7.img），等 10 个条目");
    QVERIFY2(err.contains(expected), qPrintable(err));
    QVERIFY2(!err.contains(QStringLiteral("p8.img")), qPrintable(err));   // 第 9 条起不列
}

// ---- 落盘（供 UI 接线：SPD 通道按路径读 FDL，提取出的字节必须先落盘）----
//
// 诚实边界：本组用例证明的是"字节进出文件一致 + 命名/失败语义"，不是"提取出的 FDL 在真机上可用"
//（后者属既有 SPD 刷写链的未验证范围）。UI 分支本身（跳过/回退手选对话框）无自动化覆盖 —— 见报告。

// 两份 FDL 各写成一份文件（文件名带条目名），路径可直接读回原字节。
void TestSpdPac::writeExtractedFdlsRoundTrip()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const spd::ExtractedFdl f1{QStringLiteral("FDL1"), QStringLiteral("fdl1.bin"),
                               patternedBytes(1024, 0x12345678)};
    const spd::ExtractedFdl f2{QStringLiteral("FDL2"), QStringLiteral("fdl2.bin"),
                               patternedBytes(2048, 0x9ABCDEF0)};

    QString p1, p2, err;
    QVERIFY2(spd::writeExtractedFdls(f1, f2, dir.path(), p1, p2, &err), qPrintable(err));
    QVERIFY(err.isEmpty());
    QVERIFY2(p1 != p2, "两份 FDL 写到了同一路径");
    // 文件名带条目名（容器内文件名），落在调用方给的目录里；目录里恰好两份文件
    QCOMPARE(QFileInfo(p1).fileName(), QStringLiteral("fdl1.bin"));
    QCOMPARE(QFileInfo(p2).fileName(), QStringLiteral("fdl2.bin"));
    QCOMPARE(QFileInfo(p1).absolutePath(), QDir(dir.path()).absolutePath());
    QCOMPARE(QFileInfo(p2).absolutePath(), QDir(dir.path()).absolutePath());
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), qsizetype(2));
    // 逐字节读回（不是"写了点什么"）：内容与长度都要对 —— 交换两份数据的实现在这里挂
    QFile r1(p1), r2(p2);
    QVERIFY(r1.open(QIODevice::ReadOnly));
    QVERIFY(r2.open(QIODevice::ReadOnly));
    QCOMPARE(r1.readAll(), f1.data);
    QCOMPARE(r2.readAll(), f2.data);
}

// 两份条目的容器内文件名相同：落盘路径必须各自独立 —— 同一路径会被后写的盖掉，
// 通道随后会把 FDL2 的字节按 FDL1 的时序/加载地址上传（静默发错数据）。
void TestSpdPac::writeExtractedFdlsNameCollision()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const spd::ExtractedFdl f1{QStringLiteral("FDL1"), QStringLiteral("fdl.bin"),
                               patternedBytes(1024, 0x1111)};
    const spd::ExtractedFdl f2{QStringLiteral("FDL2"), QStringLiteral("fdl.bin"),
                               patternedBytes(2048, 0x2222)};

    QString p1, p2, err;
    QVERIFY2(spd::writeExtractedFdls(f1, f2, dir.path(), p1, p2, &err), qPrintable(err));
    QVERIFY2(p1 != p2, "容器内文件名相同时两份 FDL 落到了同一路径");
    QVERIFY2(QFileInfo(p1).fileName().contains(QStringLiteral("fdl.bin")),
             qPrintable(QFileInfo(p1).fileName()));   // 区分用的前缀不该吃掉条目名
    QFile r1(p1), r2(p2);
    QVERIFY(r1.open(QIODevice::ReadOnly));
    QVERIFY(r2.open(QIODevice::ReadOnly));
    QCOMPARE(r1.readAll(), f1.data);
    QCOMPARE(r2.readAll(), f2.data);
}

// 两份条目的容器内文件名仅大小写不同（"fdl.bin" / "FDL.BIN"）：在大小写不敏感文件系统
//（Windows/macOS，本仓有 Windows 目标）上这是**同一路径** —— 后写的盖掉先写的，通道随后把
// FDL2 的字节按 FDL1 的时序/加载地址上传。落盘名必须"忽略大小写也不同"。
void TestSpdPac::writeExtractedFdlsNameCollisionCaseInsensitive()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const spd::ExtractedFdl f1{QStringLiteral("FDL1"), QStringLiteral("fdl.bin"),
                               patternedBytes(1024, 0x5555)};
    const spd::ExtractedFdl f2{QStringLiteral("FDL2"), QStringLiteral("FDL.BIN"),
                               patternedBytes(2048, 0x6666)};

    QString p1, p2, err;
    QVERIFY2(spd::writeExtractedFdls(f1, f2, dir.path(), p1, p2, &err), qPrintable(err));
    QVERIFY2(p1 != p2, "两份 FDL 落到了同一路径");
    // Linux 上"路径字符串不同"只是恰好掩盖了同一文件（大小写敏感），故这里按忽略大小写比较
    QVERIFY2(QString::compare(QFileInfo(p1).fileName(), QFileInfo(p2).fileName(),
                              Qt::CaseInsensitive) != 0,
             qPrintable(QStringLiteral("两份落盘名忽略大小写后相同：%1 / %2")
                            .arg(QFileInfo(p1).fileName(), QFileInfo(p2).fileName())));
    // 区分用的前缀不该吃掉条目名（大小写不敏感地看，仍是各自的条目名）
    QVERIFY(QFileInfo(p1).fileName().contains(QStringLiteral("fdl.bin"), Qt::CaseInsensitive));
    QVERIFY(QFileInfo(p2).fileName().contains(QStringLiteral("FDL.BIN"), Qt::CaseInsensitive));
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), qsizetype(2)); // 大小写不敏感 FS 上漏判 = 1
    QFile r1(p1), r2(p2);
    QVERIFY(r1.open(QIODevice::ReadOnly));
    QVERIFY(r2.open(QIODevice::ReadOnly));
    QCOMPARE(r1.readAll(), f1.data);   // 漏判时 p1 读回 FDL2 的字节 → 此槽必红
    QCOMPARE(r2.readAll(), f2.data);
}

// 两份条目的容器内文件名差在**尾随点/空格**（"fdl1.bin." / "fdl1.bin"）：Windows 会把尾随的
// 点与空格替我们剥掉，两份在那里是同一路径 —— 与大小写不敏感同属"落盘名要按文件系统口径判重"
// （症状同样是后写的盖掉先写的、通道把 FDL2 的字节按 FDL1 的地址上传）。落盘名必须先自己剥干净。
void TestSpdPac::writeExtractedFdlsNameTrailingDotCollision()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const spd::ExtractedFdl f1{QStringLiteral("FDL1"), QStringLiteral("fdl1.bin."),
                               patternedBytes(1024, 0x7777)};
    const spd::ExtractedFdl f2{QStringLiteral("FDL2"), QStringLiteral("fdl1.bin"),
                               patternedBytes(2048, 0x8888)};

    QString p1, p2, err;
    QVERIFY2(spd::writeExtractedFdls(f1, f2, dir.path(), p1, p2, &err), qPrintable(err));
    QVERIFY2(p1 != p2, "两份 FDL 落到了同一路径");
    // 落盘名不得以点/空格结尾：Windows 替我们剥掉后两份就同名，Linux 上"名字不同"
    // 只是恰好掩盖了这一点
    QVERIFY2(!QFileInfo(p1).fileName().endsWith(QLatin1Char('.')),
             qPrintable(QFileInfo(p1).fileName()));
    QVERIFY2(!QFileInfo(p1).fileName().endsWith(QLatin1Char(' ')),
             qPrintable(QFileInfo(p1).fileName()));
    QVERIFY2(!QFileInfo(p2).fileName().endsWith(QLatin1Char('.')),
             qPrintable(QFileInfo(p2).fileName()));
    QVERIFY2(QString::compare(QFileInfo(p1).fileName(), QFileInfo(p2).fileName(),
                              Qt::CaseInsensitive) != 0,
             qPrintable(QStringLiteral("两份落盘名忽略大小写后相同：%1 / %2")
                            .arg(QFileInfo(p1).fileName(), QFileInfo(p2).fileName())));
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files).size(), qsizetype(2));
    QFile r1(p1), r2(p2);
    QVERIFY(r1.open(QIODevice::ReadOnly));
    QVERIFY(r2.open(QIODevice::ReadOnly));
    QCOMPARE(r1.readAll(), f1.data);   // 漏剥时 p1 读回 FDL2 的字节（Windows）→ 此槽必红
    QCOMPARE(r2.readAll(), f2.data);
}

// 剥掉尾随点/空格后为空（"." / ".." / "..." / "   " 这类）：回退到下一候选（分区 ID）——
// 不能落出空文件名（空路径），也不能两份挤到同一个兜底名上。
void TestSpdPac::writeExtractedFdlsNameStrippedToEmptyFallsBack()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const spd::ExtractedFdl f1{QStringLiteral("FDL1"), QStringLiteral("..."),
                               patternedBytes(64, 0x9999)};
    const spd::ExtractedFdl f2{QStringLiteral("FDL2"), QStringLiteral("  "),
                               patternedBytes(128, 0xAAAA)};

    QString p1, p2, err;
    QVERIFY2(spd::writeExtractedFdls(f1, f2, dir.path(), p1, p2, &err), qPrintable(err));
    QCOMPARE(QFileInfo(p1).fileName(), QStringLiteral("FDL1"));   // 回退到分区 ID
    QCOMPARE(QFileInfo(p2).fileName(), QStringLiteral("FDL2"));
    QFile r1(p1), r2(p2);
    QVERIFY(r1.open(QIODevice::ReadOnly));
    QVERIFY(r2.open(QIODevice::ReadOnly));
    QCOMPARE(r1.readAll(), f1.data);
    QCOMPARE(r2.readAll(), f2.data);
}

// 落盘目录不存在：false + 可行动文案（点名目录），且出参清空 —— 调用方不得拿到半个路径接着刷。
void TestSpdPac::writeExtractedFdlsMissingDirFails()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const QString nowhere = dir.filePath(QStringLiteral("没有这个目录"));
    const spd::ExtractedFdl f1{QStringLiteral("FDL1"), QStringLiteral("fdl1.bin"),
                               patternedBytes(64, 1)};
    const spd::ExtractedFdl f2{QStringLiteral("FDL2"), QStringLiteral("fdl2.bin"),
                               patternedBytes(64, 2)};

    QString p1 = QStringLiteral("旧路径1"), p2 = QStringLiteral("旧路径2"), err;
    QVERIFY(!spd::writeExtractedFdls(f1, f2, nowhere, p1, p2, &err));
    QVERIFY2(err.contains(QStringLiteral("目录不存在"), Qt::CaseInsensitive), qPrintable(err));
    QVERIFY2(err.contains(nowhere), qPrintable(err));   // 可行动：哪条路径写不进去
    QVERIFY(p1.isEmpty());
    QVERIFY(p2.isEmpty());
}

// 空载荷拒写：0 字节 FDL 落盘后会被通道当"空 FDL"发出去，失败点被推到设备侧
//（同提取层"0 字节不算找到"的取舍）；目录保持干净。
void TestSpdPac::writeExtractedFdlsRejectsEmptyData()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const spd::ExtractedFdl f1{QStringLiteral("FDL1"), QStringLiteral("fdl1.bin"),
                               patternedBytes(64, 3)};
    const spd::ExtractedFdl f2{QStringLiteral("FDL2"), QStringLiteral("fdl2.bin"), QByteArray()};

    QString p1, p2, err;
    QVERIFY(!spd::writeExtractedFdls(f1, f2, dir.path(), p1, p2, &err));
    QVERIFY2(err.contains(QStringLiteral("0 字节")), qPrintable(err));
    QVERIFY(p1.isEmpty());
    QVERIFY(p2.isEmpty());
    QVERIFY2(QDir(dir.path()).entryList(QDir::Files).isEmpty(), "空载荷不该落盘");
}

// 第一个文件就写不进去（同名位置被一个目录占住）：整次调用失败、出参清空，且不产生任何
// 常规文件（同 writeExtractedFdlsSecondWriteFailureLeavesNoFile 对称的第一轮版本）。
void TestSpdPac::writeExtractedFdlsFirstWriteFailureLeavesNoFile()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("fdl1.bin")));   // 写同名普通文件必失败
    const spd::ExtractedFdl f1{QStringLiteral("FDL1"), QStringLiteral("fdl1.bin"),
                               patternedBytes(128, 17)};
    const spd::ExtractedFdl f2{QStringLiteral("FDL2"), QStringLiteral("fdl2.bin"),
                               patternedBytes(256, 19)};

    QString p1 = QStringLiteral("旧路径1"), p2 = QStringLiteral("旧路径2"), err;
    QVERIFY(!spd::writeExtractedFdls(f1, f2, dir.path(), p1, p2, &err));
    QVERIFY2(err.contains(QStringLiteral("fdl1.bin")), qPrintable(err));   // 失败点在第一轮
    QVERIFY(p1.isEmpty());
    QVERIFY(p2.isEmpty());
    QVERIFY2(QDir(dir.path()).entryList(QDir::Files).isEmpty(), "第一轮失败留下了常规文件");
}

// 第一轮**写不完整**（打开成功、刷盘失败）后失败：该路径上的残留条目必须清掉 ——
// writeFileChecked 的"写入…不完整"分支正是注释里"可能已写出部分内容"要清理的那条。
// 构造：path1 建符号链接指向 /dev/full（写入必 ENOSPC，见 linkToFullDevice）。
void TestSpdPac::writeExtractedFdlsPartialWriteCleansFirstPath()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const QString path1 = dir.filePath(QStringLiteral("fdl1.bin"));
    if (!linkToFullDevice(path1))
        QSKIP("本机无 /dev/full（或建不出符号链接）—— 离线构造不出'打开成功、写入失败'");

    const spd::ExtractedFdl f1{QStringLiteral("FDL1"), QStringLiteral("fdl1.bin"),
                               patternedBytes(128, 23)};
    const spd::ExtractedFdl f2{QStringLiteral("FDL2"), QStringLiteral("fdl2.bin"),
                               patternedBytes(256, 29)};

    QString p1, p2, err;
    QVERIFY(!spd::writeExtractedFdls(f1, f2, dir.path(), p1, p2, &err));
    QVERIFY2(err.contains(QStringLiteral("fdl1.bin")), qPrintable(err));
    // 刷盘失败要如实说"刷盘失败"：QFile 带缓冲，这条分支上 write() 的返回值仍是满额 ——
    // 照"写入…不完整（N 字节，实写 N 字节）"写会自相矛盾（复审 M6）
    QVERIFY2(err.contains(QStringLiteral("刷盘失败")), qPrintable(err));
    QVERIFY(p1.isEmpty());
    QVERIFY(p2.isEmpty());
    QVERIFY2(!QFileInfo(path1).isSymLink(), "第一轮写失败后 path1 仍留着条目（清理未执行）");
    QVERIFY2(QDir(dir.path()).entryList(QDir::Files).isEmpty(), "第一轮失败留下了常规文件");
}

// 第二个文件写不进去（同名位置被一个目录占住）：整次调用失败，**第一个文件也要删掉** ——
// 留下半份产物会让调用方以为拿到了可用的 FDL1 路径（同提取层"失败不留半份数据"）。
void TestSpdPac::writeExtractedFdlsSecondWriteFailureLeavesNoFile()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    // 占位目录：写同名普通文件必失败（QFile 打不开一个目录）
    QVERIFY(QDir(dir.path()).mkdir(QStringLiteral("fdl2.bin")));
    const spd::ExtractedFdl f1{QStringLiteral("FDL1"), QStringLiteral("fdl1.bin"),
                               patternedBytes(128, 7)};
    const spd::ExtractedFdl f2{QStringLiteral("FDL2"), QStringLiteral("fdl2.bin"),
                               patternedBytes(256, 9)};

    QString p1, p2, err;
    QVERIFY(!spd::writeExtractedFdls(f1, f2, dir.path(), p1, p2, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(p1.isEmpty());
    QVERIFY(p2.isEmpty());
    // 失败确实发生在**第二次**写：文案点名 fdl2.bin、不点 fdl1.bin —— 否则 stub 实现
    //（直接返回 false、什么都没写）也能让下面"第一个文件不存在"恒真通过；反过来，第一轮
    // 就失败（如打开方式写错）会在这里显出 fdl1.bin，两次断言各钉一个方向。
    QVERIFY2(!err.contains(QStringLiteral("fdl1.bin")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("fdl2.bin")), qPrintable(err));
    QVERIFY2(!QFile::exists(dir.filePath(QStringLiteral("fdl1.bin"))),
             "第一个文件没删：失败留下了半份产物");
}

// 第二轮**写不完整**（打开成功、刷盘失败）后失败：两个路径的残留都要清掉 —— 先写成的 FDL1
// 是"半份产物"（调用方以为拿到了可用的 FDL1 路径），后写的 FDL2 路径同理。
void TestSpdPac::writeExtractedFdlsPartialWriteCleansSecondPath()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const QString path2 = dir.filePath(QStringLiteral("fdl2.bin"));
    if (!linkToFullDevice(path2))
        QSKIP("本机无 /dev/full（或建不出符号链接）—— 离线构造不出'打开成功、写入失败'");

    const spd::ExtractedFdl f1{QStringLiteral("FDL1"), QStringLiteral("fdl1.bin"),
                               patternedBytes(128, 31)};
    const spd::ExtractedFdl f2{QStringLiteral("FDL2"), QStringLiteral("fdl2.bin"),
                               patternedBytes(256, 37)};

    QString p1, p2, err;
    QVERIFY(!spd::writeExtractedFdls(f1, f2, dir.path(), p1, p2, &err));
    QVERIFY2(err.contains(QStringLiteral("fdl2.bin")), qPrintable(err));
    QVERIFY(p1.isEmpty());
    QVERIFY(p2.isEmpty());
    QVERIFY2(!QFile::exists(dir.filePath(QStringLiteral("fdl1.bin"))),
             "第一个文件没删：失败留下了半份产物");
    QVERIFY2(!QFileInfo(path2).isSymLink(), "第二个路径仍留着条目（清理未执行）");
}

QTEST_APPLESS_MAIN(TestSpdPac)
#include "test_spd_pac.moc"
