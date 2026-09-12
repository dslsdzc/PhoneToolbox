#include "oppo_ops.h"

#include <QFile>
#include <QXmlStreamReader>

#include <limits>

#include "oppo_keys.h"

namespace imgopp {
namespace {

// ==================== 常量（逐条标注出处） ====================

// 尾页尺寸（FirmwareKit.Oppo.Core/Models/OppHeader.cs L168 OpsTailPage.Size；spec 速查表 §OPS）
constexpr qsizetype kTailPageSize = 0x200;
constexpr quint32 kOpsVersion = 2;   // 尾页 +0x00（A11）
constexpr quint32 kOpsFlags = 1;     // 尾页 +0x04（A11）
constexpr quint32 kOpsMagic = 0x7CEF;  // 尾页 +0x10（与 OFP-QC 共用，见 A11）

// 尾页字段偏移（OpsTailPage.Parse() L192-199；spec 速查表 §OPS）
constexpr qsizetype kTailMagicOff = 0x10;
constexpr qsizetype kTailConfigSectorOff = 0x14;  // settings.xml 扇区位置（0x200 单位）
constexpr qsizetype kTailXmlLengthOff = 0x18;     // settings.xml 明文长度
constexpr qsizetype kTailProjectIdOff = 0x1C;     // project id[16] ASCII
constexpr qsizetype kTailFirmwareOff = 0x2C;      // firmware 名[32] ASCII
constexpr int kProjectIdLen = 16;
constexpr int kFirmwareNameLen = 32;

// 扇区尺寸（OpsSectorSize，OppOpsParser.cs L227；opscrypto.py 里到处硬编码的 0x200）
constexpr quint64 kSectorSize = 0x200;

// settings.xml 长度上限（Task 终审复审补防，同 oppo_ofp.cpp kMaxManifestLength）:
// 8 MiB 宽松天花板 —— 真实清单远小于 1 MiB，病态声明在读取前被拒绝（不静默截断）。
constexpr quint64 kMaxManifestLength = 8 * 1024 * 1024;

// ==================== 小工具 ====================

// 4 字节 LE 读（越界返回 0）—— 尾页字段读取用
quint32 le32(const QByteArray &buf, qsizetype off)
{
    quint32 v = 0;
    for (int i = 0; i < 4; ++i) {
        const qsizetype idx = off + i;
        if (idx < 0 || idx >= buf.size())
            return 0;
        v |= quint32(quint8(buf.at(idx))) << (8 * i);
    }
    return v;
}

// 定长 C 字符串字段（projectId/firmwareName 等）。读侧参照 =
// reference/FirmwareKit.Oppo/FirmwareKit.Oppo.Core/Models/OppHeader.cs L198-199
// （OpsTailPage.Parse 取 +0x1C/+0x2C 定长片）与 L226-230（OfpHelper.DecodeAscii =
// ASCII 解码 + TrimEnd('\0')，只去尾部 0x00）。注意 opscrypto.py **不读**这两个
// 字段，故 bkerler 侧无对应实现；本实现采用其 ofp_mtk_decrypt.py cleancstring()
// L112-113 的更宽口径 replace(b"\x00", b"").decode('utf-8')（去全部 0x00 + UTF-8，
// 见 oppo_ofp.cpp 同名函数），比 C# 的 TrimEnd 能多容忍字段中部的填充 0x00。
QString cleanCString(const QByteArray &field)
{
    // 0x00 在 UTF-8 中只能是 U+0000 的编码（不参与任何多字节序列），故"解码后删
    // U+0000"与"删 0x00 字节后解码"逐字节等价
    QString out = QString::fromUtf8(field);
    out.remove(QChar(u'\0'));
    return out;
}

// 16 字节向上取整（C# TryDecryptXml() L183-184 的 opsBlockSize 对齐）
quint64 align16(quint64 n)
{
    return (n + 0xF) / 0x10 * 0x10;
}

// 乘法溢出防护（同 oppo_ofp.cpp mulSafe() L176-182）
bool mulSafe(quint64 a, quint64 b, quint64 &out)
{
    if (b != 0 && a > std::numeric_limits<quint64>::max() / b)
        return false;
    out = a * b;
    return true;
}

// 错误收口（A9：失败必带中文文案）
bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

// 追加提示（多个条目被跳过时逐条累积；同 oppo_extract.cpp appendNote()）
void appendNote(QString *error, const QString &message)
{
    if (!error)
        return;
    if (!error->isEmpty())
        *error += QLatin1Char('\n');
    *error += message;
}

// ==================== 清单解析 ====================

// 单个 <File>/<Image> 元素 → OpsEntry。属性语义照 opscrypto.py main() L590-637：
//   Path（SAHARA/UFS_PROVISION 组；L593/L601）或 filename（Program 组；L611/L613）
//   + FileOffsetInSrc（0x200 页单位）+ SizeInByteInSrc（落盘长度）+ SizeInSectorInSrc（长度兜底）
//   + sparse + Sha256（Program 组）。
// 名字为空 → 跳过（参照 L614-615/L629-630 的 `wfilename == "" → continue`）。
// 缺 FileOffsetInSrc → 跳过并把中文提示追加进 *error（参照此处是 KeyError 崩溃；
// 本实现不静默丢弃、也不因单条异常放弃整包，语义同 A10）。
bool appendOpsEntry(const QXmlStreamAttributes &attrs, bool decrypt, quint64 fileSize,
                    QList<OpsEntry> &out, QString *error)
{
    QString name = attrs.value(QStringLiteral("Path")).toString();
    if (name.isEmpty())
        name = attrs.value(QStringLiteral("filename")).toString();
    if (name.isEmpty())
        return true;

    bool ok = false;
    const quint64 offsetPages = attrs.value(QStringLiteral("FileOffsetInSrc")).toULongLong(&ok);
    if (!ok) {
        appendNote(error, QStringLiteral("跳过 OPS 条目：缺少 FileOffsetInSrc（%1）").arg(name));
        return true;
    }
    quint64 offset = 0;
    if (!mulSafe(offsetPages, kSectorSize, offset))
        return fail(error, QStringLiteral("OPS 文件表越界：%1 的偏移字段溢出").arg(name));

    // 落盘长度 = SizeInByteInSrc（参照 decryptfile()/copyfile() 的 length 参数）；缺失时
    // 回退 SizeInSectorInSrc × 0x200（C# AddProgramEntry() L446 的 byteSize > 0 ? : 口径）
    quint64 size = 0;
    bool haveSize = false;
    const quint64 byteSize = attrs.value(QStringLiteral("SizeInByteInSrc")).toULongLong(&haveSize);
    if (haveSize) {
        size = byteSize;
    } else {
        const quint64 sectors = attrs.value(QStringLiteral("SizeInSectorInSrc")).toULongLong(&haveSize);
        if (haveSize && !mulSafe(sectors, kSectorSize, size))
            return fail(error, QStringLiteral("OPS 文件表越界：%1 的长度字段溢出").arg(name));
    }

    // 越界防护（spec §5；同 appendQcFile() L227-233）：偏移 + 长度必须落在包内
    if (offset > fileSize || size > fileSize - offset)
        return fail(error,
                    QStringLiteral("OPS 文件表越界：%1（偏移 %2 + 长度 %3）超出包大小 %4")
                        .arg(name)
                        .arg(offset)
                        .arg(size)
                        .arg(fileSize));

    OpsEntry entry;
    entry.name = name;
    entry.offset = offset;
    entry.size = size;
    entry.decrypt = decrypt;
    entry.sha256Hex = attrs.value(QStringLiteral("Sha256")).toString();
    const QString sparse = attrs.value(QStringLiteral("sparse")).toString();
    entry.sparse = sparse == QLatin1String("1")
                   || sparse.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0;
    out.append(entry);
    return true;
}

// settings.xml 清单解析。遍历结构照 opscrypto.py main() L588-638（Python）与
// FirmwareKit OppOpsParser.ParseEntries() L249-287（C#）:
//   根元素（ProFile）的每个子元素即一个组:
//     SAHARA         → 子 <File> 条目：整段解密（L590-597）
//     UFS_PROVISION  → 子 <File> 条目：原样拷贝（L598-605）
//     标签名含 "Program" → 子元素带 filename → 条目（L611-623）；
//                          否则取其孙元素里带 filename 的条目（L624-638）
//   其余组（BasicInfo 等）忽略（L639-640 注释掉的 else）。
bool parseOpsManifest(const QByteArray &xml, quint64 fileSize, QList<OpsEntry> &out,
                      QString *error)
{
    QXmlStreamReader reader(xml);
    if (!reader.readNextStartElement())
        return fail(error, QStringLiteral("OPS settings.xml 不是有效的 XML（缺少根元素）"));

    while (reader.readNextStartElement()) {          // 顶层: 组
        const QString tag = reader.name().toString();
        if (tag == QLatin1String("SAHARA") || tag == QLatin1String("UFS_PROVISION")) {
            const bool decrypt = tag == QLatin1String("SAHARA");
            while (reader.readNextStartElement()) {  // 组内: <File>
                if (reader.name() == QLatin1String("File")
                    && !appendOpsEntry(reader.attributes(), decrypt, fileSize, out, error))
                    return false;
                reader.skipCurrentElement();
            }
        } else if (tag.contains(QLatin1String("Program"))) {
            while (reader.readNextStartElement()) {  // 子元素: <program> 或直接条目
                if (reader.attributes().hasAttribute(QStringLiteral("filename"))) {
                    if (!appendOpsEntry(reader.attributes(), false, fileSize, out, error))
                        return false;
                    reader.skipCurrentElement();
                } else {
                    // 容器: 孙元素才是条目（<program label="…"><Image filename="…"/></program>）
                    while (reader.readNextStartElement()) {
                        if (!appendOpsEntry(reader.attributes(), false, fileSize, out, error))
                            return false;
                        reader.skipCurrentElement();
                    }
                    // 容器的结束标签已由 readNextStartElement() 消费，直接继续外层循环
                }
            }
        } else {
            reader.skipCurrentElement();             // BasicInfo 等非文件组
        }
    }
    if (reader.hasError()) {
        return fail(error,
                    QStringLiteral("OPS settings.xml 解析失败：%1").arg(reader.errorString()));
    }
    return true;
}

} // namespace

bool detectOPS(const QByteArray &tail, quint64 fileSize)
{
    if (fileSize < quint64(kTailPageSize) || quint64(tail.size()) < quint64(kTailPageSize))
        return false;
    const QByteArray page = tail.right(kTailPageSize);
    return le32(page, 0) == kOpsVersion && le32(page, 4) == kOpsFlags
           && le32(page, kTailMagicOff) == kOpsMagic;
}

bool parseOPS(const QString &path, OpsInfo &info, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("无法打开 OPS 文件：%1").arg(path));
    const quint64 fileSize = quint64(file.size());
    if (fileSize < quint64(kTailPageSize))
        return fail(error,
                    QStringLiteral("OPS 文件过小（%1 字节，不足一页）：%2").arg(fileSize).arg(path));

