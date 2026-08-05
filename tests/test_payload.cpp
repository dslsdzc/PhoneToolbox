#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QTemporaryDir>
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
    void parseMultipleExtents(); // Task 15 审查: 多 extent 追加不覆盖
    void extractReplaceOp();   // Task 14: 全量 REPLACE 解包
    void extractSourceCopy();  // Task 15: SOURCE_COPY（src_extents → dst_extents）
    void extractSourceBsdiff(); // Task 15: SOURCE_BSDIFF（src_extents 旧片段 + bspatch）
    void extractReplaceBzExtentsAndHash(); // Final review C1/C2/C3: REPLACE_BZ + blob hash + dst_extents
    void extractReplaceZeroInterleave();   // Final review C1/C3: ZERO 交错 + dst_extents 落位
    void parseManifestFileRoundTrip();     // G2: 文件 manifest 解析与内存解析一致
    void extractStreamReplaceMatchesOld(); // G2: 流式 == 旧接口（REPLACE 系 + ZERO + 回退映射）
    void extractStreamDiffMatchesOld();    // G2: 流式 == 旧接口（SOURCE_COPY + SOURCE_BSDIFF）
    void extractStreamErrors();            // G2: 流式失败语义 + 输出清理
    void extractStreamAllocGuards();       // G2 审查: 超大声明分配上限防护 + 病态输入错误消息对齐
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

// data_offset 非零 + dst_length + data_sha256_hash 提取；manifest 无 block_size 字段 → 缺省 4096
static QByteArray buildPayloadWithOpFields()
{
    QByteArray op = pbwire::encodeVarint(1, 1)                             // type=REPLACE_BZ
                  + pbwire::encodeVarint(2, 65536)                         // data_offset 非零
                  + pbwire::encodeVarint(3, 128)                           // data_length
                  + pbwire::encodeVarint(7, 8192)                          // dst_length
                  + pbwire::encodeBytes(8, QByteArray(32, '\x42'));        // data_sha256_hash
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
    QCOMPARE(info.partitions[0].ops[0].dstLength, 8192ull);   // 字段 7 (varint)
    QCOMPARE(info.partitions[0].ops[0].dataHash, QByteArray(32, '\x42')); // 字段 8 (bytes)
}

// 多 extent 必须追加而非覆盖（src_extents=4 / dst_extents=6 是 repeated 字段）
void TestPayload::parseMultipleExtents()
{
    QByteArray e1 = pbwire::encodeVarint(1, 0) + pbwire::encodeVarint(2, 1);
    QByteArray e2 = pbwire::encodeVarint(1, 5) + pbwire::encodeVarint(2, 2);
    QByteArray op = pbwire::encodeVarint(1, imgpayload::OP_SOURCE_COPY)
                  + pbwire::encodeMessage(4, e1) + pbwire::encodeMessage(4, e2)
                  + pbwire::encodeMessage(6, e1) + pbwire::encodeMessage(6, e2);
    QByteArray part = pbwire::encodeBytes(1, QByteArray("boot")) + pbwire::encodeMessage(8, op);
    QByteArray manifest = pbwire::encodeMessage(13, part);
    QByteArray payload;
    payload.append("CrAU");
    putU64(payload, 2);
    putU64(payload, static_cast<quint64>(manifest.size()));
    auto put32 = [&](quint32 v) {
        for (int i = 0; i < 4; ++i) payload.append(char((v >> (i * 8)) & 0xFF));
    };
    put32(0);
    payload.append(manifest);

    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(payload, info));
    const imgpayload::InstallOp &pop = info.partitions[0].ops[0];
    QCOMPARE(pop.srcExtents.size(), 2);
    QCOMPARE(pop.srcExtents[0].startBlock, 0ull);
    QCOMPARE(pop.srcExtents[0].numBlocks, 1ull);
    QCOMPARE(pop.srcExtents[1].startBlock, 5ull);
    QCOMPARE(pop.srcExtents[1].numBlocks, 2ull);
    QCOMPARE(pop.dstExtents.size(), 2);
    QCOMPARE(pop.dstExtents[0].startBlock, 0ull);
    QCOMPARE(pop.dstExtents[1].numBlocks, 2ull);
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

// ---------- Final review 修复: REPLACE 系接入 dst_extents（C1 预算 / C2 hash / C3 定位） ----------

