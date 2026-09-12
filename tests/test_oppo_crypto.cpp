#include <QtTest>
#include "image_engine/oppo_crypto.h"

class TestOppoCrypto : public QObject
{
    Q_OBJECT
private slots:
    void aesFips197Vector();
    void cfbRoundTrip();
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
