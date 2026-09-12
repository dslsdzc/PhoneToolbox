#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>

#include "image_engine/oppo_extract.h"
#include "image_engine/oppo_keys.h"
#include "image_engine/oppo_ops.h"

// OPS（OnePlus MSM 包）解包测试。
//
// 对拍向量的来源：reference/oppo_decrypt/opscrypto.py（MIT，bkerler）的
// key_custom(..., encrypt=True) 实际产出后固化（生成命令见文件末尾注释），**不是**用本仓库
// 实现生成的密文；密码本身以外的那组定值即为本模块的最高权威。
// 合成包的打包方向用 imgopp::opsEncrypt()（同密码的加密方向，见 oppo_keys.h 说明）：
// 密码已被定值向量钉住，夹具再用它拼包，端到端用例断言的是真实产物字节。
class TestOppoOps : public QObject
{
    Q_OBJECT

private slots:
    void opsCipherAgainstPython();
    void detectOpsFromTail();
    void parseOpsKeyCandidates();
    void parseOpsSkipsEntryWithoutOffset();
    void extractOpsEndToEnd();
    void extractOpsProgramHash();
    void extractOpsRejectsUnsafeNames();
    void extractOpsRejectsBadPackages();
};

// ==================== 通用小工具（同 test_oppo_extract.cpp 的文件内实现） ====================

// 确定性伪随机（不用 qrand，避免未播种告警噪音）
static QByteArray pseudoRandom(int size, quint32 seed = 0x12345678u)
{
    QByteArray out(size, '\0');
    quint32 s = seed;
    for (int i = 0; i < size; ++i) {
        s = s * 1664525u + 1013904223u;
        out[i] = char((s >> 24) & 0xFF);
    }
    return out;
}

static QString writePkg(const QString &dirPath, const QString &name, const QByteArray &data)
{
    const QString path = QDir(dirPath).filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
        return QString();
    return path;
}

static QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

static QString hexOf(const QByteArray &data, QCryptographicHash::Algorithm algo)
{
    QCryptographicHash h(algo);
    h.addData(data);
    return QString::fromLatin1(h.result().toHex());
}

// OPS 内文件的 sha256 口径（calc_digest() L462-470）：整段 + 补零到 0x1000 边界
static QString opsDigest(const QByteArray &data)
{
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(data);
    const quint64 rem = quint64(data.size()) % 0x1000;
    if (rem != 0)
        h.addData(QByteArray(qsizetype(0x1000 - rem), '\0'));
    return QString::fromLatin1(h.result().toHex());
}

// 进度回调观察器：记录 (文件名, 百分比) 序列
struct ProgressLog
{
    QStringList names;
    QList<int> percents;

    imgopp::ExtractProgress callback()
    {
        return [this](const QString &name, int percent) {
            names << name;
            percents << percent;
        };
    }

    bool monotonic() const
    {
        for (int i = 1; i < percents.size(); ++i)
            if (percents.at(i) < percents.at(i - 1))
                return false;
        return true;
    }
};

// ==================== 定值对拍向量（Python 参照产出，见文件末尾生成命令） ====================

// 16B 明文 → 块路（走 mbox 轮密钥更新）
static QByteArray ptBlock() { return QByteArrayLiteral("TestBlock1234567"); }
static const char kCtBlockMbox5[] = "627e96484bf203f25d9d3b049f13837c";
static const char kCtBlockMbox6[] = "e51b9e2881b38200cc4278677ae1c0ff";
static const char kCtBlockMbox4[] = "984027e65d44ff276bcf80e862c963b5";

// 41B 明文 → 3 个块（末块仅 9 字节参与），参照产出 48B：钉"末块补齐 + 块链反馈"
static QByteArray ptBlock41() { return QByteArrayLiteral("BlockPath0123456789ABCDEF0123456789ABCDEF"); }
static const char kCtBlock41Mbox5[] =
    "74778a5f62ce0de55e9c38059812807dd58b2f51f536e62a1bb02e30482f7c92"
    "b678c3d0958e61f84ab9cae826c1ff15";
static const char kCtBlock41Mbox6[] =
    "f312823fa88f8c17cf437b667de0c3fe16a18e0d780b249f8ff317aae97586b2"
    "5c80bf166da28fb448350ec80da82ce3";
static const char kCtBlock41Mbox4[] =
    "8e493bf17478f13068ce83e965c860b4ee7daa24b14eb1287c26afb9a0e005c5"
    "cad09d2f990b257f250342ddaf5e7a6b";

// 512B 明文（"0123456789ABCDEF" × 32）→ 32 个整块：钉长输入的块链推进
// （每块状态 = 上一密文块，链错一处则后续全错；真实 SAHARA 载荷即 MB 级多块）
static QByteArray ptBlock512() { return QByteArrayLiteral("0123456789ABCDEF").repeated(32); }
static const char kCtBlock512Mbox5[] =
    "062ad70f3dab5aa60e954875e862f00db58162a4f9d1f575a7ee33e7c3d387ba7adaec7dc61fa4a10a68f18b035392180fe452dd942a180fcf162663599ba762e4b73899fc15851793c9a5b912a01ce0fdd66259d899f974b7c95efbd41dfabaaa259fbfed93e7cdda82a144fad6074cf2b2e76d1c7b4569b467530011db2716"
    "0929ae65cacd3ed187431ff14eab36027800a5d1d6987d119f9315e3e9578907bdd3d034fa4010a1e7159bf9c4a951c2b84e84284fddaf9dff0393c4e7374056d89de99d30d334965aefecd025b27f36a3e392879d549126642b272e8ae24b69b733f714e442a79f4fe688c55d0ada92ae8422ca22054d4a37329657c28cf15c"
    "f7cab3c849f8a56c861b9689c2e90f2e90d13c3fb18a0159c0fb73376979bcdbf624a9386da598a24689fb70168c57062463fd04669ae734a11619916787a16492dab74bc8ce3fb76f26fb63267c0bc443904349179429ca1a6d8f07d57ce4364001db66c93a37fe57c01f829fd71aa28a7a871e30c3e0ba1ccff58f6d16b66b"
    "0c32dbdd6f638fcd578e75d6750741aca3acaba01c0feebf5767e6702219230d0b4dc7c8c11ea431aa13fb61433b95fc6675de9edd1e00500ed37f120ffb250e59bb07f5a37fe709f2111396633fd5c1f2bbb2c238af449e5732702bb3a463a8c3d7204806d850bc4dc9e5e7d06ec470cdd9d02bd43d3b646ceb7d2f6c1b3a6a";

