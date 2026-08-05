#include <QtTest>
#include <QtEndian>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include "image_engine/sparse_image.h"

class TestSparse : public QObject
{
    Q_OBJECT
private slots:
    void detectSparse();
    void simg2imgRawChunk();
    void simg2imgFillAndDontcare();
    void img2simgRoundTrip();
    void img2simgRoundTripMixed();
    void img2simgPadsPartialBlock();
    void simg2imgRejectsUnknownChunk();
    void simg2imgRejectsTruncated();
    void simg2imgRejectsZeroTotalSz();
    void simg2imgFillPattern();
    void aospHeaderU16Layout();
    void img2simgWritesAospHeader();
    void simg2imgStreamMatchesOld();
    void img2simgStreamMatchesOld();
    void streamProgressCallback();
    void simg2imgStreamRejectsTruncated();
    void img2simgStreamEmptyInput();
    void systemImg2simgInterop();
    void systemSimg2imgInterop();
};

static QByteArray buildSparseHeader(quint32 totalBlks, quint32 totalChunks)
{
    // 严格按 AOSP sparse_format.h 布局：file_hdr_sz/chunk_hdr_sz 为 u16（偏移 8/10）
    QByteArray h(28, Qt::Uninitialized);
    auto put16 = [&](int off, quint16 v) { h[off] = char(v); h[off + 1] = char(v >> 8); };
    auto put32 = [&](int off, quint32 v) {
        h[off] = char(v); h[off + 1] = char(v >> 8);
        h[off + 2] = char(v >> 16); h[off + 3] = char(v >> 24);
    };
    put32(0, 0xED26FF3A);
    put16(4, 1); put16(6, 0);   // major 1, minor 0
    put16(8, 28);               // file header size (u16)
    put16(10, 12);              // chunk header size (u16)
    put32(12, 4096);            // block size
    put32(16, totalBlks);
    put32(20, totalChunks);
    return h;
}

static QByteArray buildRawChunk(const QByteArray &payload)
{
    QByteArray c(12, Qt::Uninitialized);
    auto put16 = [&](int off, quint16 v) { c[off] = char(v); c[off + 1] = char(v >> 8); };
    auto put32 = [&](int off, quint32 v) {
        c[off] = char(v); c[off + 1] = char(v >> 8); c[off + 2] = char(v >> 16); c[off + 3] = char(v >> 24);
    };
    put16(0, 0xCAC1);
    put16(2, 0);
    put32(4, static_cast<quint32>(payload.size() / 4096));
    put32(8, static_cast<quint32>(12 + payload.size()));
    c.append(payload);
    return c;
}

void TestSparse::detectSparse()
{
    QByteArray h = buildSparseHeader(0, 0);
    QVERIFY(imgsparse::isSparse(h));
    QVERIFY(!imgsparse::isSparse(QByteArray("ANDROID!")));
}

void TestSparse::simg2imgRawChunk()
{
    QByteArray payload(8192, '\xAB'); // 2 blocks
    QByteArray sparse = buildSparseHeader(2, 1) + buildRawChunk(payload);
    QCOMPARE(imgsparse::simg2img(sparse), payload);
}

void TestSparse::simg2imgFillAndDontcare()
{
    // 1 fill block + 1 dontcare block → 2 blocks raw: [0x11 * 4096][0x00 * 4096]
    QByteArray sparse = buildSparseHeader(2, 2);
    QByteArray fill(12, Qt::Uninitialized);
    auto put16 = [](QByteArray &d, int off, quint16 v) { d[off] = char(v); d[off + 1] = char(v >> 8); };
    auto put32 = [](QByteArray &d, int off, quint32 v) {
        d[off] = char(v); d[off + 1] = char(v >> 8); d[off + 2] = char(v >> 16); d[off + 3] = char(v >> 24);
    };
    put16(fill, 0, 0xCAC2); put16(fill, 2, 0); put32(fill, 4, 1); put32(fill, 8, 16);
    fill.append(QByteArray(4, '\x11'));
    QByteArray dc(12, Qt::Uninitialized);
    put16(dc, 0, 0xCAC3); put16(dc, 2, 0); put32(dc, 4, 1); put32(dc, 8, 12);
    sparse.append(fill).append(dc);
    QByteArray expect(4096, '\x11');
    expect.append(QByteArray(4096, '\x00'));
    QCOMPARE(imgsparse::simg2img(sparse), expect);
}

