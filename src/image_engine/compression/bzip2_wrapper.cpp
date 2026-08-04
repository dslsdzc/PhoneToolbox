#include "bzip2_wrapper.h"
#include <bzlib.h>

namespace imgcomp {

QByteArray bzip2Compress(const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    unsigned int outSize = static_cast<unsigned int>(data.size() * 2 + 600);
    QByteArray out(outSize, Qt::Uninitialized);
    int r = BZ2_bzBuffToBuffCompress(out.data(), &outSize,
                                     const_cast<char *>(data.constData()),
                                     static_cast<unsigned int>(data.size()), 9, 0, 30);
    if (r != BZ_OK)
        return {};
    out.truncate(static_cast<int>(outSize));
    return out;
}

QByteArray bzip2Decompress(const QByteArray &data)
{
    if (data.isEmpty())
        return {};
    // 未知原始大小：从 1KB 起倍增重试
    unsigned int outSize = 1024;
    for (;;) {
        QByteArray out(outSize, Qt::Uninitialized);
        unsigned int bufSize = outSize;
        int r = BZ2_bzBuffToBuffDecompress(out.data(), &bufSize,
                                           const_cast<char *>(data.constData()),
                                           static_cast<unsigned int>(data.size()), 0, 0);
        if (r == BZ_OK) {
            out.truncate(static_cast<int>(bufSize));
            return out;
        }
        if (r != BZ_OUTBUFF_FULL || outSize > 512 * 1024 * 1024)
            return {};
        outSize *= 2;
    }
}

} // namespace imgcomp
