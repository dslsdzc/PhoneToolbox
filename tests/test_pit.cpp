// tests/test_pit.cpp
//
// PIT 解析：合成夹具（始终跑）+ 真样本硬断言（reference/ 存在时才跑，否则 QSKIP）。
// 真样本事实依据：docs/superpowers/specs/samsung-odin-facts.md 第 3 节（控制方逐字节复核）。
#ifndef ODIN_SAMPLES_DIR
#define ODIN_SAMPLES_DIR ""
#endif

#include <QtTest>
#include <QDir>
#include <QDirIterator>
#include <QFile>

#include "core/odin/pit.h"
#include "odin_test_helpers.h"

class TestPit : public QObject
{
    Q_OBJECT
private slots:
    void parsesHeaderAndEntries();
    void cleansStringsWithCrlfAndNul();
    void passthroughAttributesAndDeviceTypeUntouched();
    void partitionBytesByDeviceType();
    void hasImageNameRules();
    void findByNameIsCaseInsensitive();
    void keepsTrailingSignature();
    void rejectsBadInput();
    // 真样本（reference/samsung-samples/，gitignored → 缺失时 QSKIP）
    void realSamplesParseWithKnownFacts();
};

using namespace odintest;

static QList<PitSpec> bootSbootNv()
{
    QList<PitSpec> es;
    PitSpec boot; boot.name = QByteArray("BOOT");  boot.identifier = 80; boot.flashFilename = QByteArray("spl.img");
    boot.blockCount = 1024; es << boot;
    PitSpec sboot; sboot.name = QByteArray("SBOOT"); sboot.identifier = 1; sboot.flashFilename = QByteArray("sboot.bin");
    sboot.blockCount = 4096; es << sboot;
    PitSpec nv; nv.name = QByteArray("wfixnv2"); nv.identifier = 4; nv.flashFilename = QByteArray("nvitem.bin");
    nv.blockCount = 2048; es << nv;
    return es;
}

void TestPit::parsesHeaderAndEntries()
{
    const QByteArray raw = buildPit(bootSbootNv());
    odin::PitTable t;
    QString err;
    QVERIFY2(odin::parsePit(raw, t, &err), qPrintable(err));
    QCOMPARE(t.comTar2, QByteArray("COM_TAR2"));
    QCOMPARE(t.cpuBlId, QByteArray("MSM8974"));
    QCOMPARE(t.entries.size(), 3);
    QCOMPARE(t.entries[0].partitionName, QStringLiteral("BOOT"));
    QCOMPARE(t.entries[0].identifier, quint32(80));
    QCOMPARE(t.entries[0].flashFilename, QStringLiteral("spl.img"));
    QCOMPARE(t.entries[1].partitionName, QStringLiteral("SBOOT"));
    QCOMPARE(t.entries[1].blockCount, quint32(4096));
    QCOMPARE(t.entries[2].flashFilename, QStringLiteral("nvitem.bin"));
    QCOMPARE(t.trailingBytes, quint64(1024));
    // 头部 u16 字段：与写入值一致（不是自造的默认值）
    QCOMPARE(t.luCount, quint16(0));
    odin::PitTable t2;
    QVERIFY(odin::parsePit(buildPit(bootSbootNv(), "COM_TAR2", "SM8750", 4), t2, &err));
    QCOMPARE(t2.luCount, quint16(4));
}

