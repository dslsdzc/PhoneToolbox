#include "root_patcher/cpio_util.h"

#include <cstdio>

namespace patcher {
namespace {

bool hex8Value(const char *p, quint32 *v)
{
    quint32 r = 0;
    for (int i = 0; i < 8; ++i) {
        const char c = p[i];
        r <<= 4;
        if (c >= '0' && c <= '9')
            r |= static_cast<quint32>(c - '0');
        else if (c >= 'a' && c <= 'f')
            r |= static_cast<quint32>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            r |= static_cast<quint32>(c - 'A' + 10);
        else
            return false;
    }
    *v = r;
    return true;
}

} // namespace

bool CpioArchive::parse(const QByteArray &buf, QString *err)
{
    auto fail = [err](const char *msg) {
        if (err)
            *err = QString::fromUtf8(msg);
        return false;
    };
    m_entries.clear();
    if (buf.size() < 110)
        return fail("cpio: 数据过短（非 cpio 镜像）"); // 合法 cpio 至少含一个 110B 头
    int pos = 0;
    while (pos + 110 <= buf.size()) {
        const QByteArray magic = buf.mid(pos, 6);
        if (magic != "070701" && magic != "070702")
            return fail("cpio: 魔数错误");
        quint32 mode, filesize, namesize;
        // 字段偏移：magic@0，ino@6，mode@14，...，filesize@54，...，namesize@94
        if (!hex8Value(buf.constData() + pos + 14, &mode) ||
            !hex8Value(buf.constData() + pos + 54, &filesize) ||
            !hex8Value(buf.constData() + pos + 94, &namesize))
            return fail("cpio: 头部字段损坏");
        const qint64 nameOff = static_cast<qint64>(pos) + 110;
        if (namesize == 0 || nameOff + namesize > buf.size())
            return fail("cpio: 名字越界");
        const QByteArray name = buf.mid(static_cast<int>(nameOff), namesize - 1);
        qint64 p = nameOff + namesize;
        p = (p + 3) & ~qint64(3);
        if (name == "." || name == "..") {
            pos = static_cast<int>(p);
            continue;
        }
        if (name == "TRAILER!!!") {
            // 多段 cpio 拼接：TRAILER 后搜索下一个 cpio 魔数
            const int next = buf.indexOf("070701", static_cast<int>(p));
            if (next < 0)
                break;
            pos = next;
            continue;
        }
        if (p + filesize > buf.size())
            return fail("cpio: 数据越界");
        CpioEntry e;
        e.mode = mode;
        e.data = buf.mid(static_cast<int>(p), static_cast<int>(filesize));
        m_entries.insert(QString::fromUtf8(name), e); // 同名替换（cpio add 语义）
        p += filesize;
        pos = static_cast<int>((p + 3) & ~qint64(3));
    }
    return true;
}

bool CpioArchive::exists(const QString &name) const
{
    return m_entries.contains(name);
}

bool CpioArchive::find(const QString &name, CpioEntry *out) const
{
    const auto it = m_entries.constFind(name);
    if (it == m_entries.constEnd())
        return false;
    if (out)
        *out = *it;
    return true;
}

bool CpioArchive::rename(const QString &from, const QString &to)
{
    const auto it = m_entries.constFind(from);
    if (it == m_entries.constEnd())
        return false;
    m_entries.insert(to, *it);
    m_entries.remove(from);
    return true;
}

void CpioArchive::addOrReplace(const QString &name, const CpioEntry &e)
{
    m_entries.insert(name, e);
}

bool CpioArchive::isMagiskPatched() const
{
    return m_entries.contains(QStringLiteral(".backup")) ||
           m_entries.contains(QStringLiteral(".backup/init"));
}

QByteArray CpioArchive::serialize() const
{
    QByteArray out;
    quint32 ino = 300000;
    char hdr[111]; // 110 字符 + NUL（与 Magisk cpio.cpp 的 header[111] 一致）
    auto putHeader = [&](quint32 mode, quint32 filesize, quint32 namesize) {
        std::snprintf(hdr, sizeof(hdr),
                      "070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x",
                      static_cast<unsigned>(ino++), static_cast<unsigned>(mode), 0u, 0u, 1u,
                      0u, static_cast<unsigned>(filesize), 0u, 0u, 0u, 0u,
                      static_cast<unsigned>(namesize), 0u);
        out.append(hdr, 110);
    };
    for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
        const QByteArray name = it.key().toUtf8();
        putHeader(it->mode, static_cast<quint32>(it->data.size()),
                  static_cast<quint32>(name.size()) + 1);
        out.append(name);
        out.append('\0');
        while (out.size() % 4)
            out.append('\0');
        if (!it->data.isEmpty()) {
            out.append(it->data);
            while (out.size() % 4)
                out.append('\0');
        }
    }
    putHeader(0755, 0, 11);
    out.append("TRAILER!!!\0", 11);
    while (out.size() % 4)
        out.append('\0');
    return out;
}

} // namespace patcher