    // 尾页（extractxml() L408-410 / ReadTailPage() L116-134）
    if (!file.seek(qint64(fileSize - quint64(kTailPageSize))))
        return fail(error, QStringLiteral("OPS 尾页定位失败：%1").arg(path));
    const QByteArray tail = file.read(kTailPageSize);
    if (quint64(tail.size()) != quint64(kTailPageSize))
        return fail(error, QStringLiteral("OPS 尾页读取不完整：%1").arg(path));
    if (!detectOPS(tail, fileSize))
        return fail(error,
                    QStringLiteral("不是 OPS 包（尾页 version/flags/0x7CEF 标记不匹配）：%1")
                        .arg(path));

    // 尾页字段（extractxml() L409-412 + OpsTailPage.Parse() L192-199）
    OpsInfo parsed;   // 失败时不动调用方的 info（同 parseOFP 语义）
    parsed.projectId = cleanCString(tail.mid(kTailProjectIdOff, kProjectIdLen));
    parsed.firmwareName = cleanCString(tail.mid(kTailFirmwareOff, kFirmwareNameLen));
    parsed.settingsLength = le32(tail, kTailXmlLengthOff);
    parsed.settingsOffset = quint64(le32(tail, kTailConfigSectorOff)) * kSectorSize;
    if (parsed.settingsLength == 0)
        return fail(error, QStringLiteral("OPS settings.xml 长度字段为 0（尾页 +0x18）：%1").arg(path));

