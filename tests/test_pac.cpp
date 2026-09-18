#include <QtTest>
#include "image_engine/pac_image.h"
#include "pac_test_helpers.h"

// 合成夹具（新/老两代布局的字段级偏移说明、put* 原语、buildNew/buildLegacy）在
// tests/pac_test_helpers.h —— test_spd_pac 用同一份夹具造含 FDL1/FDL2 的样本，
// 不再各留一份副本（夹具与解析器共用原语会自洽通过，见该头注释）。
using pactest::buildPacLegacy;
using pactest::buildPacNew;
using pactest::put32;
using pactest::putEntryNew;
using pactest::putEntryOld;
using pactest::utf16le;

class TestPac : public QObject
{
    Q_OBJECT
private slots:
    void detect();
    void parseNewFormat();
    void parseNewFormatMulti();
    void parseLegacyFormat();
    void parseLegacyVariableLength();
    void invalidInput();
};

void TestPac::detect()
{
    QVERIFY(imgpac::isPac(buildPacNew()));
    QVERIFY(imgpac::isPac(buildPacNew(QStringLiteral("BP_R2.0.1"))));
    QVERIFY(imgpac::isPac(buildPacLegacy()));
    QVERIFY(!imgpac::isPac(QByteArray()));
    QVERIFY(!imgpac::isPac(QByteArray(100, 0)));
    QVERIFY(!imgpac::isPac(QByteArray(2048, 'x'))); // 长度够但结构不符
}

void TestPac::parseNewFormat()
{
    const QByteArray pac = buildPacNew();
    QList<imgpac::PacPartition> parts;
    QString err;
    QVERIFY2(imgpac::parsePac(pac, parts, &err), qPrintable(err));
    QCOMPARE(parts.size(), 1);
    QCOMPARE(parts[0].name, QStringLiteral("boot"));
    QCOMPARE(parts[0].fileName, QStringLiteral("boot.img"));
    QCOMPARE(parts[0].offset, 4704ull);
}

void TestPac::parseNewFormatMulti()
{
    // 2 分区: FDL1/fdl1.bin + boot/boot.img（数据偏移随条目后移）
    QByteArray pac(2124 + 2 * 2580, 0);
    pac.replace(0, utf16le(QStringLiteral("BP_R2.0.1")).size(), utf16le(QStringLiteral("BP_R2.0.1")));
    const quint32 total = quint32(pac.size() + 1024 + 4096);
    put32(pac, 44, 0);
    put32(pac, 48, total);
    put32(pac, 1076, 2);
    put32(pac, 1080, 2124);
    put32(pac, 2116, 0xfffafffa);
    putEntryNew(pac, 2124, "FDL1", "fdl1.bin", 1024, 7284, 1);      // 2124+5160=7284
    putEntryNew(pac, 2124 + 2580, "boot", "boot.img", 4096, 8308, 1); // 7284+1024
    pac.append(QByteArray(1024, '\x10'));
    pac.append(QByteArray(4096, '\x5a'));
    QList<imgpac::PacPartition> parts;
    QString err;
    QVERIFY2(imgpac::parsePac(pac, parts, &err), qPrintable(err));
    QCOMPARE(parts.size(), 2);
    QCOMPARE(parts[0].name, QStringLiteral("FDL1"));
    QCOMPARE(parts[0].fileName, QStringLiteral("fdl1.bin"));
    QCOMPARE(parts[0].offset, 7284ull);
    QCOMPARE(parts[1].name, QStringLiteral("boot"));
    QCOMPARE(parts[1].fileName, QStringLiteral("boot.img"));
    QCOMPARE(parts[1].offset, 8308ull);
}

void TestPac::parseLegacyFormat()
{
    const QByteArray pac = buildPacLegacy();
    QList<imgpac::PacPartition> parts;
    QString err;
    QVERIFY2(imgpac::parsePac(pac, parts, &err), qPrintable(err));
    QCOMPARE(parts.size(), 1);
    QCOMPARE(parts[0].name, QStringLiteral("boot"));
    QCOMPARE(parts[0].fileName, QStringLiteral("boot.img"));
    QCOMPARE(parts[0].offset, 2788ull);
}

