#include <QtTest>
#include "image_engine/sin_image.h"
#include <lz4.h>

// 按三源确认的真实 SIN v3 布局构造（flashtool S1ParseLib / ROMExplorer / sin2raw, 大端 BE）:
//   头: [0]=0x03 "SIN" headerLen u32 BE type u32 BE（15B）
//   SinDataHeader: "MMCF" mmcfLen u32 BE "GPTP" gptpLen u32 BE + GPTGUID(gptpLen-8)
//   块区域长 = mmcfLen - gptpLen; 数据基址 = headerLen + mmcfLen + 8
//   ADDR 块(0x44): magic@0 blockLen@4 dataOffset@8 dataLen@16 fileOffset@24 hashType@32 SHA256@36
//   LZ4A 块(0x54): magic@0 blockLen@4 dataOffset@8 uncompDataLen@16 compDataLen@24
//                  fileOffset@32 reserved@40 hashType@48 SHA256@52
class TestSin : public QObject
{
    Q_OBJECT
private slots:
    void detect();
    void parseAddrBlock();
    void extractAddrBlock();
    void lz4aBlock();
    void invalidInput();
};

static void put32(QByteArray &d, int off, quint32 v)
{
    d[off] = char(v >> 24); d[off + 1] = char(v >> 16); d[off + 2] = char(v >> 8); d[off + 3] = char(v);
}
static void put64(QByteArray &d, int off, quint64 v)
{
    for (int i = 0; i < 8; ++i) d[off + i] = char((v >> ((7 - i) * 8)) & 0xFF);
}

// 头: 0x03 "SIN" + headerLen(15) + type(0x24)（15B）
static QByteArray makeHeader()
{
    QByteArray hdr(15, 0);
    hdr[0] = char(0x03);
    hdr.replace(1, 3, "SIN");
    put32(hdr, 4, 15);
    put32(hdr, 8, 0x24);
    return hdr;
}

// SinDataHeader + GPTGUID: mmcfLen = gptpLen + 块区域长（GUID = gptpLen-8 字节, 全 0）
static QByteArray makeDataHeader(quint32 gptpLen, quint32 blocksLen)
{
    QByteArray h(16 + (gptpLen - 8), 0);
    h.replace(0, 4, "MMCF");
    put32(h, 4, gptpLen + blocksLen);
    h.replace(8, 4, "GPTP");
    put32(h, 12, gptpLen);
    return h;
}

// ADDR 块（0x44B, BE）; dataOffset 相对数据基址
static QByteArray makeAddrBlock(quint64 dataOffset, quint64 dataLen, quint64 fileOffset)
{
    QByteArray b(0x44, 0);
    b.replace(0, 4, "ADDR");
    put32(b, 4, 0x44);
    put64(b, 8, dataOffset);
    put64(b, 16, dataLen);
    put64(b, 24, fileOffset);
    put32(b, 32, 0x02); // hashType = SHA256
    return b;
}

// LZ4A 块（0x54B, BE）; dataOffset 相对数据基址
static QByteArray makeLz4aBlock(quint64 dataOffset, quint64 uncompLen, quint64 compLen,
                                quint64 fileOffset)
{
    QByteArray b(0x54, 0);
    b.replace(0, 4, "LZ4A");
    put32(b, 4, 0x54);
    put64(b, 8, dataOffset);
    put64(b, 16, uncompLen);
    put64(b, 24, compLen);
    put64(b, 32, fileOffset);
    put64(b, 40, 0);    // reserved
    put32(b, 48, 0x02); // hashType = SHA256
    return b;
}

// ADDR 样本: 1 块, 数据区自基址起 4096B（0x44）; 数据基址 = 15 + (0x18+0x44) + 8 = 115
static QByteArray buildSinAddr()
{
    const QByteArray data(4096, '\x44');
    const QByteArray blk = makeAddrBlock(0, data.size(), 1024);
    return makeHeader() + makeDataHeader(0x18, blk.size()) + blk + data;
}

// LZ4A 样本: 1 块, payload 700B（0x5A）LZ4 raw-block 压缩; 数据基址 = 15 + (0x18+0x54) + 8 = 131
// 压缩失败返回空（调用方 QVERIFY(!sin.isEmpty()) 断言）—— 辅助函数内不可用 QVERIFY2
// （其失败分支含裸 return, 函数返回类型为 QByteArray 时无法编译）。
static QByteArray buildSinLz4a(QByteArray *payloadOut = nullptr, QByteArray *compOut = nullptr)
{
    QByteArray payload(700, '\x5a');
    QByteArray comp(static_cast<int>(LZ4_compressBound(payload.size())), Qt::Uninitialized);
    const int n = LZ4_compress_default(payload.constData(), comp.data(),
                                       static_cast<int>(payload.size()), comp.size());
    if (n <= 0)
        return {};
    comp.truncate(n);
    const QByteArray blk = makeLz4aBlock(0, payload.size(), comp.size(), 4096);
    if (payloadOut) *payloadOut = payload;
    if (compOut) *compOut = comp;
    return makeHeader() + makeDataHeader(0x18, blk.size()) + blk + comp;
}

