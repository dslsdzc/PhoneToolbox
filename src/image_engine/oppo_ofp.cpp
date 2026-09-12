#include "oppo_ofp.h"

#include <QFile>
#include <QXmlStreamReader>

#include <limits>

#include "oppo_crypto.h"
#include "oppo_keys.h"

namespace imgopp {

namespace {

// ==================== 常量与偏移（逐条标注出处） ====================

// QC 尾页魔数（末页 +0x10 LE32）
// 出处: ofp_qc_decrypt.py extract_xml() L119-123；FirmwareKit OfpQcTailPage.Magic
// （FirmwareKit.OfpReader/…/OppHeader.cs L13）。
constexpr quint32 kQcMagic = 0x7CEF;
// QC 页尺寸候选（extract_xml() L119 `for x in [0x200, 0x1000]`）
constexpr quint32 kQcPageSizeSmall = 0x200;
constexpr quint32 kQcPageSizeLarge = 0x1000;
// QC 尾页字段偏移（相对末页起点）: +0x14 清单偏移（页单位）、+0x18 清单长度（字节）
// 出处: extract_xml() L129-131
constexpr qsizetype kQcPageXmlOffsetOff = 0x14;
constexpr qsizetype kQcPageXmlLengthOff = 0x18;
// 老包（A57）清单长度重算阈值与回退量: 长度 < 200 时按 (fileSize-page)-xmlOffset-0x57 重算
// 出处: extract_xml() L132-133 注释 "A57 hack"
constexpr quint64 kQcShortXmlThreshold = 200;
constexpr quint64 kQcA57LengthAdjust = 0x57;
// 非明文组的默认局部解密长度（decryptitem() L253 decryptsize = 0x40000；
// decryptfile() L189-190 取 size = min(decryptsize, rlength)）
constexpr quint64 kQcPartialDecryptSize = 0x40000;
// QC 包最小尺寸（一页）——小于此值不可能含清单
constexpr quint64 kQcMinFileSize = kQcPageSizeSmall;

// 清单长度上限（Task 终审复审补防）: 声明长度（含 A57 重算值）只要 ≤ 文件大小就会
// 被整段读入内存，而 doDetect「选中即 parse」→ 伪造长度的大包（如 20GB 包声明 4GB）
// 会在仅选中文件时产生同量级分配，与 image_worker.cpp 的"不预载整文件"口径相悖。
// 真实 ProFile.xml 远小于 1 MiB（数十~数百条目 × 数百字节），取 8 MiB 作宽松天花板：
// 不误伤真实包，又把病态情形钉死在读取之前（超限报错，不静默截断）。
constexpr quint64 kMaxManifestLength = 8 * 1024 * 1024;

// MTK 首 16B 解密后的明文前缀（brutekey() L104-107 `data[:3] == b"MMM"`）
constexpr char kMtkPlainMagic[] = "MMM";
// MTK 尾头长度（main() L120 hdrlength = 0x6C）
constexpr quint64 kMtkHeaderSize = 0x6C;
// MTK 文件表条目长度（main() L139 `unpack("<32s Q Q Q 32s Q")` = 32+8+8+8+32+8 = 0x60）
constexpr quint64 kMtkEntrySize = 0x60;

// MTK 尾头字段偏移。出处: main() L125 `unpack("46s Q 4s 7s 5s H 32s H", hdr)` ——
// Python 默认 native 对齐，46s 后留 2B 填充使 Q 落在 8 字节边界；
// 与 FirmwareKit OfpMtkHeader.Parse()（OppHeader.cs L93-101 注释）逐字段一致：
//   prjname[46]@0 | u64@48 | reserved[4]@56 | cpu[7]@60 | flashtype[5]@67 | entries u16@72
//   | prjinfo[32]@74 | crc u16@106
constexpr qsizetype kMtkPrjNameOff = 0;
constexpr qsizetype kMtkPrjNameLen = 46;
constexpr qsizetype kMtkCpuOff = 60;
constexpr qsizetype kMtkCpuLen = 7;
constexpr qsizetype kMtkFlashTypeOff = 67;
constexpr qsizetype kMtkFlashTypeLen = 5;
constexpr qsizetype kMtkEntryCountOff = 72;

// MTK 文件表条目字段偏移（main() L139 `unpack("<32s Q Q Q 32s Q")`）
constexpr qsizetype kMtkEntryNameOff = 0;      // name[32]
constexpr qsizetype kMtkEntryStartOff = 32;    // start u64
constexpr qsizetype kMtkEntryLengthOff = 40;   // length u64（落盘总长）
constexpr qsizetype kMtkEntryEncLenOff = 48;   // encrypted_length u64（需解密的前缀长度）
constexpr qsizetype kMtkEntryFileOff = 56;     // filename[32]

// MTK 尾头/文件表解混淆 key（main() L118 hdrkey = bytearray(b"geyixue")）
const QByteArray &mtkShuffleKey()
{
    static const QByteArray key = QByteArrayLiteral("geyixue");
    return key;
}

// ==================== 小工具 ====================

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

// 小端读取。越界返回 0——调用点在校验过缓冲区长度后才读取（见各分支的显式长度检查）。
quint16 le16(const QByteArray &buf, qsizetype off)
{
    if (off < 0 || off + 2 > buf.size())
        return 0;
    return quint16(quint8(buf.at(off)) | (quint16(quint8(buf.at(off + 1))) << 8));
}

quint32 le32(const QByteArray &buf, qsizetype off)
{
    if (off < 0 || off + 4 > buf.size())
        return 0;
    quint32 v = 0;
    for (int i = 0; i < 4; ++i)
        v |= quint32(quint8(buf.at(off + i))) << (8 * i);
    return v;
}

quint64 le64(const QByteArray &buf, qsizetype off)
{
    if (off < 0 || off + 8 > buf.size())
        return 0;
    quint64 v = 0;
    for (int i = 0; i < 8; ++i)
        v |= quint64(quint8(buf.at(off + i))) << (8 * i);
    return v;
}

// 半字节交换（swap() ofp_qc_decrypt.py L13-14 同式）
quint8 nibbleSwap(quint8 v)
{
    return quint8(((v & 0x0F) << 4) | ((v & 0xF0) >> 4));
}

// MTK 尾头/文件表解混淆（amendments A2）:
//   out[i] = key[i % 7] ^ nibbleSwap(stored[i])
// 出处 mtk_shuffle() ofp_mtk_decrypt.py L23-28（`h = 半字节交换(input[i]); input[i] = k ^ h`）
// 与 FirmwareKit OppMtkKeyDerivation.MtkShuffle() L113-122 逐字一致。
// 整段一次调用：键序按缓冲区连续推进（OfpMtkParser.cs L45-48 记录的陷阱）。
QByteArray mtkShuffle(const QByteArray &stored)
{
    const QByteArray &key = mtkShuffleKey();
    QByteArray out = stored;
    for (qsizetype i = 0; i < out.size(); ++i)
        out[i] = char(quint8(key.at(i % key.size())) ^ nibbleSwap(quint8(stored.at(i))));
    return out;
}

// 定长 C 字符串字段（ofp_mtk_decrypt.py cleancstring() L112-113:
// replace(b"\x00", b"").decode('utf-8') —— 去掉全部 0x00 后按 UTF-8 解码，
// 不是首 NUL 截断 + Latin-1；仅非 ASCII 名可辨差异）
QString cleanCString(const QByteArray &field)
{
    // 0x00 在 UTF-8 中只能是 U+0000 的编码（不参与任何多字节序列），故"解码后删
    // U+0000"与"删 0x00 字节后解码"逐字节等价
    QString out = QString::fromUtf8(field);
    out.remove(QChar(u'\0'));
    return out;
}

// QC 末页尺寸扫描（extract_xml() L119-123）: 末页 +0x10 LE32 == 0x7CEF。
// 以 tail.size() 为基准而非 fileSize：tail 可为整包或末尾若干页，取"tail 末页"始终等价于"文件末页"。
quint32 qcPageSizeFromTail(const QByteArray &tail)
{
    for (const quint32 pageSize : {kQcPageSizeSmall, kQcPageSizeLarge}) {
        // 只需 tail 覆盖一个完整末页；魔数与清单字段都在末页前 0x1C 字节内
        if (quint64(tail.size()) < quint64(pageSize))
            continue;
        const qsizetype pageBase = tail.size() - qsizetype(pageSize);
        if (le32(tail, pageBase + 0x10) == kQcMagic)
            return pageSize;
    }
    return 0;
}

// MTK 密钥试解（brutekey() L101-110）: 首 16B 逐候选解密，明文前缀 == "MMM" 即命中。
// 命中时按需回填 keyId/key/iv。
bool tryMtkKey(const QByteArray &head, QString *keyId, QByteArray *key, QByteArray *iv)
{
    if (head.size() < 16)
        return false;
    const QByteArray block = head.left(16);
    for (const OppoKeyPair &candidate : mtkKeyCandidates()) {
        const QByteArray plain = aes128CfbDecrypt(block, candidate.key, candidate.iv);
        // 空返回（密码 helper 的失败契约）与明文不匹配同样按未命中处理；
        // 调用方据此走中文错误分支（A9：空返回不得静默继续）
        if (plain.size() >= 3 && plain.startsWith(kMtkPlainMagic)) {
            if (keyId)
                *keyId = candidate.keyId;
            if (key)
                *key = candidate.key;
            if (iv)
                *iv = candidate.iv;
            return true;
        }
    }
    return false;
}

// 乘法溢出防护（页数 × 页尺寸）
bool mulSafe(quint64 a, quint64 b, quint64 &out)
{
    if (b != 0 && a > std::numeric_limits<quint64>::max() / b)
        return false;
    out = a * b;
    return true;
}

// ==================== QC 清单解析 ====================

// 单个 <File> 元素 → OfpFile。属性语义出处 decryptitem() L247-272（Python）与
// OfpQcParser.DecryptItem()（C#）：
//   Path（兼容 filename）/ FileOffsetInSrc（页单位；缺失回退 SizeInSectorInSrc）/ SizeInByteInSrc
//   / SizeInSectorInSrc / md5 / sha256 / sparse。
// 无文件名或无偏移的条目跳过（参照实现返回空名/start==-1 后 continue）。
bool appendQcFile(const QXmlStreamAttributes &attrs, const QString &group, quint32 pageSize,
                  quint64 fileSize, QList<OfpFile> &out, QString *error)
{
    QString name = attrs.value(QStringLiteral("Path")).toString();
    if (name.isEmpty())
        name = attrs.value(QStringLiteral("filename")).toString();
    if (name.isEmpty())
        return true;

    bool ok = false;
    quint64 offsetPages = attrs.value(QStringLiteral("FileOffsetInSrc")).toULongLong(&ok);
    if (!ok) {
        offsetPages = attrs.value(QStringLiteral("SizeInSectorInSrc")).toULongLong(&ok);
        if (!ok)
            return true;  // 无偏移信息（decryptitem 的 start == -1 → continue）
    }
    quint64 offset = 0;
    if (!mulSafe(offsetPages, pageSize, offset))
        return fail(error, QStringLiteral("OFP 文件表越界：%1 的偏移字段溢出").arg(name));

    // 落盘长度 = SizeInByteInSrc（decryptfile() 实际写出的字节数即 rlength = SizeInByteInSrc）。
    // 属性缺失时回退 SizeInSectorInSrc × 页尺寸：Python 参照此时写出 0 字节（rlength 默认 0），
    // 本实现改从 C# 模型（OppEntry.Size = 扇区长度）取非零值——真实清单两者都在，此处仅为兜底。
    // Task 4 的提取长度一律按本字段。
    quint64 size = 0;
    bool haveSize = false;
    const quint64 byteSize = attrs.value(QStringLiteral("SizeInByteInSrc")).toULongLong(&haveSize);
    if (haveSize) {
        size = byteSize;
    } else {
        const quint64 sectors = attrs.value(QStringLiteral("SizeInSectorInSrc")).toULongLong(&haveSize);
        if (haveSize && !mulSafe(sectors, pageSize, size))
            return fail(error, QStringLiteral("OFP 文件表越界：%1 的长度字段溢出").arg(name));
    }

    // 越界防护（spec §5）: 偏移 + 长度必须落在包内
    if (offset > fileSize || size > fileSize - offset)
        return fail(error,
                    QStringLiteral("OFP 文件表越界：%1（偏移 %2 + 长度 %3）超出包大小 %4")
                        .arg(name)
                        .arg(offset)
                        .arg(size)
                        .arg(fileSize));

    OfpFile file;
    file.name = name;
    file.group = group;
    file.offset = offset;
    file.size = size;
    // 组语义映射（main() L340-347 + FirmwareKit OfpQcParser.ComputeEncryptedSize() L165-181）:
    //   Sahara                          → 整段解密（decryptsize = rlength）
    //   Firmware/DigestsToSign/Chained… → 明文（copy 路径）
    //   其余（Config/Provision/未知组）  → 仅前 min(0x40000, size) 解密
    // 注: C# 默认分支用扇区长度 length，Python decryptfile() 用 rlength 取 min；
    //     本实现从 Python 主参照（= brief 的 min(0x40000, size)）。
    if (group == QLatin1String("Sahara")) {
        file.fullDecrypt = true;
        file.encryptedSize = size;
    } else if (group == QLatin1String("Firmware") || group == QLatin1String("DigestsToSign")
               || group == QLatin1String("ChainedTableOfDigests")) {
        file.encryptedSize = 0;
    } else {
        file.encryptedSize = qMin(kQcPartialDecryptSize, size);
    }
    file.sha256Hex = attrs.value(QStringLiteral("sha256")).toString();
    file.md5Hex = attrs.value(QStringLiteral("md5")).toString();
    const QString sparse = attrs.value(QStringLiteral("sparse")).toString();
    file.sparse = sparse == QLatin1String("1") || sparse.compare(QLatin1String("true"),
                                                                Qt::CaseInsensitive) == 0;
    out.append(file);
    return true;
}

// QC 清单（ProFile.xml）解析。遍历结构照 main() L328-347（Python）与
// OfpQcParser.ParseEntries() L43-69（C#）:
//   顶层子元素 = 组; 组内元素带 Path/filename → 文件；不带 → 视为容器, 其子元素仍按该组处理。
bool parseQcManifest(const QByteArray &xml, quint32 pageSize, quint64 fileSize,
                     QList<OfpFile> &out, QString *error)
{
    QXmlStreamReader reader(xml);
    if (!reader.readNextStartElement())
        return fail(error, QStringLiteral("OFP 清单不是有效的 XML（缺少根元素）"));

    while (reader.readNextStartElement()) {          // 顶层: 组
        const QString group = reader.name().toString();
        while (reader.readNextStartElement()) {      // 组内: 文件元素或容器
            if (reader.attributes().hasAttribute(QStringLiteral("Path"))
                || reader.attributes().hasAttribute(QStringLiteral("filename"))) {
                if (!appendQcFile(reader.attributes(), group, pageSize, fileSize, out, error))
                    return false;
                reader.skipCurrentElement();
            } else {
                // 容器: 子元素按同一组处理；子元素无 Path 时 appendQcFile 静默跳过
                while (reader.readNextStartElement()) {
                    if (!appendQcFile(reader.attributes(), group, pageSize, fileSize, out, error))
                        return false;
                    reader.skipCurrentElement();
                }
                // 容器的结束标签已由 readNextStartElement() 消费，直接继续外层循环
            }
        }
    }
    if (reader.hasError()) {
        return fail(error,
                    QStringLiteral("OFP 清单 XML 解析失败：%1").arg(reader.errorString()));
    }
    return true;
}

// ==================== 分支实现 ====================

// QC 分支: 尾页字段 → 清单密文 → 逐候选试解（含 "<?xml" 即命中）→ XML 解析。
bool parseQc(QFile &file, const QByteArray &tail, quint64 fileSize, OfpInfo &info,
             QString *error)
{
    const quint32 pageSize = qcPageSizeFromTail(tail);
    if (pageSize == 0)
        return fail(error, QStringLiteral("OFP 尾页缺少 QC 标记（0x7CEF）"));
    const qsizetype pageBase = tail.size() - qsizetype(pageSize);

    const quint64 xmlOffset = quint64(le32(tail, pageBase + kQcPageXmlOffsetOff)) * pageSize;
    quint64 xmlLength = le32(tail, pageBase + kQcPageXmlLengthOff);
    if (xmlLength < kQcShortXmlThreshold) {
        // A57 hack（extract_xml() L132-133）: 长度字段不可信，按"清单紧邻末页前 0x57 字节"重算
        const qint64 recalculated = qint64(fileSize - pageSize) - qint64(xmlOffset)
                                    - qint64(kQcA57LengthAdjust);
        if (recalculated <= 0)
            return fail(error, QStringLiteral("OFP 清单长度异常（字段 %1，重算得 %2），文件可能被截断")
                                   .arg(xmlLength)
                                   .arg(recalculated));
        xmlLength = quint64(recalculated);
    }
    if (xmlOffset > fileSize || xmlLength == 0 || xmlLength > fileSize - xmlOffset)
        return fail(error, QStringLiteral("OFP 清单越界：偏移 %1 + 长度 %2 超出包大小 %3")
                               .arg(xmlOffset)
                               .arg(xmlLength)
                               .arg(fileSize));
    // 病态长度防护（Task 终审复审）：放在既有越界检查之后 —— 超出文件大小的值仍报
    // "越界"（原文案不变），只有"≤文件大小但不成比例"的声明/重算值走这里。
    // 检查在 file.read() 之前 → 不产生任何大分配。
    if (xmlLength > kMaxManifestLength)
        return fail(error,
                    QStringLiteral("OFP 清单长度异常（%1 字节，超出 %2 MiB 上限），文件可能已损坏")
                        .arg(xmlLength)
                        .arg(kMaxManifestLength / (1024 * 1024)));
    if (!file.seek(qint64(xmlOffset)))
        return fail(error, QStringLiteral("OFP 清单读取失败（偏移 %1）").arg(xmlOffset));
    const QByteArray encXml = file.read(qint64(xmlLength));
    if (quint64(encXml.size()) != xmlLength)
        return fail(error, QStringLiteral("OFP 清单读取不完整：期望 %1 字节，实得 %2")
                               .arg(xmlLength)
                               .arg(encXml.size()));

    // 逐候选试解（generatekey2() L94-112）: 解密结果含 "<?xml" 子串即命中
    const QList<OppoKeyPair> candidates = qcKeyCandidates();
    QByteArray xml;
    for (const OppoKeyPair &candidate : candidates) {
        const QByteArray plain = aes128CfbDecrypt(encXml, candidate.key, candidate.iv);
        if (plain.contains("<?xml")) {
            xml = plain;
            info.keyId = candidate.keyId;
            info.key = candidate.key;
            info.iv = candidate.iv;
            break;
        }
    }
    if (xml.isEmpty()) {
        // 试解全空（含 aes128CfbDecrypt 的空返回契约）→ 统一收口为密钥/损坏错误（A9）
        return fail(error,
                    QStringLiteral("密钥未知或文件损坏：%1 条内置公开密钥均未解出清单"
                                   "（固件晚于 2021？可尝试导入外部密钥文件）")
                        .arg(candidates.size()));
    }

    info.variant = OfpVariant::Qc;
    info.pageSize = pageSize;
    return parseQcManifest(xml, pageSize, fileSize, info.files, error);
}

// MTK 分支: 尾 0x6C 解混淆头（prjname/cpu/flashtype/条目数）→ 文件表（整表连续解混淆）。
bool parseMtk(QFile &file, const QByteArray &head, const QByteArray &tail, quint64 fileSize,
              OfpInfo &info, QString *error)
{
    QString keyId;
    QByteArray key;
    QByteArray iv;
    if (!tryMtkKey(head, &keyId, &key, &iv))
        return fail(error, QStringLiteral("密钥未知或文件损坏：首 16 字节未匹配任何 MTK 密钥"));
    info.keyId = keyId;
    info.key = key;
    info.iv = iv;

    if (quint64(tail.size()) < kMtkHeaderSize)
        return fail(error, QStringLiteral("MTK 包头不完整：尾部仅 %1 字节").arg(tail.size()));
    const QByteArray hdr = mtkShuffle(tail.right(qsizetype(kMtkHeaderSize)));
    info.projectName = cleanCString(hdr.mid(kMtkPrjNameOff, kMtkPrjNameLen));
    info.version = cleanCString(hdr.mid(kMtkFlashTypeOff, kMtkFlashTypeLen));

    const quint32 entryCount = le16(hdr, kMtkEntryCountOff);
    quint64 tableLength = 0;
    if (!mulSafe(entryCount, kMtkEntrySize, tableLength) || tableLength + kMtkHeaderSize > fileSize)
        return fail(error, QStringLiteral("MTK 文件表越界：%1 条目 × 0x60 超出包大小 %2")
                               .arg(entryCount)
                               .arg(fileSize));
    const quint64 tableOffset = fileSize - tableLength - kMtkHeaderSize;
    if (!file.seek(qint64(tableOffset)))
        return fail(error, QStringLiteral("MTK 文件表读取失败（偏移 %1）").arg(tableOffset));
    const QByteArray tableRaw = file.read(qint64(tableLength));
    if (quint64(tableRaw.size()) != tableLength)
        return fail(error, QStringLiteral("MTK 文件表读取不完整：期望 %1 字节，实得 %2")
                               .arg(tableLength)
                               .arg(tableRaw.size()));
    const QByteArray table = mtkShuffle(tableRaw);

    for (quint32 i = 0; i < entryCount; ++i) {
        const qsizetype base = qsizetype(i) * qsizetype(kMtkEntrySize);
        const QString filename = cleanCString(table.mid(base + kMtkEntryFileOff, 32));
        if (filename.isEmpty())
            continue;  // 空 filename 行 = 无效条目（真实包表尾常有多余空行，不产出空名条目）

        OfpFile entry;
        entry.name = filename;
        entry.offset = le64(table, base + kMtkEntryStartOff);
        entry.size = le64(table, base + kMtkEntryLengthOff);
        entry.encryptedSize = le64(table, base + kMtkEntryEncLenOff);
        if (entry.encryptedSize > entry.size)
            return fail(error, QStringLiteral("MTK 文件表数据段异常：%1 的解密长度 %2 超过文件长度 %3")
                                   .arg(filename)
                                   .arg(entry.encryptedSize)
                                   .arg(entry.size));
        if (entry.offset > fileSize || entry.size > fileSize - entry.offset)
            return fail(error, QStringLiteral("MTK 文件表越界：%1（偏移 %2 + 长度 %3）超出包大小 %4")
                                   .arg(filename)
                                   .arg(entry.offset)
                                   .arg(entry.size)
                                   .arg(fileSize));
        info.files.append(entry);
    }

    info.variant = OfpVariant::Mtk;
    return true;
}

} // namespace

bool detectOFP(const QByteArray &head, const QByteArray &tail, quint64 fileSize,
               OfpVariant &variant)
{
    // 一页都放不下 → 不可能是 OFP（MTK 头 + 文件表也远超一页）
    if (fileSize < kQcMinFileSize)
        return false;
    // tail 是"文件末尾若干字节"，不可能比整个文件还长（调用方参数矛盾的兜底）
    if (quint64(tail.size()) > fileSize)
        return false;

    // 顺序: 先 MTK 后 QC（A1 裁决）
    if (tryMtkKey(head, nullptr, nullptr, nullptr)) {
        variant = OfpVariant::Mtk;
        return true;
    }
    if (qcPageSizeFromTail(tail) != 0) {
        variant = OfpVariant::Qc;
        return true;
    }
    return false;
}

bool parseOFP(const QString &path, OfpInfo &info, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("无法打开 OFP 文件：%1").arg(path));