// 尾路（≤15B）：走 sbox 轮密钥更新。三个 mbox 产出相同 —— 尾路的轮密钥更新用 sbox，
// 而状态初值是常量 d1b5e3…（A3：不来自 mbox），故这组向量钉的是 sbox 路径 + 不足 4B 零补齐
static const char kCtTail8[] = "0aefec71200abac0";   // 明文 "OpsTail!"
static const char kCtTail5[] = "74adac117463d6e1";   // 明文 "12345"（末词 1 字节）
static const char kCtTail2[] = "04dd9f25";           // 明文 "AB"（末词 2 字节）

// 13/14/15B：**按参照文件流口径**（先用零补齐到 4 的倍数再 key_custom，再截断到原长）。
// 参照调用方 decryptfile() L428-430 / encryptsubsub() L440-446 都先 pad4 → 这三档落在
// **块路**（pad4 = 16B > 0xF），与本实现的 pad4 分支判定一致；裸调 helper 会在这三档走
// 尾路、结果完全不同（本机复算 len 1..41，仅 13/14/15 两口径不同）。
static const char kPtLen13[] = "ThirteenBytes";      // 13B
static const char kPtLen14[] = "FourteenBytes!";     // 14B
static const char kPtLen15[] = "FifteenBytes123";    // 15B
static const char kCtLen13Mbox5[] = "62738c4e7dfb09ff74d57d52d8";
static const char kCtLen13Mbox6[] = "e516842eb7ba880de50a3e313d";
static const char kCtLen13Mbox4[] = "984d3de06b4df52a4287c6be25";
static const char kCtLen14Mbox5[] = "7074904e7dfb09ff74d57d52d807";
static const char kCtLen14Mbox6[] = "f711982eb7ba880de50a3e313df5";
static const char kCtLen14Mbox4[] = "8a4a21e06b4df52a4287c6be25dd";
static const char kCtLen15Mbox5[] = "707283486cfb02d34fd86c449a1486";
static const char kCtLen15Mbox6[] = "f7178b28a6ba8321de072f277fe6c5";
static const char kCtLen15Mbox4[] = "8a4c32e67a4dfe06798ad7a867ce66";

// ==================== 合成 OPS 包 ====================
//
// 布局照 opscrypto.py encrypt 路径（main() L655-724 的 pos 推进 + 尾页构造）:
//   [条目 0 密文/明文，页对齐][条目 1 …][settings.xml 密文，页对齐][尾页 0x200]
// 尾页 +0x14 = settings 扇区位置、+0x18 = settings 明文长度（未按 16 对齐的真实长度）。

struct OpsFileSpec
{
    QString group;               // SAHARA / UFS_PROVISION / Program
    QString name;                // Path（SAHARA/UFS_PROVISION）或 filename（Program）
    QByteArray onDisk;           // 包内字节（SAHARA = 密文，其余 = 明文）
    quint64 plainSize = 0;       // 明文长度（SizeInByteInSrc）；0 = onDisk.size()
    quint64 offsetPages = 0;     // 0 = 顺序页对齐分配；非 0 = 强制该页偏移（测越界，不扩容）
    bool nested = false;         // Program: 包成 <program><Image filename=…/></program>
    QString sha256;              // Program: Sha256 属性（0x1000 补零口径）
    bool sparse = false;         // Program: sparse="true"
    bool omitSizeInByte = false; // 不写 SizeInByteInSrc（测扇区长度回退）
    bool omitOffsetAttr = false; // 不写 FileOffsetInSrc（测跳过 + 提示）
};

struct OpsBuildOptions
{
    int mboxIndex = 0;              // opsKeyCandidates() 下标（0=mbox5 / 1=mbox6 / 2=mbox4）
    QByteArray blobOverride;        // 非空 → 用此外部 62B blob（制造"密钥未知"包）
    QString projectId = QStringLiteral("18801");
    QString firmwareName = QStringLiteral("guacamoles_31_O.09_190820");
    quint32 configSectorOverride = 0;  // 非 0 → 尾页 +0x14 写该值（越界）
    bool omitTail = false;             // 不写尾页（包尾不是尾页）
};

struct OpsPackage
{
    QByteArray blob;
    QString keyId;                       // 加密所用候选 id（blobOverride 时为空）
    QList<quint64> offsets;              // 各条目实际偏移（夹具布局意图）
    quint64 settingsOffset = 0;
    quint32 settingsLength = 0;
    QByteArray settingsXml;              // 清单明文（供测试对照）

    bool isValid() const { return !blob.isEmpty(); }
};

static quint64 alignPage(quint64 n)
{
    return (n + 0x1FF) / 0x200 * 0x200;
}

static void putLE32(QByteArray &buf, qsizetype pos, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        buf[pos + i] = char((v >> (8 * i)) & 0xFF);
}

static void putFixed(QByteArray &buf, qsizetype pos, qsizetype width, const QString &s)
{
    const QByteArray raw = s.toLatin1();
    for (qsizetype i = 0; i < width; ++i)
        buf[pos + i] = i < raw.size() ? raw.at(i) : '\0';
}

