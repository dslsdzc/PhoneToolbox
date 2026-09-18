// tests/test_mtk_gpt.cpp
//
// MTK GPT 解析（Phase D2/D3）：真 4096 字节扇区样本（PGPT/SGPT.img）+ 合成正/负向用例。
// 真样本实测（直读文件复核，reference/mtk-samples/，gitignored）：
//   PGPT.img = 32768 B：EFI PART @4096（LBA1，512 处全零）、条目 @8192（LBA2）、entryCount=61、
//     entrySize=128，头部与条目表 CRC 均 OK；首条 "misc" first=8 last=135。
//   SGPT.img = 32768 B：EFI PART @28672（窗口最后一扇区）、part_entry_start_lba=124960760
//     （= 窗口起始 LBA）→ 条目在窗口开头、头在末尾（UEFI 备份布局）。
#include <QtTest>
#include <QDir>
#include <QFile>
#include <utility>   // std::as_const

#include "core/modes/mtk_gpt.h"
#include "mtk_test_helpers.h"

using mtkgpt::Partition;
using mtkgpt::Table;

namespace {
// 文件偏移 → 读回调（拷进 shared_ptr 让 lambda 持有）
mtkgpt::ReadFn fileReader(const QString &path)
{
    auto shared = std::make_shared<QByteArray>();
    QFile f(path);
    if (f.open(QIODevice::ReadOnly))
        *shared = f.readAll();
    return [shared](quint64 off, int len, QByteArray *out, QString *err) {
        if (off + quint64(len) > quint64(shared->size())) {
            if (err) *err = QStringLiteral("越界读");
            return false;
        }
        *out = shared->mid(int(off), len);
        return true;
    };
}

bool anyName(const Table &t, const QString &n)
{
    for (const Partition &p : std::as_const(t.partitions))
        if (p.name == n) return true;
    return false;
}
} // namespace

class TestMtkGpt : public QObject
{
    Q_OBJECT
private slots:
    void parsesRealFourKSectorPrimaryGpt();
    void parsesRealBackupWindow();
    void fallsBackToBackupWhenPrimaryCrcBad();
    void rejectsGarbageSignatureAndBadRevision();
    void rejectsBadCrc();
    void parsesSyntheticGpt();
    void logsExtensionReadFailureInsteadOfSwallowingIt();
    void primaryExtensionCapIsCountedInSectorsNotBytes();
    void crc32MatchesStandardCheckValue();

private:
    bool samplesReady(QString *why)
    {
        const QDir d(mtktest::samplesDir());
        if (!QFile::exists(d.filePath(QStringLiteral("PGPT.img")))
            || !QFile::exists(d.filePath(QStringLiteral("SGPT.img")))) {
            *why = QStringLiteral("真样本缺失（PGPT/SGPT.img）");
            return false;
        }
        return true;
    }
};

// —— 真样本：4096 字节扇区的主 GPT（LBA1@4096；512 处全零）——
void TestMtkGpt::parsesRealFourKSectorPrimaryGpt()
{
    QString why;
    if (!samplesReady(&why)) {
#if MTK_SAMPLES_REQUIRED
        QFAIL(qPrintable(why + QStringLiteral("（MTK_SAMPLES_REQUIRED=ON）")));
#else
        QSKIP(qPrintable(why));
#endif
    }
    const QString path = QDir(mtktest::samplesDir()).filePath(QStringLiteral("PGPT.img"));
    Table t;
    QStringList log;
    QString err;
    QVERIFY2(mtkgpt::readTable(fileReader(path), /*diskSectors=*/0, t, &log, &err), qPrintable(err));
    QCOMPARE(t.sectorSize, quint32(4096));
    QCOMPARE(t.partitions.size(), 61);
    QVERIFY(!t.usedBackup);
    QCOMPARE(t.partitions.at(0).name, QStringLiteral("misc"));
    QCOMPARE(t.partitions.at(0).firstLba, quint64(8));
    QCOMPARE(t.partitions.at(1).name, QStringLiteral("para"));
    QCOMPARE(t.partitions.at(2).name, QStringLiteral("expdb"));
    // 样本这 61 条真名里**没有** preloader（MTK 的 preloader 在 GPT 之外，只出现在 scatter 的
    // 裸区条目里；PGPT.img 全字节搜 "preloader" 的 UTF-16LE/ASCII 均无命中）—— 取第 61 条
    // flashinfo 做"名字解码正确 + 整张表走完"的抽查。
    QVERIFY(anyName(t, QStringLiteral("flashinfo")));
    QCOMPARE(mtkgpt::offsetBytes(t.partitions.at(0), 4096), quint64(8 * 4096));
    QCOMPARE(mtkgpt::sizeBytes(t.partitions.at(0), 4096), quint64((135 - 8 + 1) * 4096));
    QVERIFY2(log.join('\n').contains(QStringLiteral("4096")), qPrintable(log.join('\n')));
}