void TestPit::cleansStringsWithCrlfAndNul()
{
    QList<PitSpec> es = bootSbootNv();
    es[0].fotaFilename = QByteArray("remained\r\n");       // 真样本 j1xlte 的 USERDATA 形态
    es[1].flashFilename = QByteArray("sboot.bin\r\n");
    es[2].name = QByteArray(" wfixnv2 ");                  // 首尾空白
    odin::PitTable t;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(es), t, &err), qPrintable(err));
    QCOMPARE(t.entries[0].fotaFilename, QStringLiteral("remained"));
    QCOMPARE(t.entries[1].flashFilename, QStringLiteral("sboot.bin"));
    QCOMPARE(t.entries[2].partitionName, QStringLiteral("wfixnv2"));
    // 清洗函数本身：NUL 截断 + CR/LF 去除 + 首尾空白
    QCOMPARE(odin::cleanPitString(QByteArray("abc\r\n\0XYZ", 9)), QStringLiteral("abc"));
    QCOMPARE(odin::cleanPitString(QByteArray("\0", 1)), QString());
    QCOMPARE(odin::cleanPitString(QByteArray("  x\t", 4)), QStringLiteral("x"));
}

void TestPit::passthroughAttributesAndDeviceTypeUntouched()
{
    // 三方对 attributes 的解读互不一致 → 只透传原值（含高位）；deviceType 不裁枚举（8 = UFS 透传）
    QList<PitSpec> es = bootSbootNv();
    es[0].attributes = 0x80000001u;
    es[0].updateAttributes = 5;                            // 真数据里出现 5（Heimdall 的 fota=1/secure=2 解释不了）
    es[0].deviceType = 8;                                  // UFS（Heimdall 枚举只到 3）
    es[0].blockSizeOrOffset = 0xDEADBEEFu;
    odin::PitTable t;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(es), t, &err), qPrintable(err));
    QCOMPARE(t.entries[0].attributes, 0x80000001u);
    QCOMPARE(t.entries[0].updateAttributes, quint32(5));
    QCOMPARE(t.entries[0].deviceType, quint32(8));
    QCOMPARE(t.entries[0].blockSizeOrOffset, 0xDEADBEEFu);
}

void TestPit::partitionBytesByDeviceType()
{
    odin::PitTable t;
    QString err;
    QList<PitSpec> es = bootSbootNv();
    es[0].blockCount = 2048; es[0].deviceType = 2;         // MMC → 512B/扇区
    es[1].blockCount = 2048; es[1].deviceType = 8;         // UFS → 4096B/扇区（samloader-rs pit/src/lib.rs:155-160）
    es[2].blockCount = 0;                                  // 未声明（真样本 USERDATA 就是这样）
    QVERIFY2(odin::parsePit(buildPit(es), t, &err), qPrintable(err));
    QCOMPARE(t.entries[0].partitionBytes(), quint64(2048) * 512);
    QCOMPARE(t.entries[1].partitionBytes(), quint64(2048) * 4096);
    QCOMPARE(t.entries[2].partitionBytes(), quint64(0));
}

void TestPit::hasImageNameRules()
{
    odin::PitTable t;
    QString err;
    QList<PitSpec> es = bootSbootNv();
    es[1].flashFilename = QByteArray();                    // 空 = 未声明
    es[2].flashFilename = QByteArray("-");                 // 真样本 j1xlte 的字面量占位符
    es[0].name = QByteArray();                             // 无名条目：isFlashable() == false（libpit.h:107-110）
    QVERIFY2(odin::parsePit(buildPit(es), t, &err), qPrintable(err));
    QVERIFY(!t.entries[0].isFlashable());
    QVERIFY(t.entries[0].hasImageName());                  // 有文件名但没有分区名 → 仍需调用方自己判断
    QVERIFY(t.entries[1].isFlashable());
    QVERIFY(!t.entries[1].hasImageName());
    QVERIFY(!t.entries[2].hasImageName());                 // "-" 不是文件名
}

void TestPit::findByNameIsCaseInsensitive()
{
    odin::PitTable t;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(bootSbootNv()), t, &err), qPrintable(err));
    QVERIFY(t.findByName(QStringLiteral("boot")) != nullptr);
    QCOMPARE(t.findByName(QStringLiteral("boot"))->identifier, quint32(80));
    QCOMPARE(t.indexOfName(QStringLiteral("SBOOT")), 1);
    QCOMPARE(t.indexOfName(QStringLiteral("nope")), -1);
    QVERIFY(t.findByName(QStringLiteral("nope")) == nullptr);
}

