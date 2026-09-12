#pragma once
// OPPO 合成包测试夹具（Task 3 创建；Task 4/5 复用）。
//
// 职责：只按格式事实拼装字节流，绝不调用被测解析代码——期望值由调用方显式给出，避免测试自证。
// 方向与布局来源（逐条对应参照实现）：
//   - QC 清单：AES-128-CFB(segment=128)，用 oppo_crypto 的加密方向
//     （ofp_qc_decrypt.py aes_cfb() L148-151 的解密方向之逆）
//   - QC 载荷分组形态：Sahara 全加密 / Firmware·DigestsToSign·ChainedTableOfDigests 明文 /
//     其余前 min(0x40000,size) 加密（ofp_qc_decrypt.py main() L340-347 + decryptfile() L189-190）
//   - MTK 尾头与文件表：stored[i] = nibbleSwap(plain[i] ^ key[i%7])，key = ASCII "geyixue"
//     （amendments A2：解码方向 out[i] = key[i%7] ^ nibbleSwap(stored[i]) 的逆运算；
//      出处 ofp_mtk_decrypt.py mtk_shuffle() L23-28 / FirmwareKit OppMtkKeyDerivation.MtkShuffle() L113-122）
//   - MTK 包首 16B 解密后以 "MMM" 起始（ofp_mtk_decrypt.py brutekey() L101-110）
// 全部函数 header-only inline，测试目标无需额外源文件。
#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

#include "image_engine/oppo_crypto.h"
#include "image_engine/oppo_keys.h"

namespace ofptest {

// ==================== 通用小工具 ====================

// 半字节交换（swap() ofp_qc_decrypt.py L13-14 同式）
inline quint8 nibbleSwap(quint8 v)
{
    return quint8(((v & 0x0F) << 4) | ((v & 0xF0) >> 4));
}

// MTK 混淆的构造方向（A2 解码式的逆）。key 默认 7 字节 ASCII "geyixue"。
// 注意键序按整段连续推进（key[i % 7]，i 为缓冲区下标）——文件表必须整表一次调用，
// 逐条目调用会重置键位置导致后续条目解混淆错误
// （FirmwareKit OfpMtkParser.cs L45-48 明确记录该陷阱）。
inline QByteArray mtkShuffleEncode(const QByteArray &plain,
                                   const QByteArray &key = QByteArrayLiteral("geyixue"))
{
    QByteArray out = plain;
    for (qsizetype i = 0; i < out.size(); ++i)
        out[i] = char(nibbleSwap(quint8(plain.at(i)) ^ quint8(key.at(i % key.size()))));
    return out;
}

// 定长字段写入（ASCII；不足补 0x00，超长截断）。buf 须已 resize 到可容纳 pos + width。
inline void putFixed(QByteArray &buf, qsizetype pos, qsizetype width, const QString &s)
{
    const QByteArray raw = s.toLatin1();
    for (qsizetype i = 0; i < width; ++i)
        buf[pos + i] = i < raw.size() ? raw.at(i) : '\0';
}

inline void putLE16(QByteArray &buf, qsizetype pos, quint16 v)
{
    buf[pos + 0] = char(v & 0xFF);
    buf[pos + 1] = char((v >> 8) & 0xFF);
}

inline void putLE32(QByteArray &buf, qsizetype pos, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        buf[pos + i] = char((v >> (8 * i)) & 0xFF);
}

inline void putLE64(QByteArray &buf, qsizetype pos, quint64 v)
{
    for (int i = 0; i < 8; ++i)
        buf[pos + i] = char((v >> (8 * i)) & 0xFF);
}

// 取末尾 n 字节（n 超长时返回整段）——detectOFP 的 tail 入参
inline QByteArray tailOf(const QByteArray &blob, quint64 n)
{
    return blob.right(qsizetype(qMin<quint64>(n, quint64(blob.size()))));
}

// QC 组策略涉及的常量（与本模块解析层同源，供测试对照）
inline constexpr quint64 kQcPartialDecryptSize = 0x40000;  // decryptitem() L253 decryptsize
inline constexpr quint64 kQcA57LengthAdjust = 0x57;        // extract_xml() L132-133
// 清单长度字段可信下限：低于此值解析层走 A57 重算分支（extract_xml() L132-133；
// oppo_ofp.cpp parseQc() 同判据），见 buildQcPackage() 末尾的护栏
inline constexpr quint64 kQcShortXmlThreshold = 200;

// ==================== QC 合成包 ====================

// 单文件规格。plaintext = "解包后应得到" 的明文；夹具按组策略把它写成包内形态。
struct QcFileSpec
{
    QString group;            // ProFile 顶层组名（Sahara/Firmware/Config/Provision/…）
    QString path;             // Path 属性值（落盘名）
    QByteArray plaintext;     // 明文载荷；清单 SizeInByteInSrc 取其长度
    quint32 offsetPages = 0;  // FileOffsetInSrc（页单位）。0 = 从 firstDataPage 起自动顺序分配；
                              // 非 0 = 按该值写入清单（可故意越界以测拒绝路径，包体不为其扩容）
    QString sha256;           // 清单 sha256 属性（可空）
    QString md5;              // 清单 md5 属性（可空）
    bool sparse = false;      // 清单 sparse 属性
    bool omitSizeInByteAttr = false;  // 不写 SizeInByteInSrc（测长度回退扇区数 ×页尺寸）
};

struct QcBuildOptions
{
    quint32 pageSize = 0x200;       // QC 尾页候选尺寸（0x200/0x1000）
    int keyIndex = 2;               // qcKeyCandidates() 下标（2 = V1.5.13，速查表派生断言同源）
    quint32 xmlOffsetPages = 3;     // 清单所在页（尾页 +0x14 字段）
    quint32 firstDataPage = 16;     // 自动布局的数据区起始页（须在清单页之后，留出清单余量）
    bool a57XmlLengthHack = false;  // A57 老包：尾页 +0x18 写 0、清单后留 0x57 空隙，
                                    // 迫使解析走 (fileSize-page)-xmlOffset-0x57 重算分支
    QByteArray keyOverride;         // 非空 = 不用候选表，改用此外部 key（制造"密钥未知"包）
    QByteArray ivOverride;          // 与 keyOverride 配对
};

struct QcPackage
{
    QByteArray blob;          // 完整合成包
    QByteArray head;          // 前 16B（detectOFP 入参）
    QByteArray tail;          // 末 0x1000B（detectOFP 入参）
    QString keyId;            // 加密所用候选 id（keyOverride 时为空）
    QByteArray key;           // 加密所用 key
    QByteArray iv;            // 加密所用 iv
    quint32 pageSize = 0;
    quint64 xmlOffset = 0;    // 清单在包内的绝对偏移
    quint64 xmlLength = 0;    // 清单密文长度（= 明文 XML 长度，CFB 等长）