// —— 真样本：备份窗口（头在窗口最后一扇区、条目在窗口开头）——
void TestMtkGpt::parsesRealBackupWindow()
{
    QString why;
    if (!samplesReady(&why)) {
#if MTK_SAMPLES_REQUIRED
        QFAIL(qPrintable(why + QStringLiteral("（MTK_SAMPLES_REQUIRED=ON）")));
#else
        QSKIP(qPrintable(why));
#endif
    }
    const QString path = QDir(mtktest::samplesDir()).filePath(QStringLiteral("SGPT.img"));
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray raw = f.readAll();
    QCOMPARE(raw.indexOf(QByteArray("EFI PART", 8)), 7 * 4096);      // 头在窗口第 8（最后）扇区
    Table t;
    QString err;
    QVERIFY2(mtkgpt::parseBackup(raw, 4096, 124960760ull, t, &err), qPrintable(err));   // 窗口起始 LBA 来自样本头部字段
    QCOMPARE(t.partitions.size(), 61);
    QVERIFY(t.usedBackup);
    QCOMPARE(t.partitions.at(0).name, QStringLiteral("misc"));
    QCOMPARE(t.partitions.at(0).firstLba, quint64(8));
}

// —— 主 GPT 头部 CRC 坏 → readTable 走备份兜底 ——
void TestMtkGpt::fallsBackToBackupWhenPrimaryCrcBad()
{
    QString why;
    if (!samplesReady(&why)) {
#if MTK_SAMPLES_REQUIRED
        QFAIL(qPrintable(why + QStringLiteral("（MTK_SAMPLES_REQUIRED=ON）")));
#else
        QSKIP(qPrintable(why));
#endif
    }
    QFile fp(QDir(mtktest::samplesDir()).filePath(QStringLiteral("PGPT.img")));
    QFile fs(QDir(mtktest::samplesDir()).filePath(QStringLiteral("SGPT.img")));
    QVERIFY(fp.open(QIODevice::ReadOnly) && fs.open(QIODevice::ReadOnly));
    QByteArray head = fp.readAll().left(0x22 * 4096);       // 主 GPT 头 + 条目 + 余量
    const QByteArray tail = fs.readAll();                   // 备份窗口（8 扇区）
    head[4096 + 0x10] = char(quint8(head.at(4096 + 0x10)) ^ 0xFF);   // 改坏主头 CRC 字段

    const quint64 diskSectors = 124960768ull;               // 样本 backup_lba + 1
    const quint64 tailBytes = quint64(tail.size());
    const quint64 tailStart = diskSectors * 4096ull - tailBytes;     // = 124960760 × 4096
    mtkgpt::ReadFn reader = [head, tail, tailStart, tailBytes](quint64 off, int len, QByteArray *out, QString *) {
        if (off + quint64(len) <= quint64(head.size())) { *out = head.mid(int(off), len); return true; }
        if (off >= tailStart && off + quint64(len) <= tailStart + tailBytes) {
            *out = tail.mid(int(off - tailStart), len);
            return true;
        }
        return false;
    };
    Table t;
    QStringList log;
    QString err;
    QVERIFY2(mtkgpt::readTable(reader, diskSectors, t, &log, &err), qPrintable(err));
    QVERIFY2(t.usedBackup, "主头 CRC 坏必须走备份 GPT");
    QCOMPARE(t.partitions.size(), 61);
    QVERIFY2(log.join('\n').contains(QStringLiteral("备份")), qPrintable(log.join('\n')));
}