void TestSin::detect()
{
    QVERIFY(imgsin::isSinV3(QByteArray("\x03SIN", 4)));
    QVERIFY(!imgsin::isSinV3(QByteArray("CrAU")));
}

void TestSin::parseAddrBlock()
{
    QByteArray sin = buildSinAddr();
    QList<imgsin::BlockDesc> blocks;
    QString err;
    QVERIFY2(imgsin::parseSin(sin, blocks, &err), qPrintable(err));
    QCOMPARE(blocks.size(), 1);
    QVERIFY(!blocks[0].compressed);
    QCOMPARE(blocks[0].dataDest, 1024ull);   // fileOffset
    QCOMPARE(blocks[0].dataLength, 4096ull); // dataLen
    QCOMPARE(blocks[0].dataStart, 115ull);   // 基址 + dataOffset(0)
}

void TestSin::extractAddrBlock()
{
    QByteArray sin = buildSinAddr();
    QList<imgsin::BlockDesc> blocks;
    QString err;
    QVERIFY2(imgsin::parseSin(sin, blocks, &err), qPrintable(err));
    QByteArray raw = imgsin::extractRaw(sin, blocks, &err);
    QCOMPARE(raw.size(), 1024 + 4096); // 空洞 + 落位
    for (int i = 0; i < 1024; ++i)
        QVERIFY(uchar(raw[i]) == 0xFF);          // 空洞 0xFF 填充
    QCOMPARE(raw.mid(1024), sin.mid(115, 4096)); // 数据自基址起原样落位
}

void TestSin::lz4aBlock()
{
    QByteArray payload, comp;
    QByteArray sin = buildSinLz4a(&payload, &comp);
    QVERIFY2(!sin.isEmpty(), "LZ4 raw-block 压缩失败");
    const quint64 compLen = static_cast<quint64>(comp.size());
    QList<imgsin::BlockDesc> blocks;
    QString err;
    QVERIFY2(imgsin::parseSin(sin, blocks, &err), qPrintable(err));
    QCOMPARE(blocks.size(), 1);
    QVERIFY(blocks[0].compressed);
    QCOMPARE(blocks[0].blockSize, 700ull);        // uncompDataLen
    QCOMPARE(blocks[0].dataLength, compLen);      // compDataLen
    QCOMPARE(blocks[0].dataDest, 4096ull);        // fileOffset
    QCOMPARE(blocks[0].dataStart, 131ull);        // 基址 + dataOffset(0)

    QByteArray raw = imgsin::extractRaw(sin, blocks, &err);
    QCOMPARE(raw.size(), 4096 + 700);
    for (int i = 0; i < 4096; ++i)
        QVERIFY(uchar(raw[i]) == 0xFF); // 空洞 0xFF
    QCOMPARE(raw.mid(4096), payload);    // raw-block 解压后原样落位
}

void TestSin::invalidInput()
{
    QString err;
    QList<imgsin::BlockDesc> blocks;
    // 垃圾 / 截断 / 缺 MMCF
    QVERIFY(!imgsin::parseSin(QByteArray("junkjunkjunk"), blocks, &err));
    QVERIFY(!imgsin::parseSin(QByteArray("\x03SIN", 4), blocks, &err));
    QVERIFY(!imgsin::parseSin(makeHeader() + QByteArray(32, 0), blocks, &err));
    // mmcfLen 巨大 → 数据基址越界
    QByteArray badM = buildSinAddr();
    put32(badM, 19, 0xFFFFFF00); // mmcfLen @19（BE）
    QVERIFY(!imgsin::parseSin(badM, blocks, &err));
    // 块魔数损坏 → 容错终止 → 无块 → 失败
    QByteArray badB = buildSinAddr();
    badB.replace(47, 4, "XXXX");
    QVERIFY(!imgsin::parseSin(badB, blocks, &err));

    // extractRaw: 越界 / 空块列表 / 损坏压缩流 —— 均返回空不崩溃
    QByteArray sin = buildSinAddr();
    QVERIFY2(imgsin::parseSin(sin, blocks, &err), qPrintable(err));
    blocks[0].dataStart = 100000; // 越界
    QVERIFY(imgsin::extractRaw(sin, blocks, &err).isEmpty());
    blocks[0].dataStart = 115;
    blocks[0].dataLength = 999999; // 越界
    QVERIFY(imgsin::extractRaw(sin, blocks, &err).isEmpty());
    err.clear();
    QVERIFY(imgsin::extractRaw(sin, QList<imgsin::BlockDesc>(), &err).isEmpty());
    QVERIFY(!err.isEmpty()); // M2: 空块列表置 error

    QByteArray sinL = buildSinLz4a();
    QVERIFY2(!sinL.isEmpty(), "LZ4 raw-block 压缩失败");
    QVERIFY2(imgsin::parseSin(sinL, blocks, &err), qPrintable(err));
    QByteArray corrupted = sinL;
    corrupted.replace(131, 4, "BROK"); // 破坏 LZ4 压缩流
    QVERIFY(imgsin::extractRaw(corrupted, blocks, &err).isEmpty());
}

QTEST_APPLESS_MAIN(TestSin)
#include "test_sin.moc"