static QByteArray renderOpsXml(const QList<OpsFileSpec> &files, const QList<quint64> &offsets,
                               const OpsBuildOptions &opts)
{
    QString xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<ProFile>\n");
    xml += QStringLiteral("  <BasicInfo Project=\"%1\" Version=\"%2\"/>\n")
               .arg(opts.projectId, opts.firmwareName);
    for (qsizetype i = 0; i < files.size(); ++i) {
        const OpsFileSpec &spec = files.at(i);
        const quint64 plain = spec.plainSize ? spec.plainSize : quint64(spec.onDisk.size());
        // FileOffsetInSrc 为 0x200 页单位（opscrypto.py L594/L616 等）
        QString attrs;
        if (!spec.omitOffsetAttr)
            attrs += QStringLiteral(" FileOffsetInSrc=\"%1\"").arg(offsets.at(i) / 0x200);
        if (!spec.omitSizeInByte)
            attrs += QStringLiteral(" SizeInByteInSrc=\"%1\"").arg(plain);
        attrs += QStringLiteral(" SizeInSectorInSrc=\"%1\"").arg((plain + 0x1FF) / 0x200);
        if (spec.group == QLatin1String("Program")) {
            attrs += QStringLiteral(" sparse=\"%1\"")
                         .arg(spec.sparse ? QStringLiteral("true") : QStringLiteral("false"));
            if (!spec.sha256.isEmpty())
                attrs += QStringLiteral(" Sha256=\"%1\"").arg(spec.sha256);
        }
        if (spec.group == QLatin1String("SAHARA") || spec.group == QLatin1String("UFS_PROVISION")) {
            xml += QStringLiteral("  <%1>\n    <File Path=\"%2\"%3/>\n  </%1>\n")
                       .arg(spec.group, spec.name, attrs);
        } else if (spec.nested) {
            // 两层形态：<Program><program label=…><Image filename=…/></program></Program>
            xml += QStringLiteral("  <%1>\n    <program label=\"%2\">\n      <Image filename=\"%3\"%4/>\n"
                                  "    </program>\n  </%1>\n")
                       .arg(spec.group, spec.name, spec.name, attrs);
        } else {
            xml += QStringLiteral("  <%1>\n    <program filename=\"%2\"%3/>\n  </%1>\n")
                       .arg(spec.group, spec.name, attrs);
        }
    }
    xml += QStringLiteral("</ProFile>\n");
    return xml.toUtf8();
}

static OpsPackage buildOpsPackage(const QList<OpsFileSpec> &files, const OpsBuildOptions &opts = {})
{
    OpsPackage pkg;
    const QList<imgopp::OpsKey> keys = imgopp::opsKeyCandidates();
    QByteArray blob62;
    if (opts.blobOverride.isEmpty()) {
        if (opts.mboxIndex < 0 || opts.mboxIndex >= keys.size())
            return pkg;
        pkg.keyId = keys.at(opts.mboxIndex).keyId;
        blob62 = keys.at(opts.mboxIndex).mboxBlob;
    } else {
        if (opts.blobOverride.size() != 62)
            return pkg;   // 夹具自检：外部 blob 必须是 62B
        blob62 = opts.blobOverride;
    }

    // ---- 1. 布局（顺序页对齐；显式 offsetPages 优先且不参与后续推进）----
    quint64 cursor = 0;
    QList<quint64> offsets;
    for (const OpsFileSpec &spec : files) {
        if (spec.offsetPages != 0) {
            offsets.append(quint64(spec.offsetPages) * 0x200);
        } else {
            offsets.append(cursor);
            cursor = alignPage(cursor + quint64(spec.onDisk.size()));
        }
    }
    pkg.offsets = offsets;
    pkg.settingsOffset = cursor;

    // ---- 2. 清单 XML + 加密（L689-699: 明文补齐到 16 的倍数再加密；尾页写未补齐长度）----
    pkg.settingsXml = renderOpsXml(files, offsets, opts);
    const quint32 xmlLength = quint32(pkg.settingsXml.size());
    // 参照的补齐式 `(0x10 - (rlength % 0x10))` 在已对齐时也补一整块 —— 照抄，让包内密文
    // 长度与真实包同形（解析侧只读 align16(xmlLength) 字节）
    const qsizetype pad = 0x10 - (pkg.settingsXml.size() % 0x10);
    const QByteArray xmlPadded = pkg.settingsXml + QByteArray(pad, '\0');
    const QByteArray xmlCipher = imgopp::opsEncrypt(xmlPadded, blob62);
    if (xmlCipher.size() != xmlPadded.size())
        return OpsPackage();   // 密码契约不符（夹具自检，避免造出半截包）

    // ---- 3. 落盘 ----
    const quint64 total = pkg.settingsOffset + alignPage(quint64(xmlCipher.size()))
                          + (opts.omitTail ? 0 : 0x200);
    QByteArray blob(int(total), '\0');
    for (qsizetype i = 0; i < files.size(); ++i) {
        const QByteArray &d = files.at(i).onDisk;
        if (d.isEmpty())
            continue;
        // 越界载荷（故意构造的恶意条目）不写入包体
        if (offsets.at(i) + quint64(d.size()) > quint64(blob.size()))
            continue;
        blob.replace(qsizetype(offsets.at(i)), d.size(), d);
    }
    blob.replace(qsizetype(pkg.settingsOffset), xmlCipher.size(), xmlCipher);

    if (!opts.omitTail) {
        const quint64 tailBase = total - 0x200;
        putLE32(blob, qsizetype(tailBase + 0x00), 2);        // version（A11）
        putLE32(blob, qsizetype(tailBase + 0x04), 1);        // flags（A11）
        putLE32(blob, qsizetype(tailBase + 0x10), 0x7CEF);   // magic（与 OFP-QC 共用）
        putLE32(blob, qsizetype(tailBase + 0x14),
                opts.configSectorOverride ? opts.configSectorOverride
                                          : quint32(pkg.settingsOffset / 0x200));
        putLE32(blob, qsizetype(tailBase + 0x18), xmlLength);
        putFixed(blob, qsizetype(tailBase + 0x1C), 16, opts.projectId);
        putFixed(blob, qsizetype(tailBase + 0x2C), 32, opts.firmwareName);
    }

    pkg.blob = blob;
    pkg.settingsLength = xmlLength;
    return pkg;
}

// ==================== 1. 密码对拍（本模块最高风险处） ====================

