#include "oppo_keys.h"

#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>

namespace imgopp {
namespace {

// 半字节交换：swap(ch) = ((ch & 0xF) << 4) + ((ch & 0xF0) >> 4)
// （ofp_qc_decrypt.py L13-14 / ofp_mtk_decrypt.py L12-13 同名函数）
inline quint8 nibbleSwap(quint8 x)
{
    return quint8((x << 4) | ((x >> 4) & 0x0F));
}

// QC 与 MTK 共用同一派生：两者参照实现恒等，仅 triplet 参数不同——
//   ofp_qc_decrypt.py::deobfuscate() L46-51  → nibbleSwap(data[i] ^ mask[i])
//   ofp_mtk_decrypt.py::mtk_shuffle2() L30-34 → nibbleSwap(mask[i] ^ data[i])
// （XOR 可交换，故半字节交换在异或之先后不影响结果。）
// key/iv = md5(混淆后的 16 字节).toHex().left(16) 的 ASCII 字节
// （generatekey2() L106-107 / getkey() L93-94 均取 hexdigest 前 16 字符再 encode）。
// 调用方保证两参数各 16 字节（内置表与 loadOppoKeysJson 均已校验）。
QByteArray deriveQcLikeKeyIv(const QByteArray &mc16, const QByteArray &material16)
{
    QByteArray obfuscated(16, Qt::Uninitialized);
    for (int i = 0; i < 16; ++i)
        obfuscated[i] = char(nibbleSwap(quint8(material16.at(i)) ^ quint8(mc16.at(i))));
    return QCryptographicHash::hash(obfuscated, QCryptographicHash::Md5).toHex().left(16);
}

struct Triplet
{
    const char *keyId;
    const char *mc;
    const char *userkey;
    const char *iv;
};

// QC triplet 表。表序 = 尝试顺序（ofp_qc_decrypt.py generatekey2() L54-92），
// 每行机型注释取自参照实现同行注释。
const Triplet kQcTriplets[] = {
    // R9s/A57t
    {"V1.4.17", "27827963787265EF89D126B69A495A21", "82C50203285A2CE7D8C3E198383CE94C",
     "422DD5399181E223813CD8ECDF2E4D72"},
    // a3s
    {"V1.6.17", "E11AA7BB558A436A8375FD15DDD4651F", "77DDF6A0696841F6B74782C097835169",
     "A739742384A44E8BA45207AD5C3700EA"},
    // 参照实现此行无机型注释（速查表记 legacy）
    {"V1.5.13", "67657963787565E837D226B69A495D21", "F6C50203515A2CE7D8C3E1F938B7E94C",
     "42F2D5399137E2B2813CD8ECDF2F4D72"},
    // R15 Pro CPH1831 V1.6.6 / FindX CPH1871 V1.6.9 / R17 Pro CPH1877 V1.6.17 /
    // R17 PBEM00 V1.6.17 / A5 2020 V1.7.6 / K3 CPH1955 V1.6.26 UFS /
    // Reno 5G CPH1921 V1.6.26 / Realme 3 Pro RMX1851 V1.6.17 /
    // Reno 10X Zoom V1.6.26 / R17 CPH1879 V1.6.17 / R17 Neo CPH1893 / K1 PBCM30
    {"V1.6.6/1.6.9/1.6.17/1.6.24/1.6.26/1.7.6", "3C2D518D9BF2E4279DC758CD535147C3",
     "87C74A29709AC1BF2382276C4E8DF232", "598D92E967265E9BCABE2469FE4A915E"},
    // Realme X RMX1901 V1.7.2 / Realme 5 RMX1911 V1.7.2 / 5 Pro RMX1971 V1.7.2 /
    // RM1921EX V1.7.2
    {"V1.7.2", "8FB8FB261930260BE945B841AEFA9FD4", "E529E82B28F5A2F8831D860AE39E425D",
     "8A09DA60ED36F125D64709973372C1CF"},
    // OW19W8AP_11_A.23_200715
    {"V2.0.3", "E8AE288C0192C54BF10C5707E9C4705B", "D64FC385DCD52A3C9B5FBA8650F92EDA",
     "79051FD8D8B6297E2E4559E997F63B7F"},
};

// MTK triplet 表（ofp_mtk_decrypt.py keytables L46-85）。前 7 项与 QC 同派生方案，
// 注意 MTK3 == QC V1.4.17、MTK0 == QC V1.5.13 是同一 triplet（交叉校验），
// 而 MTK4 与 QC V1.6.6 族 triplet 相近但不同（3C4A…/87B13D29…/59B7A8E9…），不可混用。
const Triplet kMtkTriplets[] = {
    // A77 CPH1715EX_11_A.04_170426, F1S A1601_MT6750_EX_11_A.15_160913 FW
    {"MTK0", "67657963787565E837D226B69A495D21", "F6C50203515A2CE7D8C3E1F938B7E94C",
     "42F2D5399137E2B2813CD8ECDF2F4D72"},
    // A77 CPH1715EX_11_A.04_170426, F1S A1601_MT6750_EX_11_A.15_160913 CDT
    {"MTK1", "9E4F32639D21357D37D226B69A495D21", "A3D8D358E42F5A9E931DD3917D9A3218",
     "386935399137416B67416BECF22F519A"},
    // 参照实现本行无机型注释
    {"MTK2", "892D57E92A4D8A975E3C216B7C9DE189", "D26DF2D9913785B145D18C7219B89F26",
     "516989E4A1BFC78B365C6BC57D944391"},
    // 参照实现本行无机型注释（同 QC V1.4.17 triplet）
    {"MTK3", "27827963787265EF89D126B69A495A21", "82C50203285A2CE7D8C3E198383CE94C",
     "422DD5399181E223813CD8ECDF2E4D72"},
    // 参照实现本行无机型注释（同 QC V1.6.6 族近似行，值有差异）
    {"MTK4", "3C4A618D9BF2E4279DC758CD535147C3", "87B13D29709AC1BF2382276C4E8DF232",
     "59B7A8E967265E9BCABE2469FE4A915E"},
    // A83_CPH1827_11_A.21_2G_180923 FW, Realme 3 RMX1827EX_11_C.13_200624_1264686e
    {"MTK5", "1C3288822BF824259DC852C1733127D3", "E7918D22799181CF2312176C9E2DF298",
     "3247F889A7B6DECBCA3E28693E4AAAFE"},
    // 参照实现本行无机型注释
    {"MTK6", "1E4F32239D65A57D37D2266D9A775D43", "A332D3C3E42F5A3E931DD991729A321D",
     "3F2A35399A373377674155ECF28FD19A"},
    // 参照实现本行无机型注释
    {"MTK7", "122D57E92A518AFF5E3C786B7C34E189", "DD6DF2D9543785674522717219989FB0",
     "12698965A132C76136CC88C5DD94EE91"},
};

// MTK8：唯一的直接 ASCII 常量（ofp_mtk_decrypt.py keytables 末项 L79-82，
// getkey() 的 else 分支 L95-98 原样返回，不走 md5 派生）。
const char kMtk8Key[] = "ab3f76d7989207f2";  // AES KEY
const char kMtk8Iv[] = "2bf515b3a9737835";   // AES IV

// OPS mbox blob 构造：16B 轮密钥素材 + 44×00 填充 + 末 2B 轮数 0x0A(LE) = 62B。
// 逐字节等价于 opscrypto.py L55-78 的 mbox5/mbox6/mbox4 列表
// （asbox[0..3] 参与首轮 XOR；asbox[0x3C] = 0x0A 即轮数，见 A3 与 key_update() L304-352）。
QByteArray mboxBlob(const char *material16Hex)
{
    return QByteArray::fromHex(material16Hex) + QByteArray(44, '\0') + QByteArray::fromHex("0A00");
}

// JSON hex 字段解析：要求恰好 bytes 字节。失败返回空并填中文原因。
QByteArray hexField(const QJsonObject &obj, const QString &field, int bytes, const QString &where,
                    QString &reason)
{
    const QJsonValue value = obj.value(field);
    if (!value.isString()) {
        reason = QStringLiteral("%1 的 %2 字段缺失或不是字符串").arg(where, field);
        return QByteArray();
    }
    const QByteArray raw = QByteArray::fromHex(value.toString().toLatin1());
    if (raw.size() != bytes) {
        reason = QStringLiteral("%1 的 %2 字段不是 %3 字节 hex（实为 %4 字节）")
                     .arg(where, field)
                     .arg(bytes)
                     .arg(raw.size());
        return QByteArray();
    }
    return raw;
}

QString idField(const QJsonObject &obj, const QString &where, QString &reason)
{
    const QString id = obj.value(QStringLiteral("id")).toString();
    if (id.isEmpty())
        reason = QStringLiteral("%1 缺少 id 字段").arg(where);
    return id;
}

} // namespace

QList<OppoKeyPair> qcKeyCandidates()
{
    QList<OppoKeyPair> keys;
    keys.reserve(int(sizeof(kQcTriplets) / sizeof(kQcTriplets[0])));
    for (const Triplet &t : kQcTriplets) {
        const QByteArray mc = QByteArray::fromHex(t.mc);
        keys.append(OppoKeyPair{QString::fromLatin1(t.keyId),
                                deriveQcLikeKeyIv(mc, QByteArray::fromHex(t.userkey)),
                                deriveQcLikeKeyIv(mc, QByteArray::fromHex(t.iv))});
    }
    return keys;
}

QList<OppoKeyPair> mtkKeyCandidates()
{
    QList<OppoKeyPair> keys;
    keys.reserve(int(sizeof(kMtkTriplets) / sizeof(kMtkTriplets[0])) + 1);
    for (const Triplet &t : kMtkTriplets) {
        const QByteArray mc = QByteArray::fromHex(t.mc);
        keys.append(OppoKeyPair{QString::fromLatin1(t.keyId),
                                deriveQcLikeKeyIv(mc, QByteArray::fromHex(t.userkey)),
                                deriveQcLikeKeyIv(mc, QByteArray::fromHex(t.iv))});
    }
    keys.append(OppoKeyPair{QStringLiteral("MTK8"), QByteArray(kMtk8Key), QByteArray(kMtk8Iv)});
    return keys;
}

QList<OpsKey> opsKeyCandidates()
{
    // 尝试顺序 mbox5 → mbox6 → mbox4（spec 速查表 §OPS + A3）；
    // 机型注释取自 opscrypto.py 定义处注释（L55 / L63 / L71）。
    return {
        {QStringLiteral("mbox5"), mboxBlob("608A3F2D686BD423510CD095BB40E976")},  // guacamoles_31_O.09_190820（一加 7 Pro 族）
        {QStringLiteral("mbox6"), mboxBlob("AA69829E5DDEB13D30BB81A34665A3E1")},  // instantnoodlev_15_O.07_201103（一加 8 族）
        {QStringLiteral("mbox4"), mboxBlob("C45D057199DDBBEE29A16DC7ADBFA43F")},  // guacamolet_21_O.08_190502（一加 7T Pro 族）
    };
}

bool loadOppoKeysJson(const QString &path, QList<OppoKeyPair> &out, QList<OpsKey> &opsOut,
                      QString *error)
{
    // 统一错误出口：中文文案 + 前缀文件路径（error 允许为 nullptr）
    auto fail = [&](const QString &reason) {
        if (error)
            *error = QStringLiteral("密钥文件 %1：%2").arg(path, reason);
        return false;
    };

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法打开（%1）").arg(file.errorString()));
    const QByteArray raw = file.readAll();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        return fail(QStringLiteral("不是合法 JSON：%1（偏移 %2）")
                        .arg(parseError.errorString())
                        .arg(parseError.offset));
    }
    if (!doc.isObject())
        return fail(QStringLiteral("根节点不是 JSON 对象"));
    const QJsonObject root = doc.object();
    if (!root.contains(QStringLiteral("qc")) && !root.contains(QStringLiteral("ops")))
        return fail(QStringLiteral("没有 qc/ops 段"));

