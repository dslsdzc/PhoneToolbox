#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <functional>

namespace imgpayload {

// InstallOperation.Type
enum OpType { OP_REPLACE = 0, OP_REPLACE_BZ = 1, OP_MOVE = 2, OP_BSDIFF = 3,
              OP_SOURCE_COPY = 4, OP_SOURCE_BSDIFF = 5, OP_ZERO = 6, OP_DISCARD = 7,
              OP_REPLACE_XZ = 8, OP_PUFFDIFF = 9, OP_BROTLI_BSDIFF = 10,
              OP_REPLACE_ZSTD = 11, OP_REPLACE_ZSTD_INC_WINDOW = 12, OP_ZUCCHINI = 13 };

// 块区间（Extent.start_block=1, num_blocks=2）
struct Extent {
    quint64 startBlock = 0;
    quint64 numBlocks = 0;
};

struct InstallOp {
    int type = OP_REPLACE;
    quint64 dataOffset = 0;
    quint64 dataLength = 0;
    quint64 dstLength = 0;   // dst_length（字段 7，update_metadata.proto）
    QByteArray dataHash;     // data_sha256_hash（字段 8，update_metadata.proto）
    QList<Extent> srcExtents; // 字段 4: SOURCE_* 的源块（旧镜像内）
    QList<Extent> dstExtents; // 字段 6: 输出目标块
};

struct Partition {
    QString name;
    QList<InstallOp> ops;
};

struct PayloadInfo {
    quint64 blockSize = 4096;
    QList<Partition> partitions;
    QByteArray manifestRaw;
};

bool isPayload(const QByteArray &header);                       // "CrAU"
bool parseManifest(const QByteArray &payload, PayloadInfo &out); // 读头+解析 manifest 字段
// 从文件解析 manifest（只读头 + manifest 区，不读 blob 区；GB 级 payload 流式路径用）。
// 与 parseManifest 结果一致（含 manifestRaw）。
bool parseManifestFile(const QString &payloadPath, PayloadInfo &out);

// 解包分区镜像: REPLACE 系从 blob 取数据；diff 系（SOURCE_COPY/SOURCE_BSDIFF）基于
// oldImage 与 src/dst_extents。失败返回空并填写 error。blockSize 传 PayloadInfo.blockSize
// （缺省 4096）。
QByteArray extractPartition(const QByteArray &payload, const Partition &part,
                            const QByteArray &oldImage, QString *error,
                            quint64 blockSize = 4096);

// ---- 流式解包（GB 级 payload 路径，内存 O(op)）----
// 与旧接口 extractPartition 语义逐字节一致（同 totalBase/落位/校验/错误信息）：
// payload 与输出均为文件 —— REPLACE 系从 payload 文件 seek 到 blob 位置；OP_REPLACE 边读边写
// （内存 O(chunk)），压缩系（BZ/XZ/ZSTD）blob 整体读入解压（内存 O(op)）；diff 系从
// oldImagePath 文件流式读取 src_extents 片段（SOURCE_COPY 边读边写；SOURCE_BSDIFF 片段需
// 驻留内存供 bspatch）。输出文件先预置为完整大小（ZERO/DISCARD 空洞读回为 0，与旧接口
// 零初始化一致），逐 op 按 dst_extents 落位（无 extent 的简化 manifest 回退 dataOffset/blockSize
// 连续映射）。
// oldImagePath 可为空：仅 REPLACE/ZERO 系分区可解，遇 diff 操作报"需要旧镜像"。
// progress(bytes): 已消费 blob 字节（累计 op.dataLength）/ 本分区总 blob 字节，单调递增
// [0, total]，可传空。
// 失败返回 false 且 error 非空（可传 nullptr 忽略）；失败时输出文件会被删除。
bool extractPartitionStream(const QString &payloadPath, const Partition &part,
                            const QString &outPath,
                            const std::function<void(quint64)> &progress = {},
                            QString *error = nullptr,
                            const QString &oldImagePath = {},
                            quint64 blockSize = 4096);

} // namespace imgpayload
