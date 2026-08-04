#include "wire_format.h"

namespace pbwire {

QByteArray encodeVarintRaw(quint64 value)
{
    QByteArray out;
    while (value >= 0x80) {
        out.append(char((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.append(char(value));
    return out;
}

static bool readVarint(const QByteArray &data, int &pos, quint64 &out)
{
    out = 0;
    int shift = 0;
    for (int i = 0; i < 10; ++i) {
        if (pos >= data.size())
            return false;
        const uchar b = static_cast<uchar>(data[pos++]);
        out |= static_cast<quint64>(b & 0x7F) << shift;
        if (!(b & 0x80))
            return true;
        shift += 7;
    }
    return false;
}

QList<Field> parseMessage(const QByteArray &data, bool &ok)
{
    QList<Field> fields;
    ok = true;
    int pos = 0;
    while (pos < data.size()) {
        quint64 tag = 0;
        if (!readVarint(data, pos, tag)) { ok = false; return {}; }
        const int number = static_cast<int>(tag >> 3);
        const int wt = static_cast<int>(tag & 7);
        if (number == 0) { ok = false; return {}; }
        Field f;
        f.number = number;
        f.wireType = wt;
        switch (wt) {
        case 0:
            if (!readVarint(data, pos, f.varint)) { ok = false; return {}; }
            break;
        case 1:
            if (pos + 8 > data.size()) { ok = false; return {}; }
            f.bytes = data.mid(pos, 8); pos += 8;
            break;
        case 2: {
            quint64 len = 0;
            if (!readVarint(data, pos, len)) { ok = false; return {}; }
            if (len > static_cast<quint64>(data.size() - pos)) { ok = false; return {}; }
            f.bytes = data.mid(pos, static_cast<int>(len)); pos += static_cast<int>(len);
            break;
        }
        case 5:
            if (pos + 4 > data.size()) { ok = false; return {}; }
            f.bytes = data.mid(pos, 4); pos += 4;
            break;
        default:
            ok = false; return {};
        }
        fields.append(f);
    }
    return fields;
}

QByteArray encodeVarint(int number, quint64 value)
{
    return encodeVarintRaw((static_cast<quint64>(number) << 3) | 0) + encodeVarintRaw(value);
}

QByteArray encodeBytes(int number, const QByteArray &data)
{
    return encodeVarintRaw((static_cast<quint64>(number) << 3) | 2) + encodeVarintRaw(static_cast<quint64>(data.size())) + data;
}

QByteArray encodeMessage(int number, const QByteArray &message)
{
    return encodeBytes(number, message);
}

} // namespace pbwire