// —— 合成负向：垃圾签名 / revision 不符 ——
void TestMtkGpt::rejectsGarbageSignatureAndBadRevision()
{
    Table t;
    QString err;
    QByteArray raw(0x22 * 512, '\0');
    raw.replace(512, 4, "XXXX");
    QVERIFY(!mtkgpt::parsePrimary(raw, 512, t, &err));
    QVERIFY(!err.isEmpty());

    err.clear();
    QByteArray raw2(0x22 * 512, '\0');
    raw2.replace(512, 8, QByteArray("EFI PART", 8));
    raw2[512 + 0x0C] = 92;                    // header_size = 92（合法）
    raw2[512 + 0x08] = 1;                     // revision = 0x00000001（错）
    QVERIFY(!mtkgpt::parsePrimary(raw2, 512, t, &err));
    QVERIFY2(err.contains(QStringLiteral("revision")) || err.contains(QStringLiteral("版本")), qPrintable(err));
}

// —— 合成负向：头部 CRC 改坏 / 条目表 CRC 改坏 → 各自拒绝（fail-closed）——
void TestMtkGpt::rejectsBadCrc()
{
    const QByteArray good = mtkgpt::testBuildSyntheticGpt(512, 64);
    Table t;
    QString err;
    QVERIFY2(mtkgpt::parsePrimary(good, 512, t, &err), qPrintable(err));
    QByteArray bad = good;
    bad[512 + 0x10] = char(quint8(bad.at(512 + 0x10)) ^ 0x01);
    err.clear();
    QVERIFY(!mtkgpt::parsePrimary(bad, 512, t, &err));
    QVERIFY2(err.contains(QStringLiteral("CRC")), qPrintable(err));

    // ② 条目表内容改坏（首条名字的一个字节；头与头部 CRC 字段都不动）→ 必须由**条目表 CRC** 拦下。
    //    只翻头部 CRC 字段的①挡不住"条目表校验被删"：计划要求两个 CRC 都 fail-closed，
    //    后半段必须有自己的回归钉子。
    QByteArray badEntries = good;
    badEntries[2 * 512 + 56] = char(quint8(badEntries.at(2 * 512 + 56)) ^ 0xFF);
    err.clear();
    QVERIFY(!mtkgpt::parsePrimary(badEntries, 512, t, &err));
    QVERIFY2(err.contains(QStringLiteral("条目表 CRC")), qPrintable(err));
}

// —— 合成正向：1 个分区的合法 GPT（正确双 CRC）——
void TestMtkGpt::parsesSyntheticGpt()
{
    const QByteArray raw = mtkgpt::testBuildSyntheticGpt(512, 64);
    Table t;
    QString err;
    QVERIFY2(mtkgpt::parsePrimary(raw, 512, t, &err), qPrintable(err));
    QCOMPARE(t.partitions.size(), 1);
    QCOMPARE(t.partitions.at(0).name, QStringLiteral("boot"));
    QCOMPARE(t.partitions.at(0).firstLba, quint64(34));
    QCOMPARE(mtkgpt::sizeBytes(t.partitions.at(0), 512), quint64(2 * 512));
}

// 头部字段声明"条目区在探测窗口之外" → readTable 会按头部字段补读一次；**这次补读失败**时
// 失败原因必须进日志。此前 `extErr` 取了地址却从未被读，失败原因整条被丢（-Wall 抓不到）。
// 夹具：把 part_entry_start_lba 抬到 20（needBytes = 20×512 + 4×128 = 10752：既 > 探测窗口
// 0x2000=8192，又在补读上限 0x22×512=17408 之内 —— 否则 A-20 的扇区口径护栏会先拦下、补读根本不发起），
// 读回调**第一次**给窗口、**第二次**（补读）失败并带上可断言的注入原因。
void TestMtkGpt::logsExtensionReadFailureInsteadOfSwallowingIt()
{
    const QByteArray head = mtkgpt::testBuildSyntheticGpt(512, 64).left(0x2000);
    QByteArray probe = head;
    probe[512 + 0x48] = char(20);            // part_entry_start_lba = 20（低字节；原值 2）
    int calls = 0;
    mtkgpt::ReadFn reader = [&probe, &calls](quint64 off, int len, QByteArray *out, QString *err) {
        if (++calls == 1) {
            *out = probe.mid(int(off), len);
            return true;
        }
        if (err) *err = QStringLiteral("注入的补读失败（越界读）");
        return false;
    };
    Table t;
    QStringList log;
    QString err;
    // 整体仍失败（头字段被改坏 → 头部 CRC 不符；diskSectors=0 → 无备份兜底）——
    // 本用例只钉**失败原因的可见性**，故不对返回值做正向断言。
    QVERIFY(!mtkgpt::readTable(reader, 0, t, &log, &err));
    QCOMPARE(calls, 2);                      // 确实发起了补读
    const QString joined = log.join('\n');
    QVERIFY2(joined.contains(QStringLiteral("按头部字段补读主 GPT 失败")), qPrintable(joined));
    QVERIFY2(joined.contains(QStringLiteral("注入的补读失败")), qPrintable(joined));
}