void TestPac::parseLegacyVariableLength()
{
    // 旧格式条目可变长（尾随 dataArray[]）: length 1600, 多 32B 填充, 下一条目从链尾续接
    QByteArray pac(1220 + 1600 + 1568, 0);
    put32(pac, 1076, 2);
    put32(pac, 1080, 1220);
    put32(pac, 1220, 1600); // 第一条目 length = 1600
    const QByteArray n1 = utf16le(QStringLiteral("preloader"));
    const QByteArray f1 = utf16le(QStringLiteral("preloader.bin"));
    pac.replace(1224, n1.size(), n1);
    pac.replace(1736, f1.size(), f1);
    put32(pac, 1220 + 1540, 2048);                 // partitionSize
    put32(pac, 1220 + 1552, 1220 + 1600 + 1568);   // partitionAddrInPac
    putEntryOld(pac, 1220 + 1600, "boot", "boot.img", 4096, 1220 + 1600 + 1568 + 2048);
    pac.append(QByteArray(2048, '\x11'));
    pac.append(QByteArray(4096, '\x5a'));
    QList<imgpac::PacPartition> parts;
    QString err;
    QVERIFY2(imgpac::parsePac(pac, parts, &err), qPrintable(err));
    QCOMPARE(parts.size(), 2);
    QCOMPARE(parts[0].name, QStringLiteral("preloader"));
    QCOMPARE(parts[0].fileName, QStringLiteral("preloader.bin"));
    QCOMPARE(parts[0].offset, 1220ull + 1600 + 1568);
    QCOMPARE(parts[1].name, QStringLiteral("boot"));
    QCOMPARE(parts[1].offset, 1220ull + 1600 + 1568 + 2048);
}

void TestPac::invalidInput()
{
    QString err;
    QList<imgpac::PacPartition> parts;
    // 长度不足 / 结构垃圾
    QVERIFY(!imgpac::parsePac(QByteArray(), parts, &err));
    QVERIFY(!imgpac::parsePac(QByteArray(100, 0), parts, &err));
    QVERIFY(!imgpac::parsePac(QByteArray(2048, 'x'), parts, &err));
    QVERIFY(!err.isEmpty());
    // 新格式: partitionCount 为 0 / 巨大
    QByteArray p = buildPacNew();
    put32(p, 1076, 0);
    QVERIFY(!imgpac::parsePac(p, parts, &err));
    p = buildPacNew();
    put32(p, 1076, 0xFFFFFFFF);
    QVERIFY(!imgpac::parsePac(p, parts, &err));
    // 新格式: partitionsListStart 越界
    p = buildPacNew();
    put32(p, 1080, 99999);
    QVERIFY(!imgpac::parsePac(p, parts, &err));
    // 新格式: 分区项 length != 2580
    p = buildPacNew();
    put32(p, 2124, 0xBAD);
    QVERIFY(!imgpac::parsePac(p, parts, &err));
    // 新格式: dwSize 与实际长度不符
    p = buildPacNew();
    put32(p, 48, 4096);
    QVERIFY(!imgpac::parsePac(p, parts, &err));
    // 新格式: 分区数据偏移越界
    p = buildPacNew();
    put32(p, 2124 + 1552, 0xFFFFFF00); // loDataOffset 巨大
    QVERIFY(!imgpac::parsePac(p, parts, &err));
    // 新格式: 版本串损坏 → 按旧格式结构性校验（count/表/条目均不符, 失败）
    p = buildPacNew();
    p.replace(0, 44, QByteArray(44, 0));
    put32(p, 1076, 0xFFFFFFFF);
    QVERIFY(!imgpac::parsePac(p, parts, &err));
    // 旧格式: 条目 length 过小（< 固定部分 1568）
    p = buildPacLegacy();
    put32(p, 1220, 100);
    QVERIFY(!imgpac::parsePac(p, parts, &err));
    // 旧格式: 条目链越界（length 超出文件尾）
    p = buildPacLegacy();
    put32(p, 1220, 20000);
    QVERIFY(!imgpac::parsePac(p, parts, &err));
    // 旧格式: 数据偏移越界
    p = buildPacLegacy();
    put32(p, 1220 + 1552, 0xFFFFFF00);
    QVERIFY(!imgpac::parsePac(p, parts, &err));
    // 失败路径不污染输出
    QVERIFY(parts.isEmpty());
}

QTEST_APPLESS_MAIN(TestPac)
#include "test_pac.moc"
