#include "oppo_crypto.h"

#include <cstring>

namespace imgopp {

namespace {

// ==================== FIPS-197 常量表（逐字节抄录, 行首为高半字节） ====================
// S-box（FIPS-197 图 7, §5.1.1 SubBytes 查表）: S(a) = 仿射变换(GF(2^8) 上的乘法逆)
//   63 7c 77 7b f2 6b 6f c5 30 01 67 2b fe d7 ab 76
const quint8 kSbox[256] = {
    0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76, // 00
    0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0, // 10
    0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15, // 20
    0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75, // 30
    0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84, // 40
    0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf, // 50
    0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8, // 60
    0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2, // 70
    0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73, // 80
    0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb, // 90
    0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79, // a0
    0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08, // b0
    0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a, // c0
    0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e, // d0
    0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf, // e0
    0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16, // f0
};

// 轮常量（FIPS-197 §5.2 KeyExpansion: Rcon[i] = (RC[i], 00, 00, 00), RC[i] = x^(i-1) ∈ GF(2^8)）
// AES-128 只需 i = 1..10（Nr = 10 → 44 word）；下标 0 占位不使用。
const quint8 kRcon[11] = {0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};

// GF(2^8) 乘 2（FIPS-197 §4.2.1 xtime）: 左移 1 位, 溢出则异或不可约多项式
// m(x) = x^8 + x^4 + x^3 + x + 1（低 8 位即 0x1b）
quint8 xtime(quint8 a)
{
    return quint8((a << 1) ^ ((a & 0x80) ? 0x1b : 0x00));
}

// GF(2^8) 乘 3（§5.1.3 MixColumns 用）: 3·a = 2·a ⊕ a
quint8 gmul3(quint8 a)
{
    return quint8(xtime(a) ^ a);
}

// SubBytes（FIPS-197 §5.1.1）: 状态逐字节查 S-box
void subBytes(quint8 s[16])
{
    for (int i = 0; i < 16; ++i)
        s[i] = kSbox[s[i]];
}

// ShiftRows（FIPS-197 §5.1.2）: 第 r 行循环左移 r 字节, state'[r][c] = state[r][(c+r) mod 4]。
// 状态按列主序展平（字节下标 i ↔ 行 i%4 / 列 i/4, 与 FIPS-197 §3.4 的 state[r][c] = in[r+4c] 一致）
void shiftRows(quint8 s[16])
{
    quint8 t[16];
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            t[r + 4 * c] = s[r + 4 * ((c + r) & 3)];
    std::memcpy(s, t, 16);
}

// MixColumns（FIPS-197 §5.1.3）: 每列视为 GF(2^8) 上多项式, 模 x^4+1 乘
// a(x) = {03}x^3 + {01}x^2 + {01}x + {02}（列内字节 s0..s3 对应行 0..3）
void mixColumns(quint8 s[16])
{
    for (int c = 0; c < 4; ++c) {
        quint8 *p = s + 4 * c;
        const quint8 s0 = p[0], s1 = p[1], s2 = p[2], s3 = p[3];
        p[0] = quint8(xtime(s0) ^ gmul3(s1) ^ s2 ^ s3);
        p[1] = quint8(s0 ^ xtime(s1) ^ gmul3(s2) ^ s3);
        p[2] = quint8(s0 ^ s1 ^ xtime(s2) ^ gmul3(s3));
        p[3] = quint8(gmul3(s0) ^ s1 ^ s2 ^ xtime(s3));
    }
}

// CFB 加解密共用实现（encrypt 决定反馈取输出还是输入）:
//   C_i = P_i ⊕ MSB_s(E(C_{i-1})), C_0 的反馈为 IV（FIPS-197 不定义模式,
//   用 NIST SP 800-38A §6.3 CFB, segment_size s = 128 即全块 —— 反馈恒为"整 16B 密文块"）。
// 与 bkerler/oppo_decrypt 的 Cryptodome AES.MODE_CFB(segment_size=128) 行为一致
// （ofp_qc_decrypt.py aes_cfb(), L148-151）。末段不足 16B 时密钥流按剩余长度截断取前缀
// （Cryptodome 同为截断语义; 该反馈不再被后续段使用）。
QByteArray aes128CfbCrypt(const QByteArray &data, const QByteArray &key16,
                          const QByteArray &iv16, bool encrypt)
{
    // 契约: 空输入 / key|iv 非 16B 一律返回空 QByteArray（调用方以此判失败）
    if (data.isEmpty() || key16.size() != 16 || iv16.size() != 16)
        return QByteArray();
    Aes128 aes;
    if (!aes.setKey(reinterpret_cast<const quint8 *>(key16.constData())))
        return QByteArray();

    QByteArray out;
    out.resize(data.size());
    const quint8 *src = reinterpret_cast<const quint8 *>(data.constData());
    quint8 *dst = reinterpret_cast<quint8 *>(out.data());

    quint8 feedback[16];   // 反馈寄存器: 初值 IV, 之后为前一段密文
    std::memcpy(feedback, iv16.constData(), 16);

    const qsizetype total = data.size();
    for (qsizetype off = 0; off < total; off += 16) {
        quint8 keystream[16];
        aes.encryptBlock(feedback, keystream);              // E(IV) / E(前段密文)
        const qsizetype n = qMin<qsizetype>(16, total - off);  // 末段可不足 16B
        for (qsizetype i = 0; i < n; ++i)
            dst[off + i] = quint8(src[off + i] ^ keystream[i]);
        // 反馈 = 本段密文整 16B（加密取输出, 解密取输入）; 末段 n < 16 时循环已结束, 截断无影响
        std::memcpy(feedback, encrypt ? (dst + off) : (src + off), size_t(n));
    }
    return out;
}

} // namespace

// 密钥扩展（FIPS-197 §5.2）: 16B 密钥 → 44 word（11 轮 × 4 word = 176B）字节展平存入 m_roundKey。
//   w[i] = w[i-4] ⊕ (i % 4 == 0 ? SubWord(RotWord(w[i-1])) ⊕ Rcon[i/4] : w[i-1])
// RotWord: [b0,b1,b2,b3] → [b1,b2,b3,b0]; SubWord: 逐字节 S-box; Rcon 仅异或进首字节。
void Aes128::keyExpansion(const quint8 key[16])
{
    std::memcpy(m_roundKey, key, 16);   // w[0..3] = 原始密钥
    for (int i = 4; i < 44; ++i) {
        quint8 t[4];
        std::memcpy(t, m_roundKey + 4 * (i - 1), 4);
        if (i % 4 == 0) {
            const quint8 b0 = t[0];
            t[0] = kSbox[t[1]];
            t[1] = kSbox[t[2]];
            t[2] = kSbox[t[3]];
            t[3] = kSbox[b0];
            t[0] ^= kRcon[i / 4];
        }
        for (int b = 0; b < 4; ++b)
            m_roundKey[4 * i + b] = quint8(m_roundKey[4 * (i - 4) + b] ^ t[b]);
    }
}

bool Aes128::setKey(const quint8 key[16])
{
    m_valid = false;
    if (!key)
        return false;
    keyExpansion(key);
    m_valid = true;
    return true;
}

// 单块加密（FIPS-197 §5.1 Cipher, Nr = 10）:
//   AddRoundKey(w[0..3]) → 9 轮 {SubBytes, ShiftRows, MixColumns, AddRoundKey(4r..4r+3)}
//   → 末轮 {SubBytes, ShiftRows, AddRoundKey(w[40..43])}（末轮无 MixColumns）。
// AddRoundKey（§5.1.4）: 状态字节 i 异或 轮密钥字节 16*round + i
// （state 列主序 i = r+4c ↔ w[round*4+c] 的第 r 字节）。
// 调用方须先 setKey() 成功（isValid()）；未 setKey 时轮密钥全 0, 结果非 AES。
void Aes128::encryptBlock(const quint8 in[16], quint8 out[16]) const
{
    quint8 s[16];
    std::memcpy(s, in, 16);

    for (int i = 0; i < 16; ++i)
        s[i] ^= m_roundKey[i];
    for (int round = 1; round <= 9; ++round) {
        subBytes(s);
        shiftRows(s);
        mixColumns(s);
        for (int i = 0; i < 16; ++i)
            s[i] ^= m_roundKey[16 * round + i];
    }
    subBytes(s);
    shiftRows(s);
    for (int i = 0; i < 16; ++i)
        s[i] ^= m_roundKey[160 + i];

    std::memcpy(out, s, 16);
}

QByteArray aes128CfbDecrypt(const QByteArray &data, const QByteArray &key16,
                            const QByteArray &iv16)
{
    return aes128CfbCrypt(data, key16, iv16, false);
}

QByteArray aes128CfbEncrypt(const QByteArray &data, const QByteArray &key16,
                            const QByteArray &iv16)
{
    return aes128CfbCrypt(data, key16, iv16, true);
}

} // namespace imgopp
