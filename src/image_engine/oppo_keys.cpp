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

// ==================== OPS 自定义流密码（Task 5） ====================
//
// 逐行移植自 reference/oppo_decrypt/opscrypto.py（MIT，bkerler）：
//   gsbox()     L301-302
//   key_update() L304-352
//   key_custom() L355-401
// 副参照（双源核对，逐行等价）: reference/FirmwareKit.Oppo/FirmwareKit.OpsReader/Crypto/
//   Algorithms/OppOpsCipher.cs —— Gsbox() L29-32、KeyUpdateInPlace() L78-167、
//   ProcessBlocks() L290-323、ProcessBlock() L343-378、ProcessSubBlock() L390-434。
//
// 与参照的长度语义差异（本实现对调用方的契约）: 参照 key_custom() 的输出长度是"补齐到
// 4/16 字节"的长度（尾路按 4B 词补齐、块路按 16B 块补齐），两个调用方都只取前
// length / xmllength 字节（decryptfile() L433、extractxml() L417）→ 本实现直接返回
// "输入多长输出多长"（内部按参照补齐后截断），与参照实际写出的字节完全一致。
//
// 分支与迭代次数按**先补齐到 4 的倍数（pad4）之后**的长度算：参照的文件流调用方
// （decryptfile() L428-430、encryptsubsub() L440-446、encryptitem() L494-521）在调用
// key_custom() 之前就把数据 pad4 了，于是 0 < n ≤ 0xF 的判定用的是 pad4(n) —— 这就是
// SizeInByteInSrc ∈ {13,14,15} 时参照走**块路**（pad4 = 16B > 0xF）的原因，不是尾路。
// 注意：只有裸调 helper（不 pad4）才会在 13..15B 上翻转分支，那是审计陷阱而非目标语义；
// 本机复算 len 1..41 证明两口径仅在 13/14/15 三档不同，其余长度逐字节一致。

