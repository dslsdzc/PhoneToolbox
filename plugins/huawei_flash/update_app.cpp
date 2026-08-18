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
        // 首个 magic 前导容忍（行为观察：参考实现按字节扫描首个 magic，
        // 首个条目不假定从偏移 0 开始）：magic 不符时尝试 pos+1..pos+3，
        // 仍不符才报错
        int magicPos = pos;
        while (magicPos <= data.size() - 4 && magicPos - pos < 4
               && memcmp(data.constData() + magicPos, kMagic, 4) != 0)
            ++magicPos;
        // 窗口内（pos..pos+3）或数据耗尽处仍未找到 magic → 报错
        if (magicPos > data.size() - 4 || magicPos - pos >= 4) {
            if (error) *error = QStringLiteral("update.app 条目 magic 不符 @0x%1")
                                    .arg(pos, 0, 16);
            return false;
        }
        pos = magicPos;
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
        // 条目尾部可能带 0-3 字节 4 字节对齐填充（行为观察：每条目后
        // (4 - 当前位置%4) % 4 字节跳过）；dataLength==0/空名为列表结束标记
        const int dataEnd = dataOff + dataLen;
        int next = dataEnd;
        while (next + 4 <= data.size() && memcmp(data.constData() + next, kMagic, 4) != 0
               && next - dataEnd < 4)
            ++next; // 跳过最多 3 字节填充
        pos = next;
        // dataLength==0 或空名 → 列表结束（行为观察：解析头后即 break，
        // 该条目不追加）
        if (dataLen == 0 || p.name.isEmpty())
            break;
        p.data = data.mid(dataOff, dataLen);
        out.append(p);
    }
    if (out.isEmpty()) {
        if (error) *error = QStringLiteral("update.app 无分区条目");
        return false;
    }
    return true;
}

} // namespace hisi
