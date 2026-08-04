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

struct InstallOp {
    int type = OP_REPLACE;
    quint64 dataOffset = 0;
    quint64 dataLength = 0;
    QByteArray dataHash; // data_sha256_hash
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

} // namespace imgpayload