// 扩展 S-box: 2048B = 256 项 × 8B（每项在字节流中重复两次，故 gsbox(x*8) 与 gsbox(x*8+4)
// 同值）。逐字抄 opscrypto.py L80-143 的 `sbox = bytes.fromhex(...)`，未重算。
const char kOpsSBoxHex[] =
    "c66363a5c66363a5f87c7c84f87c7c84ee777799ee777799f67b7b8df67b7b8dfff2f20dfff2f20dd66b6bbdd66b6bbdde6f6fb1de6f6fb191c5c55491c5c554"
    "60303050603030500201010302010103ce6767a9ce6767a9562b2b7d562b2b7de7fefe19e7fefe19b5d7d762b5d7d7624dababe64dababe6ec76769aec76769a"
    "8fcaca458fcaca451f82829d1f82829d89c9c94089c9c940fa7d7d87fa7d7d87effafa15effafa15b25959ebb25959eb8e4747c98e4747c9fbf0f00bfbf0f00b"
    "41adadec41adadecb3d4d467b3d4d4675fa2a2fd5fa2a2fd45afafea45afafea239c9cbf239c9cbf53a4a4f753a4a4f7e4727296e47272969bc0c05b9bc0c05b"
    "75b7b7c275b7b7c2e1fdfd1ce1fdfd1c3d9393ae3d9393ae4c26266a4c26266a6c36365a6c36365a7e3f3f417e3f3f41f5f7f702f5f7f70283cccc4f83cccc4f"
    "6834345c6834345c51a5a5f451a5a5f4d1e5e534d1e5e534f9f1f108f9f1f108e2717193e2717193abd8d873abd8d87362313153623131532a15153f2a15153f"
    "0804040c0804040c95c7c75295c7c75246232365462323659dc3c35e9dc3c35e3018182830181828379696a1379696a10a05050f0a05050f2f9a9ab52f9a9ab5"
    "0e0707090e07070924121236241212361b80809b1b80809bdfe2e23ddfe2e23dcdebeb26cdebeb264e2727694e2727697fb2b2cd7fb2b2cdea75759fea75759f"
    "1209091b1209091b1d83839e1d83839e582c2c74582c2c74341a1a2e341a1a2e361b1b2d361b1b2ddc6e6eb2dc6e6eb2b45a5aeeb45a5aee5ba0a0fb5ba0a0fb"
    "a45252f6a45252f6763b3b4d763b3b4db7d6d661b7d6d6617db3b3ce7db3b3ce5229297b5229297bdde3e33edde3e33e5e2f2f715e2f2f711384849713848497"
    "a65353f5a65353f5b9d1d168b9d1d1680000000000000000c1eded2cc1eded2c4020206040202060e3fcfc1fe3fcfc1f79b1b1c879b1b1c8b65b5bedb65b5bed"
    "d46a6abed46a6abe8dcbcb468dcbcb4667bebed967bebed97239394b7239394b944a4ade944a4ade984c4cd4984c4cd4b05858e8b05858e885cfcf4a85cfcf4a"
    "bbd0d06bbbd0d06bc5efef2ac5efef2a4faaaae54faaaae5edfbfb16edfbfb16864343c5864343c59a4d4dd79a4d4dd766333355663333551185859411858594"
    "8a4545cf8a4545cfe9f9f910e9f9f9100402020604020206fe7f7f81fe7f7f81a05050f0a05050f0783c3c44783c3c44259f9fba259f9fba4ba8a8e34ba8a8e3"
    "a25151f3a25151f35da3a3fe5da3a3fe804040c0804040c0058f8f8a058f8f8a3f9292ad3f9292ad219d9dbc219d9dbc7038384870383848f1f5f504f1f5f504"
    "63bcbcdf63bcbcdf77b6b6c177b6b6c1afdada75afdada7542212163422121632010103020101030e5ffff1ae5ffff1afdf3f30efdf3f30ebfd2d26dbfd2d26d"
    "81cdcd4c81cdcd4c180c0c14180c0c142613133526131335c3ecec2fc3ecec2fbe5f5fe1be5f5fe1359797a2359797a2884444cc884444cc2e1717392e171739"
    "93c4c45793c4c45755a7a7f255a7a7f2fc7e7e82fc7e7e827a3d3d477a3d3d47c86464acc86464acba5d5de7ba5d5de73219192b3219192be6737395e6737395"
    "c06060a0c06060a019818198198181989e4f4fd19e4f4fd1a3dcdc7fa3dcdc7f4422226644222266542a2a7e542a2a7e3b9090ab3b9090ab0b8888830b888883"
    "8c4646ca8c4646cac7eeee29c7eeee296bb8b8d36bb8b8d32814143c2814143ca7dede79a7dede79bc5e5ee2bc5e5ee2160b0b1d160b0b1daddbdb76addbdb76"
    "dbe0e03bdbe0e03b6432325664323256743a3a4e743a3a4e140a0a1e140a0a1e924949db924949db0c06060a0c06060a4824246c4824246cb85c5ce4b85c5ce4"
    "9fc2c25d9fc2c25dbdd3d36ebdd3d36e43acacef43acacefc46262a6c46262a6399191a8399191a8319595a4319595a4d3e4e437d3e4e437f279798bf279798b"
    "d5e7e732d5e7e7328bc8c8438bc8c8436e3737596e373759da6d6db7da6d6db7018d8d8c018d8d8cb1d5d564b1d5d5649c4e4ed29c4e4ed249a9a9e049a9a9e0"
    "d86c6cb4d86c6cb4ac5656faac5656faf3f4f407f3f4f407cfeaea25cfeaea25ca6565afca6565aff47a7a8ef47a7a8e47aeaee947aeaee91008081810080818"
    "6fbabad56fbabad5f0787888f07878884a25256f4a25256f5c2e2e725c2e2e72381c1c24381c1c2457a6a6f157a6a6f173b4b4c773b4b4c797c6c65197c6c651"
    "cbe8e823cbe8e823a1dddd7ca1dddd7ce874749ce874749c3e1f1f213e1f1f21964b4bdd964b4bdd61bdbddc61bdbddc0d8b8b860d8b8b860f8a8a850f8a8a85"
    "e0707090e07070907c3e3e427c3e3e4271b5b5c471b5b5c4cc6666aacc6666aa904848d8904848d80603030506030305f7f6f601f7f6f6011c0e0e121c0e0e12"
    "c26161a3c26161a36a35355f6a35355fae5757f9ae5757f969b9b9d069b9b9d0178686911786869199c1c15899c1c1583a1d1d273a1d1d27279e9eb9279e9eb9"
    "d9e1e138d9e1e138ebf8f813ebf8f8132b9898b32b9898b32211113322111133d26969bbd26969bba9d9d970a9d9d970078e8e89078e8e89339494a7339494a7"
    "2d9b9bb62d9b9bb63c1e1e223c1e1e221587879215878792c9e9e920c9e9e92087cece4987cece49aa5555ffaa5555ff5028287850282878a5dfdf7aa5dfdf7a"
    "038c8c8f038c8c8f59a1a1f859a1a1f809898980098989801a0d0d171a0d0d1765bfbfda65bfbfdad7e6e631d7e6e631844242c6844242c6d06868b8d06868b8"
    "824141c3824141c3299999b0299999b05a2d2d775a2d2d771e0f0f111e0f0f117bb0b0cb7bb0b0cba85454fca85454fc6dbbbbd66dbbbbd62c16163a2c16163a";