void TestOppoOps::opsCipherAgainstPython()
{
    const QList<imgopp::OpsKey> keys = imgopp::opsKeyCandidates();
    QCOMPARE(keys.size(), 3);
    QCOMPARE(keys.at(0).keyId, QStringLiteral("mbox5"));
    QCOMPARE(keys.at(1).keyId, QStringLiteral("mbox6"));
    QCOMPARE(keys.at(2).keyId, QStringLiteral("mbox4"));

    // ---- 块路：16B 明文，三个候选各自的密文（钉 mbox 轮密钥材料确实参与运算）----
    const QByteArray pt = ptBlock();
    QCOMPARE(pt.size(), 16);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtBlockMbox5), keys.at(0).mboxBlob), pt);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtBlockMbox6), keys.at(1).mboxBlob), pt);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtBlockMbox4), keys.at(2).mboxBlob), pt);
    // 跨候选解不出（密文与候选错配 → 明文不同）——否则上一组断言可能被"无密钥运算"骗过
    QVERIFY(imgopp::opsDecrypt(QByteArray::fromHex(kCtBlockMbox5), keys.at(1).mboxBlob) != pt);

    // ---- 块路多块：41B 明文（参照产出 48B，第 3 块仅 9 字节）----
    // 包内实际落盘 48B（参照按 16B 块补齐），提取方只读 SizeInByteInSrc = 41B
    // （decryptfile() L428 的 rf.read(length)）→ 按 41B 解密即取回完整明文
    const QByteArray pt41 = ptBlock41();
    QCOMPARE(pt41.size(), 41);
    const QByteArray ct41Mbox5 = QByteArray::fromHex(kCtBlock41Mbox5);
    QCOMPARE(ct41Mbox5.size(), 48);   // 参照输出长度 = 16B 块补齐后的长度
    QCOMPARE(imgopp::opsDecrypt(ct41Mbox5.left(41), keys.at(0).mboxBlob), pt41);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtBlock41Mbox6).left(41), keys.at(1).mboxBlob),
             pt41);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtBlock41Mbox4).left(41), keys.at(2).mboxBlob),
             pt41);

    // ---- 长输入：512B = 32 个整块（真实 SAHARA 载荷即 MB 级多块）----
    const QByteArray pt512 = ptBlock512();
    QCOMPARE(pt512.size(), 512);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtBlock512Mbox5), keys.at(0).mboxBlob), pt512);

    // ---- 尾路：≤15B（末词不足 4B 时按零补齐；三个候选同密文，见向量注释）----
    // 参照产出 8B/8B/4B（末词补齐），包内落盘长度仍是 SizeInByteInSrc → 按输入长度解
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtTail8), keys.at(0).mboxBlob),
             QByteArrayLiteral("OpsTail!"));
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtTail5).left(5), keys.at(0).mboxBlob),
             QByteArrayLiteral("12345"));
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtTail2).left(2), keys.at(0).mboxBlob),
             QByteArrayLiteral("AB"));
    // ---- 13/14/15B：参照文件流口径（pad4 后落块路），三个候选各自的密文 ----
    // 这三档是"分支按 pad4 定"与"按裸长度定"唯一不同的长度区间（本机复算 len 1..41 确认）：
    // 若分支写成裸长度，下面 9 条会全部解出垃圾（SAHARA 段没有 Sha256 兜底 → 静默产出坏文件）。
    const QByteArray pt13 = QByteArrayLiteral("ThirteenBytes");
    const QByteArray pt14 = QByteArrayLiteral("FourteenBytes!");
    const QByteArray pt15 = QByteArrayLiteral("FifteenBytes123");
    QCOMPARE(pt13.size(), 13);
    QCOMPARE(pt14.size(), 14);
    QCOMPARE(pt15.size(), 15);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtLen13Mbox5), keys.at(0).mboxBlob), pt13);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtLen13Mbox6), keys.at(1).mboxBlob), pt13);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtLen13Mbox4), keys.at(2).mboxBlob), pt13);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtLen14Mbox5), keys.at(0).mboxBlob), pt14);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtLen14Mbox6), keys.at(1).mboxBlob), pt14);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtLen14Mbox4), keys.at(2).mboxBlob), pt14);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtLen15Mbox5), keys.at(0).mboxBlob), pt15);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtLen15Mbox6), keys.at(1).mboxBlob), pt15);
    QCOMPARE(imgopp::opsDecrypt(QByteArray::fromHex(kCtLen15Mbox4), keys.at(2).mboxBlob), pt15);
    // 跨候选解不出（同 16B 块路的反向断言：这三档现在真的用到了 mbox 轮密钥材料）
    QVERIFY(imgopp::opsDecrypt(QByteArray::fromHex(kCtLen15Mbox5), keys.at(1).mboxBlob) != pt15);

    // ---- 长度契约：任意输入等长输出；空输入空输出；blob 不足 62B → 空（A9 契约）----
    for (int n : {1, 2, 3, 7, 8, 15, 16, 17, 31, 32, 33, 64, 100})
        QCOMPARE(imgopp::opsDecrypt(pseudoRandom(n), keys.at(0).mboxBlob).size(), n);
    QVERIFY(imgopp::opsDecrypt(QByteArray(), keys.at(0).mboxBlob).isEmpty());
    QVERIFY(imgopp::opsDecrypt(QByteArrayLiteral("x"), QByteArray(61, '\0')).isEmpty());

    // ---- 加密方向（夹具用）：与参照产出（截断到输入长度）逐字节一致，且与解密互逆 ----
    // 这一步同时是对"夹具与实现同源"的自检：任一侧漂移都会在这里先炸，而不是在端到端
    // 用例里以"包解不开"的模糊形态出现。
    QCOMPARE(imgopp::opsEncrypt(pt, keys.at(0).mboxBlob), QByteArray::fromHex(kCtBlockMbox5));
    QCOMPARE(imgopp::opsEncrypt(pt41, keys.at(0).mboxBlob), ct41Mbox5.left(41));
    // 2..15 全覆盖（含 13/14/15）：参照调用方在加密与解密两侧都先 pad4 → 这三档两侧
    // 同为块路，文件流自洽；本实现按 pad4 定分支，与参照文件流逐字节一致（见上组定值向量）。
    // 注意本往返用例**不能**单独证明分支口径正确（两侧用同一口径时自洽即可通过）——
    // 定值向量才是权威；此处只做夹具与实体同源的自检。
    for (int n : {2, 3, 5, 8, 9, 12, 13, 14, 15, 16, 17, 41, 64, 100}) {
        const QByteArray x = pseudoRandom(n, 0xA5A5A5A5u);
        QCOMPARE(imgopp::opsDecrypt(imgopp::opsEncrypt(x, keys.at(0).mboxBlob), keys.at(0).mboxBlob), x);
        QCOMPARE(imgopp::opsEncrypt(x, keys.at(0).mboxBlob).size(), n);
    }
}