// 补读上限的口径是 `0x22 × sectorSize` **字节**，不是写死的 `0x22 × 4096`。
// 512B 扇区盘上：needBytes = entryLba × 512 + 4 × 128。取 entryLba = 33 与 34 卡在
// 0x22 × 512 = 17408 的**两侧**（33 → 17408 恰好等于上限，发起；34 → 17920 超限，不发起）。
// 夹具按"请求范围"记录每一次读 —— 上下两侧各自的请求条数就是唯一判据。
void TestMtkGpt::primaryExtensionCapIsCountedInSectorsNotBytes()
{
    const QByteArray full = mtkgpt::testBuildSyntheticGpt(512, 64);
    QByteArray hdr = full.left(0x2000);
    struct Case { int entryLba; int expectedReads; int expectedLen; };
    // ① 恰好到上限 → 补读发起；② 超一扇区 → 不发起。（写死 0x22×4096 的实现两侧都会发起；
    //    把比较写成 `<` 的实现会让 ① 变成"不发起"。）
    const QList<Case> cases{ {33, 2, 17408}, {34, 1, 0} };
    for (const Case &c : std::as_const(cases)) {
        QByteArray probe = hdr;
        probe[512 + 0x48] = char(c.entryLba);        // part_entry_start_lba（低字节；原值 2）
        QByteArray backing = full;                   // 补读的**后端**同样带这个改动，
        backing[512 + 0x48] = char(c.entryLba);      // 否则补读会拿回一份合法表、整体反而成功
        QList<QPair<quint64, int>> requests;
        mtkgpt::ReadFn reader = [&probe, &backing, &requests](quint64 off, int len, QByteArray *out, QString *) {
            requests.append({off, len});
            if (off == 0 && len <= probe.size()) { *out = probe.mid(int(off), len); return true; }
            if (off + quint64(len) <= quint64(backing.size())) { *out = backing.mid(int(off), len); return true; }
            return false;
        };
        Table t;
        QStringList log;
        QString err;
        // 头字段被改坏 → 头部 CRC 不符，整体必然失败；本用例只钉**读请求的范围**
        QVERIFY(!mtkgpt::readTable(reader, 0, t, &log, &err));
        QCOMPARE(requests.size(), c.expectedReads);
        if (c.expectedReads == 2)
            QCOMPARE(requests.at(1).second, c.expectedLen);   // 补读长度 = ceil(needBytes/512)×512
    }
}

// crc32 的**已知答案**：CRC-32/ISO-HDLC 的标准 check 值（"123456789" → 0xCBF43926）。
// 为什么必须有这条：自 A-30 起合成 GPT 夹具改用**生产的** mtkgpt::crc32 来算双 CRC
//（夹具构造器，两边同源才不会造出"生产不认"的盘），代价是"夹具-解析器"之间不再互为交叉校验
// ——crc32 本身算错时两边一起错、盘照样"合法"。本用例把**绝对值**钉在外参照上补回这条判据：
// 多项式/初值/反射方向任一改动都会让它红（而 GPT 用例与夹具那侧不会）。
void TestMtkGpt::crc32MatchesStandardCheckValue()
{
    QCOMPARE(mtkgpt::crc32(QByteArray("123456789", 9)), quint32(0xCBF43926));
    QCOMPARE(mtkgpt::crc32(QByteArray()), quint32(0));       // 空输入 = 0（初值取反后）
}
QTEST_APPLESS_MAIN(TestMtkGpt)
#include "test_mtk_gpt.moc"
