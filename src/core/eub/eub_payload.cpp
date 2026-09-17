// src/core/eub/eub_payload.cpp
//
// 三条来源路径的实现顺序（spec §D8）：判"是不是 tar" → 索引并挑条目 → （需要时）LZ4 解压 →
// 非空校验。判据与文案的取值出处写在各自注释里。
#include "eub_payload.h"

#include <QFile>
#include <QFileInfo>
#include <QStringList>

#include "image_engine/compression/lz4_wrapper.h"
#include "image_engine/tar_image.h"

namespace eub {

namespace {

// tar 头块长度，且魔数落在 257..261（与 tar_image.cpp:631 同一处判据）
constexpr int kTarHeaderBytes = 512;

// 单次读入内存的上限：sboot.bin 是各 SoC 的 bootloader 镜像（表里最大段 0xD1000，facts §C2），
// 而 BL 整包可达数 GB —— 本函数对 tar 只读**一条目**（流式索引，spec §D8），故这层防护挡的是
// "误选了大文件"把 QByteArray 撑爆（int 上限 2GB），不是业务约束。与 lz4_wrapper.cpp 的
// contentSize 上限同量级。
constexpr quint64 kMaxPayloadBytes = 512ull * 1024 * 1024;

bool fail(QString *error, const QString &msg)
{
    if (error)
        *error = msg;
    return false;
}

// tar 条目名可能带目录前缀（"firmware/SBOOT.BIN"）→ 取最后一段；条目选择按它匹配
QString baseName(const QString &name)
{
    return name.section(QLatin1Char('/'), -1);
}

// 包内条目名摘要（前若干个）：找不到 sboot 时文案要"可行动"——用户据此判断是不是选错了包
// （例如误选 AP 包）。截断上限避免条目多的包把错误文案撑爆。
QString listingOf(const QList<imgtar::TarIndexEntry> &idx)
{
    constexpr int kMaxListed = 8;
    QStringList shown;
    for (const imgtar::TarIndexEntry &e : idx) {
        if (shown.size() >= kMaxListed)
            break;
        shown << e.name;
    }
    if (shown.isEmpty())
        return QStringLiteral("包内没有任何条目");
    const QString tail = idx.size() > shown.size()
        ? QStringLiteral("，等 %1 个条目").arg(idx.size())
        : QStringLiteral("，共 %1 个条目").arg(idx.size());
    return QStringLiteral("包内条目：%1%2").arg(shown.join(QStringLiteral("、")), tail);
}

// 源描述文案（三个模板见 spec §D8 的三条路径；tar 内条目以"<包名> 内的 <条目名>"呈现）
QString describe(const QString &fileName, const QString &entryInTar, bool compressed)
{
    if (!entryInTar.isEmpty())
        return compressed ? QStringLiteral("%1 内的 %2（已解压）").arg(fileName, entryInTar)
                          : QStringLiteral("%1 内的 %2（未压缩）").arg(fileName, entryInTar);
    return compressed ? QStringLiteral("%1（LZ4 解压后）").arg(fileName)
                      : QStringLiteral("%1（裸镜像）").arg(fileName);
}

// 在已打开的 tar 文件里定位并读出条目字节：上限守卫 → 定位读取 → 完整性核对。
// sizeGuardReason 是超限文案里那句"为什么拒"（sboot 与 extra 各有各的说法），其余文案两处共用。
bool readTarEntryBytes(QFile &f, const imgtar::TarIndexEntry &hit, const QString &fileName,
                       const QString &sizeGuardReason, QByteArray &out, QString *error)
{
    if (hit.size > kMaxPayloadBytes)
        return fail(error, QStringLiteral("%1 内的 %2 有 %3 字节 —— %4，疑为选错文件")
                                .arg(fileName, hit.name).arg(hit.size).arg(sizeGuardReason));
    if (!f.seek(qint64(hit.offset)))
        return fail(error, QStringLiteral("读取 %1 内的 %2 失败（偏移 %3）")
                                .arg(fileName, hit.name).arg(hit.offset));
    out = f.read(qint64(hit.size));
    if (quint64(out.size()) != hit.size)
        return fail(error, QStringLiteral("%1 内的 %2 读取不完整（要 %3 字节，实得 %4）")
                                .arg(fileName, hit.name).arg(hit.size).arg(out.size()));
    return true;
}

// 按**内容**判据（LZ4 frame 魔数）解压 —— 不是按条目名后缀：名字骗人（"x.img.lz4" 里是裸字节）
// 时按原样收下（用例钉住）。命中即解压，解压失败即失败。
// imgcomp::lz4Decompress 失败时返回**空数组且无 error 出参**（lz4_wrapper.h:6）——
// 空返回必须在这里判成失败，否则会以"零长度载荷"的名义静默通过。
bool decompressIfLz4(QByteArray &bytes, const QString &path, bool *compressed, QString *error)
{
    if (compressed)
        *compressed = false;
    if (!looksLikeLz4Frame(bytes))
        return true;
    const QByteArray plain = imgcomp::lz4Decompress(bytes);
    if (plain.isEmpty())
        return fail(error, QStringLiteral("LZ4 解压失败（LZ4 frame 魔数在，但内容截断或损坏）：%1")
                                .arg(path));
    bytes = plain;
    if (compressed)
        *compressed = true;
    return true;
}

// 在索引里按名找条目：先**同名**，没有才同名 + ".lz4"；basename 大小写不敏感；目录条目跳过。
// 返回 nullptr = 两种名字都没有。
const imgtar::TarIndexEntry *findEntryByName(const QList<imgtar::TarIndexEntry> &idx,
                                             const QString &name)
{
    for (const QString &cand : {name, name + QStringLiteral(".lz4")}) {
        for (const imgtar::TarIndexEntry &e : idx) {
            if (e.isDir)
                continue;
            if (baseName(e.name).compare(cand, Qt::CaseInsensitive) == 0)
                return &e;
        }
    }
    return nullptr;
}

} // namespace

bool looksLikeLz4Frame(const QByteArray &data)
{
    // LZ4 frame 魔数 0x184D2204（小端落盘 = 04 22 4D 18）。本仓的压缩侧同为 frame 格式
    // （lz4_wrapper.cpp:6- 用 LZ4F_compressFrame），三星包内 .lz4 亦为 frame（facts §C1）。
    static const QByteArray kMagic = QByteArray::fromHex("04224d18");
    return data.startsWith(kMagic);   // 短于魔数 → startsWith 直接 false
}

bool looksLikeTar(const QByteArray &head, const QString &fileName)
{
    // 魔数判据与 tar_image.cpp:631 逐字一致（同一份定义，避免两处判据分歧）；文件名后缀是补充 ——
    // 空归档/魔数之外的变体仍按 tar 报错，而不是被当成裸镜像收下。head 短于 262 字节时 mid() 取不满
    // 5 字节 → 与 "ustar" 不等 → 魔数判据不成立（不越界、不抛）。
    return head.mid(257, 5) == QByteArray("ustar", 5)
           || fileName.endsWith(QStringLiteral(".tar"), Qt::CaseInsensitive)
           || fileName.endsWith(QStringLiteral(".tar.md5"), Qt::CaseInsensitive);
}

bool loadSbootBytes(const QString &path, QByteArray &out, SbootSource *source, QString *error)
{
    out.clear();                       // 失败路径不留陈旧字节（见头文件注释）
    if (error)
        error->clear();
    if (source)                        // 失败时出参一律复位：仓内约定同 eub_loadout.cpp:235
        *source = SbootSource{};       // （调用方忽略返回值也拿不到上一次的来源描述/压缩标志）

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("无法打开文件：%1").arg(path));

