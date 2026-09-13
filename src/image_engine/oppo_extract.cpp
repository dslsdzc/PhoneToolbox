#include "oppo_extract.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>

#include "oppo_crypto.h"
#include "oppo_keys.h"
#include "oppo_ofp.h"
#include "oppo_ops.h"

namespace imgopp {

namespace {

// ==================== 常量（逐条标注出处） ====================

// 分块搬运的块尺寸（copysub() ofp_qc_decrypt.py L153-165 每次读写 0x100000 字节）
constexpr quint64 kStreamChunk = 0x100000;

// ==================== 错误收口（A9：失败必带中文文案） ====================

// 追加一条诊断（多个条目被跳过时逐条累积，不用后者覆盖前者；换行分隔）
void appendNote(QString *error, const QString &message)
{
    if (!error)
        return;
    if (!error->isEmpty())
        *error += QLatin1Char('\n');
    *error += message;
}

// 失败收口（A9：失败必带中文文案）。**追加而非覆盖**（清扫 PA6）：致命失败常发生在"已经跳过了
// 若干条目"之后 —— 跳过用上面的 appendNote 累积，若这里直接赋值，`*error` 会把"哪几个条目被跳过"
// 整段吞掉；而包不完整/解不开时，恰恰最需要那份清单（否则用户只看到最后一句"无法创建输出目录"
// 之类，误以为包本身没问题）。追加后仍保持"一条诊断一行"的既有格式。
bool fail(QString *error, const QString &message)
{
    appendNote(error, message);
    return false;
}

// ==================== 条目名净化（spec §5 恶意输入防护） ====================

// 穿越防护（跨平台一致；比 tar_image.cpp safeTarName() L79-96 更严 —— tar 允许子目录，
// OFP 的条目名是扁平分区分名，任何路径成分都属异常输入）:
//   - 空名 → 拒绝
//   - 含 '/' → 拒绝（既挡住绝对路径与 "a/b" 子路径，也挡住 ".." 成分）
//   - 含 '\\' → 拒绝（Windows 下 '\' 是路径分隔符，`a\..\evil.bin`、`..\evil` 可逃逸 outDir）
//   - 驱动器前缀（"C:evil.img"，Windows 下的绝对路径）→ 拒绝
//   - ".." / "." 成分 → 拒绝
// 注: 空名条目在解析层（appendQcFile()/parseMtk()）已被丢弃，此处属纵深防御。
bool isSafeEntryName(const QString &name)
{
    if (name.isEmpty())
        return false;
    if (name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')))
        return false;
    if (name.size() >= 2 && name.at(1) == QLatin1Char(':')) {
        const QChar c0 = name.at(0);
        if ((c0 >= QLatin1Char('A') && c0 <= QLatin1Char('Z'))
            || (c0 >= QLatin1Char('a') && c0 <= QLatin1Char('z')))
            return false;
    }
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return false;
    return true;
}

// ==================== 路径守卫（spec §5 流式写保护） ====================

// 路径同一性判定（沿用 src/ui/image_worker.cpp sameFile() L107-117 语义）:
// 存在时用 canonicalFilePath（解析符号链接/相对成分），否则比绝对路径。
bool sameFile(const QString &a, const QString &b)
{
    if (a == b)
        return true;
    const QFileInfo fa(a), fb(b);
    const QString ca = fa.exists() ? fa.canonicalFilePath() : fa.absoluteFilePath();
    const QString cb = fb.exists() ? fb.canonicalFilePath() : fb.absoluteFilePath();
    return !ca.isEmpty() && ca == cb;
}

// ==================== 流式搬运构件（Task 5 的 extractOPS 共用） ====================

// 产物写出通道：文件句柄 + 可选摘要（清单给了哪个属性才算哪个，避免无谓计算）。
struct ProductWriter
{
    QFile out;
    QCryptographicHash md5{QCryptographicHash::Md5};
    QCryptographicHash sha256{QCryptographicHash::Sha256};
    bool wantMd5 = false;
    bool wantSha256 = false;

    // 写出一个块，并把同一份字节喂给摘要 —— 单次遍历，不二次读盘
    bool write(const QByteArray &chunk, QString *error)
    {
        if (out.write(chunk) != chunk.size())
            return fail(error, QStringLiteral("写入产物失败：%1").arg(out.fileName()));
        if (wantMd5)
            md5.addData(chunk);
        if (wantSha256)
            sha256.addData(chunk);
        return true;
    }
};

// 分块读取包内 [offset, offset+length) 并交给 sink（内存 O(kStreamChunk)）。
// 读不满即报错（包被截断/解析后被改写）—— 不静默产出短文件。
bool streamRange(QFile &in, quint64 offset, quint64 length,
                 const std::function<bool(const QByteArray &chunk)> &sink, QString *error)
{
    if (!in.seek(qint64(offset)))
        return fail(error, QStringLiteral("包内定位失败（偏移 %1）").arg(offset));
    quint64 remain = length;
    while (remain > 0) {
        const qint64 want = qint64(qMin<quint64>(remain, kStreamChunk));
        const QByteArray chunk = in.read(want);
        if (qint64(chunk.size()) != want) {
            return fail(error,
                        QStringLiteral("包数据读取不完整（偏移 %1 期望 %2 字节，实得 %3），文件可能被截断")
                            .arg(offset + (length - remain))
                            .arg(want)
                            .arg(chunk.size()));
        }
        if (!sink(chunk))
            return false;
        remain -= quint64(chunk.size());
    }
    return true;
}

// 原样拷贝包内区间（copy() L167-175 的流式版）
bool copyRange(QFile &in, quint64 offset, quint64 length, ProductWriter &writer, QString *error)
{
    return streamRange(
        in, offset, length,
        [&writer, error](const QByteArray &chunk) { return writer.write(chunk, error); },
        error);
}

// CFB-128 分块解密包内区间（decryptfile() L177-204 的流式版）。
// 跨块 IV 推进: CFB 的反馈是「上一密文块」（oppo_crypto.cpp aes128CfbCrypt() L101-112），
// 故每块解密前把上一块末 16B 密文当 IV 传入；块长恒为 16 的整数倍（1 MiB）→ 与整段一次性
// 解密逐字节等价。末块不足 16B 时其结果不再被后续块使用，无需补齐。
bool decryptRange(QFile &in, quint64 offset, quint64 length, const QByteArray &key,
                  const QByteArray &iv, ProductWriter &writer, QString *error)
{
    QByteArray blockIv = iv;
    return streamRange(
        in, offset, length,
        [&blockIv, &key, &writer, error](const QByteArray &chunk) {
            const QByteArray plain = aes128CfbDecrypt(chunk, key, blockIv);
            // 空返回 = 密码 helper 的失败契约（key/iv 非法）→ 收口为中文错误（A9），不静默继续
            if (plain.size() != chunk.size())
                return fail(error, QStringLiteral("数据段解密失败（密钥未知或文件损坏）"));
            if (!writer.write(plain, error))
                return false;
            if (chunk.size() >= 16)
                blockIv = chunk.right(16);
            return true;
        },
        error);
}

// 单条目搬运（三策略归一，decryptfile() L188-198）:
//   decryptLen = fullDecrypt ? size : min(encryptedSize, size)
//   前 decryptLen 字节解密（decryptLen == 0 退化为纯拷贝），其余原样拷贝。
bool writeEntry(QFile &in, const OfpFile &file, const QByteArray &key, const QByteArray &iv,
                ProductWriter &writer, QString *error)
{
    const quint64 decryptLen = file.fullDecrypt ? file.size : qMin(file.encryptedSize, file.size);
    if (decryptLen > 0 && !decryptRange(in, file.offset, decryptLen, key, iv, writer, error))
        return false;
    if (file.size > decryptLen
        && !copyRange(in, file.offset + decryptLen, file.size - decryptLen, writer, error))
        return false;
    return true;
}

// 摘要校验（checkhashfile() L206-245）。十六进制比较大小写不敏感（C# 参照
// OppReader.VerifyEntry() L1163/L1184 用 OrdinalIgnoreCase）。
// 注: Python 参照的 md5 只取产物前 0x40000 字节（checkhashfile() L213-215），属其自身
//     实现偏差；C# 参照对 md5/sha256 均为整段口径 → 本实现从 C#（两算法均整段）。
bool verifyHashes(const ProductWriter &writer, const OfpFile &file, QString *error)
{
    if (writer.wantSha256) {
        const QString actual = QString::fromLatin1(writer.sha256.result().toHex());
        if (QString::compare(actual, file.sha256Hex, Qt::CaseInsensitive) != 0)
            return fail(error, QStringLiteral("校验失败: %1 sha256 不匹配（清单 %2，实际 %3）")
                                   .arg(file.name, file.sha256Hex, actual));
    }
    if (writer.wantMd5) {
        const QString actual = QString::fromLatin1(writer.md5.result().toHex());
        if (QString::compare(actual, file.md5Hex, Qt::CaseInsensitive) != 0)
            return fail(error, QStringLiteral("校验失败: %1 md5 不匹配（清单 %2，实际 %3）")
                                   .arg(file.name, file.md5Hex, actual));
    }
    return true;
}

// ==================== OPS 条目搬运与校验（Task 5） ====================

// 单条目搬运（opscrypto.py main() L589-638 的三个分支归一）:
//   decrypt == true（SAHARA 组）→ 整段 opsDecrypt() 解密。参照 decryptfile() L423-437 也是
//     整段读入再解（密码是带链式反馈的流密码，逐块调用的状态接不上），SAHARA 组只有
//     Firehose programmer（MB 级）→ 整段读入可接受
//   decrypt == false（UFS_PROVISION / Program）→ 原样拷贝（copyfile() L487-491）
bool writeOpsEntry(QFile &in, const OpsEntry &entry, const QByteArray &mboxBlob,
                   ProductWriter &writer, QString *error)
{
    if (!entry.decrypt)
        return copyRange(in, entry.offset, entry.size, writer, error);

    if (!in.seek(qint64(entry.offset)))
        return fail(error, QStringLiteral("包内定位失败（偏移 %1）").arg(entry.offset));
    const QByteArray cipher = in.read(qint64(entry.size));
    if (quint64(cipher.size()) != entry.size) {
        return fail(error,
                    QStringLiteral("包数据读取不完整（偏移 %1 期望 %2 字节，实得 %3），文件可能被截断")
                        .arg(entry.offset)
                        .arg(entry.size)
                        .arg(cipher.size()));
    }
    const QByteArray plain = opsDecrypt(cipher, mboxBlob);
    // 空/长度不符 = 密码原语的失败契约（A9）→ 收口为中文错误，不静默写出半截产物
    if (quint64(plain.size()) != entry.size)
        return fail(error, QStringLiteral("数据段解密失败（密钥未知或文件损坏）：%1").arg(entry.name));
    return writer.write(plain, error);
}

// OPS 的 sha256 校验（opscrypto.py L619-623 的 `sha256 != csha256` + calc_digest() L462-470）:
// 摘要按"整段 + 补零到 0x1000 边界"计算 —— 与 extractOFP 的整段口径不同（A10 附注），
// 故不复用 verifyHashes()。比较大小写不敏感（同 Task 4）。sparse 跳过由调用方决定
// （参照 L622 的 `and not sparse`）。
bool verifyOpsHash(ProductWriter &writer, const OpsEntry &entry, QString *error)
{
    if (!writer.wantSha256)
        return true;
    const quint64 rem = entry.size % 0x1000;
    if (rem != 0)
        writer.sha256.addData(QByteArray(qsizetype(0x1000 - rem), '\0'));
    const QString actual = QString::fromLatin1(writer.sha256.result().toHex());
    if (QString::compare(actual, entry.sha256Hex, Qt::CaseInsensitive) != 0)
        return fail(error, QStringLiteral("校验失败: %1 sha256 不匹配（清单 %2，实际 %3）")
                               .arg(entry.name, entry.sha256Hex, actual));
    return true;
}

} // namespace

bool extractOFP(const QString &path, const QString &outDir, const ExtractProgress &progress,
                QString *error, const ExtractCancel &cancel)
{
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("无法打开 OFP 文件：%1").arg(path));