const QByteArray &opsSBox()
{
    static const QByteArray sbox = QByteArray::fromHex(kOpsSBoxHex);
    return sbox;
}

// 4 字节 LE 读，语义 = Python int.from_bytes(buf[pos:pos+4], "little")：
// 切片越界/不足时按现有字节零扩展（gsbox(2047) 只能取到 1 字节，必须同语义），越界取 0。
quint32 leWordAt(const QByteArray &buf, qsizetype pos)
{
    quint32 v = 0;
    for (int i = 0; i < 4; ++i) {
        const qsizetype idx = pos + i;
        if (idx < 0 || idx >= buf.size())
            break;
        v |= quint32(quint8(buf.at(idx))) << (8 * i);
    }
    return v;
}

// asbox 字节读（越界取 0）。62B blob 的轮数 asbox[0x3c] 由外部 JSON 提供，可能是任意字节
// （最大 255 → 主循环取到 asbox[1023]），故不做裸下标 —— 对合法 blob 结果不变。
quint8 abAt(const QByteArray &asbox, qsizetype i)
{
    return i >= 0 && i < asbox.size() ? quint8(asbox.at(i)) : 0;
}

// gsbox() opscrypto.py L301-302: sbox 的**字节**偏移（不是条目下标）处取 LE u32。
// 注意偏移是字节级的: gsbox(x*8) 与 gsbox(x*8+1) 只差 1 字节（T 表四种旋转）。
quint32 gsbox(quint32 offset)
{
    return leWordAt(opsSBox(), qsizetype(offset));
}

void appendLe32(QByteArray &out, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        out.append(char((v >> (8 * i)) & 0xFF));
}