// ==================== 2. 尾页识别（A11） ====================

void TestOppoOps::detectOpsFromTail()
{
    QByteArray tail(0x200, '\0');
    putLE32(tail, 0x00, 2);
    putLE32(tail, 0x04, 1);
    putLE32(tail, 0x10, 0x7CEF);
    QVERIFY(imgopp::detectOPS(tail, 0x2000));
    QVERIFY(imgopp::detectOPS(tail, 0x200));   // fileSize 恰好一页

    // tail 允许是整包或末若干页（同 detectOFP 契约）：只取末 0x200 页判定
    const QByteArray whole = pseudoRandom(0x1000, 0x11u) + tail;
    QVERIFY(imgopp::detectOPS(whole, quint64(whole.size())));
    QVERIFY(imgopp::detectOPS(whole.right(0x1000), quint64(whole.size())));

    // ---- 判据不得放宽（A11）：任一条件不满足即 false ----
    QByteArray t = tail;
    putLE32(t, 0x00, 0);
    QVERIFY(!imgopp::detectOPS(t, 0x2000));            // QC 式尾页：version=0
    t = tail;
    putLE32(t, 0x04, 0);
    QVERIFY(!imgopp::detectOPS(t, 0x2000));            // flags=0
    t = tail;
    putLE32(t, 0x04, 2);
    QVERIFY(!imgopp::detectOPS(t, 0x2000));            // flags=2
    t = tail;
    putLE32(t, 0x00, 3);
    QVERIFY(!imgopp::detectOPS(t, 0x2000));            // version=3
    t = tail;
    putLE32(t, 0x10, 0x7CF0);
    QVERIFY(!imgopp::detectOPS(t, 0x2000));            // 魔数不符
    // 只带 0x7CEF（OFP-QC 的判据）→ false：这正是"探测必须先 OPS 后 OFP"的前提
    t = QByteArray(0x200, '\0');
    putLE32(t, 0x10, 0x7CEF);
    QVERIFY(!imgopp::detectOPS(t, 0x2000));

    // 文件不足一页 / tail 不足一页
    QVERIFY(!imgopp::detectOPS(tail, 0x100));
    QVERIFY(!imgopp::detectOPS(tail.left(0x100), 0x2000));
}

// ==================== 3. 密钥候选与尾页字段 ====================

void TestOppoOps::parseOpsKeyCandidates()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QList<imgopp::OpsKey> keys = imgopp::opsKeyCandidates();

    const QByteArray saharaPlain = QByteArrayLiteral("BlockPath0123456789ABCDEF0123456789ABCDEF");
    const QByteArray ctByMbox[3] = {QByteArray::fromHex(kCtBlock41Mbox5),
                                    QByteArray::fromHex(kCtBlock41Mbox6),
                                    QByteArray::fromHex(kCtBlock41Mbox4)};
    const QByteArray provision = pseudoRandom(0x100, 0x33u);

    // 三个候选各打一个包（settings.xml 用该候选的 blob 加密）→ 解析必须命中同一 id
    for (int idx = 0; idx < 3; ++idx) {
        OpsBuildOptions opts;
        opts.mboxIndex = idx;
        const OpsPackage pkg = buildOpsPackage(
            {{QStringLiteral("SAHARA"), QStringLiteral("prog_ufs_firehose_test.elf"), ctByMbox[idx], 41},
             {QStringLiteral("UFS_PROVISION"), QStringLiteral("provision.xml"), provision, 0}},
            opts);
        QVERIFY(pkg.isValid());
        const QString path = writePkg(dir.path(), QStringLiteral("key%1.ops").arg(idx), pkg.blob);
        QVERIFY(!path.isEmpty());

        imgopp::OpsInfo info;
        QString err;
        QVERIFY2(imgopp::parseOPS(path, info, &err), qPrintable(err));
        QVERIFY(err.isEmpty());
        QCOMPARE(info.keyId, keys.at(idx).keyId);
        QCOMPARE(info.mboxBlob, keys.at(idx).mboxBlob);
        QCOMPARE(info.projectId, QStringLiteral("18801"));
        QCOMPARE(info.firmwareName, QStringLiteral("guacamoles_31_O.09_190820"));
        QCOMPARE(info.settingsOffset, pkg.settingsOffset);
        QCOMPARE(info.settingsOffset % 0x200, quint64(0));   // 尾页 +0x14 是扇区号
        QCOMPARE(quint64(info.settingsLength), quint64(pkg.settingsXml.size()));
        QCOMPARE(info.entries.size(), 2);

        const imgopp::OpsEntry &sahara = info.entries.at(0);
        QCOMPARE(sahara.name, QStringLiteral("prog_ufs_firehose_test.elf"));
        QCOMPARE(sahara.offset, quint64(0));   // 首个条目在偏移 0（0 是合法偏移，不能当"缺失"）
        QCOMPARE(sahara.size, quint64(41));
        QVERIFY(sahara.decrypt);
        QVERIFY(sahara.sha256Hex.isEmpty());
        const imgopp::OpsEntry &ufs = info.entries.at(1);
        QCOMPARE(ufs.name, QStringLiteral("provision.xml"));
        QCOMPARE(ufs.offset, pkg.offsets.at(1));
        QCOMPARE(ufs.size, quint64(provision.size()));
        QVERIFY(!ufs.decrypt);

        // 失败时 info 保持调用方原值（同 parseOFP 语义）
        imgopp::OpsInfo keep;
        keep.keyId = QStringLiteral("sentinel");
        OpsBuildOptions bad;
        bad.blobOverride = QByteArray(62, '\x5A');   // 不在候选表里的密钥材料
        const OpsPackage badPkg = buildOpsPackage(
            {{QStringLiteral("SAHARA"), QStringLiteral("prog.elf"), ctByMbox[0], 41}}, bad);
        QVERIFY(badPkg.isValid());
        const QString badPath = writePkg(dir.path(), QStringLiteral("bad%1.ops").arg(idx), badPkg.blob);
        QVERIFY(!badPath.isEmpty());
        err.clear();
        QVERIFY(!imgopp::parseOPS(badPath, keep, &err));
        QVERIFY2(err.contains(QStringLiteral("密钥")), qPrintable(err));
        QVERIFY2(err.contains(QStringLiteral("mbox")), qPrintable(err));   // 点出试过的候选
        QCOMPARE(keep.keyId, QStringLiteral("sentinel"));
        QVERIFY(keep.entries.isEmpty());
    }
}

