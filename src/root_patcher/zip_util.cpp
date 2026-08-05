#include "root_patcher/zip_util.h"

#include <QList>

#include <zlib.h>

namespace patcher {
namespace {

quint16 le16(const QByteArray &d, int pos)
{
    return static_cast<quint16>(static_cast<uchar>(d[pos])) |
           (static_cast<quint16>(static_cast<uchar>(d[pos + 1])) << 8);
}

quint32 le32(const QByteArray &d, int pos)
{
    return static_cast<quint32>(static_cast<uchar>(d[pos])) |
           (static_cast<quint32>(static_cast<uchar>(d[pos + 1])) << 8) |
           (static_cast<quint32>(static_cast<uchar>(d[pos + 2])) << 16) |
           (static_cast<quint32>(static_cast<uchar>(d[pos + 3])) << 24);
}

struct ZipEntry {
    QString name;
    quint16 method = 0; // 0=store 8=deflate
    quint32 compSize = 0;
    quint32 uncompSize = 0;
    quint32 localOff = 0;
};

// 从文件尾 64 KiB + EOCD 内扫描 EOCD（0x06054b50），校验注释长度落在文件尾。
bool findEocd(const QByteArray &zip, quint16 *entryCount, quint32 *cdSize, quint32 *cdOffset)
{
    if (zip.size() < 22) // 最小 EOCD 22B
        return false;
    const int scan = qMin(zip.size(), 65536 + 22);
    for (int i = zip.size() - 22; i >= zip.size() - scan; --i) {
        if (i < 0)
            break;
        if (le32(zip, i) == 0x06054b50u) {
            const quint16 commentLen = le16(zip, i + 20);
            if (i + 22 + commentLen == zip.size()) {
                *entryCount = le16(zip, i + 10);
                *cdSize = le32(zip, i + 12);
                *cdOffset = le32(zip, i + 16);
                return true;
            }
        }
    }
    return false;
}

// 解析中央目录。zip64 标记（entryCount/偏移为 0xFFFF/0xFFFFFFFF）明确报错：
// Magisk 系官方 APK 与 SuperSU 刷入包均为普通 ZIP（联网验证）。
bool loadZipEntries(const QByteArray &zip, QList<ZipEntry> *out, QString *err)
{
    auto fail = [err](const QString &msg) {
        if (err)
            *err = msg;
        return false;
    };
    quint16 count;
    quint32 cdSize, cdOffset;
    if (!findEocd(zip, &count, &cdSize, &cdOffset))
        return fail(QStringLiteral("ZIP: 未找到 EOCD 记录"));
    if (count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu)
        return fail(QStringLiteral("ZIP: zip64 归档暂不支持"));
    if (static_cast<quint64>(cdOffset) + cdSize > static_cast<quint64>(zip.size()))
        return fail(QStringLiteral("ZIP: 中央目录越界"));
    // 位置运算全程 qint64：cdOffset 可接近 INT_MAX（QByteArray 上限），
    // 与 46/nameLen 等相加时避免 int 溢出（理论边缘，防御性处理）
    qint64 pos = cdOffset;
    for (int i = 0; i < count; ++i) {
        if (pos + 46 > zip.size() || le32(zip, static_cast<int>(pos)) != 0x02014b50u)
            return fail(QStringLiteral("ZIP: 中央目录损坏"));
        const quint16 nameLen = le16(zip, static_cast<int>(pos) + 28);
        const quint16 extraLen = le16(zip, static_cast<int>(pos) + 30);
        const quint16 commentLen = le16(zip, static_cast<int>(pos) + 32);
        const qint64 entryEnd = pos + 46 + nameLen + extraLen + commentLen;
        if (entryEnd > zip.size())
            return fail(QStringLiteral("ZIP: 中央目录条目越界"));
        ZipEntry e;
        e.name = QString::fromUtf8(zip.constData() + static_cast<int>(pos) + 46, nameLen);
        e.method = le16(zip, static_cast<int>(pos) + 10);
        e.compSize = le32(zip, static_cast<int>(pos) + 20);
        e.uncompSize = le32(zip, static_cast<int>(pos) + 24);
        e.localOff = le32(zip, static_cast<int>(pos) + 42);
        out->append(e);
        pos = entryEnd;
    }
    return true;
}

// 按中央目录大小切片条目数据（对 bit3 data descriptor 同样成立），
// STORE 直取，DEFLATE 用 zlib 解压（输出大小以目录字段为准，防恶意头）。
bool extractEntryAt(const QByteArray &zip, const ZipEntry *found, QByteArray &out, QString *err)
{
    auto fail = [err](const QString &msg) {
        if (err)
            *err = msg;
        return false;
    };
    if (static_cast<quint64>(found->localOff) + 30 > static_cast<quint64>(zip.size()) ||
        le32(zip, static_cast<int>(found->localOff)) != 0x04034b50u)
        return fail(QStringLiteral("ZIP: 本地文件头损坏"));
    const quint16 lhNameLen = le16(zip, static_cast<int>(found->localOff) + 26);
    const quint16 lhExtraLen = le16(zip, static_cast<int>(found->localOff) + 28);
    const qint64 dataOff = static_cast<qint64>(found->localOff) + 30 + lhNameLen + lhExtraLen;
    if (dataOff + found->compSize > zip.size())
        return fail(QStringLiteral("ZIP: 条目数据越界"));
    const QByteArray comp = zip.mid(static_cast<int>(dataOff), static_cast<int>(found->compSize));
    if (found->method == 0) {
        out = comp;
        return true;
    }
    if (found->method == 8) {
        // raw DEFLATE：ZIP 条目是无 zlib 头的原始 deflate 流，zlib 的
        // uncompress()（期望 zlib 封装流）不适用，须用 inflateInit2(-MAX_WBITS)。
        // 用真实 Magisk APK / SuperSU 刷入包验证过（见 task-C3/C7 report）。
        // uncompSize 来自中央目录（攻击者可控）：64MB 上限防恶意 ZIP 强制
        // 分配内存致 bad_alloc terminate（真实条目：magiskinit ~200-500KB、
        // SuperSU su ~100KB、Superuser.apk ~6MB）
        if (found->uncompSize > 64u * 1024 * 1024)
            return fail(QStringLiteral("ZIP: 条目解压后过大"));
        QByteArray buf(static_cast<int>(found->uncompSize), Qt::Uninitialized);
        z_stream strm = {};
        if (inflateInit2(&strm, -MAX_WBITS) != Z_OK)
            return fail(QStringLiteral("ZIP: inflate 初始化失败"));
        strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(comp.constData()));
        strm.avail_in = static_cast<uInt>(comp.size());
        strm.next_out = reinterpret_cast<Bytef *>(buf.data());
        strm.avail_out = static_cast<uInt>(buf.size());
        const int rc = inflate(&strm, Z_FINISH);
        const bool streamOk = (rc == Z_STREAM_END);
        const int produced = static_cast<int>(buf.size()) - static_cast<int>(strm.avail_out);
        inflateEnd(&strm);
        if (!streamOk)
            return fail(QStringLiteral("ZIP: DEFLATE 解压失败"));
        out = buf.left(produced);
        return true;
    }
    return fail(QStringLiteral("ZIP: 不支持的压缩方法 %1").arg(found->method));
}

} // namespace