void TestPayload::extractReplaceBzExtentsAndHash()
{
    // 真实 OTA 语义: REPLACE_BZ 的 data_sha256_hash 对压缩后 blob 计算（AOSP
    // delta_performer 的 ValidateOperationHash 校验原始 blob）；dst_extents 决定写入位置
    // （startBlock=3，而 dataOffset/blockSize=0 —— 若按旧"dataOffset 连续映射"会写到块 0）；
    // 输出预算按 dst_extents 覆盖范围（5 块）而非 dataOffset+dataLength（blob 压缩后较小）。
    const quint64 bs = 4096;
    const QByteArray payloadData(static_cast<int>(2 * bs), 'D'); // 2 块解压后数据
    QByteArray blob = imgcomp::bzip2Compress(payloadData);
    QVERIFY(!blob.isEmpty());
    QVERIFY(blob.size() < payloadData.size()); // 压缩有效（若 bzip2 意外退化为不压缩，断言仍成立）

    const QByteArray blobHash = QCryptographicHash::hash(blob, QCryptographicHash::Sha256);
    QByteArray dstExt = pbwire::encodeVarint(1, 3) + pbwire::encodeVarint(2, 2); // 块 3..4

    auto build = [&](const QByteArray &hashBytes) {
        QByteArray op = pbwire::encodeVarint(1, imgpayload::OP_REPLACE_BZ)
                      + pbwire::encodeVarint(2, 64)                                  // data_offset 非零（blobs 区偏移）
                      + pbwire::encodeVarint(3, static_cast<quint64>(blob.size()))
                      + pbwire::encodeMessage(6, dstExt)
                      + pbwire::encodeBytes(8, hashBytes);
        QByteArray part = pbwire::encodeBytes(1, QByteArray("vendor")) + pbwire::encodeMessage(8, op);
        QByteArray manifest = pbwire::encodeVarint(3, bs) + pbwire::encodeMessage(13, part);
        QByteArray payload;
        payload.append("CrAU");
        putU64(payload, 2);
        putU64(payload, static_cast<quint64>(manifest.size()));
        for (int i = 0; i < 4; ++i) payload.append(char(0)); // metadata_signature_size
        payload.append(manifest);
        payload.append(QByteArray(64, '\xEE')); // blobs 区填充（dataOffset=64 起点）
        payload.append(blob);
        return payload;
    };

    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(build(blobHash), info));
    const imgpayload::InstallOp &pop = info.partitions[0].ops[0];
    QCOMPARE(pop.type, imgpayload::OP_REPLACE_BZ);
    QCOMPARE(pop.dstExtents.size(), 1);
    QCOMPARE(pop.dstExtents[0].startBlock, 3ull);
    QCOMPARE(pop.dstExtents[0].numBlocks, 2ull);
    QCOMPARE(pop.dataHash, blobHash);

    QString err;
    QByteArray out = imgpayload::extractPartition(build(blobHash), info.partitions[0],
                                                  QByteArray(), &err, bs);
    QVERIFY(err.isEmpty());
    QCOMPARE(out.size(), static_cast<int>(5 * bs)); // 预算 = dst_extents 末端（块 5）
    QVERIFY(out.left(static_cast<int>(3 * bs)) == QByteArray(static_cast<int>(3 * bs), '\0'));
    QVERIFY(out.mid(static_cast<int>(3 * bs), static_cast<int>(2 * bs)) == payloadData);

    // 负向: hash 对"解压后"数据计算 → 与 AOSP 语义不符，必须校验失败
    // （用各自 payload 解析出的 manifest，保证校验的是该 payload 携带的 dataHash）
    const QByteArray wrongHash = QCryptographicHash::hash(payloadData, QCryptographicHash::Sha256);
    QVERIFY(wrongHash != blobHash);
    imgpayload::PayloadInfo infoWrong;
    QVERIFY(imgpayload::parseManifest(build(wrongHash), infoWrong));
    QString err2;
    QVERIFY(imgpayload::extractPartition(build(wrongHash), infoWrong.partitions[0],
                                         QByteArray(), &err2, bs).isEmpty());
    QVERIFY(err2.contains("SHA-256"));

    // blob 损坏 → 同样校验失败
    QByteArray corrupt = build(blobHash);
    corrupt.chop(1);
    corrupt.append('\xFF');
    imgpayload::PayloadInfo infoCorrupt;
    QVERIFY(imgpayload::parseManifest(corrupt, infoCorrupt));
    QString err3;
    QVERIFY(imgpayload::extractPartition(corrupt, infoCorrupt.partitions[0],
                                         QByteArray(), &err3, bs).isEmpty());
    QVERIFY(err3.contains("SHA-256"));
}

