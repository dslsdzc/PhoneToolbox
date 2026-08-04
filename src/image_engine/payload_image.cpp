#include "payload_image.h"
#include "wire_format.h"
#include <QtEndian>

namespace imgpayload {

namespace {
quint64 readU64(const QByteArray &d, int off)
{
    return qFromLittleEndian<quint64>(reinterpret_cast<const uchar *>(d.constData() + off));
}
} // namespace

bool isPayload(const QByteArray &header)
{
    return header.size() >= 4 && header.left(4) == "CrAU";
}

bool parseManifest(const QByteArray &payload, PayloadInfo &out)
{
    if (!isPayload(payload) || payload.size() < 20)
        return false;
    const quint64 version = readU64(payload, 4);
    const quint64 manifestSize = readU64(payload, 12);
    int dataStart = 20 + (version >= 2 ? 4 : 0);
    if (dataStart > payload.size() || manifestSize > static_cast<quint64>(payload.size() - dataStart))
        return false;
    out.manifestRaw = payload.mid(dataStart, static_cast<int>(manifestSize));
    bool ok = false;
    const QList<pbwire::Field> manifest = pbwire::parseMessage(out.manifestRaw, ok);
    if (!ok)
        return false;
    for (const pbwire::Field &f : manifest) {
        if (f.number == 3 && f.wireType == 0)
            out.blockSize = f.varint;
        else if (f.number == 13 && f.wireType == 2) {
            bool pok = false;
            const QList<pbwire::Field> partFields = pbwire::parseMessage(f.bytes, pok);
            if (!pok)
                return false;
            Partition part;
            for (const pbwire::Field &pf : partFields) {
                if (pf.number == 1 && pf.wireType == 2)
                    part.name = QString::fromLatin1(pf.bytes);
                else if (pf.number == 8 && pf.wireType == 2) {
                    bool ook = false;
                    const QList<pbwire::Field> opFields = pbwire::parseMessage(pf.bytes, ook);
                    if (!ook)
                        return false;
                    InstallOp op;
                    for (const pbwire::Field &of : opFields) {
                        switch (of.number) {
                        case 1: if (of.wireType == 0) op.type = static_cast<int>(of.varint); break;
                        case 2: if (of.wireType == 0) op.dataOffset = of.varint; break;
                        case 3: if (of.wireType == 0) op.dataLength = of.varint; break;
                        case 7: if (of.wireType == 2) op.dataHash = of.bytes; break;
                        }
                    }
                    part.ops.append(op);
                }
            }
            out.partitions.append(part);
        }
    }
    return true;
}

} // namespace imgpayload
