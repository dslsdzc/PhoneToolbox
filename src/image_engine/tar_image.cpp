#include "tar_image.h"
#include <QCryptographicHash>

namespace imgtar {

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
    // 真实三星 .tar.md5: [tar][32hex]  name\n —— 校验行直接接在归档后（md5sum 输出行），
    // 中间无空行；校验行带尾 '\n'。早期实现多插了一个 '\n' 分隔符，与 Odin 实际格式不符。
    return tar + hash + "  " + QByteArray("firmware.tar.md5") + '\n';
}

bool verifyMd5Footer(const QByteArray &tarMd5)
{
    // 兼容三种形态（校验行均在文件尾，由"32hex + 两个空格"定位，从尾部取最大匹配）:
    //   [tar][32hex]  name\n         真实三星 .tar.md5（appendMd5Footer 现产物）
    //   [tar]\n[32hex]  name         旧 appendMd5Footer 产物（分隔 \n，无尾 \n）
    //   [tar]\n[32hex]  name\n       分隔 \n + 尾 \n 变体
    // 校验行前缀若是 '\n'（旧格式分隔符）则不计入归档；tar 内部即使含 32hex+空格
    // 也不会误判（真实校验行在文件尾，匹配位置最大）。
    const QByteArray &s = tarMd5;
    for (int i = s.size() - 1; i >= 0; --i) {
        if (i + 34 > s.size() || s[i + 32] != ' ' || s[i + 33] != ' ')
            continue;
        bool hex = true;
        for (int k = i; k < i + 32; ++k)
            if (!QByteArray("0123456789abcdefABCDEF").contains(s[k])) { hex = false; break; }
        if (!hex)
            continue;
        const QByteArray hashHex = s.mid(i, 32);
        const int tarEnd = (i > 0 && s[i - 1] == '\n') ? i - 1 : i;
        const QByteArray tarPart = s.left(tarEnd);
        return QCryptographicHash::hash(tarPart, QCryptographicHash::Md5).toHex() == hashHex;
    }
    return false;
}

// ---- Task 11: tar 打包 (ustar) ----

namespace {
QByteArray toOctalField(qint64 value, int fieldLen)
{
    QByteArray s = QByteArray::number(value, 8).rightJustified(fieldLen - 1, '0');
    return s + ' ';
}

QByteArray buildTarHeader(const QString &name, qint64 size, char type,
                          const QString &linkTarget = QString())
{
    QByteArray hdr(512, 0);
    // 注意: QByteArray::replace(int,int,const char*/QByteArray) 为"删 len 插新串"语义,
    // 替换串短于 len 会缩短整个 QByteArray —— 必须按实际长度替换, 保持头块恒为 512 字节。
    QByteArray nb = name.toLatin1().left(100);
    hdr.replace(0, nb.size(), nb);
    hdr.replace(100, 8, toOctalField(0644, 8));
    hdr.replace(108, 8, toOctalField(0, 8));
    hdr.replace(116, 8, toOctalField(0, 8));
    hdr.replace(124, 12, toOctalField(size, 12));
    hdr.replace(136, 12, toOctalField(0, 12));
    hdr[156] = type;
    QByteArray lt = linkTarget.toLatin1().left(100);
    hdr.replace(157, lt.size(), lt); // 同 512 字节保持: 按实际长度替换
    hdr.replace(257, 6, QByteArray("ustar\0", 6));
    hdr.replace(263, 2, "00");
    // 校验和: chksum 字段先置 8 空格, 全部字节相加 (POSIX), 再写回。
    // 必须在所有字段(含 linkname)就位后计算, 否则符号链接项校验和会与实际头块不符。
    hdr.replace(148, 8, "        ");
    quint32 sum = 0;
    for (char c : hdr)
        sum += static_cast<uchar>(c);
    hdr.replace(148, 8, toOctalField(sum, 8).left(7) + '\0');
    return hdr;
}
} // namespace

QByteArray buildTar(const QList<TarEntry> &entries)
{
    QByteArray tar;
    for (const TarEntry &e : entries) {
        const bool isDir = e.isDir || e.name.endsWith('/');
        const QString name = isDir && !e.name.endsWith('/') ? e.name + '/' : e.name;
        const char type = e.isSymlink ? '2' : (isDir ? '5' : '0');
        QByteArray hdr = buildTarHeader(name, e.isSymlink ? 0 : e.data.size(), type,
                                        e.isSymlink ? e.linkTarget : QString());
        tar.append(hdr);
        if (!isDir && !e.isSymlink) {
            tar.append(e.data);
            if (e.data.size() % 512)
                tar.append(512 - e.data.size() % 512, '\0');
        }
    }
    tar.append(QByteArray(1024, 0)); // 两个空块
    return tar;
}

} // namespace imgtar