    const quint64 fileSize = quint64(file.size());
    if (fileSize < kQcMinFileSize) {
        return fail(error, QStringLiteral("OFP 文件过小（%1 字节，不足一页 %2），不可能是有效固件包")
                               .arg(fileSize)
                               .arg(kQcMinFileSize));
    }

    const QByteArray head = file.read(qsizetype(16));   // MTK 试解需完整 16B
    const quint64 tailLength = qMin<quint64>(fileSize, kQcPageSizeLarge);
    if (!file.seek(qint64(fileSize - tailLength)))
        return fail(error, QStringLiteral("OFP 文件读取失败：%1").arg(path));
    const QByteArray tail = file.read(qint64(tailLength));
    if (quint64(tail.size()) != tailLength)
        return fail(error, QStringLiteral("OFP 文件读取不完整：%1").arg(path));

    // 老式密码 ZIP 包（spec §7；main() L286-290 的同款 PK 判定，先于任何密钥尝试）
    if (head.startsWith("PK"))
        return fail(error, QStringLiteral("旧式密码 ZIP 打包的 OFP 暂不支持（Phase A 范围外）"));

    OfpVariant variant = OfpVariant::Unknown;
    if (!detectOFP(head, tail, fileSize, variant)) {
        return fail(error,
                    QStringLiteral("不是有效的 OFP 包：尾部未找到 QC 页标记（0x7CEF），"
                                   "首 16 字节也不匹配任何 MTK 密钥"));
    }

    // 解析结果先落局部对象: 失败时调用方拿到的 info 保持原值（无半截状态）
    OfpInfo parsed;
    const bool ok = (variant == OfpVariant::Qc)
                        ? parseQc(file, tail, fileSize, parsed, error)
                        : parseMtk(file, head, tail, fileSize, parsed, error);
    if (!ok)
        return false;
    info = parsed;
    return true;
}

} // namespace imgopp
