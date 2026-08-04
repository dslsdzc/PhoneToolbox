#include <QtTest>
#include "image_engine/pac_image.h"

// 按社区参考实现构造真实布局（两源独立确认）:
//   divinebird/pacextractor (C)          —— 旧格式, 头 1220B 无魔数
//   HemanthJabalapuri/pacextractor (Java/Python) —— 新格式 BP_R1.0.0/BP_R2.0.1
// 注: .pac 实为 Spreadtrum/Unisoc (SPD ResearchDownload) 容器; MTK 侧 SP Flash Tool
//     使用 scatter 文件而非 pac。社区所有 pac 解包工具均针对 SPD, 字段级布局以其为准。
//
// 新格式 (BP_R1.0.0 / BP_R2.0.1), 头 2124B, 全部小端:
//   version u16[22]@0(UTF-16LE) + hiSize@44 + loSize@48 + productName@52(512B)
//   + firmwareName@564(512B) + partitionCount@1076 + partitionsListStart@1080
//   + mode/flashType/nandStrategy/isNvBackup/nandPageType@1084..1100 + prdAlias@1104(200B)
//   + omaDmPrdFlag@1304 + isOmaDM@1308 + isPreload@1312 + reserved@1316(800B)
//   + magic 0xfffafffa@2116 + CRC1@2120 + CRC2@2122
//   分区项固定 2580B, 从 partitionsListStart 连续排布:
//   length@0(==2580) + partitionName@4(512B) + fileName@516(512B) + szFileName@1028(504B)
//   + hiPartitionSize@1532 + hiDataOffset@1536 + loPartitionSize@1540 + nFileFlag@1544
//   + nCheckFlag@1548 + loDataOffset@1552 + dwCanOmitFlag@1556 + dwAddrNum@1560
//   + dwAddr[5]@1564 + dwReserved[249]@1584
//   partitionSize  = hiPartitionSize<<32 | loPartitionSize（64 位）
//   partitionAddrInPac = hiDataOffset<<32 | loDataOffset（64 位）
//
// 旧格式 (divinebird C), 头 1220B 无魔数:
//   someField u16[24]@0 + someInt@48 + productName@52(512B) + firmwareName@564(512B)
//   + partitionCount@1076 + partitionsListStart@1080 + 其余保留
//   分区项变长, 以 length@0 链式步进（固定部分 1568B, 尾随 dataArray[]）:
//   length(4) + partitionName@4(512B) + fileName@516(1024B) + partitionSize@1540
//   + 保留@1544(8B) + partitionAddrInPac@1552 + 保留@1556(12B)
//   字符串为 UTF-16LE 单元数组（旧工具 getString 只取低字节, 对 ASCII 等价）
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

static QByteArray utf16le(const QString &s)
{
    QByteArray out;
    out.reserve(s.size() * 2);
    for (const QChar c : s) {
        out.append(char(c.unicode() & 0xFF));
        out.append(char((c.unicode() >> 8) & 0xFF));
    }
    return out;
}
static void put16(QByteArray &d, int off, quint32 v)
{
    d[off] = char(v & 0xFF); d[off + 1] = char((v >> 8) & 0xFF);
}
static void put32(QByteArray &d, int off, quint32 v)
{
    for (int i = 0; i < 4; ++i) d[off + i] = char((v >> (i * 8)) & 0xFF);
}
static void put64(QByteArray &d, int off, quint64 v)
{
    for (int i = 0; i < 8; ++i) d[off + i] = char((v >> (i * 8)) & 0xFF);
}

// 新格式分区项（2580B 固定）。注意: 字符串以实际 UTF-16LE 字节写入定宽字段
// （QByteArray::replace 遇更短数据会收缩数组, 必须只替换实际字节数）
static void putEntryNew(QByteArray &d, int off, const QString &name, const QString &file,
                        quint64 size, quint64 dataOffset, quint32 fileFlag)
{
    put32(d, off + 0, 2580);                       // length == SIZE_OF_PARTITION_HEADER
    const QByteArray n = utf16le(name), f = utf16le(file);
    d.replace(off + 4, n.size(), n);
    d.replace(off + 516, f.size(), f);
    put32(d, off + 1532, quint32(size >> 32));     // hiPartitionSize
    put32(d, off + 1536, quint32(dataOffset >> 32)); // hiDataOffset
    put32(d, off + 1540, quint32(size & 0xFFFFFFFF)); // loPartitionSize
    put32(d, off + 1544, fileFlag);                // nFileFlag: 1 = 需要文件
    put32(d, off + 1552, quint32(dataOffset & 0xFFFFFFFF)); // loDataOffset
}

// 旧格式分区项（最小 1568B）
static void putEntryOld(QByteArray &d, int off, const QString &name, const QString &file,
                        quint32 size, quint32 dataOffset)
{
    put32(d, off + 0, 1568);                       // length
    const QByteArray n = utf16le(name), f = utf16le(file);
    d.replace(off + 4, n.size(), n);
    d.replace(off + 516, f.size(), f);
    put32(d, off + 1540, size);                    // partitionSize
    put32(d, off + 1552, dataOffset);              // partitionAddrInPac
}

// 新格式样本: 1 分区 boot/boot.img, 数据 4096B 位于 2124+2580=4704; dwSize = 8800
static QByteArray buildPacNew(const QString &version = QStringLiteral("BP_R1.0.0"))
{
    QByteArray pac(2124 + 2580, 0);
    pac.replace(0, utf16le(version).size(), utf16le(version));
    const quint32 total = quint32(pac.size() + 4096);
    put32(pac, 44, 0);           // dwHiSize
    put32(pac, 48, total);       // dwLoSize
    put32(pac, 1076, 1);         // partitionCount
    put32(pac, 1080, 2124);      // partitionsListStart
    put32(pac, 2116, 0xfffafffa); // dwMagic
    putEntryNew(pac, 2124, "boot", "boot.img", 4096, 4704, 1);
    pac.append(QByteArray(4096, '\x5a'));
    return pac;
}

// 旧格式样本: 1 分区 boot/boot.img, 数据 4096B 位于 1220+1568=2788
static QByteArray buildPacLegacy()
{
    QByteArray pac(1220 + 1568, 0);
    put32(pac, 1076, 1);         // partitionCount
    put32(pac, 1080, 1220);      // partitionsListStart
    putEntryOld(pac, 1220, "boot", "boot.img", 4096, 2788);
    pac.append(QByteArray(4096, '\x5a'));
    return pac;
}

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
