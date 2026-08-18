#include "update_app.h"

#include <cstring>

namespace hisi {

namespace {
constexpr quint8 kMagic[4] = { 0x55, 0xAA, 0x5A, 0xA5 };
constexpr int kHeaderFixed = 98; // magic(4)+len(4)+4+8+4+dlen(4)+16+16+name(32)+6

quint32 le32(const QByteArray &b, int off)
{
    return quint32(quint8(b[off])) | (quint32(quint8(b[off + 1])) << 8)
         | (quint32(quint8(b[off + 2])) << 16) | (quint32(quint8(b[off + 3])) << 24);
}

} // namespace

bool parseUpdateApp(const QByteArray &data, QList<AppPartition> &out, QString *error)
{
    out.clear();
    int pos = 0;
    while (pos + 4 <= data.size()) {
        if (memcmp(data.constData() + pos, kMagic, 4) != 0) {
            if (error) *error = QStringLiteral("update.app 条目 magic 不符 @0x%1")
                                    .arg(pos, 0, 16);
            return false;
        }
        const int headerLen = int(le32(data, pos + 4));
        if (headerLen < kHeaderFixed || pos + headerLen > data.size()) {
            if (error) *error = QStringLiteral("update.app 条目头长非法 @0x%1")
                                    .arg(pos, 0, 16);
            return false;
        }
        const quint32 dataLen = le32(data, pos + 24); // magic+4+4+8+4 = 偏移 24
        AppPartition p;
        p.header = data.mid(pos, headerLen);
        // 分区名：头内偏移 56（magic 4 + headerLen 4 + 4 + 8 + 4 + dataLen 4 + 16 + 16）
        const QByteArray nameRaw = p.header.mid(56, 32);
        const int nul = nameRaw.indexOf('\0');
        p.name = QString::fromUtf8(nul >= 0 ? nameRaw.left(nul) : nameRaw);
        if (p.name.isEmpty()) {
            if (error) *error = QStringLiteral("update.app 条目名为空 @0x%1").arg(pos, 0, 16);
            return false;
        }
        const int dataOff = pos + headerLen;
        if (dataOff + int(dataLen) > data.size()) {
            if (error) *error = QStringLiteral("update.app 分区数据越界: %1").arg(p.name);
            return false;
        }
        p.data = data.mid(dataOff, int(dataLen));
        out.append(p);
        pos = dataOff + int(dataLen);
    }
    if (out.isEmpty()) {
        if (error) *error = QStringLiteral("update.app 无分区条目");
        return false;
    }
    return true;
}

} // namespace hisi
