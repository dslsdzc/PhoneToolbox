#include <QtTest>
#include <QByteArray>
#include <QList>
#include <QString>
#include <zlib.h>
#include "image_engine/kdz_image.h"

// 构造工具：按 IOMonster kdztools (unkdz.py/undz.py + libexec/kdz.py/dz.py, mkkdz.py) 的
// 精确字节布局构造最小 KDZ v3 / DZ，供真实布局解析测试使用。
namespace {

void putU32(QByteArray &b, int off, quint32 v)
{
    for (int i = 0; i < 4; ++i) b[off + i] = char((v >> (i * 8)) & 0xFF);
}
void putU64(QByteArray &b, int off, quint64 v)
{
    for (int i = 0; i < 8; ++i) b[off + i] = char((v >> (i * 8)) & 0xFF);
}
QByteArray padStr(const QByteArray &s, int width) { return s.leftJustified(width, '\0'); }

QByteArray zlibCompress(const QByteArray &raw)
{
    if (raw.isEmpty()) return {};
    uLongf cap = compressBound(static_cast<uLong>(raw.size()));
    QByteArray out(static_cast<int>(cap), Qt::Uninitialized);
    if (compress2(reinterpret_cast<Bytef *>(out.data()), &cap,
                  reinterpret_cast<const Bytef *>(raw.constData()),
                  static_cast<uLong>(raw.size()), 9) != Z_OK)
        return {};
    out.truncate(static_cast<int>(cap));
    return out;
}

// ---- KDZ v3 构造（参考 mkkdz.py cmdCreateFile 布局）----
// 记录: name[256] + length u64 LE + offset u64 LE (272B); 最后一个记录前写 0x03。
QByteArray kdzRecord(const QByteArray &name, quint64 length, quint64 offset)
{
    QByteArray rec(272, 0);
    rec.replace(0, 256, padStr(name, 256));
    putU64(rec, 256, length);
    putU64(rec, 264, offset);
    return rec;
}
// magic(8B) + 记录表 + 对齐填充 + 数据（记录 offset 指向各自数据）
// 0x03 标记: unkdz.py 读取约定是"下一条记录为最后一条"，故仅当记录数 >= 2 时
// 写于最后一个记录前（mkkdz.py 单记录输出因该标记出现在偏移 8 而无法被 unkdz 读回，
// 属参考工具自身的单记录缺陷；真实文件遵循读取约定：魔数后即第一条记录）。
QByteArray buildKdz(const QList<QByteArray> &names, const QList<QByteArray> &payloads)
{
    QByteArray head(QByteArray::fromHex("2805000024382225"));
    qsizetype headerSize = head.size();
    for (int i = 0; i < names.size(); ++i) {
        if (names.size() >= 2 && i == names.size() - 1) ++headerSize; // 0x03 标记
        headerSize += 272;
    }
    const qsizetype dataStart = (headerSize + 511) & ~qsizetype(511);
    QByteArray out = head;
    quint64 cur = static_cast<quint64>(dataStart);
    for (int i = 0; i < names.size(); ++i) {
        if (names.size() >= 2 && i == names.size() - 1) out.append(char(0x03));
        out.append(kdzRecord(names[i], static_cast<quint64>(payloads[i].size()), cur));
        cur += static_cast<quint64>(payloads[i].size());
    }
    out.resize(dataStart);
    for (const QByteArray &p : payloads) out.append(p);
    return out;
}

// ---- DZ 构造（参考 undz.py + libexec/dz.py 布局）----
// 主头 512B: magic@0, formatMajor u32@4, formatMinor u32@8, chunkCount u32@192
QByteArray dzHeader(quint32 chunkCount)
{
    QByteArray h(512, 0);
    h.replace(0, 4, QByteArray::fromHex("32961874"));
    putU32(h, 4, 2);
    putU32(h, 8, 1);
    putU32(h, 192, chunkCount);
    return h;
}
// chunk 子头 512B: magic@0, sliceName[32]@4, chunkName[64]@36,
// targetSize u32@100, dataSize u32@104, targetAddr u32@124 (eMMC 块号, 偏移 = 块号<<9)
QByteArray dzChunkHeader(const QByteArray &slice, const QByteArray &chunk,
                         quint32 targetSize, quint32 dataSize, quint32 targetAddr)
{
    QByteArray h(512, 0);
    h.replace(0, 4, QByteArray::fromHex("30129578"));
    h.replace(4, 32, padStr(slice, 32));
    h.replace(36, 64, padStr(chunk, 64));
    putU32(h, 100, targetSize);
    putU32(h, 104, dataSize);
    putU32(h, 124, targetAddr);
    return h;
}

} // namespace