    const QString fileName = QFileInfo(path).fileName();
    const QByteArray head = f.read(kTarHeaderBytes);

    // "是不是 tar"：判据在 looksLikeTar（本文件），对话框侧的 sourceLooksLikeTar 也调它 ——
    // 三份同义判据曾散在载荷层/对话框层/索引层，这里收口到一处。
    const bool isTar = looksLikeTar(head, fileName);

    QString entryInTar;                // 非空 = 载荷取自 tar 条目
    QByteArray bytes;
    if (isTar) {
        QList<imgtar::TarIndexEntry> idx;
        QString tarErr;
        // 失败 → 直接报错，**不回退**裸镜像路径（spec §D8：以 .tar 名义给出的文件，其内容
        // 完整性由索引层裁定；回退会让"坏包"被当成镜像刷进设备）
        if (!imgtar::indexTarStream(path, idx, nullptr, &tarErr))
            return fail(error, QStringLiteral("按 tar 解析 %1 失败：%2").arg(fileName, tarErr));

        // 条目选择：先 sboot.bin，没有才 sboot.bin.lz4；basename 大小写不敏感（spec §D8 的
        // "按名找"，包内命名随工具/版本大小写不一）——规则与 loadNamedEntriesFromTar 同一份实现
        const imgtar::TarIndexEntry *hit = findEntryByName(idx, QStringLiteral("sboot.bin"));
        if (!hit)
            return fail(error, QStringLiteral("%1 里找不到 sboot.bin / sboot.bin.lz4（%2）")
                                    .arg(fileName, listingOf(idx)));

        if (!readTarEntryBytes(f, *hit, fileName, QStringLiteral("sboot.bin 是 bootloader 镜像"),
                               bytes, error))
            return false;
        entryInTar = hit->name;
    } else {
        // QFile::size() 在异常路径可返回 -1：夹取后再进文案，否则会印出 18446744073709551615
        //（T5 审查 Minor 6；夹成 0 后下面的"读取不完整"守卫会给出诚实的失败文案）
        const qint64 fileSize = f.size() > 0 ? f.size() : 0;
        if (quint64(fileSize) > kMaxPayloadBytes)
            return fail(error, QStringLiteral("%1 有 %2 字节 —— 超出本函数单次读入上限，疑为选错文件")
                                    .arg(fileName)
                                    .arg(fileSize));
        if (!f.seek(0))
            return fail(error, QStringLiteral("读取 %1 失败（无法回到文件头）").arg(fileName));
        // 有界读而非 readAll：对 /dev/zero 一类 size()==0 的特殊文件，readAll 会一直读到内存耗尽
        // （上限与上面的 kMaxPayloadBytes 同源）；普通文件的实读数必须与 size() 相符
        bytes = f.read(qint64(kMaxPayloadBytes) + 1);
        if (qint64(bytes.size()) != fileSize)
            return fail(error, QStringLiteral("读取 %1 不完整（文件 %2 字节，实读 %3 字节；"
                                              "非普通文件或读取期间被改动）")
                                    .arg(fileName)
                                    .arg(fileSize)
                                    .arg(bytes.size()));
    }