// key_update() opscrypto.py L304-352（C# KeyUpdateInPlace() L78-167）。
// rkey: 4 词状态（就地更新）；asbox: 轮常量源 —— 62B mbox blob 或 2048B sbox。
// 注意 gsbox() 恒读模块级 sbox，asbox 只提供 XOR 常量与轮数 asbox[0x3c]。
// 变量名保持参照（d/a/b/c/e/h/i 与 g），主循环临时量按副参照命名（d2/m/s/z/l/t，
// 参照里 d 被重用为 e>>0x18）。
void keyUpdate(quint32 rkey[4], const QByteArray &asbox)
{
    quint32 d = rkey[0] ^ quint32(abAt(asbox, 0));
    quint32 a = rkey[1] ^ quint32(abAt(asbox, 1));
    quint32 b = rkey[2] ^ quint32(abAt(asbox, 2));
    quint32 c = rkey[3] ^ quint32(abAt(asbox, 3));
    quint32 e = gsbox(((b >> 0x10) & 0xff) * 8 + 2) ^ gsbox(((a >> 8) & 0xff) * 8 + 3)
                ^ gsbox((c >> 0x18) * 8 + 1) ^ gsbox((d & 0xff) * 8) ^ quint32(abAt(asbox, 4));
    quint32 h = gsbox(((c >> 0x10) & 0xff) * 8 + 2) ^ gsbox(((b >> 8) & 0xff) * 8 + 3)
                ^ gsbox((d >> 0x18) * 8 + 1) ^ gsbox((a & 0xff) * 8) ^ quint32(abAt(asbox, 5));
    quint32 i = gsbox(((d >> 0x10) & 0xff) * 8 + 2) ^ gsbox(((c >> 8) & 0xff) * 8 + 3)
                ^ gsbox((a >> 0x18) * 8 + 1) ^ gsbox((b & 0xff) * 8) ^ quint32(abAt(asbox, 6));
    a = gsbox(((d >> 8) & 0xff) * 8 + 3) ^ gsbox(((a >> 0x10) & 0xff) * 8 + 2)
        ^ gsbox((b >> 0x18) * 8 + 1) ^ gsbox((c & 0xff) * 8) ^ quint32(abAt(asbox, 7));

    quint32 g = 8;
    for (int f = 0; f < int(abAt(asbox, 0x3c)) - 2; ++f) {
        const quint32 d2 = e >> 0x18;
        const quint32 m = h >> 0x10;
        const quint32 s = h >> 0x18;
        const quint32 z = e >> 0x10;
        const quint32 l = i >> 0x18;
        const quint32 t = e >> 8;
        e = gsbox(((i >> 0x10) & 0xff) * 8 + 2) ^ gsbox(((h >> 8) & 0xff) * 8 + 3)
            ^ gsbox((a >> 0x18) * 8 + 1) ^ gsbox((e & 0xff) * 8) ^ quint32(abAt(asbox, g));
        h = gsbox(((a >> 0x10) & 0xff) * 8 + 2) ^ gsbox(((i >> 8) & 0xff) * 8 + 3)
            ^ gsbox(d2 * 8 + 1) ^ gsbox((h & 0xff) * 8) ^ quint32(abAt(asbox, g + 1));
        i = gsbox((z & 0xff) * 8 + 2) ^ gsbox(((a >> 8) & 0xff) * 8 + 3)
            ^ gsbox(s * 8 + 1) ^ gsbox((i & 0xff) * 8) ^ quint32(abAt(asbox, g + 2));
        a = gsbox((t & 0xff) * 8 + 3) ^ gsbox((m & 0xff) * 8 + 2)
            ^ gsbox(l * 8 + 1) ^ gsbox((a & 0xff) * 8) ^ quint32(abAt(asbox, g + 3));
        g = g + 4;
    }

    // L345-352: 4 词合成（asbox 下标映射: word0→g, word1→g+3, word2→g+2, word3→g+1）
    rkey[0] = (gsbox(((i >> 0x10) & 0xff) * 8) & 0xff0000)
              ^ (gsbox(((h >> 8) & 0xff) * 8 + 1) & 0xff00)
              ^ (gsbox((a >> 0x18) * 8 + 3) & 0xff000000)
              ^ (gsbox((e & 0xff) * 8 + 2) & 0xFF) ^ quint32(abAt(asbox, g));
    rkey[1] = (gsbox(((a >> 0x10) & 0xff) * 8) & 0xff0000)
              ^ (gsbox(((i >> 8) & 0xff) * 8 + 1) & 0xff00)
              ^ (gsbox((e >> 0x18) * 8 + 3) & 0xff000000)
              ^ (gsbox((h & 0xff) * 8 + 2) & 0xFF) ^ quint32(abAt(asbox, g + 3));
    rkey[2] = (gsbox(((e >> 0x10) & 0xff) * 8) & 0xff0000)
              ^ (gsbox(((a >> 8) & 0xff) * 8 + 1) & 0xff00)
              ^ (gsbox((h >> 0x18) * 8 + 3) & 0xff000000)
              ^ (gsbox((i & 0xff) * 8 + 2) & 0xFF) ^ quint32(abAt(asbox, g + 2));
    rkey[3] = (gsbox(((h >> 0x10) & 0xff) * 8) & 0xff0000)
              ^ (gsbox(((e >> 8) & 0xff) * 8 + 1) & 0xff00)
              ^ (gsbox((i >> 0x18) * 8 + 3) & 0xff000000)
              ^ (gsbox((a & 0xff) * 8 + 2) & 0xFF) ^ quint32(abAt(asbox, g + 1));
}

