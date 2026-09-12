#include <QtTest>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QTemporaryDir>
#include "image_engine/oppo_keys.h"

// OPPO 密钥库断言。派生期望值出自 docs/superpowers/specs/oppo-format-notes.md
// 的"派生自测断言"行（QC 全部 6 条 / MTK1 / MTK5）与速查表常量；速查表未列出的
// MTK2/3/4/6/7 由参照实现 reference/oppo_decrypt/ofp_mtk_decrypt.py
// getkey()+keytables（L46-99）独立复算取得（外部预言机，非本实现自证）。
class TestOppoKeys : public QObject
{
    Q_OBJECT

private slots:
    void qcDerivation();
    void mtkDerivation();
    void opsKeyCandidates();
    void loadOppoKeysJson();
};

// QC triplet 表：key/iv = md5(nibbleSwap(素材 XOR mc)) 十六进制前 16 字符的 ASCII。
// 顺序 = ofp_qc_decrypt.py generatekey2() 的尝试顺序（L56-90）。
void TestOppoKeys::qcDerivation()
{
    const auto keys = imgopp::qcKeyCandidates();
    QCOMPARE(keys.size(), 6);

    // 速查表 §QC 派生自测断言（逐条）
    QCOMPARE(keys[0].keyId, QStringLiteral("V1.4.17"));
    QCOMPARE(keys[0].key, QByteArray("d154afeeaafa958f"));
    QCOMPARE(keys[0].iv, QByteArray("2c040f5786829207"));
    QCOMPARE(keys[1].keyId, QStringLiteral("V1.6.17"));
    QCOMPARE(keys[1].key, QByteArray("2e96d7f462591a0f"));
    QCOMPARE(keys[1].iv, QByteArray("17cc63224c208708"));
    QCOMPARE(keys[2].keyId, QStringLiteral("V1.5.13"));
    QCOMPARE(keys[2].key, QByteArray("94d62e831cf1a1a0"));
    QCOMPARE(keys[2].iv, QByteArray("7ab5e33bd50d81ca"));
    QCOMPARE(keys[3].keyId, QStringLiteral("V1.6.6/1.6.9/1.6.17/1.6.24/1.6.26/1.7.6"));
    QCOMPARE(keys[3].key, QByteArray("4a837229e6fc77d4"));  // V1.6.6 族
    QCOMPARE(keys[3].iv, QByteArray("00bed47b80eec9d7"));
    QCOMPARE(keys[4].keyId, QStringLiteral("V1.7.2"));
    QCOMPARE(keys[4].key, QByteArray("3398699acebda0da"));
    QCOMPARE(keys[4].iv, QByteArray("b39a46f5cc4f0d45"));
    QCOMPARE(keys[5].keyId, QStringLiteral("V2.0.3"));
    QCOMPARE(keys[5].key, QByteArray("b4b7358eea220991"));
    QCOMPARE(keys[5].iv, QByteArray("e9077e26ab102d1b"));

    // 结构：AES-128 的 key/iv 均是 16 字节 ASCII 十六进制字符
    for (const auto &k : keys) {
        QCOMPARE(k.key.size(), 16);
        QCOMPARE(k.iv.size(), 16);
    }
}