void TestSparse::img2simgRoundTrip()
{
    QByteArray raw;
    raw.append(QByteArray(4096 * 3, '\x42'));
    QByteArray sparse = imgsparse::img2simg(raw);
    QVERIFY(imgsparse::isSparse(sparse));
    QCOMPARE(imgsparse::simg2img(sparse), raw);
}

void TestSparse::img2simgRoundTripMixed()
{
    // 两块非全同内容（触发 RAW 分支而非 FILL）往返一致
    QByteArray raw(4096 * 2, '\x00');
    for (int i = 0; i < raw.size(); ++i)
        raw[i] = char((i * 31) & 0xFF);
    QByteArray sparse = imgsparse::img2simg(raw);
    QVERIFY(imgsparse::isSparse(sparse));
    QCOMPARE(imgsparse::simg2img(sparse), raw);
}

void TestSparse::img2simgPadsPartialBlock()
{
    // 4196 = 1 整块 + 100 字节：末块不足块大小 → img2simg 补零 pad，
    // simg2img 还原为 pad 后的结果（simg2img(img2simg(x)) == pad(x)）
    QByteArray raw(4196, '\x00');
    for (int i = 0; i < raw.size(); ++i)
        raw[i] = char(0x10 + (i % 7)); // 非全同，走 RAW 分支
    QByteArray sparse = imgsparse::img2simg(raw);
    QByteArray expect = raw + QByteArray(4096 - 100, '\x00');
    QCOMPARE(imgsparse::simg2img(sparse), expect);
}

void TestSparse::simg2imgRejectsUnknownChunk()
{
    // 未知 chunk 类型 0xBEEF → 拒绝返回空
    QByteArray sparse = buildSparseHeader(1, 1);
    QByteArray c(12, Qt::Uninitialized);
    auto put16 = [](QByteArray &d, int off, quint16 v) { d[off] = char(v); d[off + 1] = char(v >> 8); };
    auto put32 = [](QByteArray &d, int off, quint32 v) {
        d[off] = char(v); d[off + 1] = char(v >> 8); d[off + 2] = char(v >> 16); d[off + 3] = char(v >> 24);
    };
    put16(c, 0, 0xBEEF); put16(c, 2, 0); put32(c, 4, 1); put32(c, 8, 12);
    sparse.append(c);
    QVERIFY(imgsparse::simg2img(sparse).isEmpty());
}

void TestSparse::simg2imgRejectsTruncated()
{
    // RAW chunk 声明 2 块（8192 字节）但文件里只有 1 块数据 → 拒绝返回空
    QByteArray sparse = buildSparseHeader(2, 1);
    QByteArray c(12, Qt::Uninitialized);
    auto put16 = [](QByteArray &d, int off, quint16 v) { d[off] = char(v); d[off + 1] = char(v >> 8); };
    auto put32 = [](QByteArray &d, int off, quint32 v) {
        d[off] = char(v); d[off + 1] = char(v >> 8); d[off + 2] = char(v >> 16); d[off + 3] = char(v >> 24);
    };
    put16(c, 0, 0xCAC1); put16(c, 2, 0); put32(c, 4, 2); put32(c, 8, 12 + 8192);
    sparse.append(c).append(QByteArray(4096, '\x55'));
    QVERIFY(imgsparse::simg2img(sparse).isEmpty());
}

