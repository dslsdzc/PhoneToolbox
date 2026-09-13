#include <QtTest>
#include "image_engine/oppo_crypto.h"

class TestOppoCrypto : public QObject
{
    Q_OBJECT
private slots:
    void aesFips197Vector();
    void aesSetKeyRejectsNull();
    void cfbRoundTrip();
    void cfbShortInputKnownAnswer();
    void cfbKnownAnswer();
};

// FIPS-197 Appendix C.1: AES-128
// key 000102030405060708090a0b0c0d0e0f
// plaintext 00112233445566778899aabbccddeeff -> ciphertext 69c4e0d86a7b0430d8cdb78070b4c55a
void TestOppoCrypto::aesFips197Vector()
{
    imgopp::Aes128 aes;
    const QByteArray key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    QVERIFY(aes.setKey(reinterpret_cast<const quint8 *>(key.constData())));
    QVERIFY(aes.isValid());
    const QByteArray pt = QByteArray::fromHex("00112233445566778899aabbccddeeff");
    quint8 out[16];
    aes.encryptBlock(reinterpret_cast<const quint8 *>(pt.constData()), out);
    QCOMPARE(QByteArray(reinterpret_cast<char *>(out), 16),
             QByteArray::fromHex("69c4e0d86a7b0430d8cdb78070b4c55a"));
}

// 空指针密钥：契约是**返回 false 且 isValid() 保持 false**（oppo_crypto.h 的 setKey 契约、
// 实现 oppo_crypto.cpp:141-149 的 `if (!key) return false;`）。病态输入不得被当成"设了密钥"——
// 否则后续 encryptBlock 会拿全 0 轮密钥算出非 AES 的结果（实现注释 :156 明说这一点）。
void TestOppoCrypto::aesSetKeyRejectsNull()
{
    imgopp::Aes128 aes;
    QVERIFY(!aes.setKey(nullptr));
    QVERIFY(!aes.isValid());
    // 失败不污染状态：随后一次正常 setKey 仍须成功（否则"先误传 nullptr"会把对象永久废掉）
    const QByteArray key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    QVERIFY(aes.setKey(reinterpret_cast<const quint8 *>(key.constData())));
    QVERIFY(aes.isValid());
}

// CFB 自洽：解密(加密(x)) == x（用已知 key 手工构造 CFB 密文）
void TestOppoCrypto::cfbRoundTrip()
{
    const QByteArray key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const QByteArray iv  = QByteArray::fromHex("0f0e0d0c0b0a09080706050403020100");
    const QByteArray pt  = QByteArray("hello cfb world!!");  // 17 字节，跨块
    // 参考密文由 Cryptodome AES.MODE_CFB(segment_size=128) 生成，
    // 并与 openssl enc -aes-128-cfb 对拍一致（两路输出同值，防单源错误）：
    //   48cc95fedb6c388e663f8bb31ec2fd4b5a
    const QByteArray ct = QByteArray::fromHex("48cc95fedb6c388e663f8bb31ec2fd4b5a");
    QCOMPARE(imgopp::aes128CfbDecrypt(ct, key, iv), pt);
    // 自实现加解密互逆（供合成包夹具信任）
    QCOMPARE(imgopp::aes128CfbEncrypt(pt, key, iv), ct);
}

// <16B 短输入的 KAT（CFB 末段**截断**路径的回归保护）。上两条用例覆盖不到它：
//   * cfbRoundTrip 的 17 字节 = 一个整块 + 1 字节尾巴，虽也走截断路径，但期望值靠"自实现两向互逆"
//     与单条密文，测的是自洽；
//   * 长度非法用例走的是**入口早退**（key/iv 非 16B、data 空），根本没进加密循环。
// 加密循环（oppo_crypto.cpp:105-113）对末段的处理是 `n = min(16, total - off)` + 按 n 复制反馈，
// 若把 n 误写成 16（或漏 qMin），整块输入全绿、只有"最后一段不足 16B"的包会解密出垃圾。
// 两条向量都是**整段短输入**：11B（一个残块）与 5B（连一个整块都没有）。
// 参考密文由 Cryptodome AES.MODE_CFB(segment_size=128) 生成，并与
// `openssl enc -aes-128-cfb` 对拍一致（两路同值，防单源错误；生成命令与 key/iv 同 cfbRoundTrip）。
void TestOppoCrypto::cfbShortInputKnownAnswer()
{
    const QByteArray key = QByteArray::fromHex("000102030405060708090a0b0c0d0e0f");
    const QByteArray iv  = QByteArray::fromHex("0f0e0d0c0b0a09080706050403020100");

    const QByteArray pt15 = QByteArray("cfb tail 15byte");       // 15B：末段差 1 字节满块
    const QByteArray ct15 = QByteArray::fromHex("43cf9bb2c02d3284242ec9be15dafc");
    QCOMPARE(imgopp::aes128CfbEncrypt(pt15, key, iv), ct15);
    QCOMPARE(imgopp::aes128CfbDecrypt(ct15, key, iv), pt15);

    const QByteArray pt5 = QByteArray("short");                  // 5B：无整块
    const QByteArray ct5 = QByteArray::fromHex("53c196e0c0");
    QCOMPARE(imgopp::aes128CfbEncrypt(pt5, key, iv), ct5);
    QCOMPARE(imgopp::aes128CfbDecrypt(ct5, key, iv), pt5);
}

void TestOppoCrypto::cfbKnownAnswer()
{
    // 空输入返回空
    QCOMPARE(imgopp::aes128CfbDecrypt(QByteArray(), QByteArray(16, 0), QByteArray(16, 0)),
             QByteArray());
    // 长度非法（key/iv 非 16B）返回空
    QCOMPARE(imgopp::aes128CfbDecrypt(QByteArray(16, 1), QByteArray(15, 0), QByteArray(16, 0)),
             QByteArray());
    QCOMPARE(imgopp::aes128CfbEncrypt(QByteArray(16, 1), QByteArray(16, 0), QByteArray(15, 0)),
             QByteArray());
}

QTEST_APPLESS_MAIN(TestOppoCrypto)
#include "test_oppo_crypto.moc"