bool isZip(const QByteArray &data)
{
    return data.size() >= 4 && static_cast<uchar>(data[0]) == 'P' &&
           static_cast<uchar>(data[1]) == 'K' && static_cast<uchar>(data[2]) == 0x03 &&
           static_cast<uchar>(data[3]) == 0x04;
}

bool zipEntryNames(const QByteArray &zip, QStringList *out, QString *err)
{
    QList<ZipEntry> entries;
    if (!loadZipEntries(zip, &entries, err))
        return false;
    out->clear();
    for (const auto &e : entries)
        out->append(e.name);
    return true;
}

bool extractZipEntry(const QByteArray &zip, const QString &entryName, QByteArray &out,
                     QString *err)
{
    auto fail = [err](const QString &msg) {
        if (err)
            *err = msg;
        return false;
    };
    QList<ZipEntry> entries;
    if (!loadZipEntries(zip, &entries, err))
        return false;
    for (const auto &e : entries) {
        if (e.name == entryName)
            return extractEntryAt(zip, &e, out, err);
    }
    return fail(QStringLiteral("ZIP: 未找到条目 %1").arg(entryName));
}

bool extractZipEntryByPrefix(const QByteArray &zip, const QString &prefix, QByteArray &out,
                             QString *err)
{
    auto fail = [err](const QString &msg) {
        if (err)
            *err = msg;
        return false;
    };
    QList<ZipEntry> entries;
    if (!loadZipEntries(zip, &entries, err))
        return false;
    for (const auto &e : entries) {
        if (e.name.startsWith(prefix))
            return extractEntryAt(zip, &e, out, err);
    }
    return fail(QStringLiteral("ZIP: 未找到 %1 前缀条目").arg(prefix));
}

} // namespace patcher
