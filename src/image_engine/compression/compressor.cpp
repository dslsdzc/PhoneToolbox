#include "compressor.h"
#include "lz4_wrapper.h"
#include "zstd_wrapper.h"

namespace imgcomp {

QByteArray compress(Type t, const QByteArray &data)
{
    switch (t) {
    case Type::Zstd: return zstdCompress(data);
    case Type::Lz4: return lz4Compress(data);
    default: return {};
    }
}

QByteArray decompress(Type t, const QByteArray &data)
{
    switch (t) {
    case Type::Zstd: return zstdDecompress(data);
    case Type::Lz4: return lz4Decompress(data);
    default: return {};
    }
}

} // namespace imgcomp
