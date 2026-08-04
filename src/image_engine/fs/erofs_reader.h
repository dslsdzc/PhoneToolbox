#pragma once
#include <QByteArray>

namespace imgerofs {

// 来自 Linux fs/erofs/erofs_fs.h 的 erofs_super_block（EROFS_SUPER_OFFSET=1024）
struct SuperBlock {
    quint32 blockSize = 4096;
    quint64 rootNid = 0;
    bool isLz4 = false;
};

bool isErofs(const QByteArray &image);            // magic 0xE0F5E1E2 @1024
bool parseSuper(const QByteArray &image, SuperBlock &out);

} // namespace imgerofs
