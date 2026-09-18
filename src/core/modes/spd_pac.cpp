#include "core/modes/spd_pac.h"

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

} // namespace spd