void TestPayload::extractReplaceZeroInterleave()
{
    // ZERO op 不消费 blob 但占输出空间（0x00）；其后 REPLACE 必须按 dst_extents 落位。
    // 旧实现按 dataOffset/blockSize 连续映射会把后续 REPLACE 整体错位（ZERO 无 blob，
    // 其 dataOffset 不推进），本用例 ZERO 前缀 + 中间交错 + 两块 REPLACE 同时验证。
    const quint64 bs = 4096;
    const QByteArray blobB(static_cast<int>(bs), 'B');
    const QByteArray blobC(static_cast<int>(bs), 'C');
    QByteArray zeroExt = pbwire::encodeVarint(1, 0) + pbwire::encodeVarint(2, 1);   // 块 0 (ZERO)
    QByteArray repBExt = pbwire::encodeVarint(1, 1) + pbwire::encodeVarint(2, 1);   // 块 1
    QByteArray zeroExt2 = pbwire::encodeVarint(1, 2) + pbwire::encodeVarint(2, 1);  // 块 2 (ZERO)
    QByteArray repCExt = pbwire::encodeVarint(1, 3) + pbwire::encodeVarint(2, 1);   // 块 3
    QByteArray opZero = pbwire::encodeVarint(1, imgpayload::OP_ZERO)
                      + pbwire::encodeMessage(6, zeroExt);
    QByteArray opB = pbwire::encodeVarint(1, imgpayload::OP_REPLACE)
                   + pbwire::encodeVarint(2, 0)
                   + pbwire::encodeVarint(3, static_cast<quint64>(blobB.size()))
                   + pbwire::encodeMessage(6, repBExt);
    QByteArray opZero2 = pbwire::encodeVarint(1, imgpayload::OP_ZERO)
                       + pbwire::encodeMessage(6, zeroExt2);
    QByteArray opC = pbwire::encodeVarint(1, imgpayload::OP_REPLACE)
                   + pbwire::encodeVarint(2, static_cast<quint64>(blobB.size()))
                   + pbwire::encodeVarint(3, static_cast<quint64>(blobC.size()))
                   + pbwire::encodeMessage(6, repCExt);
    QByteArray part = pbwire::encodeBytes(1, QByteArray("system"))
                    + pbwire::encodeMessage(8, opZero) + pbwire::encodeMessage(8, opB)
                    + pbwire::encodeMessage(8, opZero2) + pbwire::encodeMessage(8, opC);
    QByteArray manifest = pbwire::encodeVarint(3, bs) + pbwire::encodeMessage(13, part);
    QByteArray payload;
    payload.append("CrAU");
    putU64(payload, 2);
    putU64(payload, static_cast<quint64>(manifest.size()));
    for (int i = 0; i < 4; ++i) payload.append(char(0));
    payload.append(manifest);
    payload.append(blobB).append(blobC); // blobs 区: 仅两块 REPLACE 数据

    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(payload, info));
    QCOMPARE(info.partitions[0].ops.size(), 4);
    QString err;
    QByteArray out = imgpayload::extractPartition(payload, info.partitions[0], QByteArray(), &err, bs);
    QVERIFY(err.isEmpty());
    QCOMPARE(out.size(), static_cast<int>(4 * bs)); // 预算含 ZERO 的输出空间（块 4 末端）
    QVERIFY(out.left(static_cast<int>(bs)) == QByteArray(static_cast<int>(bs), '\0'));
    QVERIFY(out.mid(static_cast<int>(bs), static_cast<int>(bs)) == blobB);
    QVERIFY(out.mid(static_cast<int>(2 * bs), static_cast<int>(bs)) == QByteArray(static_cast<int>(bs), '\0'));
    QVERIFY(out.mid(static_cast<int>(3 * bs), static_cast<int>(bs)) == blobC);
}

// ---------- G2: payload 解包流式化（extractPartitionStream / parseManifestFile） ----------

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

static QByteArray extentMsg(quint64 start, quint64 n)
{
    return pbwire::encodeVarint(1, start) + pbwire::encodeVarint(2, n);
}