// 缺 FileOffsetInSrc 的条目：跳过 + 中文提示追加进 *error，但整包仍算解析成功（A10 语义）
void TestOppoOps::parseOpsSkipsEntryWithoutOffset()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray payload = pseudoRandom(0x80, 0x44u);
    const OpsPackage pkg = buildOpsPackage(
        {{QStringLiteral("UFS_PROVISION"), QStringLiteral("good.bin"), payload, 0},
         {QStringLiteral("UFS_PROVISION"), QStringLiteral("nofield.bin"), payload, 0, 0,
          /*nested=*/false, QString(), /*sparse=*/false, /*omitSizeInByte=*/false,
          /*omitOffsetAttr=*/true}});
    QVERIFY(pkg.isValid());
    const QString path = writePkg(dir.path(), QStringLiteral("nooff.ops"), pkg.blob);
    QVERIFY(!path.isEmpty());

    imgopp::OpsInfo info;
    QString err;
    QVERIFY2(imgopp::parseOPS(path, info, &err), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("FileOffsetInSrc")), qPrintable(err));
    QCOMPARE(info.entries.size(), 1);
    QCOMPARE(info.entries.at(0).name, QStringLiteral("good.bin"));
    QCOMPARE(info.keyId, QStringLiteral("mbox5"));
}

// ==================== 4. 端到端解包 ====================

void TestOppoOps::extractOpsEndToEnd()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // SAHARA 载荷用参照产出的定值密文（夹具不加密它）→ 产物必须逐字节等于参照的明文
    const QByteArray saharaPlain = ptBlock41();
    const QByteArray saharaCipher = QByteArray::fromHex(kCtBlock41Mbox5);
    const QByteArray provisionPlain = pseudoRandom(0x180, 0x9A9Au);
    const QByteArray programPlain = pseudoRandom(0x240, 0x7C7Cu);
    const QByteArray nestedPlain = pseudoRandom(0x120, 0x5E5Eu);

    const OpsBuildOptions opts;   // mbox5
    const OpsPackage pkg = buildOpsPackage(
        {{QStringLiteral("SAHARA"), QStringLiteral("prog_ufs_firehose_test.elf"), saharaCipher, 41},
         {QStringLiteral("UFS_PROVISION"), QStringLiteral("provision.xml"), provisionPlain, 0},
         {QStringLiteral("Program"), QStringLiteral("boot.img"), programPlain, 0},
         {QStringLiteral("Program"), QStringLiteral("system.img"), nestedPlain, 0, 0,
          /*nested=*/true}},
        opts);
    QVERIFY(pkg.isValid());
    const QString pkgPath = writePkg(dir.path(), QStringLiteral("test.ops"), pkg.blob);
    QVERIFY(!pkgPath.isEmpty());
    const QString outDir = dir.filePath(QStringLiteral("out"));
    QVERIFY(QDir().mkpath(outDir));

    // 反自证：包内 SAHARA 段确为密文（前 16B 与明文不同）→ "产物 == 明文"才说明解密真发生
    QVERIFY(pkg.blob.mid(qsizetype(pkg.offsets.at(0)), 16) != saharaPlain.left(16));
    // 明文组则逐字节等于产物（若实现误对这些段做解密，产物必坏）
    QCOMPARE(pkg.blob.mid(qsizetype(pkg.offsets.at(1)), provisionPlain.size()), provisionPlain);

    ProgressLog log;
    QString err;
    QVERIFY2(imgopp::extractOPS(pkgPath, outDir, log.callback(), &err), qPrintable(err));
    QVERIFY(err.isEmpty());
    QCOMPARE(readFile(outDir + "/prog_ufs_firehose_test.elf"), saharaPlain);
    QCOMPARE(readFile(outDir + "/provision.xml"), provisionPlain);
    QCOMPARE(readFile(outDir + "/boot.img"), programPlain);
    QCOMPARE(readFile(outDir + "/system.img"), nestedPlain);   // 两层 program/Image 也提取
    QCOMPARE(QFileInfo(outDir + "/prog_ufs_firehose_test.elf").size(), qint64(41));
    QCOMPARE(log.names.size(), 4);
    QCOMPARE(log.percents.last(), 100);
    QVERIFY(log.monotonic());

    // error 允许为 nullptr；输出目录不存在时自动创建（嵌套亦建）
    const QString outDir2 = dir.filePath(QStringLiteral("out2/nested"));
    QVERIFY(!QDir(outDir2).exists());
    QVERIFY(imgopp::extractOPS(pkgPath, outDir2, {}, nullptr));
    QCOMPARE(readFile(outDir2 + "/boot.img"), programPlain);
}

// ==================== 5. Program 的 Sha256 校验（0x1000 补零口径） ====================