class TestKdz : public QObject
{
    Q_OBJECT
private slots:
    void mergeChunks();
    void mergeChunksOverLimit();
    void parseKdz();
    void parseKdzMkkdzSingle();
    void parseKdzMulti();
    void parseKdzInvalid();
    void parseDz();
    void parseDzInvalid();
};

// 两个 chunk: boot 偏移 100 数据 "AAA", system 偏移 200 数据 "BBB"
// merge 结果: [0..99]=0, [100..102]=AAA, [103..199]=0, [200..202]=BBB
void TestKdz::mergeChunks()
{
    QList<imgkdz::DzChunk> chunks;
    imgkdz::DzChunk c1; c1.partition = "boot"; c1.offset = 100; c1.data = "AAA";
    chunks.append(c1);
    imgkdz::DzChunk c2; c2.partition = "system"; c2.offset = 200; c2.data = "BBB";
    chunks.append(c2);
    QString err;
    QByteArray merged = imgkdz::mergeChunks(chunks, &err);
    QCOMPARE(merged.size(), 203);
    QCOMPARE(merged.mid(100, 3), QByteArray("AAA"));
    QCOMPARE(merged.mid(200, 3), QByteArray("BBB"));
    QCOMPARE(merged.left(100), QByteArray(100, '\0'));
}

void TestKdz::mergeChunksOverLimit()
{
    // 上限防呆: maxEnd 超过 2^33 (8GB) 必须失败返回空, 不得尝试分配
    QList<imgkdz::DzChunk> chunks;
    imgkdz::DzChunk c;
    c.partition = "system";
    c.offset = (1ULL << 33) - 1;
    c.data = QByteArray(2, '\x01'); // maxEnd = 2^33 + 1
    chunks.append(c);
    QString err;
    QByteArray merged = imgkdz::mergeChunks(chunks, &err);
    QVERIFY(merged.isEmpty());
    QVERIFY(err.contains("过大"));
}

// 真实布局: 8B 魔数 28 05 00 00 24 38 22 25 + 1 条 272B 记录 (name[256]+length+offset)
// + 对齐填充 + 数据
void TestKdz::parseKdz()
{
    const QByteArray payload = QByteArray("DZ-data-payload").repeated(3);
    QByteArray kdz = buildKdz({QByteArray("firmware.dz")}, {payload});
    QList<imgkdz::DzFile> files;
    QString err;
    QVERIFY2(imgkdz::parseKdz(kdz, files, &err), qPrintable(err));
    QCOMPARE(files.size(), 1);
    QCOMPARE(files[0].name, QString("firmware.dz"));
    QCOMPARE(files[0].data, payload);
}

// mkkdz.py 单记录缺陷布局: 0x03 标记被写在魔数后偏移 8（该工具自身也无法读回,
// 真实文件不会如此; 我们的解析器兼容跳过后仍能正确解析）
void TestKdz::parseKdzMkkdzSingle()
{
    const QByteArray payload(8, '\x33');
    QByteArray kdz(QByteArray::fromHex("2805000024382225"));
    kdz.append(char(0x03));
    kdz.append(kdzRecord(QByteArray("f.bin"), 8, 300));
    kdz.resize(300);
    kdz.append(payload);
    QList<imgkdz::DzFile> files;
    QString err;
    QVERIFY2(imgkdz::parseKdz(kdz, files, &err), qPrintable(err));
    QCOMPARE(files.size(), 1);
    QCOMPARE(files[0].name, QString("f.bin"));
    QCOMPARE(files[0].data, payload);
}

// 双记录: 最后一个记录前有 0x03 标记（mkkdz.py 写文件布局）
void TestKdz::parseKdzMulti()
{
    const QByteArray p1 = QByteArray(16, '\x11');
    const QByteArray p2 = QByteArray(24, '\x22');
    QByteArray kdz = buildKdz({QByteArray("a.bin"), QByteArray("b.dz")}, {p1, p2});
    QList<imgkdz::DzFile> files;
    QString err;
    QVERIFY2(imgkdz::parseKdz(kdz, files, &err), qPrintable(err));
    QCOMPARE(files.size(), 2);
    QCOMPARE(files[0].name, QString("a.bin"));
    QCOMPARE(files[0].data, p1);
    QCOMPARE(files[1].name, QString("b.dz"));
    QCOMPARE(files[1].data, p2);
}