void TestPit::keepsTrailingSignature()
{
    // 尾部签名长度不定（真样本 256/272/512/652/1024B）→ 只记长度、不解释、不影响解析
    odin::PitTable t;
    QString err;
    QVERIFY2(odin::parsePit(buildPit(bootSbootNv(), "COM_TAR2", "MSM8974", 0,
                                    QByteArray("\x01\x02\x03", 3)), t, &err), qPrintable(err));
    QCOMPARE(t.trailingBytes, quint64(3));
    // 恰好等长（无尾）也算合法：0 字节尾部
    odin::PitTable t2;
    QVERIFY2(odin::parsePit(buildPit(bootSbootNv(), "COM_TAR2", "MSM8974", 0, QByteArray()), t2, &err),
             qPrintable(err));
    QCOMPARE(t2.trailingBytes, quint64(0));
}

void TestPit::rejectsBadInput()
{
    QString err;
    odin::PitTable t;

    // 太短（< 28 字节头）
    QVERIFY(!odin::parsePit(QByteArray(20, '\0'), t, &err));
    QVERIFY(!err.isEmpty());

    // 魔数不符
    QByteArray bad = buildPit(bootSbootNv());
    bad[0] = char(bad[0] ^ 0xFF);
    err.clear();
    QVERIFY(!odin::parsePit(bad, t, &err));
    QVERIFY(err.contains(QStringLiteral("魔数")));

    // 条目数声称 100 但文件只够 3 条（截断）
    QByteArray trunc = buildPit(bootSbootNv());
    odintest::putU32(trunc, 4, 100);
    err.clear();
    QVERIFY(!odin::parsePit(trunc, t, &err));
    QVERIFY(!err.isEmpty());

    // 条目数 0 → 拒（空计划由计划层再拦一道，但空 PIT 本身无意义）
    err.clear();
    QVERIFY(!odin::parsePit(buildPit({}), t, &err));
    QVERIFY(!err.isEmpty());

    // 文件不存在
    err.clear();
    QVERIFY(!odin::parsePitFile(QStringLiteral("/nonexistent/x.pit"), t, &err));
    QVERIFY(!err.isEmpty());
}