    // 复用 Task 3: 变体识别 + 清单解析 + 命中 key/iv（失败文案由 parseOFP 负责，不覆盖）
    OfpInfo info;
    if (!parseOFP(path, info, error)) {
        // 契约兜底（A9: 失败必带中文文案；parseOFP 各失败路径均已填，此处仅防漏）
        if (error && error->isEmpty())
            *error = QStringLiteral("OFP 解析失败：%1").arg(path);
        return false;
    }
    if (info.files.isEmpty())
        return fail(error, QStringLiteral("OFP 清单中没有可提取的文件：%1").arg(path));

    // 条目名净化先行: 跳过的条目不计入进度分母，最后一个文件完成时的百分比才是 100
    QList<OfpFile> entries;
    entries.reserve(info.files.size());
    for (const OfpFile &file : info.files) {
        if (!isSafeEntryName(file.name)) {
            appendNote(error, QStringLiteral("跳过条目：文件名不安全（%1）").arg(file.name));
            continue;
        }
        entries.append(file);
    }
    if (entries.isEmpty())
        return false;   // error 已含上面逐条累积的跳过原因

    if (!QDir().mkpath(outDir))
        return fail(error, QStringLiteral("无法创建输出目录：%1").arg(outDir));

    const quint64 fileSize = quint64(in.size());
    quint64 total = 0;
    for (const OfpFile &file : entries)
        total += file.size;

