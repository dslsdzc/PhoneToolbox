#include <QtTest>
#include "image_engine/wire_format.h"
#include "image_engine/payload_image.h"
#include "image_engine/bspatch_image.h"
#include "image_engine/compression/bzip2_wrapper.h"

class TestWire : public QObject
{
    Q_OBJECT
private slots:
    void parseVarint();
    void parseLengthDelimited();
    void encodeRoundTrip();
    void parseInvalid();            // Task 13: wire 层负向用例（Task 12 审查遗留补上）
};

void TestWire::parseVarint()
{
    // field 1 varint value 300 → 0x08 0xAC 0x02
    QByteArray d("\x08\xAC\x02", 3);
    bool ok = false;
    QList<pbwire::Field> fields = pbwire::parseMessage(d, ok);
    QVERIFY(ok);
    QCOMPARE(fields.size(), 1);
    QCOMPARE(fields[0].number, 1);
    QCOMPARE(fields[0].varint, 300ull);
}

void TestWire::parseLengthDelimited()
{
    // field 3 bytes "hi" → 0x1A 0x02 'h' 'i'
    QByteArray d("\x1A\x02hi", 4);
    bool ok = false;
    QList<pbwire::Field> fields = pbwire::parseMessage(d, ok);
    QVERIFY(ok);
    QCOMPARE(fields[0].number, 3);
    QCOMPARE(fields[0].bytes, QByteArray("hi"));
}

void TestWire::encodeRoundTrip()
{
    QByteArray msg = pbwire::encodeVarint(1, 300);
    QByteArray nested = pbwire::encodeBytes(3, QByteArray("hi"));
    QByteArray combined = msg + nested;
    bool ok = false;
    QList<pbwire::Field> fields = pbwire::parseMessage(combined, ok);
    QVERIFY(ok);
    QCOMPARE(fields.size(), 2);
    QCOMPARE(fields[0].varint, 300ull);
    QCOMPARE(fields[1].bytes, QByteArray("hi"));
}

void TestWire::parseInvalid()
{
    // 非法 wire 数据 → ok=false，不得崩溃
    bool ok = true;
    // varint 截断（0x80 无终止字节）
    QVERIFY(pbwire::parseMessage(QByteArray("\x08\x80", 2), ok).isEmpty());
    QVERIFY(!ok);
    // varint 超过 10 字节上限
    ok = true;
    QVERIFY(pbwire::parseMessage(QByteArray("\x08", 1) + QByteArray(11, '\x80'), ok).isEmpty());
    QVERIFY(!ok);
    // 长度字段超界
    ok = true;
    QVERIFY(pbwire::parseMessage(QByteArray("\x1A\x05hi", 4), ok).isEmpty());
    QVERIFY(!ok);
    // field number == 0
    ok = true;
    QVERIFY(pbwire::parseMessage(QByteArray("\x00", 1), ok).isEmpty());
    QVERIFY(!ok);
    // 未知 wireType (tag & 7 == 7)
    ok = true;
    QVERIFY(pbwire::parseMessage(QByteArray("\x0F", 1), ok).isEmpty());
    QVERIFY(!ok);
    // 64-bit 字段越界（仅 1 字节）
    ok = true;
    QVERIFY(pbwire::parseMessage(QByteArray("\x09", 1), ok).isEmpty());
    QVERIFY(!ok);
    // 32-bit 字段越界（仅 1 字节）
    ok = true;
    QVERIFY(pbwire::parseMessage(QByteArray("\x0D", 1), ok).isEmpty());
    QVERIFY(!ok);
}

// ---------- Task 13: payload manifest 解析 ----------

class TestPayload : public QObject
{
    Q_OBJECT
private slots:
    void detect();
    void parseMinimalManifest();
    void parseInvalid();
    void parseV1();
    void parseUnknownFields();
    void parseOpFields();
    void extractReplaceOp();   // Task 14: 全量 REPLACE 解包
    void extractSourceCopy();  // Task 15: SOURCE_COPY（src_extents → dst_extents）
    void extractSourceBsdiff(); // Task 15: SOURCE_BSDIFF（src_extents 旧片段 + bspatch）
};

// 小端 64 位写入
static void putU64(QByteArray &d, quint64 v)
{
    for (int i = 0; i < 8; ++i)
        d.append(char((v >> (i * 8)) & 0xFF));
}

