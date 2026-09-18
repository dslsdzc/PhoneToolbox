#include "core/modes/spd_pac.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include "image_engine/pac_image.h"

namespace spd {
namespace {

bool fail(QString *error, const QString &msg)
{
    if (error)
        *error = msg;
    return false;
}

// FDL 代次细分：isFdlPartition 只判"是不是 FDL"（fdl / fdl1 / fdl2），**不区分代次** ——
// 这里按名字含 fdl1 / fdl2 细分（同样大小写不敏感，与 isFdlPartition 同口径；不另立判据）。
// 裸 "fdl"（两个都不匹配）不猜：猜错等于把 FDL2 的字节当 FDL1 上传（刷写链按各自加载地址
// 与握手时序用它），风险高于让用户手选 —— 按 spec §3.2 回退到手选即可。
enum class FdlKind { None, Fdl1, Fdl2 };

FdlKind classifyFdl(const QString &partitionName)
{
    if (!isFdlPartition(partitionName))
        return FdlKind::None;
    const QString n = partitionName.toLower();
    if (n.contains(QLatin1String("fdl1")))
        return FdlKind::Fdl1;
    if (n.contains(QLatin1String("fdl2")))
        return FdlKind::Fdl2;
    return FdlKind::None;
}

// 包内条目摘要（前若干个）：提取失败时文案要"可行动"—— 用户据此判断是不是选错了包
// （例如选了个不含 FDL 的整包），或回退手选。带上容器内文件名（分区 ID 命名不一时
// 仍可辨认，如 "preloader（fdl1.bin）"）。截断上限避免条目多的包把错误文案撑爆
//（口径同 eub_payload 的 listingOf）。
QString listingOf(const QList<imgpac::PacPartition> &parts)
{
    constexpr int kMaxListed = 8;
    QStringList shown;
    for (const imgpac::PacPartition &p : parts) {
        if (shown.size() >= kMaxListed)
            break;
        if (p.name.isEmpty()) {
            shown << (p.fileName.isEmpty() ? QStringLiteral("(无名条目)") : p.fileName);
            continue;
        }
        shown << (p.fileName.isEmpty() || p.fileName == p.name
                      ? p.name
                      : QStringLiteral("%1（%2）").arg(p.name, p.fileName));
    }
    if (shown.isEmpty())
        return QStringLiteral("包内没有任何条目");
    const QString tail = parts.size() > shown.size()
        ? QStringLiteral("，等 %1 个条目").arg(parts.size())
        : QStringLiteral("，共 %1 个条目").arg(parts.size());
    return QStringLiteral("包内条目：%1%2").arg(shown.join(QStringLiteral("、")), tail);
}

// ---- 落盘（writeExtractedFdls）用的私有件 ----

// 把包内名字洗成文件名：只取基名（QFileInfo 去掉路径成分，顺带处理 "a/../x" 这类），
// 再把路径分隔/控制字符与 Windows 保留字符替换为 '_' —— 名字来自外部包，不假设它干净。
QString sanitizeFileName(const QString &raw)
{
    QString out;
    out.reserve(raw.size());
    for (const QChar c : raw) {
        const ushort u = c.unicode();
        const bool bad = u < 0x20 || u == u'/' || u == u'\\' || u == u':' || u == u'*'
            || u == u'?' || u == u'"' || u == u'<' || u == u'>' || u == u'|';
        out.append(bad ? QLatin1Char('_') : c);
    }
    return out;
}

// 落盘文件名用的条目名：优先容器内文件名（用户在包里看得到的名字），为空回退分区 ID；
// 两者都不可用（空 / "." / ".."）时用 fdl.bin —— 此时调用方"两份重名"的分支会再加前缀区分。
QString tempFileNameFor(const ExtractedFdl &fdl)
{
    for (const QString &raw : {fdl.fileName, fdl.name}) {
        const QString base = sanitizeFileName(QFileInfo(raw).fileName());
        if (!base.isEmpty() && base != QLatin1String(".") && base != QLatin1String(".."))
            return base;
    }
    return QStringLiteral("fdl.bin");
}

// 写文件并查到"落盘确实成功"这一层：write() 的返回值 + close() 后的错误状态都要看 ——
// 磁盘满时数据可能先停在用户态缓冲，close() 才现形（本项就在 /tmp 满的机器上开发）。
bool writeFileChecked(const QString &path, const QByteArray &data, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("无法写入 FDL 文件 %1：%2").arg(path, f.errorString());
        return false;
    }
    const qint64 written = f.write(data);
    if (written != qint64(data.size()) || !f.flush() || f.error() != QFileDevice::NoError) {
        const QString why = f.errorString();
        f.close();
        if (error)
            *error = QStringLiteral("写入 FDL 文件 %1 不完整（%2 字节，实写 %3 字节）：%4")
                         .arg(path)
                         .arg(data.size())
                         .arg(written)
                         .arg(why);
        return false;
    }
    f.close();
    if (f.error() != QFileDevice::NoError) {
        if (error)
            *error = QStringLiteral("关闭 FDL 文件 %1 失败：%2").arg(path, f.errorString());
        return false;
    }
    return true;
}

} // namespace

