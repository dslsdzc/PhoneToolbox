#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

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

// 解包分区镜像: REPLACE 系从 blob 取数据；diff 系（SOURCE_COPY/SOURCE_BSDIFF）基于
// oldImage 与 src/dst_extents。失败返回空并填写 error。blockSize 传 PayloadInfo.blockSize
// （缺省 4096）。
QByteArray extractPartition(const QByteArray &payload, const Partition &part,
                            const QByteArray &oldImage, QString *error,
                            quint64 blockSize = 4096);

} // namespace imgpayload