// 混合 REPLACE 系 payload: ZERO + REPLACE_BZ(压缩+hash+dst_extents) + REPLACE(hash+dst_extents)
// + REPLACE(无 extent 回退映射)。blob 区布局: bzBlob(0) + repData(L1) + pad 到 16384 + fbData(16384
// → 回退映射落位块 4)。输出预算 = 5 块（块 0 ZERO / 1-2 BZ / 3 REPLACE / 4 回退）。
static QByteArray buildStreamReplacePayload(int &bzBlobLen, int &repDataLen)
{
    const quint64 bs = 4096;
    const QByteArray bzData(static_cast<int>(2 * bs), 'D');
    const QByteArray bzBlob = imgcomp::bzip2Compress(bzData);
    const QByteArray repData(static_cast<int>(bs), 'B');
    const QByteArray fbData(static_cast<int>(bs), 'F');
    if (bzBlob.isEmpty())
        return {};
    bzBlobLen = bzBlob.size();
    repDataLen = repData.size();

    const quint64 fbOffset = 16384; // 4 * 4096 → 回退映射到块 4
    QByteArray opZero = pbwire::encodeVarint(1, imgpayload::OP_ZERO)
                      + pbwire::encodeMessage(6, extentMsg(0, 1));
    QByteArray opBz = pbwire::encodeVarint(1, imgpayload::OP_REPLACE_BZ)
                    + pbwire::encodeVarint(2, 0)
                    + pbwire::encodeVarint(3, static_cast<quint64>(bzBlob.size()))
                    + pbwire::encodeMessage(6, extentMsg(1, 2))
                    + pbwire::encodeBytes(8, QCryptographicHash::hash(bzBlob, QCryptographicHash::Sha256));
    QByteArray opRep = pbwire::encodeVarint(1, imgpayload::OP_REPLACE)
                     + pbwire::encodeVarint(2, static_cast<quint64>(bzBlob.size()))
                     + pbwire::encodeVarint(3, static_cast<quint64>(repData.size()))
                     + pbwire::encodeMessage(6, extentMsg(3, 1))
                     + pbwire::encodeBytes(8, QCryptographicHash::hash(repData, QCryptographicHash::Sha256));
    QByteArray opFb = pbwire::encodeVarint(1, imgpayload::OP_REPLACE)
                    + pbwire::encodeVarint(2, fbOffset)
                    + pbwire::encodeVarint(3, static_cast<quint64>(fbData.size()));
    QByteArray part = pbwire::encodeBytes(1, QByteArray("system"))
                    + pbwire::encodeMessage(8, opZero) + pbwire::encodeMessage(8, opBz)
                    + pbwire::encodeMessage(8, opRep) + pbwire::encodeMessage(8, opFb);
    QByteArray manifest = pbwire::encodeVarint(3, bs) + pbwire::encodeMessage(13, part);
    QByteArray payload;
    payload.append("CrAU");
    putU64(payload, 2);
    putU64(payload, static_cast<quint64>(manifest.size()));
    for (int i = 0; i < 4; ++i) payload.append(char(0));
    payload.append(manifest);
    payload.append(bzBlob).append(repData);
    payload.append(QByteArray(static_cast<int>(fbOffset) - bzBlob.size() - repData.size(), '\xEE'));
    payload.append(fbData);
    return payload;
}

// diff 系 payload: SOURCE_COPY（旧块 1..2 → 块 0..1）+ SOURCE_BSDIFF（旧块 3 'F' +1 → 块 2 'G'）。
// 旧镜像 4 块 'A','C','E','F'；输出 = 'C','E','G'。
static QByteArray buildStreamDiffPayload(QByteArray &oldImage, quint64 &patchLen)
{
    const quint64 bs = 4096;
    oldImage = QByteArray(static_cast<int>(bs), 'A') + QByteArray(static_cast<int>(bs), 'C')
             + QByteArray(static_cast<int>(bs), 'E') + QByteArray(static_cast<int>(bs), 'F');
    QByteArray ctrl, diff(static_cast<int>(bs), '\x01');
    putU64(ctrl, bs); putU64(ctrl, 0); putU64(ctrl, 0);
    QByteArray patch;
    patch.append("BSDIFF40");
    putU64(patch, static_cast<quint64>(imgcomp::bzip2Compress(ctrl).size()));
    putU64(patch, static_cast<quint64>(imgcomp::bzip2Compress(diff).size()));
    putU64(patch, bs);
    patch.append(imgcomp::bzip2Compress(ctrl)).append(imgcomp::bzip2Compress(diff));
    patchLen = patch.size();

    QByteArray opCopy = pbwire::encodeVarint(1, imgpayload::OP_SOURCE_COPY)
                      + pbwire::encodeMessage(4, extentMsg(1, 2))
                      + pbwire::encodeMessage(6, extentMsg(0, 2));
    QByteArray opDiff = pbwire::encodeVarint(1, imgpayload::OP_SOURCE_BSDIFF)
                      + pbwire::encodeVarint(2, 0)
                      + pbwire::encodeVarint(3, static_cast<quint64>(patch.size()))
                      + pbwire::encodeMessage(4, extentMsg(3, 1))
                      + pbwire::encodeMessage(6, extentMsg(2, 1));
    QByteArray part = pbwire::encodeBytes(1, QByteArray("boot"))
                    + pbwire::encodeMessage(8, opCopy) + pbwire::encodeMessage(8, opDiff);
    QByteArray manifest = pbwire::encodeVarint(3, bs) + pbwire::encodeMessage(13, part);
    QByteArray payload;
    payload.append("CrAU");
    putU64(payload, 2);
    putU64(payload, static_cast<quint64>(manifest.size()));
    for (int i = 0; i < 4; ++i) payload.append(char(0));
    payload.append(manifest);
    payload.append(patch); // blob 起点即 patch（data_offset=0）
    return payload;
}