bool isFdlPartition(const QString &partitionName)
{
    const QString n = partitionName.toLower();
    return n == QStringLiteral("fdl") || n == QStringLiteral("fdl1")
        || n == QStringLiteral("fdl2");
}

bool extractFdlsFromPac(const QString &pacPath, ExtractedFdl &fdl1, ExtractedFdl &fdl2,
                        QString *error)
{
    // 失败不留半份数据（仓内约定）：出参在入口清空，只有两个 FDL 都拿到才写入
    fdl1 = ExtractedFdl();
    fdl2 = ExtractedFdl();
    if (error)
        error->clear();

    QFile pacFile(pacPath);
    if (!pacFile.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("无法打开 pac：%1").arg(pacPath));
    const QString fileName = QFileInfo(pacPath).fileName();

    // 按 QFile::size() 有界读入而非 readAll：对 /dev/zero 一类 size()==0 的特殊文件，
    // readAll 会一直读到内存耗尽（口径同 eub_payload）。整包读入是解析器 API 决定的
    //（parsePac 收 QByteArray；runSpdFlash 同样整包读）。
    const qint64 fileSize = pacFile.size() > 0 ? pacFile.size() : 0;
    if (fileSize == 0)
        return fail(error, QStringLiteral("无法读取 %1（文件长度为 0 或无法确定大小；"
                                          "非普通文件请另存为普通文件后重试）").arg(fileName));
    const QByteArray pacData = pacFile.read(fileSize);
    if (qint64(pacData.size()) != fileSize)
        return fail(error, QStringLiteral("读取 %1 不完整（文件 %2 字节，实读 %3 字节）")
                                .arg(fileName).arg(fileSize).arg(pacData.size()));

    QList<imgpac::PacPartition> parts;
    QString parseErr;
    if (!imgpac::parsePac(pacData, parts, &parseErr))
        return fail(error, QStringLiteral("按 pac 解析 %1 失败：%2").arg(fileName, parseErr));

    // 按名归位（与条目出现顺序无关）。两个槽各自"先出现的有数据条目为准"：
    //   • 重名条目（包内有两个 FDL1）取先出现者 —— 顺序即打包工具写入顺序；
    //   • 0 字节条目不留位（它不含数据，无法上传）：后续有数据的同名条目可以顶上。
    // parsePac 已校验数据范围，故 mid 取出的长度必等于 p.size（同 runSpdFlash 口径）。
    ExtractedFdl found1, found2;
    bool have1 = false, have2 = false;
    bool zero1 = false, zero2 = false;   // 判名命中但 0 字节（文案里要单独说明）
    for (const imgpac::PacPartition &p : parts) {
        const FdlKind kind = classifyFdl(p.name);
        if (kind == FdlKind::None)
            continue;
        const bool isF1 = (kind == FdlKind::Fdl1);
        bool &have = isF1 ? have1 : have2;
        bool &zero = isF1 ? zero1 : zero2;
        ExtractedFdl &slot = isF1 ? found1 : found2;
        if (have)
            continue;
        if (p.size == 0) {
            zero = true;   // 先记"条目在但没数据"；后续有数据的同名条目可以顶上
            continue;
        }
        have = true;
        zero = false;
        slot.name = p.name;
        slot.fileName = p.fileName;
        slot.data = pacData.mid(qsizetype(p.offset), qsizetype(p.size));
    }

    if (!have1 || !have2) {
        // 缺哪个 + 包内有哪些条目（可行动）。0 字节的槽在括号里说清"条目在但没数据"，
        // 免得用户去包里翻一个明明存在的条目。
        QStringList missing;
        if (!have1) {
            missing << (zero1 ? QStringLiteral("FDL1（条目存在但为 0 字节，无数据可上传）")
                              : QStringLiteral("FDL1"));
        }
        if (!have2) {
            missing << (zero2 ? QStringLiteral("FDL2（条目存在但为 0 字节，无数据可上传）")
                              : QStringLiteral("FDL2"));
        }
        return fail(error, QStringLiteral("%1 里找不到可用的 %2（%3）")
                                .arg(fileName, missing.join(QStringLiteral(" 与 ")),
                                     listingOf(parts)));
    }

    fdl1 = found1;
    fdl2 = found2;
    return true;
}