    // 解析进临时列表：任一环节失败则不改动调用方的出参（避免半截密钥列表）
    QList<OppoKeyPair> parsedQc;
    QList<OpsKey> parsedOps;

    const QJsonValue qcValue = root.value(QStringLiteral("qc"));
    if (!qcValue.isUndefined()) {
        if (!qcValue.isArray())
            return fail(QStringLiteral("qc 段不是数组"));
        const QJsonArray qcArray = qcValue.toArray();
        for (int i = 0; i < qcArray.size(); ++i) {
            const QString where = QStringLiteral("qc[%1]").arg(i);
            if (!qcArray.at(i).isObject())
                return fail(QStringLiteral("%1 不是对象").arg(where));
            const QJsonObject entry = qcArray.at(i).toObject();
            QString reason;
            const QString id = idField(entry, where, reason);
            if (id.isEmpty())
                return fail(reason);
            const QByteArray mc = hexField(entry, QStringLiteral("mc"), 16, where, reason);
            if (mc.isEmpty())
                return fail(reason);
            const QByteArray userkey = hexField(entry, QStringLiteral("userkey"), 16, where, reason);
            if (userkey.isEmpty())
                return fail(reason);
            const QByteArray iv = hexField(entry, QStringLiteral("iv"), 16, where, reason);
            if (iv.isEmpty())
                return fail(reason);
            // 外部只提供 triplet，key/iv 与内置表同款派生（不直接收 key/iv）
            parsedQc.append(OppoKeyPair{id, deriveQcLikeKeyIv(mc, userkey),
                                        deriveQcLikeKeyIv(mc, iv)});
        }
    }