    quint64 done = 0;
    int doneFiles = 0;   // 已写完并通过校验的条目数（取消文案要如实报"完成到哪"，见 ExtractCancel）
    for (const OfpFile &file : entries) {
        // 取消点 = **条目边界**（ExtractCancel 契约）：上一轮循环已把前一个文件写完并校验完，
        // 从此处返回不会留下半截产物；文件内部不设取消点。
        if (cancel && cancel()) {
            appendNote(error, QStringLiteral("用户取消：已完成 %1/%2 个文件（当前文件已写完并校验完，已写产物未回滚）")
                                  .arg(doneFiles).arg(entries.size()));
            return false;
        }
        // 读前复核包内区间（parseOFP 已校验；解析后被截断/改写的包在此兜底）
        if (file.offset > fileSize || file.size > fileSize - file.offset)
            return fail(error, QStringLiteral("文件表越界：%1（偏移 %2 + 长度 %3）超出包大小 %4")
                                   .arg(file.name)
                                   .arg(file.offset)
                                   .arg(file.size)
                                   .arg(fileSize));

        const QString outPath = QDir(outDir).filePath(file.name);
        // 路径守卫先判后开: 两者相同时 Truncate 会毁掉包本体（spec §5 流式写保护）
        if (sameFile(path, outPath))
            return fail(error, QStringLiteral("输出路径与输入包相同，拒绝覆盖：%1").arg(outPath));

        ProductWriter writer;
        writer.wantMd5 = !file.md5Hex.isEmpty();
        writer.wantSha256 = !file.sha256Hex.isEmpty();
        writer.out.setFileName(outPath);
        if (!writer.out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail(error, QStringLiteral("无法写入产物：%1").arg(outPath));

        const bool written = writeEntry(in, file, info.key, info.iv, writer, error);
        writer.out.close();
        if (!written)
            return false;   // 已写产物保留（spec §5: 不回滚已写文件）

        if (!verifyHashes(writer, file, error))
            return false;   // 同上：校验失败亦不回滚，用户可重跑

        done += file.size;
        ++doneFiles;
        if (progress) {
            const int percent = total == 0 ? 100 : int(done * 100 / total);
            // sparse 产物原样输出，仅在此标注（spec §4: 不自动转 raw，转换由镜像引擎另行处理）
            progress(file.sparse
                         ? QStringLiteral("%1（sparse 镜像，原样输出）").arg(file.name)
                         : file.name,
                     percent);
        }
    }
    return true;
}

// OPS 解包入口（流程与 extractOFP 同构：解析 → 条目名净化 → 逐条搬运 → 校验 → 进度）
bool extractOPS(const QString &path, const QString &outDir, const ExtractProgress &progress,
                QString *error, const ExtractCancel &cancel)
{
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("无法打开 OPS 文件：%1").arg(path));