// MTK0-7 与 QC 同派生方案（XOR 与半字节交换可交换），MTK8 是直接 ASCII 常量。
void TestOppoKeys::mtkDerivation()
{
    const auto keys = imgopp::mtkKeyCandidates();
    QCOMPARE(keys.size(), 9);

    QCOMPARE(keys[0].keyId, QStringLiteral("MTK0"));
    QCOMPARE(keys[0].key, QByteArray("94d62e831cf1a1a0"));  // MTK0 == QC V1.5.13 同 triplet
    QCOMPARE(keys[0].iv, QByteArray("7ab5e33bd50d81ca"));
    QCOMPARE(keys[1].keyId, QStringLiteral("MTK1"));
    QCOMPARE(keys[1].key, QByteArray("52dddab2c46aab56"));  // 速查表派生断言
    QCOMPARE(keys[1].iv, QByteArray("35f19b6877f9c360"));
    QCOMPARE(keys[2].key, QByteArray("403200d7e3ccbd16"));
    QCOMPARE(keys[2].iv, QByteArray("e6a12148a4d75e7d"));
    QCOMPARE(keys[3].key, QByteArray("d154afeeaafa958f"));  // == QC V1.4.17 同 triplet
    QCOMPARE(keys[3].iv, QByteArray("2c040f5786829207"));
    // MTK4 与 QC V1.6.6 族 triplet 相近但不同（3C4A.. vs 3C2D..）——防抄错行
    QCOMPARE(keys[4].key, QByteArray("264f527f9ad1ae8c"));
    QCOMPARE(keys[4].iv, QByteArray("788391d4ae2fd10c"));
    QCOMPARE(keys[5].key, QByteArray("443ec2fc7f543de6"));  // 速查表派生断言
    QCOMPARE(keys[5].iv, QByteArray("f02df2210580c734"));
    QCOMPARE(keys[6].key, QByteArray("be594eab795ccc85"));
    QCOMPARE(keys[6].iv, QByteArray("2016267766ad552b"));
    QCOMPARE(keys[7].key, QByteArray("1c1ca204c41d4f83"));
    QCOMPARE(keys[7].iv, QByteArray("916e63a107139707"));
    // MTK8 直接 ASCII 常量（不走派生）
    QCOMPARE(keys[8].keyId, QStringLiteral("MTK8"));
    QCOMPARE(keys[8].key, QByteArray("ab3f76d7989207f2"));
    QCOMPARE(keys[8].iv, QByteArray("2bf515b3a9737835"));

    for (const auto &k : keys) {
        QCOMPARE(k.key.size(), 16);
        QCOMPARE(k.iv.size(), 16);
    }
}