    const QJsonValue opsValue = root.value(QStringLiteral("ops"));
    if (!opsValue.isUndefined()) {
        if (!opsValue.isArray())
            return fail(QStringLiteral("ops 段不是数组"));
        const QJsonArray opsArray = opsValue.toArray();
        for (int i = 0; i < opsArray.size(); ++i) {
            const QString where = QStringLiteral("ops[%1]").arg(i);
            if (!opsArray.at(i).isObject())
                return fail(QStringLiteral("%1 不是对象").arg(where));
            const QJsonObject entry = opsArray.at(i).toObject();
            QString reason;
            const QString id = idField(entry, where, reason);
            if (id.isEmpty())
                return fail(reason);
            // A3：OPS 密钥材料是 62B 全量 mbox blob（长度显式校验，回归保护点）
            const QByteArray blob = hexField(entry, QStringLiteral("key"), 62, where, reason);
            if (blob.isEmpty())
                return fail(reason);
            parsedOps.append(OpsKey{id, blob});
        }
    }

    out.append(parsedQc);
    opsOut.append(parsedOps);
    if (error)
        error->clear();
    return true;
}

// Task 5 实现（OPS 自定义密码移植）
QByteArray opsDecrypt(const QByteArray &data, const QByteArray &mboxBlob62)
{
    Q_UNUSED(data);
    Q_UNUSED(mboxBlob62);
    return QByteArray();
}

} // namespace imgopp