    // 复用 oppo_ops: 尾页校验 + settings.xml 定位/试解 + 清单解析（失败文案由 parseOPS 负责）
    OpsInfo info;
    if (!parseOPS(path, info, error)) {
        // 契约兜底（A9: 失败必带中文文案；parseOPS 各失败路径均已填，此处仅防漏）
        if (error && error->isEmpty())
            *error = QStringLiteral("OPS 解析失败：%1").arg(path);
        return false;
    }
    if (info.entries.isEmpty())
        return fail(error, QStringLiteral("OPS 清单中没有可提取的文件：%1").arg(path));

    // 条目名净化先行: 跳过的条目不计入进度分母（A10 第一条：跳过 + 提示 + 其余继续）
    QList<OpsEntry> entries;
    entries.reserve(info.entries.size());
    for (const OpsEntry &entry : info.entries) {
        if (!isSafeEntryName(entry.name)) {
            appendNote(error, QStringLiteral("跳过条目：文件名不安全（%1）").arg(entry.name));
            continue;
        }
        entries.append(entry);
    }
    if (entries.isEmpty())
        return false;   // error 已含上面逐条累积的跳过原因（A10 第二条）

    if (!QDir().mkpath(outDir))
        return fail(error, QStringLiteral("无法创建输出目录：%1").arg(outDir));

