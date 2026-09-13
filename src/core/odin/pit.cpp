// src/core/odin/pit.cpp
//
// PIT 解析（纯函数，无 IO 依赖；parsePitFile 只是薄外壳）。
// 布局依据：reference/heimdall/libpit/source/libpit.h:43-303（头 28B / 条目 132B / 小端）、
// reference/thor/TheAirBlow.Thor.Library/PIT/PitData.cs:19-54（头部字段语义）、
// reference/samloader-rs/pit/src/lib.rs:27-33,79-93,155-160（UFS 枚举与扇区换算）。
// 真样本实证：docs/superpowers/specs/samsung-odin-facts.md §3.3（9+1 个 PIT 的魔数/条目数/尾部/CRLF）。
#include "pit.h"

#include <QFile>

namespace odin {
namespace {

constexpr quint32 kPitMagic = 0x12349876u;
constexpr int kHeaderSize = 28;
constexpr int kEntrySize = 132;

quint32 rdU32(const QByteArray &d, int off)
{
    return quint32(quint8(d.at(off)))
         | (quint32(quint8(d.at(off + 1))) << 8)
         | (quint32(quint8(d.at(off + 2))) << 16)
         | (quint32(quint8(d.at(off + 3))) << 24);
}

quint16 rdU16(const QByteArray &d, int off)
{
    return quint16(quint8(d.at(off)) | (quint16(quint8(d.at(off + 1))) << 8));
}

void setErr(QString *error, const QString &msg)
{
    if (error) *error = msg;
}

QString hex32(quint32 v)
{
    return QStringLiteral("0x%1").arg(v, 8, 16, QLatin1Char('0'));
}

} // namespace

QString cleanPitString(const QByteArray &field)
{
    // 真数据形态（事实报告 §3.3.9）：NUL 填充 + 字面 CR/LF（"remained\r\n"）。
    QByteArray cut = field;
    const int nul = cut.indexOf('\0');
    if (nul >= 0)
        cut.truncate(nul);
    // Qt6 的 QByteArray 没有 remove(char)/remove(QByteArrayView)（任务书伪码是 Qt5 写法），
    // 用 replace(char, QByteArrayView) 替换为空串 —— 语义相同：删除所有 CR/LF。
    cut.replace('\r', "");
    cut.replace('\n', "");
    return QString::fromLatin1(cut).trimmed();
}

quint64 PitEntry::partitionBytes() const
{
    // 扇区单位按 deviceType 换算（samloader-rs pit/src/lib.rs:155-160：MMC→512、UFS→4096）；
    // deviceType 只到 3 的老枚举一律按 512（Heimdall 的 OneNand/File/MMC/All 都是这块老语境）。
    const quint64 unit = (deviceType == 8) ? 4096 : 512;
    return quint64(blockCount) * unit;
}

bool PitEntry::hasImageName() const
{
    // 真数据里 "-" 是占位符（samsung-sm-j110h-j1xlte-LSI3475.pit 的 PIT/MD5HDR/BOTA0/BOTA1/OTA/RESERVED2）
    if (flashFilename.isEmpty())
        return false;
    return flashFilename != QLatin1String("-");
}

const PitEntry *PitTable::findByName(const QString &name) const
{
    const int i = indexOfName(name);
    return i < 0 ? nullptr : &entries.at(i);
}

int PitTable::indexOfName(const QString &name) const
{
    for (int i = 0; i < entries.size(); ++i)
        if (entries.at(i).partitionName.compare(name, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}

bool parsePit(const QByteArray &data, PitTable &out, QString *error)
{
    out = PitTable{};
    if (data.size() < kHeaderSize) {
        setErr(error, QStringLiteral("PIT 数据太短（%1 字节，头部需要 %2）").arg(data.size()).arg(kHeaderSize));
        return false;
    }
    const quint32 magic = rdU32(data, 0);
    if (magic != kPitMagic) {
        setErr(error, QStringLiteral("PIT 魔数不符：读到 %1，期望 0x12349876").arg(hex32(magic)));
        return false;
    }
    const quint32 count = rdU32(data, 4);
    if (count == 0) {
        setErr(error, QStringLiteral("PIT 条目数为 0（无意义，拒绝）"));
        return false;
    }
    // 用 64 位算所需长度：count 是攻击者可控值，32 位会溢出
    const quint64 need = quint64(kHeaderSize) + quint64(count) * quint64(kEntrySize);
    if (quint64(data.size()) < need) {
        setErr(error, QStringLiteral("PIT 截断：声明 %1 条目（需要 %2 字节），实际只有 %3 字节")
                          .arg(count).arg(need).arg(data.size()));
        return false;
    }

    out.comTar2 = data.mid(8, 8);
    while (out.comTar2.endsWith('\0'))
        out.comTar2.chop(1);
    out.cpuBlId = data.mid(16, 8);
    while (out.cpuBlId.endsWith('\0'))
        out.cpuBlId.chop(1);
    // 头 24..25 = lu_count、26..27 = reserved —— **不当 padding**（Heimdall 把它们读成 unknown7/unknown8，
    // 见事实报告 §1.3.1）。真数据 J1POP3G=0 / SM-Q7MQ=4 → 只记不拒。
    out.luCount = rdU16(data, 24);
    out.reserved = rdU16(data, 26);
    // 尾部签名块：长度不定（真样本 256..1024B），**原样收、不解释**，只记长度。
    // 要求 filesize == 28+count*132 或尾部全零的解析器会在全部真样本上失败（事实报告 §3.3）。
    out.trailingBytes = quint64(data.size()) - need;

    out.entries.reserve(int(count));
    for (quint32 i = 0; i < count; ++i) {
        const int o = kHeaderSize + int(i) * kEntrySize;
        PitEntry e;
        e.binaryType        = rdU32(data, o + 0);
        e.deviceType        = rdU32(data, o + 4);
        e.identifier        = rdU32(data, o + 8);
        e.attributes        = rdU32(data, o + 12);
        e.updateAttributes  = rdU32(data, o + 16);
        e.blockSizeOrOffset = rdU32(data, o + 20);
        e.blockCount        = rdU32(data, o + 24);
        e.fileOffset        = rdU32(data, o + 28);
        e.fileSize          = rdU32(data, o + 32);
        e.partitionName     = cleanPitString(data.mid(o + 36, 32));
        e.flashFilename     = cleanPitString(data.mid(o + 68, 32));
        e.fotaFilename      = cleanPitString(data.mid(o + 100, 32));
        out.entries.append(e);
    }
    return true;
}

bool parsePitFile(const QString &path, PitTable &out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        setErr(error, QStringLiteral("无法打开 PIT 文件：%1").arg(path));
        return false;
    }
    const QByteArray data = f.readAll();
    if (!parsePit(data, out, error)) {
        if (error)
            *error = QStringLiteral("%1：%2").arg(path, *error);
        return false;
    }
    return true;
}

} // namespace odin