    // 解压判据是**内容**（LZ4 frame 魔数），不是文件名后缀：命中即解压，解压失败即失败（见助手）
    bool compressed = false;
    if (!decompressIfLz4(bytes, path, &compressed, error))
        return false;

    // 空载荷无意义（空文件、0 字节条目、解压出 0 字节都在此收口）
    if (bytes.isEmpty())
        return fail(error, entryInTar.isEmpty()
                               ? QStringLiteral("%1 没有载荷数据（0 字节）").arg(fileName)
                               : QStringLiteral("%1 内的 %2 是空条目（0 字节）")
                                     .arg(fileName, entryInTar));

    if (source) {
        source->description = describe(fileName, entryInTar, compressed);
        source->wasCompressed = compressed;
    }
    out = bytes;
    return true;
}

bool loadNamedEntriesFromTar(const QString &tarPath, const QStringList &baseNames,
                             QList<QByteArray> &out, QString *error)
{
    out.clear();                        // 失败路径不留陈旧字节（头文件契约）
    if (error)
        error->clear();
    // 空请求 = 没有要求任何条目：直接成功且**不读文件**（调用方传空表时不该被"文件坏"绊住）。
    if (baseNames.isEmpty())
        return true;

    QFile f(tarPath);
    if (!f.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("无法打开文件：%1").arg(tarPath));
    const QString fileName = QFileInfo(tarPath).fileName();

    QList<imgtar::TarIndexEntry> idx;
    QString tarErr;
    // 与 loadSbootBytes 同款：按 tar 解析失败就**直接报错、不回退**（以 .tar 名义给出的文件，
    // 其内容完整性由索引层裁定）。
    if (!imgtar::indexTarStream(tarPath, idx, nullptr, &tarErr))
        return fail(error, QStringLiteral("按 tar 解析 %1 失败：%2").arg(fileName, tarErr));

    // 逐名取条目；**缺失的名字先累加、最后一次性报出**（报第一个就停会让用户来回试，
    // 9830 恰好要两个文件）。已取到的字节先进局部表：任一环节失败时 out 保持为空（fail-closed）。
    QStringList missing;
    QList<QByteArray> found;
    for (const QString &want : baseNames) {
        const imgtar::TarIndexEntry *hit = findEntryByName(idx, want);
        if (!hit) {
            missing << want;
            continue;
        }
        QByteArray bytes;
        if (!readTarEntryBytes(f, *hit, fileName,
                               QStringLiteral("超出本函数单次读入上限"), bytes, error))
            return false;
        // 本路径不消费压缩标志（描述信息只由 loadSbootBytes 那条路径产出）：传 nullptr ——
        // 留一个从不读取的局部变量会让读者以为这里用了它（T1 审查 M1；取地址故 -Wall 抓不到）。
        if (!decompressIfLz4(bytes, tarPath, nullptr, error))
            return false;
        // 空载荷无意义（0 字节条目、解压出 0 字节都在此收口）：发一个空文件既无意义、
        // sendSegment 也会以"空载荷"拒发。
        if (bytes.isEmpty())
            return fail(error, QStringLiteral("%1 内的 %2 是空条目（0 字节）")
                                    .arg(fileName, hit->name));
        found << bytes;
    }
    if (!missing.isEmpty())
        return fail(error, QStringLiteral("%1 里找不到 %2（%3）")
                                .arg(fileName, missing.join(QStringLiteral("、")), listingOf(idx)));

    out = found;
    return true;
}

} // namespace eub