    bool isValid() const { return !blob.isEmpty(); }
};

inline QcPackage buildQcPackage(const QList<QcFileSpec> &files, const QcBuildOptions &opts = {})
{
    QcPackage pkg;
    pkg.pageSize = opts.pageSize;
    if (opts.pageSize == 0)
        return pkg;

    if (opts.keyOverride.isEmpty() || opts.ivOverride.isEmpty()) {
        const QList<imgopp::OppoKeyPair> candidates = imgopp::qcKeyCandidates();
        if (opts.keyIndex < 0 || opts.keyIndex >= candidates.size())
            return pkg;
        pkg.keyId = candidates[opts.keyIndex].keyId;
        pkg.key = candidates[opts.keyIndex].key;
        pkg.iv = candidates[opts.keyIndex].iv;
    } else {
        pkg.key = opts.keyOverride;
        pkg.iv = opts.ivOverride;
    }

    // ---- 1. 载荷形态（密文/明文 混合，按组策略；main() L340-347）----
    QList<QByteArray> onDisk;
    onDisk.reserve(files.size());
    for (const QcFileSpec &spec : files) {
        const QByteArray &plain = spec.plaintext;
        if (spec.group == QLatin1String("Sahara")) {
            // Sahara 组整段解密 → 包内整段加密（decryptsize = rlength）
            onDisk.append(imgopp::aes128CfbEncrypt(plain, pkg.key, pkg.iv));
        } else if (spec.group == QLatin1String("Firmware")
                   || spec.group == QLatin1String("DigestsToSign")
                   || spec.group == QLatin1String("ChainedTableOfDigests")) {
            // 明文组 → 包内明文（main() 的 copy 路径）
            onDisk.append(plain);
        } else {
            // 其余组 → 前 min(0x40000, size) 加密，余下明文
            const quint64 n = qMin<quint64>(kQcPartialDecryptSize, quint64(plain.size()));
            onDisk.append(imgopp::aes128CfbEncrypt(plain.left(qsizetype(n)), pkg.key, pkg.iv)
                          + plain.mid(qsizetype(n)));
        }
    }

    // ---- 2. 页对齐布局（显式 offsetPages 优先；显式值不参与包尺寸推导 → 可制造越界条目）----
    QList<quint64> offsets;
    offsets.reserve(files.size());
    quint64 cursorPage = opts.a57XmlLengthHack ? 0 : opts.firstDataPage;
    for (const QcFileSpec &spec : files) {
        const quint64 len = quint64(onDisk.at(offsets.size()).size());
        const quint64 pages = qMax<quint64>(1, (len + opts.pageSize - 1) / opts.pageSize);
        if (spec.offsetPages == 0) {
            offsets.append(cursorPage * opts.pageSize);
            cursorPage += pages;
        } else {
            offsets.append(quint64(spec.offsetPages) * opts.pageSize);
        }
    }
    const quint64 dataEnd = cursorPage * opts.pageSize;

    // ---- 3. 清单 XML（属性集与 brief Step 1 示例一致；偏移已在步 2 定型）----
    QString xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<ProFile>\n");
    for (qsizetype i = 0; i < files.size(); ++i) {
        const QcFileSpec &spec = files.at(i);
        const quint64 sector = (quint64(spec.plaintext.size()) + opts.pageSize - 1) / opts.pageSize;
        QString elem = QStringLiteral("    <File Path=\"%1\" FileOffsetInSrc=\"%2\"")
                           .arg(spec.path)
                           .arg(offsets.at(i) / opts.pageSize);
        if (!spec.omitSizeInByteAttr)
            elem += QStringLiteral(" SizeInByteInSrc=\"%1\"").arg(spec.plaintext.size());
        elem += QStringLiteral(" SizeInSectorInSrc=\"%1\" md5=\"%2\" sha256=\"%3\" sparse=\"%4\"/>\n")
                    .arg(sector)
                    .arg(spec.md5, spec.sha256,
                         spec.sparse ? QStringLiteral("true") : QStringLiteral("false"));
        xml += QStringLiteral("  <%1>\n").arg(spec.group) + elem
               + QStringLiteral("  </%1>\n").arg(spec.group);
    }
    xml += QStringLiteral("</ProFile>\n");

    const QByteArray encXml = imgopp::aes128CfbEncrypt(xml.toUtf8(), pkg.key, pkg.iv);
    if (encXml.isEmpty())
        return QcPackage();
    // 护栏（Task 4 补）：清单长度 < 200 时解析层会走 A57 长度重算分支
    // （extract_xml() L132-133 / oppo_ofp.cpp parseQc()），只有 a57XmlLengthHack 模式是
    // 刻意构造。其余情况直接拒绝构造 → 调用方以 isValid() 拦住，避免"夹具无意造出短清单
    // → 解析走错路径 → 断言恒真/恒假"的假测试。
    if (!opts.a57XmlLengthHack && quint64(encXml.size()) < kQcShortXmlThreshold)
        return QcPackage();
    const quint64 xmlOffset = quint64(opts.xmlOffsetPages) * opts.pageSize;
    const quint64 xmlEnd = xmlOffset + quint64(encXml.size());

    // 载荷不得与清单区重叠（夹具布局错误 → 返回空包，测试以 isValid() 拦住）
    for (qsizetype i = 0; i < files.size(); ++i) {
        const QByteArray &d = onDisk.at(i);
        if (!d.isEmpty() && offsets.at(i) < xmlEnd && offsets.at(i) + quint64(d.size()) > xmlOffset)
            return QcPackage();
    }

    // ---- 4. 总长（尾页在最后；A57 模式留 0x57 空隙使重算长度恰好 == 清单长度）----
    quint64 total = 0;
    if (opts.a57XmlLengthHack) {
        if (dataEnd > xmlOffset)
            return QcPackage();  // 数据区与清单页冲突
        total = opts.pageSize + xmlOffset + quint64(encXml.size()) + kQcA57LengthAdjust;
    } else {
        const quint64 contentPages = (qMax(dataEnd, xmlEnd) + opts.pageSize - 1) / opts.pageSize;
        total = (contentPages + 1) * opts.pageSize;
    }

    // ---- 5. 落盘 ----
    QByteArray blob(int(total), '\0');
    for (qsizetype i = 0; i < files.size(); ++i) {
        const QByteArray &d = onDisk.at(i);
        if (d.isEmpty())
            continue;
        // 越界载荷（故意构造的恶意条目）不写入包体
        if (offsets.at(i) + quint64(d.size()) > quint64(blob.size()))
            continue;
        blob.replace(qsizetype(offsets.at(i)), d.size(), d);
    }
    blob.replace(qsizetype(xmlOffset), encXml.size(), encXml);

    const quint64 tailBase = total - opts.pageSize;
    putLE32(blob, qsizetype(tailBase + 0x10), 0x7CEF);   // 尾页魔数（extract_xml() L119-123）
    putLE32(blob, qsizetype(tailBase + 0x14), opts.xmlOffsetPages);
    putLE32(blob, qsizetype(tailBase + 0x18),
            opts.a57XmlLengthHack ? 0u : quint32(encXml.size()));

    pkg.blob = blob;
    pkg.head = blob.left(16);
    pkg.tail = tailOf(blob, 0x1000);
    pkg.xmlOffset = xmlOffset;
    pkg.xmlLength = quint64(encXml.size());
    return pkg;
}

// ==================== MTK 合成包 ====================

// MTK 文件表条目规格。
struct MtkFileSpec
{
    QString name;                  // 条目 name[32]（分区名）
    QString filename;              // 条目 filename[32]（落盘名；空 = 保留空行，解析应跳过）
    QByteArray plaintext;          // 明文载荷；条目 length 取其长度
    quint64 start = 0;             // 包内绝对偏移。0 = 从包头区之后自动顺序分配；
                                   // 非 0 = 写入该值（可故意越界以测拒绝路径，包体不为其扩容）
    qint64 encryptedLength = -1;   // 需解密的前缀长度：-1 = 全部加密，0 = 全明文，其余为字节数
    quint64 crc = 0;               // 条目 crc u64@88（解析层不使用，仅供字节级对照）
};

struct MtkBuildOptions
{
    int keyIndex = 0;                 // mtkKeyCandidates() 下标（0 = MTK0）
    QString prjname = QStringLiteral("TESTPRJ");   // 尾头 prjname[46]
    QString cpu = QStringLiteral("MT6765");        // 尾头 cpu[7]
    QString flashtype = QStringLiteral("UFS");     // 尾头 flashtype[5]
    QString prjinfo = QStringLiteral("PRJINFO");   // 尾头 prjinfo[32]
    quint32 entryCount = 0;           // 0 = files.size()；更大的值 → 表尾补空条目（测跳过逻辑）
    quint32 headerRegionSize = 0x200; // 包首 "MMM" 头区尺寸（其前 16B 解密后以 MMM 起始）
};

struct MtkPackage
{
    QByteArray blob;
    QByteArray head;              // 前 16B（detectOFP 入参）
    QByteArray tail;              // 末 0x1000B（detectOFP 入参）
    QString keyId;
    QByteArray key;
    QByteArray iv;
    quint64 headerOffset = 0;     // 尾 0x6C 头偏移
    quint64 tableOffset = 0;      // 文件表偏移
    quint32 entryCount = 0;
    QList<quint64> starts;        // 各文件实际起始偏移（夹具布局意图，供测试对照）