void TestPayload::parseManifestFileRoundTrip()
{
    // 文件解析只读头 + manifest 区，结果与内存解析一致
    const QByteArray payload = buildPayloadWithOpFields();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("payload.bin");
    QVERIFY(writeFile(path, payload));
    imgpayload::PayloadInfo fromFile, fromMem;
    QVERIFY(imgpayload::parseManifestFile(path, fromFile));
    QVERIFY(imgpayload::parseManifest(payload, fromMem));
    QCOMPARE(fromFile.blockSize, fromMem.blockSize);
    QCOMPARE(fromFile.manifestRaw, fromMem.manifestRaw);
    QCOMPARE(fromFile.partitions.size(), fromMem.partitions.size());
    QCOMPARE(fromFile.partitions[0].name, fromMem.partitions[0].name);
    const imgpayload::InstallOp &a = fromFile.partitions[0].ops[0];
    const imgpayload::InstallOp &b = fromMem.partitions[0].ops[0];
    QCOMPARE(a.type, b.type);
    QCOMPARE(a.dataOffset, b.dataOffset);
    QCOMPARE(a.dataLength, b.dataLength);
    QCOMPARE(a.dstLength, b.dstLength);
    QCOMPARE(a.dataHash, b.dataHash);
    // 不存在的文件 → false，不崩溃
    QVERIFY(!imgpayload::parseManifestFile(dir.filePath("nope.bin"), fromFile));
}

void TestPayload::extractStreamReplaceMatchesOld()
{
    int bzLen = 0, repLen = 0;
    const QByteArray payload = buildStreamReplacePayload(bzLen, repLen);
    QVERIFY(!payload.isEmpty()); // bzip2 可用时构建成功
    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(payload, info));
    const quint64 bs = info.blockSize;

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString payloadPath = dir.filePath("payload.bin");
    const QString outPath = dir.filePath("out.img");
    QVERIFY(writeFile(payloadPath, payload));

    // 旧接口全内存结果
    QString err;
    const QByteArray old = imgpayload::extractPartition(payload, info.partitions[0], QByteArray(), &err, bs);
    QVERIFY(err.isEmpty());
    QCOMPARE(old.size(), static_cast<int>(5 * bs));
    QVERIFY(old.left(static_cast<int>(bs)) == QByteArray(static_cast<int>(bs), '\0'));
    QVERIFY(old.mid(static_cast<int>(bs), static_cast<int>(2 * bs)) == QByteArray(static_cast<int>(2 * bs), 'D'));
    QVERIFY(old.mid(static_cast<int>(3 * bs), static_cast<int>(bs)) == QByteArray(static_cast<int>(bs), 'B'));
    QVERIFY(old.mid(static_cast<int>(4 * bs)) == QByteArray(static_cast<int>(bs), 'F'));

    // 流式接口（无旧镜像，纯 REPLACE 系可解）
    QString serr;
    QList<quint64> prog;
    QVERIFY(imgpayload::extractPartitionStream(payloadPath, info.partitions[0], outPath,
                                               [&](quint64 b) { prog.append(b); }, &serr,
                                               QString(), bs));
    QVERIFY(serr.isEmpty());
    QCOMPARE(readFile(outPath), old); // 与旧接口逐字节一致

    // 进度: 单调递增, 0 .. 总 blob 字节
    QVERIFY(!prog.isEmpty());
    QCOMPARE(prog.first(), 0ull);
    const quint64 blobTotal = static_cast<quint64>(bzLen) + repLen + repLen;
    QCOMPARE(prog.last(), blobTotal);
    for (int i = 1; i < prog.size(); ++i)
        QVERIFY(prog[i] >= prog[i - 1]);
}

