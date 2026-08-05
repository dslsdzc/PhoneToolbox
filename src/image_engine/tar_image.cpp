#include "tar_image.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QCryptographicHash>
#include <limits>
#include <new>

namespace imgtar {

namespace {
// 流式 I/O 复用读缓冲（内存 O(chunk)，与 G1/G2 的 1MB chunk 一致）
constexpr qint64 kIoChunk = 1024 * 1024;
// 尾部 MD5 校验行扫描窗口（校验行 ≤ 200B：32hex + 2 空格 + 名称 + \n，取 4KB 足够）
constexpr qint64 kFooterScanTail = 4096;

void setErr(QString *error, const QString &msg)
{
    if (error) *error = msg;
}

bool readExact(QIODevice *d, char *buf, qint64 len)
{
    qint64 got = 0;
    while (got < len) {
        const qint64 n = d->read(buf + got, len - got);
        if (n <= 0)
            return false;
        got += n;
    }
    return true;
}

bool writeAll(QIODevice *d, const char *buf, qint64 len)
{
    qint64 done = 0;
    while (done < len) {
        const qint64 n = d->write(buf + done, len - done);
        if (n <= 0)
            return false;
        done += n;
    }
    return true;
}

// 条目数据区对齐 512 后的长度（防御恶意 size 引发的 qint64 溢出）；溢出返回 -1
qint64 paddedSize(qint64 size)
{
    if (size <= 0)
        return 0;
    const qint64 rem = size % 512;
    if (rem == 0)
        return size;
    const qint64 pad = 512 - rem;
    return (size > std::numeric_limits<qint64>::max() - pad) ? -1 : size + pad;
}

// 不可信输入下的缓冲分配（对齐 G2 kMaxOpAlloc 模式：分配失败返回 false，不崩溃）
bool allocFixed(qint64 size, QByteArray &out, QString *error)
{
    try {
        out = QByteArray(size, Qt::Uninitialized);
    } catch (const std::bad_alloc &) {
        setErr(error, QStringLiteral("内存分配失败"));
        return false;
    }
    return true;
}

// 条目名穿越防护（对齐 worker safeJoin 语义的加强版）：前导 '/' 或含 ".." 成分 → 拒绝
bool safeTarName(const QString &name)
{
    if (name.isEmpty() || name.startsWith(QLatin1Char('/')))
        return false;
    const QStringList comps = name.split(QLatin1Char('/'));
    for (const QString &c : comps)
        if (c == QLatin1String(".."))
            return false;
    return true;
}

// 定位并校验尾部 MD5 校验行（流式，不整档读入）。返回:
//   1 = 无校验行（tarEnd 置 fileSize）
//   0 = 有校验行且 MD5 校验通过（tarEnd 置归档区结束位置，含尾双空块）
//  -1 = 有校验行但校验不符（error 已置）
//  -2 = IO / 内存错误（error 已置）
// 扫描语义对齐 verifyMd5Footer（自尾部向前找 [32hex][空格][空格]，取位置最大匹配），
// 额外要求校验行其余部分为可打印 ASCII 并以 '\n' 或 EOF 结束 —— 防 tar 数据内巧合
// 出现 32hex+空格 被误判为校验行而错误拒绝归档。
int scanMd5Footer(QFile &f, qint64 fileSize, qint64 &tarEnd, QString *error)
{
    static const QByteArray kHexChars("0123456789abcdefABCDEF");
    tarEnd = fileSize;
    const qint64 tailLen = qMin(fileSize, kFooterScanTail);
    if (tailLen <= 0)
        return 1;
    if (!f.seek(fileSize - tailLen)) {
        setErr(error, QStringLiteral("无法读取文件尾部"));
        return -2;
    }
    QByteArray tail;
    if (!allocFixed(tailLen, tail, error))
        return -2;
    if (!readExact(&f, tail.data(), tailLen)) {
        setErr(error, QStringLiteral("无法读取文件尾部"));
        return -2;
    }
    for (qint64 i = tailLen - 1; i >= 0; --i) {
        if (i + 34 > tailLen || tail.at(int(i + 32)) != ' ' || tail.at(int(i + 33)) != ' ')
            continue;
        bool hex = true;
        for (qint64 k = i; k < i + 32; ++k)
            if (!kHexChars.contains(tail.at(int(k)))) { hex = false; break; }
        if (!hex)
            continue;
        // 校验行名称部分：可打印 ASCII，以 '\n' 或 EOF 结束
        qint64 j = i + 34;
        for (; j < tailLen; ++j)
            if (tail.at(int(j)) == '\n')
                break;
        if (j == i + 34) // 名称为空
            continue;
        bool printable = true;
        for (qint64 k = i + 34; k < j; ++k) {
            const char c = tail.at(int(k));
            if (c < 0x20 || c > 0x7e) { printable = false; break; }
        }
        if (!printable)
            continue;
        if (j >= tailLen && tailLen < fileSize) // 名称可能被窗口截断，无法确认
            continue;
        const qint64 hashStart = fileSize - (tailLen - i);
        // 兼容旧 appendMd5Footer 变体：校验行前缀 '\n'（分隔符）不计入归档
        tarEnd = (i > 0 && tail.at(int(i - 1)) == '\n') ? hashStart - 1 : hashStart;
        const QByteArray hashHex = tail.mid(int(i), 32);
        // 流式计算 [0, tarEnd) 的 MD5
        if (!f.seek(0)) {
            setErr(error, QStringLiteral("无法读取文件"));
            return -2;
        }
        QCryptographicHash h(QCryptographicHash::Md5);
        QByteArray buf;
        if (!allocFixed(kIoChunk, buf, error))
            return -2;
        qint64 remain = tarEnd;
        while (remain > 0) {
            const qint64 want = qMin(remain, kIoChunk);
            if (!readExact(&f, buf.data(), want)) {
                setErr(error, QStringLiteral("文件被截断"));
                return -2;
            }
            h.addData(buf.constData(), int(want));
            remain -= want;
        }
        if (h.result().toHex() == hashHex)
            return 0;
        setErr(error, QStringLiteral("MD5 校验失败（尾部校验行与归档内容不符）"));
        return -1;
    }
    return 1;
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

// ---- Task G3: 流式接口（内存 O(chunk)）----

bool buildTarStream(const QStringList &inputFiles, const QString &outPath,
                    const std::function<void(quint64)> &progress, QString *error)
{
    // 1) 预扫描输入：条目名（重名拒绝，对齐 worker 契约）、类型、总字节（进度分母）
    struct Input { QString path, name, linkTarget; bool isDir = false, isSym = false; qint64 size = 0; };
    QList<Input> inputs;
    QSet<QString> seen;
    quint64 total = 0;
    for (const QString &p : inputFiles) {
        const QFileInfo fi(p);
        if (!fi.exists()) {
            setErr(error, QStringLiteral("输入文件不存在: %1").arg(p));
            return false;
        }
        Input in;
        in.path = p;
        in.name = fi.fileName();
        if (seen.contains(in.name)) {
            setErr(error, QStringLiteral("输入文件包含重名条目: %1（tar 内条目名须唯一）")
                       .arg(in.name));
            return false;
        }
        seen.insert(in.name);
        in.isSym = fi.isSymLink();
        in.isDir = fi.isDir();
        in.linkTarget = in.isSym ? fi.symLinkTarget() : QString();
        in.size = (in.isDir || in.isSym) ? 0 : fi.size();
        total += quint64(in.size);
        inputs.append(in);
    }
    // 2) 打开输出
    QFile out(outPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setErr(error, QStringLiteral("无法打开输出文件: %1").arg(outPath));
        return false;
    }
    const auto fail = [&](const QString &msg) {
        out.close();
        QFile::remove(outPath);
        setErr(error, msg);
        return false;
    };
    if (progress) progress(0);
    // 3) 逐输入写 ustar 头 + 流式拷数据（与旧接口 buildTar 逐字节一致）
    QByteArray buf;
    if (!allocFixed(kIoChunk, buf, error)) {
        out.close();
        QFile::remove(outPath);
        return false;
    }
    const QByteArray zeroBlock(512, '\0');
    quint64 done = 0;
    for (const Input &in : inputs) {
        const QString entryName =
            (in.isDir && !in.name.endsWith(QLatin1Char('/'))) ? in.name + QLatin1Char('/') : in.name;
        const char type = in.isSym ? '2' : (in.isDir ? '5' : '0');
        const QByteArray hdr = buildTarHeader(entryName, in.size, type, in.linkTarget);
        if (!writeAll(&out, hdr.constData(), hdr.size()))
            return fail(QStringLiteral("写入 tar 头失败: %1").arg(outPath));
        if (!in.isDir && !in.isSym) {
            QFile inFile(in.path);
            if (!inFile.open(QIODevice::ReadOnly))
                return fail(QStringLiteral("无法打开输入文件: %1").arg(in.path));
            qint64 remain = in.size;
            while (remain > 0) {
                const qint64 want = qMin(remain, kIoChunk);
                if (!readExact(&inFile, buf.data(), want))
                    return fail(QStringLiteral("读取输入文件失败: %1").arg(in.path));
                if (!writeAll(&out, buf.constData(), want))
                    return fail(QStringLiteral("写入 tar 失败: %1").arg(outPath));
                remain -= want;
                done += quint64(want);
                if (progress) progress(done);
            }
            if (in.size % 512) // 数据区按 512 对齐补零
                if (!writeAll(&out, zeroBlock.constData(), 512 - in.size % 512))
                    return fail(QStringLiteral("写入 tar 失败: %1").arg(outPath));
        }
    }
    // 4) 两个空块结尾（与旧接口一致）
    const QByteArray endBlocks(1024, '\0');
    if (!writeAll(&out, endBlocks.constData(), endBlocks.size()))
        return fail(QStringLiteral("写入 tar 失败: %1").arg(outPath));
    out.close();
    if (progress) progress(total);
    return true;
}

bool extractTarStream(const QString &tarPath, const QString &outDir,
                      const std::function<void(quint64)> &progress, QString *error)
{
    QFile in(tarPath);
    if (!in.open(QIODevice::ReadOnly)) {
        setErr(error, QStringLiteral("无法打开 tar 文件: %1").arg(tarPath));
        return false;
    }
    const qint64 fileSize = in.size();
    // 尾部 MD5 校验行（三星 .tar.md5）：有则流式校验，不符直接失败
    qint64 tarEnd = fileSize;
    if (scanMd5Footer(in, fileSize, tarEnd, error) < 0)
        return false;
    if (progress) progress(0);
    QByteArray hdrBuf;
    if (!allocFixed(512, hdrBuf, error))
        return false;
    QByteArray dataBuf;
    if (!allocFixed(kIoChunk, dataBuf, error))
        return false;
    const QByteArray zeroHeader(512, '\0');
    const auto fail = [&](const QString &msg) {
        in.close();
        setErr(error, msg);
        return false;
    };
    qint64 pos = 0;
    while (pos + 512 <= tarEnd) {
        if (!in.seek(pos) || !readExact(&in, hdrBuf.data(), 512))
            return fail(QStringLiteral("tar 文件被截断（头部读取失败）"));
        if (hdrBuf == zeroHeader)
            break; // 结束块
        const QByteArray name = hdrBuf.left(100).split('\0').first();
        if (name.isEmpty())
            break; // 与旧 extractTar 一致：空名视为归档结束
        bool sizeOk = false;
        const qint64 size = hdrBuf.mid(124, 12).trimmed().toLongLong(&sizeOk, 8);
        if (!sizeOk || size < 0)
            return fail(QStringLiteral("tar 条目 size 字段非法"));
        const char type = hdrBuf.at(156);
        const qint64 dataStart = pos + 512;
        if (safeTarName(QString::fromLatin1(name))) {
            if (type == '5') { // 目录
                const QString dir = QDir(outDir).filePath(QString::fromLatin1(name));
                if (!QDir().mkpath(dir))
                    return fail(QStringLiteral("无法创建目录: %1").arg(dir));
            } else if (type != '2') { // 普通文件：流式写盘；'2'（符号链接）不落盘（对齐 worker）
                if (size > tarEnd - dataStart)
                    return fail(QStringLiteral("tar 文件被截断（条目数据越界）"));
                const QString full = QDir(outDir).filePath(QString::fromLatin1(name));
                if (!QDir().mkpath(QFileInfo(full).absolutePath()))
                    return fail(QStringLiteral("无法创建目录: %1").arg(QFileInfo(full).absolutePath()));
                QFile o(full);
                if (!o.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    return fail(QStringLiteral("无法打开输出文件: %1").arg(full));
                qint64 remain = size;
                qint64 off = dataStart;
                bool writeFailed = false;
                while (remain > 0) {
                    const qint64 want = qMin(remain, kIoChunk);
                    if (!in.seek(off) || !readExact(&in, dataBuf.data(), want)) {
                        writeFailed = true;
                        break;
                    }
                    if (!writeAll(&o, dataBuf.constData(), want)) {
                        writeFailed = true;
                        break;
                    }
                    remain -= want;
                    off += want;
                    pos = off;
                    if (progress) progress(qMin(pos, tarEnd));
                }
                o.close();
                if (writeFailed) {
                    QFile::remove(full); // 部分产物删除（本接口拥有正在写入的输出文件）
                    return fail(QStringLiteral("tar 文件被截断（条目数据不完整）"));
                }
            }
        }
        // 跳过数据区（含 dir/symlink 的声明数据；对齐旧 extractTar 不对跳过条目做越界检查，
        // 超出归档尾部即终止）
        const qint64 padded = paddedSize(size);
        if (padded < 0)
            return fail(QStringLiteral("tar 条目大小超出支持范围"));
        pos = (padded > tarEnd - dataStart) ? tarEnd : dataStart + padded;
        if (progress) progress(qMin(pos, tarEnd));
    }
    in.close();
    if (progress) progress(tarEnd);
    return true;
}

bool appendMd5FooterStream(const QString &tarPath, QString *error)
{
    // 先写 tar 再追加：增量读算 MD5，再追加 "32hex  firmware.tar.md5\n"（与整读接口逐字节一致）
    QFile f(tarPath);
    if (!f.open(QIODevice::ReadOnly)) {
        setErr(error, QStringLiteral("无法打开文件: %1").arg(tarPath));
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Md5);
    QByteArray buf;
    if (!allocFixed(kIoChunk, buf, error))
        return false;
    qint64 got = 0;
    while ((got = f.read(buf.data(), buf.size())) > 0)
        hash.addData(buf.constData(), int(got));
    if (got < 0) {
        setErr(error, QStringLiteral("读取文件失败: %1").arg(tarPath));
        return false;
    }
    f.close();
    if (!f.open(QIODevice::Append | QIODevice::WriteOnly)) {
        setErr(error, QStringLiteral("无法打开文件追加: %1").arg(tarPath));
        return false;
    }
    const QByteArray line = hash.result().toHex() + QByteArray("  firmware.tar.md5\n");
    const bool ok = writeAll(&f, line.constData(), line.size());
    f.close();
    if (!ok)
        setErr(error, QStringLiteral("写入校验行失败: %1").arg(tarPath));
    return ok;
}

bool verifyMd5FooterStream(const QString &tarMd5Path, bool *hasFooter, QString *error)
{
    if (hasFooter)
        *hasFooter = false;
    QFile f(tarMd5Path);
    if (!f.open(QIODevice::ReadOnly)) {
        setErr(error, QStringLiteral("无法打开文件: %1").arg(tarMd5Path));
        return false;
    }
    qint64 tarEnd = f.size();
    const int ret = scanMd5Footer(f, f.size(), tarEnd, error);
    // hasFooter 语义：检测到校验行（无论校验是否通过）；IO 错误（-2）不算检测到
    if (hasFooter)
        *hasFooter = (ret == 0 || ret == -1);
    return ret != -1 && ret != -2;
}

} // namespace imgtar