    bool isValid() const { return !blob.isEmpty(); }
};

// 布局：[0x200 包头区("MMM"+0)][数据区（页对齐）][文件表 entryCount×0x60][尾 0x6C 头]
// 尾头字段偏移出处 ofp_mtk_decrypt.py main() L125（native 对齐版）＝ FirmwareKit OppHeader.cs L93-101。
inline MtkPackage buildMtkPackage(const QList<MtkFileSpec> &files, const MtkBuildOptions &opts = {})
{
    MtkPackage pkg;
    const QList<imgopp::OppoKeyPair> candidates = imgopp::mtkKeyCandidates();
    if (opts.keyIndex < 0 || opts.keyIndex >= candidates.size())
        return pkg;
    pkg.keyId = candidates[opts.keyIndex].keyId;
    pkg.key = candidates[opts.keyIndex].key;
    pkg.iv = candidates[opts.keyIndex].iv;

    constexpr quint64 kHeaderSize = 0x6C;    // main() L120 hdrlength
    constexpr quint64 kEntrySize = 0x60;     // main() L139 "<32s Q Q Q 32s Q"
    const quint64 headerRegion = qMax<quint64>(opts.headerRegionSize, 0x200);

    // 数据区：自动布局从包头区之后按页推进；显式 start 不参与推进
    quint64 cursor = headerRegion;
    for (const MtkFileSpec &spec : files) {
        const quint64 pages = qMax<quint64>(
            1, (quint64(spec.plaintext.size()) + 0x200 - 1) / 0x200);
        if (spec.start == 0) {
            pkg.starts.append(cursor);
            cursor += pages * 0x200;
        } else {
            pkg.starts.append(spec.start);
        }
    }

    const quint32 entryCount = opts.entryCount ? opts.entryCount : quint32(files.size());
    const quint64 tableLen = quint64(entryCount) * kEntrySize;
    const quint64 tableOffset = cursor;
    const quint64 total = tableOffset + tableLen + kHeaderSize;

    // 文件表（整表混淆：键序跨条目连续，见 mkShuffleEncode 注释）
    QByteArray table(int(tableLen), '\0');
    for (qsizetype i = 0; i < files.size(); ++i) {
        const MtkFileSpec &spec = files.at(i);
        const quint64 base = quint64(i) * kEntrySize;
        const quint64 enc = spec.encryptedLength < 0 ? quint64(spec.plaintext.size())
                                                     : quint64(spec.encryptedLength);
        putFixed(table, qsizetype(base + 0x00), 32, spec.name);      // name[32]@0
        putLE64(table, qsizetype(base + 0x20), pkg.starts.at(i));    // start u64@32
        putLE64(table, qsizetype(base + 0x28), quint64(spec.plaintext.size()));  // length u64@40
        putLE64(table, qsizetype(base + 0x30), enc);                 // encrypted_length u64@48
        putFixed(table, qsizetype(base + 0x38), 32, spec.filename);  // filename[32]@56
        putLE64(table, qsizetype(base + 0x58), spec.crc);            // crc u64@88
    }
    const QByteArray tableStored = mtkShuffleEncode(table);

    // 尾 0x6C 头
    QByteArray hdr(int(kHeaderSize), '\0');
    putFixed(hdr, 0x00, 46, opts.prjname);       // prjname[46]@0（46..47 为 Q 对齐填充）
    putFixed(hdr, 0x3C, 7, opts.cpu);            // cpu[7]@60
    putFixed(hdr, 0x43, 5, opts.flashtype);      // flashtype[5]@67
    putLE16(hdr, 0x48, quint16(entryCount));     // hdr2entries u16@72
    putFixed(hdr, 0x4A, 32, opts.prjinfo);       // prjinfo[32]@74
    const QByteArray hdrStored = mtkShuffleEncode(hdr);

    // 组装
    QByteArray blob(int(total), '\0');
    QByteArray pkgHeader(int(headerRegion), '\0');
    pkgHeader.replace(0, 3, QByteArrayLiteral("MMM"));   // brutekey() L107 判定前缀
    blob.replace(0, pkgHeader.size(), imgopp::aes128CfbEncrypt(pkgHeader, pkg.key, pkg.iv));
    for (qsizetype i = 0; i < files.size(); ++i) {
        const MtkFileSpec &spec = files.at(i);
        const quint64 start = pkg.starts.at(i);
        if (spec.plaintext.isEmpty()
            || start + quint64(spec.plaintext.size()) > quint64(blob.size()))
            continue;   // 越界载荷（恶意条目）不写入包体
        const quint64 enc = spec.encryptedLength < 0 ? quint64(spec.plaintext.size())
                                                     : quint64(spec.encryptedLength);
        const QByteArray headBytes =
            imgopp::aes128CfbEncrypt(spec.plaintext.left(qsizetype(enc)), pkg.key, pkg.iv);
        blob.replace(qsizetype(start), spec.plaintext.size(),
                     headBytes + spec.plaintext.mid(qsizetype(enc)));
    }
    blob.replace(qsizetype(tableOffset), tableStored.size(), tableStored);
    blob.replace(qsizetype(total - kHeaderSize), hdrStored.size(), hdrStored);

    pkg.blob = blob;
    pkg.head = blob.left(16);
    pkg.tail = tailOf(blob, 0x1000);
    pkg.headerOffset = total - kHeaderSize;
    pkg.tableOffset = tableOffset;
    pkg.entryCount = entryCount;
    return pkg;
}

} // namespace ofptest