void TestSparse::simg2imgRejectsZeroTotalSz()
{
    // DONTCARE chunk totalSz=0 < chunkHdrSz：前置校验拒绝，防止 pos 不推进导致自旋
    QByteArray sparse = buildSparseHeader(1, 1);
    QByteArray c(12, Qt::Uninitialized);
    auto put16 = [](QByteArray &d, int off, quint16 v) { d[off] = char(v); d[off + 1] = char(v >> 8); };
    auto put32 = [](QByteArray &d, int off, quint32 v) {
        d[off] = char(v); d[off + 1] = char(v >> 8); d[off + 2] = char(v >> 16); d[off + 3] = char(v >> 24);
    };
    put16(c, 0, 0xCAC3); put16(c, 2, 0); put32(c, 4, 1); put32(c, 8, 0);
    sparse.append(c);
    QVERIFY(imgsparse::simg2img(sparse).isEmpty());
}

void TestSparse::simg2imgFillPattern()
{
    // 非 uniform 4 字节 pattern [0x11,0x22,0x33,0x44] 按 pattern[i % 4] 重复填充整块
    QByteArray sparse = buildSparseHeader(1, 1);
    QByteArray c(12, Qt::Uninitialized);
    auto put16 = [](QByteArray &d, int off, quint16 v) { d[off] = char(v); d[off + 1] = char(v >> 8); };
    auto put32 = [](QByteArray &d, int off, quint32 v) {
        d[off] = char(v); d[off + 1] = char(v >> 8); d[off + 2] = char(v >> 16); d[off + 3] = char(v >> 24);
    };
    put16(c, 0, 0xCAC2); put16(c, 2, 0); put32(c, 4, 1); put32(c, 8, 16);
    c.append(QByteArray("\x11\x22\x33\x44", 4));
    sparse.append(c);
    const QByteArray pattern("\x11\x22\x33\x44", 4);
    QByteArray expect(4096, Qt::Uninitialized);
    for (int i = 0; i < expect.size(); ++i)
        expect[i] = pattern[i % 4];
    QCOMPARE(imgsparse::simg2img(sparse), expect);
}

void TestSparse::aospHeaderU16Layout()
{
    // G0 发现的真实 bug 回归：file_hdr_sz/chunk_hdr_sz 是 u16（偏移 8/10）。
    // 用字面字节构造 AOSP 标准头（偏移 8-11 == 1c 00 0c 00），按 u16 解析必须成功；
    // 若被误读为 u32（偏移 8/12），fileHdrSz=786460、blkSz=4096 会被读成 totalBlks，
    // chunk 起点错位 → 必然失败/输出错误。
    QByteArray h(28, 0);
    auto put32 = [&](int off, quint32 v) {
        h[off] = char(v); h[off + 1] = char(v >> 8);
        h[off + 2] = char(v >> 16); h[off + 3] = char(v >> 24);
    };
    put32(0, 0xED26FF3A);
    h[4] = 1; h[5] = 0;    // major 1 (u16)
    h[6] = 0; h[7] = 0;    // minor 0 (u16)
    h[8] = 28; h[9] = 0;   // file_hdr_sz = 28 (u16) → 字节 1c 00
    h[10] = 12; h[11] = 0; // chunk_hdr_sz = 12 (u16) → 字节 0c 00
    put32(12, 4096);       // blk_sz
    put32(16, 2);          // total_blks
    put32(20, 1);          // total_chunks
    QByteArray payload(8192, '\xAB'); // 2 blocks RAW
    QByteArray c(12, Qt::Uninitialized);
    c[0] = 0xC1; c[1] = 0xCA; // 0xCAC1 RAW (u16 LE)
    c[2] = 0; c[3] = 0;
    c[4] = 2; c[5] = 0; c[6] = 0; c[7] = 0;                 // chunk_sz = 2
    c[8] = char(12 + 8192); c[9] = char((12 + 8192) >> 8);
    c[10] = char((12 + 8192) >> 16); c[11] = char((12 + 8192) >> 24);
    QByteArray sparse = h + c + payload;
    QCOMPARE(imgsparse::simg2img(sparse), payload);
}