    // settings.xml 定位与越界防护：密文必须完整落在尾页之前。
    // 起点取尾页 +0x14 的扇区号 × 0x200（brief Step 4 与 C# TryDecryptXml() L170 口径）；
    // 主参照 extractxml() L412 用 filesize-0x200-(xmllength+xmlpad) 反算 —— 在扇区号页对齐
    // 的正常包里两者等价（本机合成包与 encrypter 的 pos 推进都可验证），而 +0x14 是编码端
    // 显式写下的位置，取它才能在字段异常时给出明确的越界错误而不是读到错误的字节。
    const quint64 tailStart = fileSize - quint64(kTailPageSize);
    if (parsed.settingsOffset >= tailStart
        || quint64(parsed.settingsLength) > tailStart - parsed.settingsOffset)
        return fail(error,
                    QStringLiteral("OPS settings.xml 区域越界（偏移 %1 + 长度 %2，尾页起点 %3）：%4")
                        .arg(parsed.settingsOffset)
                        .arg(parsed.settingsLength)
                        .arg(tailStart)
                        .arg(path));

    // 病态长度防护（Task 终审复审）：声明长度只要 ≤ 可用空间就会被整段读入，而
    // doDetect「选中即 parse」→ 伪造长度的大包（如 20GB 包声明 4GB）仅选中文件即
    // 产生同量级分配，与 image_worker.cpp 的"不预载整文件"口径相悖。真实 settings.xml
    // 远小于 1 MiB，取 8 MiB 作宽松天花板；检查在 file.read() 之前 → 不产生大分配。
    // 放在既有区域越界检查之后：超出可用空间的值仍报"区域越界"（原文案不变）。
    if (quint64(parsed.settingsLength) > kMaxManifestLength)
        return fail(error,
                    QStringLiteral("OPS settings.xml 长度异常（%1 字节，超出 %2 MiB 上限），"
                                   "文件可能已损坏")
                        .arg(parsed.settingsLength)
                        .arg(kMaxManifestLength / (1024 * 1024)));