void TestPayload::extractStreamDiffMatchesOld()
{
    QByteArray oldImage;
    quint64 patchLen = 0;
    const QByteArray payload = buildStreamDiffPayload(oldImage, patchLen);
    imgpayload::PayloadInfo info;
    QVERIFY(imgpayload::parseManifest(payload, info));
    const quint64 bs = info.blockSize;

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString payloadPath = dir.filePath("payload.bin");
    const QString oldPath = dir.filePath("old.img");
    const QString outPath = dir.filePath("out.img");
    QVERIFY(writeFile(payloadPath, payload));
    QVERIFY(writeFile(oldPath, oldImage));

    QString err;
    const QByteArray old = imgpayload::extractPartition(payload, info.partitions[0], oldImage, &err, bs);
    QVERIFY(err.isEmpty());
    QCOMPARE(old.size(), static_cast<int>(3 * bs));
    QVERIFY(old.left(static_cast<int>(2 * bs)) ==
            QByteArray(static_cast<int>(bs), 'C') + QByteArray(static_cast<int>(bs), 'E'));
    QVERIFY(old.mid(static_cast<int>(2 * bs)) == QByteArray(static_cast<int>(bs), 'G'));

    QString serr;
    QList<quint64> prog;
    QVERIFY(imgpayload::extractPartitionStream(payloadPath, info.partitions[0], outPath,
                                               [&](quint64 b) { prog.append(b); }, &serr,
                                               oldPath, bs));
    QVERIFY(serr.isEmpty());
    QCOMPARE(readFile(outPath), old); // 与旧接口逐字节一致
    QVERIFY(!prog.isEmpty());
    QCOMPARE(prog.last(), patchLen);   // 仅 SOURCE_BSDIFF 消费 blob
}

void TestPayload::extractStreamErrors()
{
    const quint64 bs = 4096;
    const QByteArray blob(static_cast<int>(bs), 'B');
    auto build = [&](const QByteArray &partBytes, const QByteArray &blobArea) {
        QByteArray manifest = pbwire::encodeVarint(3, bs) + pbwire::encodeMessage(13, partBytes);
        QByteArray payload;
        payload.append("CrAU");
        putU64(payload, 2);
        putU64(payload, static_cast<quint64>(manifest.size()));
        for (int i = 0; i < 4; ++i) payload.append(char(0));
        payload.append(manifest);
        payload.append(blobArea);
        return payload;
    };

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString payloadPath = dir.filePath("payload.bin");
    const QString outPath = dir.filePath("out.img");

    // 1) diff op 无旧镜像 → "需要旧镜像" + 输出文件被删除
    QByteArray opCopy = pbwire::encodeVarint(1, imgpayload::OP_SOURCE_COPY)
                      + pbwire::encodeMessage(4, extentMsg(0, 1))
                      + pbwire::encodeMessage(6, extentMsg(0, 1));
    QByteArray part1 = pbwire::encodeBytes(1, QByteArray("boot")) + pbwire::encodeMessage(8, opCopy);
    const QByteArray payload1 = build(part1, {});
    QVERIFY(writeFile(payloadPath, payload1));
    imgpayload::PayloadInfo info1;
    QVERIFY(imgpayload::parseManifest(payload1, info1));
    QString err;
    QVERIFY(!imgpayload::extractPartitionStream(payloadPath, info1.partitions[0], outPath, {}, &err));
    QVERIFY(err.contains("需要旧镜像"));
    QVERIFY(!QFile::exists(outPath));

    // 2) blob 越界（dataOffset+dataLength 超出文件）→ "payload 数据越界"
    QByteArray opRep = pbwire::encodeVarint(1, imgpayload::OP_REPLACE)
                     + pbwire::encodeVarint(2, 16)
                     + pbwire::encodeVarint(3, static_cast<quint64>(blob.size()))
                     + pbwire::encodeMessage(6, extentMsg(0, 1));
    QByteArray part2 = pbwire::encodeBytes(1, QByteArray("boot")) + pbwire::encodeMessage(8, opRep);
    const QByteArray payload2 = build(part2, QByteArray(8, '\xAB')); // blob 区仅 8 字节
    QVERIFY(writeFile(payloadPath, payload2));
    imgpayload::PayloadInfo info2;
    QVERIFY(imgpayload::parseManifest(payload2, info2));
    err.clear();
    QVERIFY(!imgpayload::extractPartitionStream(payloadPath, info2.partitions[0], outPath, {}, &err));
    QVERIFY(err.contains("数据越界"));
    QVERIFY(!QFile::exists(outPath));

    // 3) blob hash 不符（写盘后校验失败）→ "SHA-256 校验失败" + 输出被清理
    QByteArray opHash = pbwire::encodeVarint(1, imgpayload::OP_REPLACE)
                      + pbwire::encodeVarint(2, 0)
                      + pbwire::encodeVarint(3, static_cast<quint64>(blob.size()))
                      + pbwire::encodeMessage(6, extentMsg(0, 1))
                      + pbwire::encodeBytes(8, QByteArray(32, '\x42'));
    QByteArray part3 = pbwire::encodeBytes(1, QByteArray("boot")) + pbwire::encodeMessage(8, opHash);
    const QByteArray payload3 = build(part3, blob);
    QVERIFY(writeFile(payloadPath, payload3));
    imgpayload::PayloadInfo info3;
    QVERIFY(imgpayload::parseManifest(payload3, info3));
    err.clear();
    QVERIFY(!imgpayload::extractPartitionStream(payloadPath, info3.partitions[0], outPath, {}, &err));
    QVERIFY(err.contains("SHA-256"));
    QVERIFY(!QFile::exists(outPath));

    // 4) blob 区截断（dataLength 超出文件实际大小）→ 与旧接口一致的 "payload 数据越界"
    QByteArray opTrunc = pbwire::encodeVarint(1, imgpayload::OP_REPLACE)
                       + pbwire::encodeVarint(2, 0)
                       + pbwire::encodeVarint(3, static_cast<quint64>(blob.size()))
                       + pbwire::encodeMessage(6, extentMsg(0, 1));
    QByteArray part4 = pbwire::encodeBytes(1, QByteArray("boot")) + pbwire::encodeMessage(8, opTrunc);
    const QByteArray payload4 = build(part4, QByteArray(static_cast<int>(bs) / 2, '\xAB'));
    QVERIFY(writeFile(payloadPath, payload4));
    imgpayload::PayloadInfo info4;
    QVERIFY(imgpayload::parseManifest(payload4, info4));
    err.clear();
    QVERIFY(!imgpayload::extractPartitionStream(payloadPath, info4.partitions[0], outPath, {}, &err));
    QVERIFY(err.contains("数据越界"));
    QVERIFY(!QFile::exists(outPath));
    // 旧接口对同一截断 payload 报同样错误（错误语义一致）
    QString oerr;
    QVERIFY(imgpayload::extractPartition(payload4, info4.partitions[0], QByteArray(), &oerr, bs).isEmpty());
    QVERIFY(oerr.contains("数据越界"));

    // 5) block_size=0 → 拒绝，不崩溃
    err.clear();
    QVERIFY(!imgpayload::extractPartitionStream(payloadPath, info4.partitions[0], outPath, {}, &err, QString(), 0));
    QVERIFY(err.contains("block_size"));
}

