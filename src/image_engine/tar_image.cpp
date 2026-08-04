#include "tar_image.h"
#include <QCryptographicHash>

namespace imgtar {

namespace {
bool readOctal(const QByteArray &s)
{
    // 八进制字段：数字后可跟空格或 \0
    bool ok = false;
    long long v = s.trimmed().toLongLong(&ok, 8);
    return ok ? v >= 0 : false;
}
} // namespace

bool extractTar(const QByteArray &tar, QList<TarEntry> &entries)
{
    entries.clear();
    qint64 pos = 0;
    while (pos + 512 <= tar.size()) {
        const QByteArray hdr = tar.mid(pos, 512);
        if (hdr == QByteArray(512, 0))
            break; // 结束块
        const QByteArray name = hdr.left(100).split('\0').first();
        if (name.isEmpty())
            break;
        bool sizeOk = false;
        const qint64 size = hdr.mid(124, 12).trimmed().toLongLong(&sizeOk, 8);
        if (!sizeOk || size < 0)
            return false;
        const char type = hdr[156];
        TarEntry e;
        e.name = QString::fromLatin1(name);
        e.isDir = (type == '5');
        e.isSymlink = (type == '2');
        if (e.isSymlink)
            e.linkTarget = QString::fromLatin1(hdr.mid(157, 100).split('\0').first());
        const qint64 dataStart = pos + 512;
        if (!e.isDir && !e.isSymlink) {
            if (dataStart + size > tar.size())
                return false;
            e.data = tar.mid(dataStart, size);
        }
        entries.append(e);
        pos = dataStart + ((size + 511) / 512) * 512;
    }
    return true;
}

QByteArray appendMd5Footer(const QByteArray &tar)
{
    const QByteArray hash = QCryptographicHash::hash(tar, QCryptographicHash::Md5).toHex();
    // 校验行作为最后一个 '\n' 之后的独立行：verifyMd5Footer 取最后一行做 32hex 校验，
    // 并对 '\n' 之前的部分计算 md5 —— 故归档与校验行之间需以 '\n' 分隔。
    return tar + '\n' + hash + "  " + QByteArray("firmware.tar.md5");
}

bool verifyMd5Footer(const QByteArray &tarMd5)
{
    // 尾部行格式: 32hex 空格 [*]名称
    const int nl = tarMd5.lastIndexOf('\n');
    const QByteArray lastLine = nl >= 0 ? tarMd5.mid(nl + 1).trimmed() : tarMd5.trimmed();
    if (lastLine.size() < 32)
        return false;
    const QByteArray hashHex = lastLine.left(32);
    for (char c : hashHex)
        if (!QByteArray("0123456789abcdefABCDEF").contains(c))
            return false;
    const QByteArray tarPart = nl >= 0 ? tarMd5.left(nl) : tarMd5;
    return QCryptographicHash::hash(tarPart, QCryptographicHash::Md5).toHex() == hashHex;
}

} // namespace imgtar