// key_custom() opscrypto.py L355-401（C# ProcessBlocks() L290-323、ProcessBlock() L343-378、
// ProcessSubBlock() L390-434）。encrypt = 参照的 encrypt 参数。
// outlength 恒为 0（参照两个调用方都传 0）→ 参照 L362-370 的 "outlength != 0" 分支不移植
// （该分支只服务重打包，Phase A 明确不做）。
QByteArray keyCustom(const QByteArray &inp, const QByteArray &mbox, bool encrypt)
{
    const qsizetype len = inp.size();
    QByteArray outp;
    if (len == 0)
        return outp;   // 参照：length == 0 → 两个分支都不进 → 空输出

    // 状态初值（L53）：常量 d1b5e39e5eea049d671dd5abd2afcbaf 的 4 个 LE u32，不来自 mbox（A3）
    quint32 rkey[4] = {0x9ee3b5d1u, 0x9d04ea5eu, 0xabd51d67u, 0xafcbafd2u};
    outp.reserve(len);

    // 参照调用方先 pad4（decryptfile() L428-430 / encryptsubsub() L440-446），key_custom()
    // 内部的 length = len(pad4 后的数据) —— 分支与迭代次数都按它算；超出 len 的补齐字节
    // 由 leWordAt() 的零扩展提供（等价于参照切片/显式补零），输出最后统一截回 len。
    const qsizetype pad4 = (len + 3) & ~qsizetype(3);
    qsizetype length = pad4;
    if (length > 0xF) {
        // L373 `for ptr in range(0, length, 0x10)`: range 在循环创建时求值 →
        // 迭代次数 = ceil(pad4/16)，循环体内对 length 的递减不改变迭代次数
        for (qsizetype ptr = 0; ptr < pad4; ptr += 0x10) {
            keyUpdate(rkey, mbox);
            // L375-378: pos 恒为 0（outlength=0）→ slen = ((0xf - 0) >> 2) + 1 = 4
            quint32 tmp[4];
            for (int i = 0; i < 4; ++i)
                tmp[i] = rkey[i] ^ leWordAt(inp, ptr + i * 4);
            for (int i = 0; i < 4; ++i)
                appendLe32(outp, tmp[i]);
            if (encrypt) {
                for (int i = 0; i < 4; ++i)   // L380: rkey = tmp（反馈 = 输出词）
                    rkey[i] = tmp[i];
            } else {
                for (int i = 0; i < 4; ++i)   // L382: rkey = 输入词（反馈 = 密文词）
                    rkey[i] = leWordAt(inp, ptr + i * 4);
            }
            length -= 0x10;
        }
    }
    if (length != 0) {
        // L384-401: 尾路（0 < pad4 ≤ 0xF，即 len ≤ 12）。块路走完后 length ≤ 0 → while 体
        // 一次都不执行（参照仍会调用 key_update(rkey, sbox)，其结果随即被丢弃 → 无观测差异，
        // 为可审性保留原样）。
        keyUpdate(rkey, opsSBox());
        qsizetype m = 0;
        // 参照读的是 inp[j + ptr : …]，此处 j 从 0 起 = 参照的 ptr（进入尾路时 ptr 恒为 0：
        // 块路未执行则 ptr 停在 L360 的初值 0，执行过则 length ≤ 0 → while 体不执行）
        for (qsizetype j = 0; length > 0; j += 4, ++m, length -= 4) {
            const quint32 data = leWordAt(inp, j);   // L389-391: 不足 4B 按零补齐
            const quint32 out = data ^ rkey[m];
            appendLe32(outp, out);
            rkey[m] = encrypt ? out : data;          // L394-397
        }
    }
    outp.truncate(len);   // 契约：返回长度 == 输入长度（参照由调用方截断）
    return outp;
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

// OPS 自定义流密码（移植见文件上方匿名命名空间；契约见 oppo_keys.h）
QByteArray opsDecrypt(const QByteArray &data, const QByteArray &mboxBlob62)
{
    // 密钥材料不足一个 asbox（轮常量 + 轮数在 0x3c 内）→ 空返回（A9 空返回契约），
    // 调用方必须在自己的中文错误分支收口，不得静默继续
    if (mboxBlob62.size() < 62)
        return QByteArray();
    return keyCustom(data, mboxBlob62, false);
}

QByteArray opsEncrypt(const QByteArray &data, const QByteArray &mboxBlob62)
{
    if (mboxBlob62.size() < 62)
        return QByteArray();
    return keyCustom(data, mboxBlob62, true);
}

} // namespace imgopp