void TestPayload::extractStreamAllocGuards()
{
    // 审查修复: 不可信输入不得崩溃 —— 稀疏文件可伪造逻辑大小骗过"越界"检查，超大声明
    // 必须在上限处被拒（false + error）；病态输入的错误消息与旧接口对齐。
    const quint64 bs = 4096;
    const quint64 g4 = 4ull * 1024 * 1024 * 1024;
    auto build = [&](const QByteArray &partBytes, const QByteArray &blobArea) {
        QByteArray manifest = pbwire::encodeVarint(3, bs) + pbwire::encodeMessage(13, partBytes);
        QByteArray payload;
        payload.append("CrAU");
        putU64(payload, 2);
        putU64(payload, static_cast<quint64>(manifest.size()));
        for (int i = 0; i < 4; ++i) payload.append(char(0));
        payload.append(manifest);
        payload.append(blobArea);
        return payload;
    };
    // 写内容后用同一句柄稀疏扩展（QFile 的 WriteOnly 打开隐含截断，另开句柄会清空已写内容）
    auto writeAndExtend = [](const QString &path, const QByteArray &content, qint64 logicalSize) {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return false;
        if (f.write(content) != content.size())
            return false;
        return f.resize(logicalSize); // Linux 稀疏文件: 逻辑大、物理小
    };

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString payloadPath = dir.filePath("payload.bin");
    const QString outPath = dir.filePath("out.img");
    const QString oldPath = dir.filePath("old.img");

    // 1) 压缩系 op 声明 4GiB blob（文件稀疏扩展到 6GiB 通过越界检查）→ 分配上限拒绝，不崩溃
    {
        QByteArray op = pbwire::encodeVarint(1, imgpayload::OP_REPLACE_BZ)
                      + pbwire::encodeVarint(2, 0)
                      + pbwire::encodeVarint(3, g4)
                      + pbwire::encodeMessage(6, extentMsg(0, 1));
        QByteArray part = pbwire::encodeBytes(1, QByteArray("system")) + pbwire::encodeMessage(8, op);
        const QByteArray payload = build(part, QByteArray(16, '\xAB'));
        QVERIFY(writeAndExtend(payloadPath, payload, 6LL * 1024 * 1024 * 1024));
        imgpayload::PayloadInfo info;
        QVERIFY(imgpayload::parseManifest(payload, info));
        QString err;
        QVERIFY(!imgpayload::extractPartitionStream(payloadPath, info.partitions[0], outPath, {}, &err));
        QVERIFY(err.contains("上限"));
        QVERIFY(!QFile::exists(outPath));
    }

    // 2) SOURCE_BSDIFF 旧片段声明 4GiB（旧镜像稀疏扩展到 6GiB）→ 分配上限拒绝，不崩溃
    {
        const QByteArray patch(64, '\x01');
        QByteArray op = pbwire::encodeVarint(1, imgpayload::OP_SOURCE_BSDIFF)
                      + pbwire::encodeVarint(2, 0)
                      + pbwire::encodeVarint(3, static_cast<quint64>(patch.size()))
                      + pbwire::encodeMessage(4, extentMsg(0, 1024 * 1024)) // 4GiB 旧片段
                      + pbwire::encodeMessage(6, extentMsg(0, 1024 * 1024))
                      + pbwire::encodeBytes(8, QCryptographicHash::hash(patch, QCryptographicHash::Sha256));
        QByteArray part = pbwire::encodeBytes(1, QByteArray("boot")) + pbwire::encodeMessage(8, op);
        const QByteArray payload = build(part, patch);
        QVERIFY(writeAndExtend(payloadPath, payload, 6LL * 1024 * 1024 * 1024));
        QVERIFY(writeAndExtend(oldPath, QByteArray(4096, '\x00'), 6LL * 1024 * 1024 * 1024));
        imgpayload::PayloadInfo info;
        QVERIFY(imgpayload::parseManifest(payload, info));
        QString err;
        QVERIFY(!imgpayload::extractPartitionStream(payloadPath, info.partitions[0], outPath, {}, &err,
                                                    oldPath, bs));
        QVERIFY(err.contains("过大")); // 旧片段分配上限（kMaxOpAlloc）
        QVERIFY(!QFile::exists(outPath));
    }

    // 3) manifest_size 声明 4GiB（稀疏文件通过越界检查）→ parseManifestFile false，不崩溃
    {
        QByteArray head;
        head.append("CrAU");
        putU64(head, 2);
        putU64(head, g4); // manifest_size = 4GiB
        for (int i = 0; i < 4; ++i) head.append(char(0));
        QVERIFY(writeAndExtend(payloadPath, head, 6LL * 1024 * 1024 * 1024));
        imgpayload::PayloadInfo info;
        QVERIFY(!imgpayload::parseManifestFile(payloadPath, info));
    }

    // 4) M1: 空 blob + hash 不匹配的 REPLACE → 与旧接口一致先报 "SHA-256 校验失败"
    {
        QByteArray op = pbwire::encodeVarint(1, imgpayload::OP_REPLACE)
                      + pbwire::encodeVarint(2, 0)
                      + pbwire::encodeVarint(3, 0) // data_length = 0
                      + pbwire::encodeMessage(6, extentMsg(0, 1))
                      + pbwire::encodeBytes(8, QByteArray(32, '\x42'));
        QByteArray part = pbwire::encodeBytes(1, QByteArray("boot")) + pbwire::encodeMessage(8, op);
        const QByteArray payload = build(part, {});
        QVERIFY(writeFile(payloadPath, payload));
        imgpayload::PayloadInfo info;
        QVERIFY(imgpayload::parseManifest(payload, info));
        QString oldErr;
        QVERIFY(imgpayload::extractPartition(payload, info.partitions[0], QByteArray(), &oldErr, bs).isEmpty());
        QString err;
        QVERIFY(!imgpayload::extractPartitionStream(payloadPath, info.partitions[0], outPath, {}, &err));
        QCOMPARE(err, oldErr); // 与旧接口同消息（SHA-256 校验失败）
        QVERIFY(!QFile::exists(outPath));
    }

    // 5) M2: 病态块号（startBlock/numBlocks ≈ 2^51，合计超预算）→ 与旧接口一致报
    //    "extent 超出输出/数据范围"（而非"extent 越界"）
    {
        const quint64 hugeBlock = (1ull << 51) + 1;
        QByteArray op = pbwire::encodeVarint(1, imgpayload::OP_REPLACE)
                      + pbwire::encodeVarint(2, 0)
                      + pbwire::encodeVarint(3, static_cast<quint64>(bs))
                      + pbwire::encodeMessage(6, extentMsg(hugeBlock, hugeBlock));
        QByteArray part = pbwire::encodeBytes(1, QByteArray("boot")) + pbwire::encodeMessage(8, op);
        const QByteArray payload = build(part, QByteArray(static_cast<int>(bs), '\xAB'));
        QVERIFY(writeFile(payloadPath, payload));
        imgpayload::PayloadInfo info;
        QVERIFY(imgpayload::parseManifest(payload, info));
        QString oldErr;
        QVERIFY(imgpayload::extractPartition(payload, info.partitions[0], QByteArray(), &oldErr, bs).isEmpty());
        QString err;
        QVERIFY(!imgpayload::extractPartitionStream(payloadPath, info.partitions[0], outPath, {}, &err));
        QCOMPARE(err, oldErr); // 与旧接口同消息（extent 超出输出/数据范围）
        QVERIFY(!QFile::exists(outPath));
    }
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