void TestOppoOps::extractOpsProgramHash()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 长度不是 0x1000 的整数倍 → 两种摘要口径必然不同（否则本用例无法区分）
    const QByteArray payload = pseudoRandom(0x300, 0x600Du);
    const QString padded = opsDigest(payload);
    const QString unpadded = hexOf(payload, QCryptographicHash::Sha256);
    QVERIFY(padded != unpadded);

    // ---- 1) 补零口径命中 → 通过 ----
    const OpsPackage okPkg = buildOpsPackage(
        {{QStringLiteral("Program"), QStringLiteral("boot.img"), payload, 0, 0, /*nested=*/false,
          padded}});
    QVERIFY(okPkg.isValid());
    const QString okPath = writePkg(dir.path(), QStringLiteral("hash-ok.ops"), okPkg.blob);
    QVERIFY(!okPath.isEmpty());
    QString err;
    ProgressLog log;
    QVERIFY2(imgopp::extractOPS(okPath, dir.filePath(QStringLiteral("out-ok")), log.callback(), &err),
             qPrintable(err));
    QVERIFY(err.isEmpty());
    QCOMPARE(readFile(dir.filePath(QStringLiteral("out-ok/boot.img"))), payload);

    // ---- 2) 不补零口径（= extractOFP 的整段口径）→ 必须失败：证明 OPS 侧走的是补零口径 ----
    const OpsPackage badPkg = buildOpsPackage(
        {{QStringLiteral("Program"), QStringLiteral("recovery.img"), payload, 0},
         {QStringLiteral("Program"), QStringLiteral("boot.img"), payload, 0, 0, /*nested=*/false,
          unpadded}});
    QVERIFY(badPkg.isValid());
    const QString badPath = writePkg(dir.path(), QStringLiteral("hash-bad.ops"), badPkg.blob);
    QVERIFY(!badPath.isEmpty());
    const QString outDir = dir.filePath(QStringLiteral("out-bad"));
    err.clear();
    QVERIFY(!imgopp::extractOPS(badPath, outDir, {}, &err));
    QVERIFY2(err.contains(QStringLiteral("校验失败")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("boot.img")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("sha256")), qPrintable(err));
    // 已写产物保留（spec §5/A10：不回滚）；后续条目不提取
    QCOMPARE(readFile(outDir + "/recovery.img"), payload);
    QCOMPARE(readFile(outDir + "/boot.img"), payload);

    // ---- 3) sparse="true" → 跳过校验（L622），即使摘要不符也成功 ----
    const OpsPackage sparsePkg = buildOpsPackage(
        {{QStringLiteral("Program"), QStringLiteral("super.img"), payload, 0, 0, /*nested=*/false,
          unpadded, /*sparse=*/true}});
    QVERIFY(sparsePkg.isValid());
    const QString sparsePath = writePkg(dir.path(), QStringLiteral("hash-sparse.ops"), sparsePkg.blob);
    QVERIFY(!sparsePath.isEmpty());
    const QString outDir3 = dir.filePath(QStringLiteral("out-sparse"));
    err.clear();
    ProgressLog log3;
    QVERIFY2(imgopp::extractOPS(sparsePath, outDir3, log3.callback(), &err), qPrintable(err));
    QVERIFY(err.isEmpty());
    QCOMPARE(readFile(outDir3 + "/super.img"), payload);
    QCOMPARE(log3.names.first(), QStringLiteral("super.img（sparse 镜像，原样输出）"));
}

// ==================== 6. 恶意输入防护（A10） ====================

void TestOppoOps::extractOpsRejectsUnsafeNames()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray legit = pseudoRandom(0x180, 0x5AFEu);
    const QByteArray evil(0x40, 'E');
    const OpsPackage pkg = buildOpsPackage(
        {{QStringLiteral("Program"), QStringLiteral("../evil.img"), evil, 0},
         {QStringLiteral("Program"), QStringLiteral("/abs/evil.img"), evil, 0},
         {QStringLiteral("Program"), QStringLiteral("sub/evil.img"), evil, 0},
         {QStringLiteral("Program"), QStringLiteral("boot.img"), legit, 0}});
    QVERIFY(pkg.isValid());
    const QString pkgPath = writePkg(dir.path(), QStringLiteral("unsafe.ops"), pkg.blob);
    QVERIFY(!pkgPath.isEmpty());
    const QString outDir = dir.filePath(QStringLiteral("out"));

    ProgressLog log;
    QString err;
    // 跳过恶意条目但整包继续：有合法条目被提取 → true，*error 携带逐条跳过原因（A10 第一条）
    QVERIFY2(imgopp::extractOPS(pkgPath, outDir, log.callback(), &err), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("文件名")), qPrintable(err));
    QCOMPARE(readFile(outDir + "/boot.img"), legit);
    QCOMPARE(log.names.size(), 1);        // 被跳过的条目既无产物也不计进度
    QCOMPARE(log.percents.last(), 100);
    // 一个字节都没落到 outDir 之外
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("evil.img"))));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("abs"))));
    QVERIFY(!QDir(outDir).exists(QStringLiteral("sub")));

    // 全部条目名不安全 → 无任何产物 → false（A10 第二条）
    const OpsPackage only = buildOpsPackage(
        {{QStringLiteral("Program"), QStringLiteral("../evil.img"), evil, 0}});
    QVERIFY(only.isValid());
    const QString onlyPath = writePkg(dir.path(), QStringLiteral("only-evil.ops"), only.blob);
    QVERIFY(!onlyPath.isEmpty());
    QString err2;
    QVERIFY(!imgopp::extractOPS(onlyPath, dir.filePath(QStringLiteral("out2")), {}, &err2));
    QVERIFY2(err2.contains(QStringLiteral("文件名")), qPrintable(err2));
}

// ==================== 7. 异常包与路径守卫 ====================