// OPS：A3 定为 62 字节全量 mbox blob（非 16B key），顺序 mbox5 → mbox6 → mbox4。
void TestOppoKeys::opsKeyCandidates()
{
    const auto keys = imgopp::opsKeyCandidates();
    QCOMPARE(keys.size(), 3);
    QCOMPARE(keys[0].keyId, QStringLiteral("mbox5"));
    QCOMPARE(keys[1].keyId, QStringLiteral("mbox6"));
    QCOMPARE(keys[2].keyId, QStringLiteral("mbox4"));

    // 逐字核对 reference/oppo_decrypt/opscrypto.py L55-78：
    // 16B 轮密钥素材 + 44×00 + 末 2B "0A00"（asbox[0x3C] = 轮数 0x0A）
    const QByteArray mbox5 = QByteArray::fromHex(
        "608A3F2D686BD423510CD095BB40E976"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0A00");
    const QByteArray mbox6 = QByteArray::fromHex(
        "AA69829E5DDEB13D30BB81A34665A3E1"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0A00");
    const QByteArray mbox4 = QByteArray::fromHex(
        "C45D057199DDBBEE29A16DC7ADBFA43F"
        "0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
        "0A00");
    QCOMPARE(mbox5.size(), 62);  // A3：62B 全量 blob（非 brief 原 16B 写法）
    QCOMPARE(mbox6.size(), 62);
    QCOMPARE(mbox4.size(), 62);
    QCOMPARE(keys[0].mboxBlob, mbox5);
    QCOMPARE(keys[1].mboxBlob, mbox6);
    QCOMPARE(keys[2].mboxBlob, mbox4);
    // 首 16B 素材 = 速查表 §OPS 列出的 mbox（guacamole / instantnoodle / guacamolet）
    QCOMPARE(keys[0].mboxBlob.left(16), QByteArray::fromHex("608A3F2D686BD423510CD095BB40E976"));
    QCOMPARE(keys[1].mboxBlob.left(16), QByteArray::fromHex("AA69829E5DDEB13D30BB81A34665A3E1"));
    QCOMPARE(keys[2].mboxBlob.left(16), QByteArray::fromHex("C45D057199DDBBEE29A16DC7ADBFA43F"));
    QCOMPARE(keys[0].mboxBlob.at(0x3C), char(0x0A));  // 轮数
}

// 外部 JSON 追加：成功路径复用 QC 派生断言（证明导入的是派生值而非原样 hex）；
// 失败路径必须写中文 error 并返回 false。
void TestOppoKeys::loadOppoKeysJson()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    // 成功：qc 段（V1.7.2 triplet）+ 追加语义（不清空 out）
    const QString okPath = dir.filePath(QStringLiteral("keys.json"));
    {
        QFile f(okPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const QByteArray json = R"({
            "qc": [{"id": "V1.7.2", "mc": "8FB8FB261930260BE945B841AEFA9FD4",
                    "userkey": "E529E82B28F5A2F8831D860AE39E425D",
                    "iv": "8A09DA60ED36F125D64709973372C1CF"}]
        })";
        QVERIFY(f.write(json) == json.size());
    }
    QList<imgopp::OppoKeyPair> out;
    out.append(imgopp::OppoKeyPair{QStringLiteral("已有条目"), QByteArray(16, 'x'), QByteArray(16, 'y')});
    QString error;
    QVERIFY(imgopp::loadOppoKeysJson(okPath, out, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(out.size(), 2);  // 追加，不清空调用方已有条目
    QCOMPARE(out[1].keyId, QStringLiteral("V1.7.2"));
    QCOMPARE(out[1].key, QByteArray("3398699acebda0da"));  // 与 qcDerivation 同源断言
    QCOMPARE(out[1].iv, QByteArray("b39a46f5cc4f0d45"));

    // 失败 1：文件不存在（error 中文文案；out 保持调用方原样）
    QList<imgopp::OppoKeyPair> outMissing;
    outMissing.append(imgopp::OppoKeyPair{QStringLiteral("哨兵"), QByteArray(16, 'z'), QByteArray(16, 'z')});
    QString errorMissing;
    QVERIFY(!imgopp::loadOppoKeysJson(dir.filePath(QStringLiteral("nope.json")),
                                      outMissing, &errorMissing));
    QVERIFY(errorMissing.startsWith(QStringLiteral("密钥文件")));
    QCOMPARE(outMissing.size(), 1);
    QCOMPARE(outMissing[0].keyId, QStringLiteral("哨兵"));

    // 失败 2：非法 JSON
    const QString badJsonPath = dir.filePath(QStringLiteral("bad.json"));
    {
        QFile f(badJsonPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const QByteArray truncated = R"({"qc": [)";  // 截断的 JSON
        QVERIFY(f.write(truncated) == truncated.size());
    }
    QList<imgopp::OppoKeyPair> outBad;
    outBad.append(imgopp::OppoKeyPair{QStringLiteral("哨兵"), QByteArray(16, 'z'), QByteArray(16, 'z')});
    QString errorBad;
    QVERIFY(!imgopp::loadOppoKeysJson(badJsonPath, outBad, &errorBad));
    QVERIFY(errorBad.startsWith(QStringLiteral("密钥文件")));
    QCOMPARE(outBad.size(), 1);

    // 失败 3：hex 字段长度不是 16 字节（mc 少一字节）
    const QString badHexPath = dir.filePath(QStringLiteral("badhex.json"));
    {
        QFile f(badHexPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        const QByteArray json = R"({
            "qc": [{"id": "X", "mc": "8FB8FB261930260BE945B841AEFA9F",
                    "userkey": "E529E82B28F5A2F8831D860AE39E425D",
                    "iv": "8A09DA60ED36F125D64709973372C1CF"}]
        })";
        QVERIFY(f.write(json) == json.size());
    }
    QList<imgopp::OppoKeyPair> outBadHex;
    outBadHex.append(imgopp::OppoKeyPair{QStringLiteral("哨兵"), QByteArray(16, 'z'), QByteArray(16, 'z')});
    QString errorBadHex;
    QVERIFY(!imgopp::loadOppoKeysJson(badHexPath, outBadHex, &errorBadHex));
    QVERIFY(errorBadHex.startsWith(QStringLiteral("密钥文件")));
    QVERIFY(errorBadHex.contains(QStringLiteral("mc")));  // 指认出错字段
    QCOMPARE(outBadHex.size(), 1);
}

QTEST_APPLESS_MAIN(TestOppoKeys)
#include "test_oppo_keys.moc"
