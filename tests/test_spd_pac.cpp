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
    void zeroByteEntryThenDataEntry();
    // ---- 失败路径 ----
    void missingFdl2ListsEntries();
    void missingBothFdls();
    void bareFdlNameIsNotGuessed();
    void zeroByteFdlRejected();
    void notAPacFails();
    void missingFileFails();
    void outputsClearedOnFailure();
    // ---- 落盘（UI 接线：提取出的字节要先落盘，通道按路径读文件）----
    void writeExtractedFdlsRoundTrip();
    void writeExtractedFdlsNameCollision();
    void writeExtractedFdlsMissingDirFails();
    void writeExtractedFdlsRejectsEmptyData();
    void writeExtractedFdlsSecondWriteFailureLeavesNoFile();

private:
    static QString writeSample(const QTemporaryDir &dir, const QByteArray &bytes,
                               const QString &name = QStringLiteral("sample.pac"));
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
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    if (f.write(bytes) != bytes.size()) {
        f.close();
        return QString();
    }
    return path;
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
    QVERIFY(!path.isEmpty());

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
    QVERIFY(!path.isEmpty());

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
    QVERIFY(!path.isEmpty());

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
    QVERIFY(!path.isEmpty());

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
    QVERIFY(!path.isEmpty());

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY2(spd::extractFdlsFromPac(path, fdl1, fdl2, &err), qPrintable(err));
    QCOMPARE(fdl1.fileName, QStringLiteral("fdl1.bin"));
    QCOMPARE(fdl1.data, kFdl1Bytes);
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
    QVERIFY(!path.isEmpty());

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY2(spd::extractFdlsFromPac(path, fdl1, fdl2, &err), qPrintable(err));
    QCOMPARE(fdl1.fileName, QStringLiteral("fdl1.bin"));
    QCOMPARE(fdl1.data, kFdl1Bytes);
    QCOMPARE(fdl2.data, kFdl2Bytes);
}

// 缺 FDL2：报缺哪个 + 包内条目（可行动 —— 用户据此判断选错包，或回退手选）
void TestSpdPac::missingFdl2ListsEntries()
{
    QTemporaryDir dir;
    QVERIFY2(dir.isValid(), "临时目录创建失败（/tmp 不可写？）");
    const pactest::PacSample s = pactest::buildNew({fdl1Entry(), bootEntry()});
    const QString path = writeSample(dir, s.data);
    QVERIFY(!path.isEmpty());

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
    QVERIFY(!path.isEmpty());

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
    QVERIFY(!path.isEmpty());

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
    QVERIFY(!path.isEmpty());

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
    QVERIFY(!path.isEmpty());

    spd::ExtractedFdl fdl1, fdl2;
    QString err;
    QVERIFY(!spd::extractFdlsFromPac(path, fdl1, fdl2, &err));
    QVERIFY(!err.isEmpty());
    // 文案要指向"pac 解析"这一步（parsePac 的失败文案都含 PAC）—— 钉住失败来自解析，
    // 而不是"提取器自己坏了"或别的路径
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
    QVERIFY(!path.isEmpty());

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
    QVERIFY2(!QFile::exists(dir.filePath(QStringLiteral("fdl1.bin"))),
             "第一个文件没删：失败留下了半份产物");
}

QTEST_APPLESS_MAIN(TestSpdPac)
#include "test_spd_pac.moc"
