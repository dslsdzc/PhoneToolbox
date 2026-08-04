#include "compressor.h"
#include "zstd_wrapper.h"

namespace imgcomp {

QByteArray compress(Type t, const QByteArray &data)
{
    switch (t) {
    case Type::Zstd: return zstdCompress(data);
    default: return {};
    }
}

QByteArray decompress(Type t, const QByteArray &data)
{
    switch (t) {
    case Type::Zstd: return zstdDecompress(data);
    default: return {};
    }
}

} // namespace imgcomp