    const quint64 fileSize = quint64(in.size());
    quint64 total = 0;
    for (const OpsEntry &entry : entries)
        total += entry.size;

    quint64 done = 0;
    int doneFiles = 0;   // 同 extractOFP：取消文案要如实报"完成到哪"
    for (const OpsEntry &entry : entries) {
        // 取消点 = **条目边界**（ExtractCancel 契约，同 extractOFP）：上一个文件已写完并校验完
        // 才走到这里，返回时不会留下半截产物。
        if (cancel && cancel()) {
            appendNote(error, QStringLiteral("用户取消：已完成 %1/%2 个文件（当前文件已写完并校验完，已写产物未回滚）")
                                  .arg(doneFiles).arg(entries.size()));
            return false;
        }
        // 读前复核包内区间（parseOPS 已校验；解析后被截断/改写的包在此兜底）
        if (entry.offset > fileSize || entry.size > fileSize - entry.offset)
            return fail(error, QStringLiteral("OPS 文件表越界：%1（偏移 %2 + 长度 %3）超出包大小 %4")
                                   .arg(entry.name)
                                   .arg(entry.offset)
                                   .arg(entry.size)
                                   .arg(fileSize));

        const QString outPath = QDir(outDir).filePath(entry.name);
        // 路径守卫先判后开: 两者相同时 Truncate 会毁掉包本体（spec §5 流式写保护）
        if (sameFile(path, outPath))
            return fail(error, QStringLiteral("输出路径与输入包相同，拒绝覆盖：%1").arg(outPath));

        ProductWriter writer;
        // Program 的 Sha256 只在非 sparse 时校验（L622）→ sparse 条目无需计算摘要
        writer.wantSha256 = !entry.sha256Hex.isEmpty() && !entry.sparse;
        writer.out.setFileName(outPath);
        if (!writer.out.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail(error, QStringLiteral("无法写入产物：%1").arg(outPath));

        const bool written = writeOpsEntry(in, entry, info.mboxBlob, writer, error);
        writer.out.close();
        if (!written)
            return false;   // 已写产物保留（spec §5: 不回滚已写文件）

        if (!verifyOpsHash(writer, entry, error))
            return false;   // 同上：校验失败亦不回滚，用户可重跑（wantSha256 判断在函数内）

        done += entry.size;
        ++doneFiles;
        if (progress) {
            const int percent = total == 0 ? 100 : int(done * 100 / total);
            // sparse 产物原样输出，仅在此标注（spec §4，同 extractOFP）
            progress(entry.sparse
                         ? QStringLiteral("%1（sparse 镜像，原样输出）").arg(entry.name)
                         : entry.name,
                     percent);
        }
    }
    return true;
}

} // namespace imgopp