static QByteArray buildMinimalPayload()
{
    // InstallOperation: type=1(REPLACE), data_offset=2, data_length=3
    QByteArray op = pbwire::encodeVarint(1, 0) + pbwire::encodeVarint(2, 0)
                  + pbwire::encodeVarint(3, 8);
    // PartitionUpdate: partition_name=1("boot"), operations=8(op)
    QByteArray part = pbwire::encodeBytes(1, QByteArray("boot")) + pbwire::encodeMessage(8, op);
    // Manifest: block_size=3(4096), partitions=13(part)
    QByteArray manifest = pbwire::encodeVarint(3, 4096) + pbwire::encodeMessage(13, part);
    QByteArray payload;
    payload.append("CrAU");
    auto put64 = [&](quint64 v) {
        for (int i = 0; i < 8; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put64(2);                      // version
    put64(static_cast<quint64>(manifest.size())); // manifest_size
    auto put32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put32(0);                      // metadata_signature_size
    payload.append(manifest);
    payload.append(QByteArray(8, '\xAB')); // blob
    return payload;
}

// v1: 无 metadata_signature_size，manifest 数据起始 = 20
static QByteArray buildV1Payload()
{
    QByteArray manifest = pbwire::encodeVarint(3, 8192);
    QByteArray payload;
    payload.append("CrAU");
    auto put64 = [&](quint64 v) {
        for (int i = 0; i < 8; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put64(1);                      // version = 1
    put64(static_cast<quint64>(manifest.size()));
    payload.append(manifest);
    return payload;
}

// manifest/partition/op 三层各插入未知字段 → 按 protobuf 语义跳过
static QByteArray buildPayloadWithUnknownFields()
{
    QByteArray op = pbwire::encodeVarint(1, 0) + pbwire::encodeVarint(2, 0)
                  + pbwire::encodeVarint(3, 8)
                  + pbwire::encodeVarint(99, 12345);            // 未知字段
    QByteArray part = pbwire::encodeBytes(1, QByteArray("boot"))
                    + pbwire::encodeMessage(8, op)
                    + pbwire::encodeVarint(99, 1);              // 未知字段
    QByteArray manifest = pbwire::encodeVarint(3, 4096)
                        + pbwire::encodeMessage(13, part)
                        + pbwire::encodeVarint(99, 1);          // 未知字段
    QByteArray payload;
    payload.append("CrAU");
    auto put64 = [&](quint64 v) {
        for (int i = 0; i < 8; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put64(2);
    put64(static_cast<quint64>(manifest.size()));
    auto put32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put32(0);
    payload.append(manifest);
    return payload;
}

// manifest 内嵌套消息截断（头部尺寸自洽）→ false
static QByteArray buildTruncatedInnerPayload()
{
    QByteArray op = pbwire::encodeVarint(1, 0) + pbwire::encodeVarint(2, 0)
                  + pbwire::encodeVarint(3, 8);
    QByteArray badPart = pbwire::encodeBytes(1, QByteArray("boot"))
                       + pbwire::encodeMessage(8, op.left(3));  // ops 内容截断
    QByteArray manifest = pbwire::encodeVarint(3, 4096) + pbwire::encodeMessage(13, badPart);
    QByteArray payload;
    payload.append("CrAU");
    auto put64 = [&](quint64 v) {
        for (int i = 0; i < 8; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put64(2);
    put64(static_cast<quint64>(manifest.size()));
    auto put32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put32(0);
    payload.append(manifest);
    return payload;
}

void TestPayload::detect()
{
    QVERIFY(imgpayload::isPayload(QByteArray("CrAU")));
    QVERIFY(!imgpayload::isPayload(QByteArray("ANDROID!")));
}

void TestPayload::parseMinimalManifest()
{
    QByteArray p = buildMinimalPayload();
    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(p, info));
    QCOMPARE(info.blockSize, 4096ull);
    QCOMPARE(info.partitions.size(), 1);
    QCOMPARE(info.partitions[0].name, "boot");
    QCOMPARE(info.partitions[0].ops.size(), 1);
    QCOMPARE(info.partitions[0].ops[0].type, 0);
    QCOMPARE(info.partitions[0].ops[0].dataLength, 8ull);
}

void TestPayload::parseInvalid()
{
    imgpayload::PayloadInfo info;
    // 空 / 过短（< 20 字节）/ 坏魔数
    QVERIFY(!imgpayload::parseManifest(QByteArray(), info));
    QVERIFY(!imgpayload::parseManifest(QByteArray("CrAU"), info));
    QVERIFY(!imgpayload::parseManifest(QByteArray("ANDROID!") + QByteArray(16, 0), info));
    // manifest_size 超界（篡改为全 0xFF → 越界拒绝）
    QByteArray p = buildMinimalPayload();
    p.replace(12, 8, QByteArray(8, '\xFF'));
    QVERIFY(!imgpayload::parseManifest(p, info));
    // manifest 内部嵌套消息截断 → false
    QVERIFY(!imgpayload::parseManifest(buildTruncatedInnerPayload(), info));
    // v2 头在 metadata_signature_size 处截断（仅 20 字节, manifest_size=5）
    // → dataStart=24 > size, 负差不可再越过界检查（审查 Important 修复）
    QByteArray t;
    t.append("CrAU");
    auto put64 = [&](quint64 v) {
        for (int i = 0; i < 8; ++i) t.append(char((v >> (i * 8)) & 0xFF));
    };
    put64(2); // version
    put64(5); // manifest_size
    QVERIFY(!imgpayload::parseManifest(t, info));
}

void TestPayload::parseV1()
{
    // v1: 无 metadata_signature_size，manifest 数据起始 = 20
    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(buildV1Payload(), info));
    QCOMPARE(info.blockSize, 8192ull);
    QCOMPARE(info.partitions.size(), 0);
    QCOMPARE(info.manifestRaw, pbwire::encodeVarint(3, 8192));
}

void TestPayload::parseUnknownFields()
{
    // 未知字段（protobuf 语义: 跳过）→ 解析成功且已知字段不受影响
    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(buildPayloadWithUnknownFields(), info));
    QCOMPARE(info.blockSize, 4096ull);
    QCOMPARE(info.partitions.size(), 1);
    QCOMPARE(info.partitions[0].name, "boot");
    QCOMPARE(info.partitions[0].ops.size(), 1);
    QCOMPARE(info.partitions[0].ops[0].type, 0);
    QCOMPARE(info.partitions[0].ops[0].dataLength, 8ull);
}

// data_offset 非零 + data_sha256_hash 提取；manifest 无 block_size 字段 → 缺省 4096
static QByteArray buildPayloadWithOpFields()
{
    QByteArray op = pbwire::encodeVarint(1, 1)                             // type=REPLACE_BZ
                  + pbwire::encodeVarint(2, 65536)                         // data_offset 非零
                  + pbwire::encodeVarint(3, 128)                           // data_length
                  + pbwire::encodeBytes(7, QByteArray(32, '\x42'));        // data_sha256_hash
    QByteArray part = pbwire::encodeBytes(1, QByteArray("vendor"))
                    + pbwire::encodeMessage(8, op);
    QByteArray manifest = pbwire::encodeMessage(13, part);                 // 无 block_size
    QByteArray payload;
    payload.append("CrAU");
    auto put64 = [&](quint64 v) {
        for (int i = 0; i < 8; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put64(2);
    put64(static_cast<quint64>(manifest.size()));
    auto put32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put32(0);
    payload.append(manifest);
    return payload;
}

void TestPayload::parseOpFields()
{
    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(buildPayloadWithOpFields(), info));
    QCOMPARE(info.blockSize, 4096ull);                                     // 缺省值
    QCOMPARE(info.partitions.size(), 1);
    QCOMPARE(info.partitions[0].name, "vendor");
    QCOMPARE(info.partitions[0].ops.size(), 1);
    QCOMPARE(info.partitions[0].ops[0].type, 1);
    QCOMPARE(info.partitions[0].ops[0].dataOffset, 65536ull);
    QCOMPARE(info.partitions[0].ops[0].dataLength, 128ull);
    QCOMPARE(info.partitions[0].ops[0].dataHash, QByteArray(32, '\x42'));
}

// ---------- Task 14: payload 全量解包（REPLACE 系） ----------

void TestPayload::extractReplaceOp()
{
    // data_offset 语义: 相对 blob 起点的偏移（真实 payload 格式，AOSP 首个 op 即 data_offset=0），
    // 读取位置 = totalBase(4+8+8+4+manifest.size) + data_offset。
    // brief 原构造把 data_offset 写成 blob 的绝对起点(24+manifest.size) —— 与 totalBase 重复
    // 叠加后越界，构造不自洽。此处重建: 数据放 blob[8..16)，data_offset=8；blob 用 0xEE 填充
    // 其余位置 —— 若实现漏加 totalBase 或加错位置，断言会读到 0xEE 而失败，真实验证定位。
    QByteArray p = buildMinimalPayload();
    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(p, info));
    // blob 实际起点(绝对) = 4("CrAU") + 8(version) + 8(manifest_size) + 4(meta_sig_size) + manifest.size
    const int blobStart = 4 + 8 + 8 + 4 + info.manifestRaw.size();
    // 重建 op: REPLACE data_offset=8(blob 相对) len=8
    QByteArray op = pbwire::encodeVarint(1, 0) + pbwire::encodeVarint(2, 8)
                  + pbwire::encodeVarint(3, 8);
    QByteArray part = pbwire::encodeBytes(1, QByteArray("boot")) + pbwire::encodeMessage(8, op);
    QByteArray manifest = pbwire::encodeVarint(3, 4096) + pbwire::encodeMessage(13, part);
    QByteArray payload;
    payload.append("CrAU");
    auto put64 = [&](quint64 v) { for (int i = 0; i < 8; ++i) payload.append(char((v >> (i * 8)) & 0xFF)); };
    put64(2); put64(static_cast<quint64>(manifest.size()));
    auto put32 = [&](quint32 v) { for (int i = 0; i < 4; ++i) payload.append(char((v >> (i * 8)) & 0xFF)); };
    put32(0);
    payload.append(manifest);
    payload.append(QByteArray(8, '\xEE'));  // blob 区填充（非数据，验证定位用）
    payload.append(QByteArray(8, '\xCD'));  // blob[8..16): REPLACE 数据
    payload.append(QByteArray(16, '\xEE')); // blob 区余量
    imgpayload::PayloadInfo info2;
    QVERIFY(imgpayload::parseManifest(payload, info2));
    // 自洽性: 数据必须真实位于 (blob 起点 + data_offset) 处。两次构造的 op 形状相同
    // （data_offset 均为 1 字节 varint）→ manifest 同尺寸 → blobStart 与真实起点一致;
    // manifest 大小若变化，blob 起点随之漂移，此处直接用第二次解析结果重算并验证
    const int realStart = 4 + 8 + 8 + 4 + info2.manifestRaw.size();
    QCOMPARE(info2.manifestRaw.size(), info.manifestRaw.size());
    QCOMPARE(realStart, blobStart);
    QVERIFY(payload.mid(realStart + 8, 8) == QByteArray(8, '\xCD'));
    QCOMPARE(info2.partitions[0].ops[0].dataOffset, 8ull);
    QString err;
    QByteArray out = imgpayload::extractPartition(payload, info2.partitions[0], QByteArray(), &err);
    // 输出大小 = maxEnd(dataOffset+dataLength=16) ceil 到 blockSize → 1 块 4096
    QCOMPARE(out.size(), 4096);
    QVERIFY(out.left(8) == QByteArray(8, '\xCD')); // REPLACE 数据写到了输出头部
    QVERIFY(err.isEmpty());
}

// ---------- Task 15: payload diff 解包（SOURCE_COPY / SOURCE_BSDIFF） ----------

void TestPayload::extractSourceCopy()
{
    // SOURCE_COPY: src_extents(4) 单连续块复制到 dst_extents(6)，无 blob 数据。
    // oldImage 3 块 'A','C','E'，src={1,2} → 源块 1..2 为 'C','E' ——
    // 若实现忽略 src startBlock 或误用"整分区拷贝"，断言失败。
    const quint64 bs = 4096;
    QByteArray oldImage = QByteArray(static_cast<int>(bs), 'A') + QByteArray(static_cast<int>(bs), 'C')
                        + QByteArray(static_cast<int>(bs), 'E');
    QByteArray srcExt = pbwire::encodeVarint(1, 1) + pbwire::encodeVarint(2, 2); // 块 1..2
    QByteArray dstExt = pbwire::encodeVarint(1, 0) + pbwire::encodeVarint(2, 2); // 块 0..1
    QByteArray op = pbwire::encodeVarint(1, imgpayload::OP_SOURCE_COPY)
                  + pbwire::encodeMessage(4, srcExt)
                  + pbwire::encodeMessage(6, dstExt);
    QByteArray part = pbwire::encodeBytes(1, QByteArray("boot")) + pbwire::encodeMessage(8, op);
    QByteArray manifest = pbwire::encodeVarint(3, bs) + pbwire::encodeMessage(13, part);
    QByteArray payload;
    payload.append("CrAU");
    putU64(payload, 2);
    putU64(payload, static_cast<quint64>(manifest.size()));
    auto put32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put32(0);
    payload.append(manifest);
    payload.append(QByteArray(16, '\xEE')); // blob 区（SOURCE_COPY 不使用）

    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(payload, info));
    QCOMPARE(info.partitions[0].ops[0].srcExtents.size(), 1);
    QCOMPARE(info.partitions[0].ops[0].srcExtents[0].startBlock, 1ull);
    QCOMPARE(info.partitions[0].ops[0].srcExtents[0].numBlocks, 2ull);
    QCOMPARE(info.partitions[0].ops[0].dstExtents.size(), 1);
    QCOMPARE(info.partitions[0].ops[0].dstExtents[0].startBlock, 0ull);
    QCOMPARE(info.partitions[0].ops[0].dstExtents[0].numBlocks, 2ull);

    QString err;
    QByteArray out = imgpayload::extractPartition(payload, info.partitions[0], oldImage, &err, bs);
    QVERIFY(err.isEmpty());
    QCOMPARE(out.size(), static_cast<int>(2 * bs));
    QVERIFY(out.left(static_cast<int>(bs)) == QByteArray(static_cast<int>(bs), 'C'));
    QVERIFY(out.mid(static_cast<int>(bs)) == QByteArray(static_cast<int>(bs), 'E'));

    // 无旧镜像 → 明确报"需要旧镜像"
    QString err2;
    QVERIFY(imgpayload::extractPartition(payload, info.partitions[0], QByteArray(), &err2).isEmpty());
    QVERIFY(err2.contains("旧镜像"));
}

void TestPayload::extractSourceBsdiff()
{
    // SOURCE_BSDIFF: patch 应用对象是 src_extents 对应的旧数据块（本用例块 1 = 'C'），
    // 结果按 dst_extents 写入。若实现误把整分区当旧数据，会得到 'B' 而非 'D'。
    const quint64 bs = 4096;
    QByteArray oldImage = QByteArray(static_cast<int>(bs), 'A') + QByteArray(static_cast<int>(bs), 'C');
    // bsdiff patch: newLen=4096, diff[i]=1（'C'+1='D'），ctrl=(4096,0,0)
    QByteArray ctrl, diff(static_cast<int>(bs), '\x01');
    putU64(ctrl, bs); putU64(ctrl, 0); putU64(ctrl, 0);
    QByteArray ctrlBz = imgcomp::bzip2Compress(ctrl);
    QByteArray diffBz = imgcomp::bzip2Compress(diff);
    QByteArray patch;
    patch.append("BSDIFF40");
    putU64(patch, static_cast<quint64>(ctrlBz.size()));
    putU64(patch, static_cast<quint64>(diffBz.size()));
    putU64(patch, bs); // 第 3 字段 = new_len（bspatch.c 语义），不是 extra 长度
    patch.append(ctrlBz).append(diffBz);

    QByteArray srcExt = pbwire::encodeVarint(1, 1) + pbwire::encodeVarint(2, 1); // 块 1
    QByteArray dstExt = pbwire::encodeVarint(1, 0) + pbwire::encodeVarint(2, 1); // 块 0
    QByteArray op = pbwire::encodeVarint(1, imgpayload::OP_SOURCE_BSDIFF)
                  + pbwire::encodeVarint(2, 0)                                  // data_offset=0（blob 相对）
                  + pbwire::encodeVarint(3, static_cast<quint64>(patch.size()))
                  + pbwire::encodeMessage(4, srcExt)
                  + pbwire::encodeMessage(6, dstExt);
    QByteArray part = pbwire::encodeBytes(1, QByteArray("boot")) + pbwire::encodeMessage(8, op);
    QByteArray manifest = pbwire::encodeVarint(3, bs) + pbwire::encodeMessage(13, part);
    QByteArray payload;
    payload.append("CrAU");
    putU64(payload, 2);
    putU64(payload, static_cast<quint64>(manifest.size()));
    auto put32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put32(0);
    payload.append(manifest);
    payload.append(patch); // blob 起点即 patch（data_offset=0）

    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(payload, info));
    QCOMPARE(info.blockSize, bs);
    QString err;
    QByteArray out = imgpayload::extractPartition(payload, info.partitions[0], oldImage, &err, bs);
    QVERIFY(err.isEmpty());
    QCOMPARE(out.size(), static_cast<int>(bs));
    QVERIFY(out == QByteArray(static_cast<int>(bs), 'D'));

    // 独立验证: src_extents 旧片段 + patch → 'D'
    QByteArray frag = oldImage.mid(static_cast<int>(bs), static_cast<int>(bs));
    QCOMPARE(imgbspatch::applyBsdiff(frag, patch), QByteArray(static_cast<int>(bs), 'D'));
}

// 双测试类（TestWire + TestPayload）共用主函数
int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    int status = 0;
    {
        TestWire tc;
        status |= QTest::qExec(&tc, argc, argv);
    }
    {
        TestPayload tc;
        status |= QTest::qExec(&tc, argc, argv);
    }
    return status;
}
#include "test_payload.moc"