void TestKdz::parseKdzInvalid()
{
    QString err;
    QList<imgkdz::DzFile> files;
    // 错误魔数
    QByteArray bad1 = QByteArray::fromHex("2805000024382226") + QByteArray(272, 0);
    QVERIFY(!imgkdz::parseKdz(bad1, files, &err));
    // 长度不足 (无完整记录)
    QVERIFY(!imgkdz::parseKdz(QByteArray(200, 0), files, &err));
    // 记录 offset 越界
    QByteArray kdz2(QByteArray::fromHex("2805000024382225"));
    QByteArray rec2(272, 0);
    rec2.replace(0, 256, padStr(QByteArray("f.bin"), 256));
    putU64(rec2, 256, 4);
    putU64(rec2, 264, 100000); // 文件远小于 100000+4
    kdz2.append(rec2);
    QVERIFY(!imgkdz::parseKdz(kdz2, files, &err));
    // 记录名内部含 NUL (参考工具同样拒绝)
    QByteArray kdz3(QByteArray::fromHex("2805000024382225"));
    QByteArray rec3(272, 0);
    QByteArray nm("a\0b", 3);
    rec3.replace(0, 256, nm.leftJustified(256, '\0'));
    putU64(rec3, 256, 1);
    putU64(rec3, 264, 300);
    kdz3.append(rec3);
    kdz3.resize(600);
    QVERIFY(!imgkdz::parseKdz(kdz3, files, &err));
}

// 真实布局: 512B 主头 + (512B chunk 子头 + zlib 压缩数据) * N;
// chunk eMMC 偏移 = targetAddr << 9 (512 字节扇区)
void TestKdz::parseDz()
{
    const QByteArray d1 = QByteArray(4096, '\xAB');
    const QByteArray d2 = QByteArray(2048, '\xCD');
    const QByteArray z1 = zlibCompress(d1);
    const QByteArray z2 = zlibCompress(d2);
    QByteArray dz = dzHeader(2);
    dz.append(dzChunkHeader(QByteArray("system"), QByteArray("system_100.bin"),
                            static_cast<quint32>(d1.size()), static_cast<quint32>(z1.size()), 100));
    dz.append(z1);
    dz.append(dzChunkHeader(QByteArray("boot"), QByteArray("boot_200.bin"),
                            static_cast<quint32>(d2.size()), static_cast<quint32>(z2.size()), 200));
    dz.append(z2);

    QList<imgkdz::DzChunk> chunks;
    QString err;
    QVERIFY2(imgkdz::parseDz(dz, chunks, &err), qPrintable(err));
    QCOMPARE(chunks.size(), 2);
    QCOMPARE(chunks[0].partition, QString("system"));
    QCOMPARE(chunks[0].offset, quint64(100) << 9);
    QCOMPARE(chunks[0].data, d1);
    QCOMPARE(chunks[1].partition, QString("boot"));
    QCOMPARE(chunks[1].offset, quint64(200) << 9);
    QCOMPARE(chunks[1].data, d2);
}

void TestKdz::parseDzInvalid()
{
    QString err;
    QList<imgkdz::DzChunk> chunks;
    // 错误主魔数
    QByteArray bad1 = dzHeader(1);
    bad1[0] = 0x00;
    QVERIFY(!imgkdz::parseDz(bad1, chunks, &err));
    // 长度不足
    QVERIFY(!imgkdz::parseDz(QByteArray(100, 0), chunks, &err));
    // chunk 子头魔数错误
    QByteArray dz2 = dzHeader(1);
    dz2.append(QByteArray(512, 0)); // 全零子头 → 魔数不符
    QVERIFY(!imgkdz::parseDz(dz2, chunks, &err));
    // 数据越界: dataSize 超文件尾
    QByteArray dz3 = dzHeader(1);
    dz3.append(dzChunkHeader(QByteArray("x"), QByteArray("x_1.bin"), 8, 100, 1));
    dz3.append(QByteArray(10, 0)); // 声明 100B 数据, 实际只有 10B
    QVERIFY(!imgkdz::parseDz(dz3, chunks, &err));
    // 数据不是合法 zlib 流 → 解压失败
    QByteArray dz4 = dzHeader(1);
    dz4.append(dzChunkHeader(QByteArray("x"), QByteArray("x_1.bin"), 8, 8, 1));
    dz4.append(QByteArray("notzlib!", 8));
    QVERIFY(!imgkdz::parseDz(dz4, chunks, &err));
}

QTEST_APPLESS_MAIN(TestKdz)
#include "test_kdz.moc"
