#include <QtTest>
#include "image_engine/sparse_image.h"

class TestSparse : public QObject
{
    Q_OBJECT
private slots:
    void detectSparse();
    void simg2imgRawChunk();
    void simg2imgFillAndDontcare();
    void img2simgRoundTrip();
};

static QByteArray buildSparseHeader(quint32 totalBlks, quint32 totalChunks)
{
    QByteArray h(28, Qt::Uninitialized);
    auto put32 = [&](int off, quint32 v) {
        h[off] = char(v); h[off + 1] = char(v >> 8);
        h[off + 2] = char(v >> 16); h[off + 3] = char(v >> 24);
    };
    put32(0, 0xED26FF3A);
    h[4] = 1; h[5] = 0;   // major 1
    h[6] = 0; h[7] = 0;   // minor 0
    put32(8, 28);         // file header size
    put32(12, 12);        // chunk header size
    put32(16, 4096);      // block size
    put32(20, totalBlks);
    put32(24, totalChunks);
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

QTEST_APPLESS_MAIN(TestSparse)
#include "test_sparse.moc"