void TestSparse::img2simgWritesAospHeader()
{
    // img2simg 产物头必须为 AOSP u16 布局，否则系统 simg2img 读 chunk_hdr_sz(u16@10)=0 而拒绝
    QByteArray raw(4096 * 2, '\x42');
    QByteArray s = imgsparse::img2simg(raw);
    QVERIFY(s.size() >= 44);
    // file_hdr_sz = 28 u16 @8, chunk_hdr_sz = 12 u16 @10 → 字节 1c 00 0c 00
    QCOMPARE(quint8(s[8]), quint8(28));
    QCOMPARE(quint8(s[9]), quint8(0));
    QCOMPARE(quint8(s[10]), quint8(12));
    QCOMPARE(quint8(s[11]), quint8(0));
    // major/minor (u16@4/6)
    QCOMPARE(quint8(s[4]), quint8(1));
    QCOMPARE(quint8(s[5]), quint8(0));
    // blk_sz@12 / total_blks@16 / total_chunks@20
    QCOMPARE(qFromLittleEndian<quint32>(s.constData() + 12), 4096u);
    QCOMPARE(qFromLittleEndian<quint32>(s.constData() + 16), 2u);
    QCOMPARE(qFromLittleEndian<quint32>(s.constData() + 20), 1u);
    // 首个 chunk 头：type u16@0=0xCAC2(FILL), chunk_sz u32@4=2, total_sz u32@8=16
    QCOMPARE(qFromLittleEndian<quint16>(s.constData() + 28), quint16(0xCAC2));
    QCOMPARE(qFromLittleEndian<quint32>(s.constData() + 32), 2u);
    QCOMPARE(qFromLittleEndian<quint32>(s.constData() + 36), 16u);
}

static bool writeFile(const QString &path, const QByteArray &d)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    return f.write(d) == d.size();
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

void TestSparse::simg2imgStreamMatchesOld()
{
    // 流式输出必须与旧接口逐字节一致（含 FILL/DONTCARE/RAW 混合、非 uniform pattern）
    QTemporaryDir dir;
    const QString inPath = dir.filePath("in.sparse"), outPath = dir.filePath("out.img");

    // 用例 1：2 块 RAW
    {
        QByteArray payload(8192, '\xAB');
        QByteArray sparse = buildSparseHeader(2, 1) + buildRawChunk(payload);
        QVERIFY(writeFile(inPath, sparse));
        QString err;
        QVERIFY(imgsparse::simg2imgStream(inPath, outPath, {}, &err));
        QCOMPARE(readFile(outPath), payload);
        QCOMPARE(readFile(outPath), imgsparse::simg2img(sparse));
    }
    // 用例 2：FILL + DONTCARE
    {
        QByteArray sparse = buildSparseHeader(2, 2);
        QByteArray fill(12, Qt::Uninitialized);
        auto put16 = [](QByteArray &d, int off, quint16 v) { d[off] = char(v); d[off + 1] = char(v >> 8); };
        auto put32 = [](QByteArray &d, int off, quint32 v) {
            d[off] = char(v); d[off + 1] = char(v >> 8); d[off + 2] = char(v >> 16); d[off + 3] = char(v >> 24);
        };
        put16(fill, 0, 0xCAC2); put16(fill, 2, 0); put32(fill, 4, 1); put32(fill, 8, 16);
        fill.append(QByteArray(4, '\x11'));
        QByteArray dc(12, Qt::Uninitialized);
        put16(dc, 0, 0xCAC3); put16(dc, 2, 0); put32(dc, 4, 1); put32(dc, 8, 12);
        sparse.append(fill).append(dc);
        QVERIFY(writeFile(inPath, sparse));
        QString err;
        QVERIFY(imgsparse::simg2imgStream(inPath, outPath, {}, &err));
        QCOMPARE(readFile(outPath), imgsparse::simg2img(sparse));
    }
    // 用例 3：非 uniform 4 字节 FILL pattern
    {
        QByteArray sparse = buildSparseHeader(1, 1);
        QByteArray c(12, Qt::Uninitialized);
        auto put16 = [](QByteArray &d, int off, quint16 v) { d[off] = char(v); d[off + 1] = char(v >> 8); };
        auto put32 = [](QByteArray &d, int off, quint32 v) {
            d[off] = char(v); d[off + 1] = char(v >> 8); d[off + 2] = char(v >> 16); d[off + 3] = char(v >> 24);
        };
        put16(c, 0, 0xCAC2); put16(c, 2, 0); put32(c, 4, 1); put32(c, 8, 16);
        c.append(QByteArray("\x11\x22\x33\x44", 4));
        sparse.append(c);
        QVERIFY(writeFile(inPath, sparse));
        QString err;
        QVERIFY(imgsparse::simg2imgStream(inPath, outPath, {}, &err));
        QCOMPARE(readFile(outPath), imgsparse::simg2img(sparse));
    }
}

