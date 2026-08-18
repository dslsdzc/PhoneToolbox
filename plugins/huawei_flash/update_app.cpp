#include "update_app.h"

#include <climits>
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
    // 明确上限：int 偏移解析器仅支持 ≤ 2 GiB 总量（完整固件 update.app
    // 多为多 GB —— 超限显式拒绝而非回绕 OOB；流式解析为后续任务）
    if (data.size() > qsizetype(INT_MAX)) {
        if (error) *error = QStringLiteral("update.app 过大（>2GiB 暂不支持，流式解析为后续任务）");
        return false;
    }
    int pos = 0;
    while (pos <= data.size() - 4) {
        if (memcmp(data.constData() + pos, kMagic, 4) != 0) {
            if (error) *error = QStringLiteral("update.app 条目 magic 不符 @0x%1")
                                    .arg(pos, 0, 16);
            return false;
        }
        // 恶意/损坏文件防御：长度字段来自文件（不可信），拒绝 > INT_MAX 与越界
        // （项目先例：len > INT_MAX 拒绝，7ad80f8）
        const int headerLen = int(le32(data, pos + 4));
        // data.size() - pos 为 qsizetype 比较：≥ 2 GiB 的 update.app 不再被
        // int 截断误拒（超 2 GiB 总量已由入口守卫显式拒绝）
        if (headerLen < kHeaderFixed || headerLen > data.size() - pos) {
            if (error) *error = QStringLiteral("update.app 条目头长非法 @0x%1")
                                    .arg(pos, 0, 16);
            return false;
        }
        const int dataOff = pos + headerLen;
        AppPartition p;
        p.header = data.mid(pos, headerLen);
        // 分区名：头内偏移 60（magic 4 + headerLen 4 + 4 + 8 + 4 + dataLen 4
        // + 16 + 16 = 60，行为观察核实）
        const QByteArray nameRaw = p.header.mid(60, 32);
        const int nul = nameRaw.indexOf('\0');
        p.name = QString::fromUtf8(nul >= 0 ? nameRaw.left(nul) : nameRaw);
        // 恶意/损坏文件防御：数据长度字段同样不可信，拒绝 > INT_MAX 与越界
        // （项目先例：len > INT_MAX 拒绝，7ad80f8）；分区名已取，错误串携带真名
        const quint64 dataLen64 = le32(data, pos + 24); // magic+4+4+8+4 = 偏移 24
        if (dataLen64 > quint64(INT_MAX)
            || dataLen64 > quint64(data.size()) - quint64(dataOff)) {
            if (error) *error = QStringLiteral("update.app 分区数据越界: %1").arg(p.name);
            return false;
        }
        const int dataLen = int(dataLen64);
        if (p.name.isEmpty()) {
            if (error) *error = QStringLiteral("update.app 条目名为空 @0x%1").arg(pos, 0, 16);
            return false;
        }
        p.data = data.mid(dataOff, dataLen);
        out.append(p);
        pos = dataOff + dataLen;
    }
    if (out.isEmpty()) {
        if (error) *error = QStringLiteral("update.app 无分区条目");
        return false;
    }
    return true;
}

} // namespace hisi