void TestOppoOps::extractOpsRejectsBadPackages()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray payload = pseudoRandom(0x100, 0x1234u);
    const OpsFileSpec spec{QStringLiteral("Program"), QStringLiteral("boot.img"), payload, 0};

    // 包不存在 → 明确中文错误
    QString err;
    QVERIFY(!imgopp::extractOPS(dir.filePath(QStringLiteral("nope.ops")), dir.path(), {}, &err));
    QVERIFY2(err.contains(QStringLiteral("无法打开")), qPrintable(err));

    // 低于一页 → 解析层拒绝
    imgopp::OpsInfo info;
    err.clear();
    const QString tiny = writePkg(dir.path(), QStringLiteral("tiny.ops"), QByteArray(0x100, '\0'));
    QVERIFY(!tiny.isEmpty());
    QVERIFY(!imgopp::parseOPS(tiny, info, &err));
    QVERIFY2(err.contains(QStringLiteral("过小")), qPrintable(err));

    // 尾页标记不符：version 字段被改掉（尾页存在但判据不成立）→ "不是 OPS 包"
    const OpsPackage basePkg = buildOpsPackage({spec});
    QVERIFY(basePkg.isValid());
    QByteArray badVerBlob = basePkg.blob;
    putLE32(badVerBlob, badVerBlob.size() - 0x200 + 0x00, 0);
    const QString badVerPath = writePkg(dir.path(), QStringLiteral("badver.ops"), badVerBlob);
    QVERIFY(!badVerPath.isEmpty());
    err.clear();
    QVERIFY(!imgopp::parseOPS(badVerPath, info, &err));
    QVERIFY2(err.contains(QStringLiteral("不是 OPS")), qPrintable(err));

    // 包尾不是尾页（settings 密文之后没有尾页）→ 同一判据拒绝
    OpsBuildOptions noTail;
    noTail.omitTail = true;
    const OpsPackage noTailPkg = buildOpsPackage({spec}, noTail);
    QVERIFY(noTailPkg.isValid());
    const QString noTailPath = writePkg(dir.path(), QStringLiteral("notail.ops"), noTailPkg.blob);
    QVERIFY(!noTailPath.isEmpty());
    err.clear();
    QVERIFY(!imgopp::parseOPS(noTailPath, info, &err));
    QVERIFY(!err.isEmpty());

    // settings.xml 长度字段为 0
    QByteArray zeroBlob = basePkg.blob;
    putLE32(zeroBlob, zeroBlob.size() - 0x200 + 0x18, 0);
    const QString zeroPath = writePkg(dir.path(), QStringLiteral("zerolen.ops"), zeroBlob);
    QVERIFY(!zeroPath.isEmpty());
    err.clear();
    QVERIFY(!imgopp::parseOPS(zeroPath, info, &err));
    QVERIFY2(err.contains(QStringLiteral("长度")), qPrintable(err));

    // settings.xml 区域越界（尾页 +0x14 指向尾页之后）
    OpsBuildOptions cfgBad;
    cfgBad.configSectorOverride = quint32((basePkg.blob.size() / 0x200) + 4);
    const OpsPackage cfgPkg = buildOpsPackage({spec}, cfgBad);
    QVERIFY(cfgPkg.isValid());
    const QString cfgPath = writePkg(dir.path(), QStringLiteral("cfg.ops"), cfgPkg.blob);
    QVERIFY(!cfgPath.isEmpty());
    err.clear();
    QVERIFY(!imgopp::parseOPS(cfgPath, info, &err));
    QVERIFY2(err.contains(QStringLiteral("越界")), qPrintable(err));

    // 条目越界（FileOffsetInSrc 指向包外）→ 解析层拒绝
    OpsBuildOptions oob;
    const OpsPackage oobPkg = buildOpsPackage(
        {{QStringLiteral("Program"), QStringLiteral("boot.img"), payload, 0,
          /*offsetPages=*/quint64(0x10000)}},
        oob);
    QVERIFY(oobPkg.isValid());
    const QString oobPath = writePkg(dir.path(), QStringLiteral("oob.ops"), oobPkg.blob);
    QVERIFY(!oobPath.isEmpty());
    err.clear();
    QVERIFY(!imgopp::parseOPS(oobPath, info, &err));
    QVERIFY2(err.contains(QStringLiteral("越界")), qPrintable(err));
    err.clear();
    QVERIFY(!imgopp::extractOPS(oobPath, dir.filePath(QStringLiteral("out-oob")), {}, &err));
    QVERIFY(!err.isEmpty());

    // 清单为空（只有 BasicInfo，没有可提取条目）→ 拒绝，不产出空目录
    const OpsPackage emptyPkg = buildOpsPackage({});
    QVERIFY(emptyPkg.isValid());
    const QString emptyPath = writePkg(dir.path(), QStringLiteral("empty.ops"), emptyPkg.blob);
    QVERIFY(!emptyPath.isEmpty());
    err.clear();
    QVERIFY(!imgopp::extractOPS(emptyPath, dir.filePath(QStringLiteral("out-empty")), {}, &err));
    QVERIFY2(err.contains(QStringLiteral("没有可提取")), qPrintable(err));

    // 路径守卫：outDir = 包所在目录 且 条目名 == 包文件名 → 拒绝且"先判后开"（包不被截断）
    const QString pkgName = QStringLiteral("guard.ops");
    const OpsPackage guardPkg = buildOpsPackage(
        {{QStringLiteral("Program"), pkgName, payload, 0}});
    QVERIFY(guardPkg.isValid());
    const QString guardPath = writePkg(dir.path(), pkgName, guardPkg.blob);
    QVERIFY(!guardPath.isEmpty());
    err.clear();
    QVERIFY(!imgopp::extractOPS(guardPath, dir.path(), {}, &err));
    QVERIFY2(err.contains(QStringLiteral("相同")), qPrintable(err));
    QCOMPARE(QFileInfo(guardPath).size(), qint64(guardPkg.blob.size()));
    imgopp::OpsInfo guardInfo;
    QVERIFY2(imgopp::parseOPS(guardPath, guardInfo, &err), qPrintable(err));
}

QTEST_APPLESS_MAIN(TestOppoOps)
#include "test_oppo_ops.moc"

// ==================== 定值向量的生成命令（opscrypto.py 是本模块的权威参照） ====================
//
// opscrypto.py 在 import 期会执行 docopt（模块级 args = docopt(__doc__)），故注入桩绕开；
// mbox 是全局变量，逐次赋值为 mbox5/mbox6/mbox4：
//
//   cd <repo> && python3 - <<'PY'
//   import sys, types
//   m = types.ModuleType('docopt'); m.docopt = lambda *a, **k: {}
//   sys.modules['docopt'] = m
//   sys.path.insert(0, 'reference/oppo_decrypt')
//   import opscrypto as oc
//   for name, mb in (('mbox5', oc.mbox5), ('mbox6', oc.mbox6), ('mbox4', oc.mbox4)):
//       oc.mbox = mb
//       for pt in (b'TestBlock1234567',
//                  b'BlockPath0123456789ABCDEF0123456789ABCDEF',
//                  b'OpsTail!', b'12345', b'AB', b'FifteenBytes123'):
//           ct = oc.key_custom(pt, oc.key, 0, True)
//           rt = bytes(oc.key_custom(ct[:len(pt)], oc.key, 0, False))
//           assert rt[:len(pt)] == pt, (name, pt)      # 等长口径回环自检
//           print(name, pt[:12], ct.hex())
//   PY
//
// 输出即上面 kCtBlockMbox* / kCtBlock41Mbox* / kCtTail* / kCtLen15 的十六进制值
// （kCtBlock41* 与 kCtBlockMbox* 中的短向量已按 32 hex/行折行，值不变）。