void TestSparse::img2simgStreamMatchesOld()
{
    QTemporaryDir dir;
    const QString inPath = dir.filePath("in.raw"), outPath = dir.filePath("out.sparse");

    // 用例 1：全同块（FILL）
    QByteArray raw1(4096 * 3, '\x42');
    QVERIFY(writeFile(inPath, raw1));
    QString err;
    QVERIFY(imgsparse::img2simgStream(inPath, outPath, 4096, {}, &err));
    QCOMPARE(readFile(outPath), imgsparse::img2simg(raw1));
    // 用例 2：伪随机 + FILL 混排（RAW/FILL 边界）
    QByteArray raw2(4096 * 16, '\x00');
    for (int i = 0; i < raw2.size(); ++i)
        raw2[i] = char((i * 31 + (i / 4096) * 7) & 0xFF);
    for (int b = 3; b < 6; ++b) raw2.replace(b * 4096, 4096, QByteArray(4096, '\x77'));
    QVERIFY(writeFile(inPath, raw2));
    QVERIFY(imgsparse::img2simgStream(inPath, outPath, 4096, {}, &err));
    QCOMPARE(readFile(outPath), imgsparse::img2simg(raw2));
    // 用例 3：末块不足整块（pad 语义一致）
    QByteArray raw3(4196, '\x00');
    for (int i = 0; i < raw3.size(); ++i)
        raw3[i] = char(0x10 + (i % 7));
    QVERIFY(writeFile(inPath, raw3));
    QVERIFY(imgsparse::img2simgStream(inPath, outPath, 4096, {}, &err));
    QCOMPARE(readFile(outPath), imgsparse::img2simg(raw3));
}

void TestSparse::streamProgressCallback()
{
    QTemporaryDir dir;
    const QString inPath = dir.filePath("in.raw"), outPath = dir.filePath("out.sparse");
    QByteArray raw(4096 * 4, '\x00');
    for (int i = 0; i < raw.size(); ++i)
        raw[i] = char((i * 13) & 0xFF);
    raw.replace(0, 4096, QByteArray(4096, '\xEE')); // 开头一段 FILL
    QVERIFY(writeFile(inPath, raw));
    QList<quint64> calls;
    QString err;
    QVERIFY(imgsparse::img2simgStream(inPath, outPath, 4096,
                                      [&](quint64 b) { calls.append(b); }, &err));
    QVERIFY(calls.size() >= 2);
    for (int i = 1; i < calls.size(); ++i)
        QVERIFY(calls[i] >= calls[i - 1]); // 单调不减
    QCOMPARE(calls.last(), quint64(raw.size()));
    QCOMPARE(calls.first(), quint64(0));
}

