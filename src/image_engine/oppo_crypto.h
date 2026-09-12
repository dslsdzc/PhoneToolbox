#pragma once
// OPPO 固件解密密码核心（Phase A，spec 2026-09-03）
// AES-128 自实现（FIPS-197）；CFB segment_size=128 与 bkerler/oppo_decrypt
// 的 Cryptodome AES.MODE_CFB 逐字兼容（ofp_qc_decrypt.py aes_cfb()）。
#include <QByteArray>
#include <QtGlobal>

namespace imgopp {

class Aes128
{
public:
    // key: 16 字节；成功 true。失败（null）后 isValid() 为 false
    bool setKey(const quint8 key[16]);
    bool isValid() const { return m_valid; }
    void encryptBlock(const quint8 in[16], quint8 out[16]) const;

private:
    void keyExpansion(const quint8 key[16]);
    quint8 m_roundKey[176] = {};   // 11 轮 × 16B
    bool m_valid = false;
};

// AES-128-CFB（segment_size=128）解密。key16/iv16 必须各 16 字节，
// data 为空返回空，参数非法返回空 QByteArray。
QByteArray aes128CfbDecrypt(const QByteArray &data, const QByteArray &key16,
                            const QByteArray &iv16);

// AES-128-CFB（segment_size=128）加密（与 aes128CfbDecrypt 互逆，语义同
// Cryptodome AES.MODE_CFB(segment_size=128).encrypt —— 合成包测试夹具需要真正的
// 加密路径，不可用解密函数反向代替）。key16/iv16 必须各 16 字节，
// data 为空返回空，参数非法返回空 QByteArray。
QByteArray aes128CfbEncrypt(const QByteArray &data, const QByteArray &key16,
                            const QByteArray &iv16);

} // namespace imgopp