    // 读取长度 = align16(settingsLength)，按"尾页之前可用字节"封顶（C# TryDecryptXml()
    // L183-191 的 opsBlockSize 对齐 + maxAvailable 封顶）。密码块路对末块本就按零扩展，
    // 故封顶不改变前 settingsLength 字节。
    quint64 need = align16(quint64(parsed.settingsLength));
    if (need > tailStart - parsed.settingsOffset)
        need = tailStart - parsed.settingsOffset;
    if (!file.seek(qint64(parsed.settingsOffset)))
        return fail(error,
                    QStringLiteral("OPS settings.xml 定位失败（偏移 %1）：%2")
                        .arg(parsed.settingsOffset)
                        .arg(path));
    const QByteArray cipher = file.read(qint64(need));
    if (quint64(cipher.size()) != need)
        return fail(error,
                    QStringLiteral("OPS settings.xml 读取不完整（偏移 %1 期望 %2 字节，实得 %3），"
                                   "文件可能被截断：%4")
                        .arg(parsed.settingsOffset)
                        .arg(need)
                        .arg(cipher.size())
                        .arg(path));

    // 密钥试解：按 opsKeyCandidates() 表序 mbox5 → mbox6 → mbox4（opscrypto.py main() L571-587
    // 的嵌套 if 顺序）。命中判定 = 解出内容含 "<?xml"（C# AutoDecryptOpsXml() L325）或
    // "xml "（Python extractxml() L415 的子串判定）。
    QByteArray xml;
    for (const OpsKey &candidate : opsKeyCandidates()) {
        const QByteArray plain = opsDecrypt(cipher, candidate.mboxBlob);
        if (plain.size() != cipher.size())
            continue;   // 密钥材料非法 → 空返回（A9 契约）→ 该候选未命中
        if (plain.contains("<?xml") || plain.contains("xml ")) {
            parsed.keyId = candidate.keyId;
            parsed.mboxBlob = candidate.mboxBlob;
            // 产物只用前 settingsLength 字节（extractxml() L417-419 写 xmllength 字节）：
            // 其后是明文补齐的零字节，带进 XML 解析会报错
            xml = plain.left(qsizetype(parsed.settingsLength));
            break;
        }
    }
    if (parsed.keyId.isEmpty()) {
        // A11 残余风险的回退提示：.ops 与 OFP-QC 共用尾页 +0x10 的 0x7CEF，若某 QC 包的
        // +0x00/+0x04 恰为 02/01 会被先判成 OPS（探测定序见 A11），此时用户看到的就是这条
        return fail(error,
                    QStringLiteral("OPS 密钥未知或文件损坏（settings.xml 试解 mbox5/mbox6/mbox4 "
                                   "均失败）：%1。若该文件实为 OFP-QC 包（两格式尾页判据重叠，"
                                   "见修订 A11），请按 OFP 重新解析")
                        .arg(path));
    }

    // settings.xml 清单（组语义见 parseOpsManifest 注释）
    if (!parseOpsManifest(xml, fileSize, parsed.entries, error))
        return false;

    info = parsed;
    return true;
}

} // namespace imgopp