void TestSparse::simg2imgStreamRejectsTruncated()
{
    QTemporaryDir dir;
    const QString inPath = dir.filePath("trunc.sparse"), outPath = dir.filePath("out.img");
    QByteArray sparse = buildSparseHeader(2, 1);
    QByteArray c(12, Qt::Uninitialized);
    auto put16 = [](QByteArray &d, int off, quint16 v) { d[off] = char(v); d[off + 1] = char(v >> 8); };
    auto put32 = [](QByteArray &d, int off, quint32 v) {
        d[off] = char(v); d[off + 1] = char(v >> 8); d[off + 2] = char(v >> 16); d[off + 3] = char(v >> 24);
    };
    put16(c, 0, 0xCAC1); put16(c, 2, 0); put32(c, 4, 2); put32(c, 8, 12 + 8192);
    sparse.append(c).append(QByteArray(4096, '\x55')); // 只有 1 块数据，声明 2 块
    QVERIFY(writeFile(inPath, sparse));
    QString err;
    QVERIFY(!imgsparse::simg2imgStream(inPath, outPath, {}, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(!QFile::exists(outPath)); // 失败时输出文件被清理
    // 非 sparse 输入
    QVERIFY(writeFile(inPath, QByteArray("ANDROID!")));
    QVERIFY(!imgsparse::simg2imgStream(inPath, outPath, {}, &err));
    QVERIFY(!err.isEmpty());
}

void TestSparse::img2simgStreamEmptyInput()
{
    QTemporaryDir dir;
    const QString inPath = dir.filePath("empty.raw"), outPath = dir.filePath("out.sparse");
    QVERIFY(writeFile(inPath, QByteArray()));
    QString err;
    QVERIFY(!imgsparse::img2simgStream(inPath, outPath, 4096, {}, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(!QFile::exists(outPath));
}

void TestSparse::systemImg2simgInterop()
{
    if (!QFile::exists(QStringLiteral("/usr/bin/img2simg")))
        QSKIP("系统 img2simg 不存在，跳过 AOSP 互操作实测");
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("raw.bin"), sparsePath = dir.filePath("sys.sparse"),
                  outPath = dir.filePath("out.img");
    // 5.0MB 伪随机 + 100KB 全同 + 尾部 123456 字节（末块不足）
    QByteArray raw;
    for (int b = 0; b < 1250; ++b) {
        QByteArray blk(4096, '\x00');
        if (b % 16 == 0) blk.fill(char('A' + (b % 8))); // 1/16 全同块
        else for (int i = 0; i < blk.size(); ++i) blk[i] = char((b * 131 + i * 7) & 0xFF);
        raw.append(blk);
    }
    raw.append(QByteArray(123456, '\x00'));
    for (int i = 0; i < 123456; ++i) raw[raw.size() - 123456 + i] = char((i * 11) & 0xFF);
    QVERIFY(writeFile(rawPath, raw));
    QProcess p;
    p.start(QStringLiteral("/usr/bin/img2simg"), { rawPath, sparsePath });
    QVERIFY(p.waitForFinished(60000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0);
    // AOSP 工具产物（u16 头字段）必须能被流式接口正确解析，且逐字节还原（末块补零 pad 一致）
    QString err;
    QVERIFY2(imgsparse::simg2imgStream(sparsePath, outPath, {}, &err), qPrintable(err));
    QByteArray expect = raw + QByteArray((-raw.size()) & (4096 - 1), '\0');
    QCOMPARE(readFile(outPath), expect);
}

void TestSparse::systemSimg2imgInterop()
{
    if (!QFile::exists(QStringLiteral("/usr/bin/simg2img")))
        QSKIP("系统 simg2img 不存在，跳过 AOSP 互操作实测");
    QTemporaryDir dir;
    const QString rawPath = dir.filePath("raw.bin"), sparsePath = dir.filePath("ours.sparse"),
                  outPath = dir.filePath("out.img");
    QByteArray raw(4096 * 8, '\x00');
    for (int i = 0; i < raw.size(); ++i)
        raw[i] = char((i * 23 + (i / 4096) * 5) & 0xFF);
    raw.replace(2 * 4096, 4096, QByteArray(4096, '\x00')); // 注意：0x00 块是合法的 FILL
    raw.replace(4 * 4096, 2 * 4096, QByteArray(2 * 4096, '\x33'));
    QVERIFY(writeFile(rawPath, raw));
    // 库产物必须能被系统 simg2img（AOSP libsparse）接受并逐字节还原
    QString err;
    QVERIFY2(imgsparse::img2simgStream(rawPath, sparsePath, 4096, {}, &err), qPrintable(err));
    QProcess p;
    p.start(QStringLiteral("/usr/bin/simg2img"), { sparsePath, outPath });
    QVERIFY(p.waitForFinished(60000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0);
    QCOMPARE(readFile(outPath), raw);
}

QTEST_APPLESS_MAIN(TestSparse)
#include "test_sparse.moc"