bool writeExtractedFdls(const ExtractedFdl &fdl1, const ExtractedFdl &fdl2, const QString &dir,
                        QString &fdl1Path, QString &fdl2Path, QString *error)
{
    fdl1Path.clear();
    fdl2Path.clear();
    if (error)
        error->clear();

    if (fdl1.data.isEmpty() || fdl2.data.isEmpty())
        return fail(error, QStringLiteral("不落盘：提取结果含空载荷（FDL1 %1 字节 / FDL2 %2 字节）"
                                          "—— 空 FDL 无法上传")
                               .arg(fdl1.data.size()).arg(fdl2.data.size()));

    // 容器内文件名相同（两份都叫 fdl.bin）时加 FDL1-/FDL2- 前缀区分：同一路径会被后写的盖掉，
    // 通道随后把 FDL2 的字节按 FDL1 的时序/加载地址上传 —— 静默发错数据，比失败更糟。
    // 判重按大小写不敏感比较：为大小写不敏感文件系统（Windows/macOS，本仓有 Windows 目标）而比较 ——
    // "fdl.bin" 与 "FDL.BIN" 在那里落到同一路径，精确比较会漏判，症状同上。
    QString name1 = tempFileNameFor(fdl1);
    QString name2 = tempFileNameFor(fdl2);
    if (name1.compare(name2, Qt::CaseInsensitive) == 0) {
        name1.prepend(QStringLiteral("FDL1-"));
        name2.prepend(QStringLiteral("FDL2-"));
    }

    const QDir outDir(dir);
    if (!outDir.exists())
        return fail(error, QStringLiteral("FDL 落盘目录不存在或不是目录：%1").arg(dir));
    const QString path1 = outDir.filePath(name1);
    const QString path2 = outDir.filePath(name2);

    QString why;
    if (!writeFileChecked(path1, fdl1.data, &why)) {
        QFile::remove(path1);   // 可能已写出部分内容（写一半失败）——不留半份产物
        return fail(error, why);
    }
    if (!writeFileChecked(path2, fdl2.data, &why)) {
        QFile::remove(path1);
        QFile::remove(path2);
        return fail(error, why);
    }

    fdl1Path = path1;
    fdl2Path = path2;
    return true;
}

} // namespace spd