// ---- 真样本硬断言（本计划最强的离线证据）----
// 逐条依据：reference/samsung-samples/<file>（sha256 与来源 URL 见事实报告 §3.1/§3.2）。
void TestPit::realSamplesParseWithKnownFacts()
{
    const QString root = QString::fromLatin1(ODIN_SAMPLES_DIR);
    QDir dir(root);
    if (root.isEmpty() || !dir.exists())
        QSKIP("真样本目录不存在（reference/ 为 gitignored；见 spec §2）");

    QStringList pits;
    QDirIterator it(root, {QStringLiteral("*.pit")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        pits << it.next();
    pits.sort();
    QVERIFY2(pits.size() >= 9, qPrintable(QStringLiteral("真 PIT 少于 9 个：%1").arg(pits.size())));

    for (const QString &p : qAsConst(pits)) {
        odin::PitTable t;
        QString err;
        QVERIFY2(odin::parsePitFile(p, t, &err), qPrintable(QFileInfo(p).fileName() + ": " + err));
        QVERIFY(!t.entries.isEmpty());
        QVERIFY(!t.comTar2.isEmpty());
        // 尾部签名块：10/10 样本都非空（256..1024B）—— 要求 filesize == 28+count*132 的解析器会全灭
        QVERIFY2(t.trailingBytes > 0, qPrintable(QFileInfo(p).fileName()));
        // 每个分区名都清洗过（无 CR/LF/NUL）
        for (const odin::PitEntry &e : qAsConst(t.entries)) {
            QVERIFY(!e.partitionName.contains(QLatin1Char('\r')));
            QVERIFY(!e.partitionName.contains(QLatin1Char('\n')));
        }
        // 至少一个条目声明了文件名
        bool anyName = false;
        for (const odin::PitEntry &e : qAsConst(t.entries))
            anyName = anyName || e.hasImageName();
        QVERIFY(anyName);
    }

    // —— 具体样本的硬断言（值由控制方逐字节复核，不是转述）——
    odin::PitTable j1;
    QString err;
    QVERIFY2(odin::parsePitFile(root + QStringLiteral("/sm-j110h/J1POP3G.pit"), j1, &err), qPrintable(err));
    QCOMPARE(j1.entries.size(), 30);
    QCOMPARE(j1.cpuBlId, QByteArray("SPRD8735"));
    QCOMPARE(j1.trailingBytes, quint64(1024));
    QCOMPARE(j1.luCount, quint16(0));                       // 不是 padding，但也不保证非 0（见 SM-Q7MQ）
    QCOMPARE(j1.entries[0].partitionName, QStringLiteral("BOOT"));
    QCOMPARE(j1.entries[0].flashFilename, QStringLiteral("spl.img"));
    QCOMPARE(j1.entries[0].identifier, quint32(80));
    QCOMPARE(j1.entries[0].deviceType, quint32(2));
    QCOMPARE(j1.entries[0].attributes, quint32(0x2));
    // 已知反例：PIT 条目声明的文件名是 J1POP3G_LTN_OPEN.pit，而 CSC 包内是 J1POP3G.pit
    QCOMPARE(j1.entries[2].partitionName, QStringLiteral("PIT"));
    QCOMPARE(j1.entries[2].flashFilename, QStringLiteral("J1POP3G_LTN_OPEN.pit"));
    // USERDATA：blockCount=0（未声明）+ fotaFilename="remained"
    QCOMPARE(j1.entries[29].partitionName, QStringLiteral("USERDATA"));
    QCOMPARE(j1.entries[29].blockCount, quint32(0));
    QCOMPARE(j1.entries[29].partitionBytes(), quint64(0));
    QCOMPARE(j1.entries[29].fotaFilename, QStringLiteral("remained"));

    // SM8750（UFS）：lu_count=4、全部条目 deviceType=8 —— "lu_count 恒 0""deviceType 只到 3" 都不成立
    odin::PitTable q7;
    QVERIFY2(odin::parsePitFile(root + QStringLiteral("/samsung-sm-q7mq-eur-openx-SM8750.pit"), q7, &err),
             qPrintable(err));
    QCOMPARE(q7.entries.size(), 136);
    QCOMPARE(q7.luCount, quint16(4));
    for (const odin::PitEntry &e : qAsConst(q7.entries))
        QCOMPARE(e.deviceType, quint32(8));

    // CR/LF 清洗的真证据：j1xlte 样本的 USERDATA.fotaFilename 在**文件里**是 "remained\r\n"
    const QByteArray raw = [&] {
        QFile f(root + QStringLiteral("/samsung-sm-j110h-j1xlte-LSI3475.pit"));
        if (!f.open(QIODevice::ReadOnly)) return QByteArray();
        return f.readAll();
    }();
    QVERIFY(raw.contains(QByteArray("remained\r\n")));      // 原始字节确有 CRLF
    odin::PitTable j1x;
    QVERIFY2(odin::parsePitFile(root + QStringLiteral("/samsung-sm-j110h-j1xlte-LSI3475.pit"), j1x, &err),
             qPrintable(err));
    QCOMPARE(j1x.entries.last().fotaFilename, QStringLiteral("remained"));  // 解析后已清洗
    QCOMPARE(j1x.entries[1].flashFilename, QStringLiteral("-"));            // 字面占位符
    QVERIFY(!j1x.entries[1].hasImageName());
}

QTEST_APPLESS_MAIN(TestPit)
#include "test_pit.moc"
