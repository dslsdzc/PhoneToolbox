#include <QtTest>
#include <QByteArray>
#include <QFile>
#include <QList>
#include <QString>
#include <QTemporaryDir>
#include "image_engine/huawei_image.h"

// 华为 update.app 最小容器构造: 512B 头 + 64B 文件表条目 + 数据段
// （布局: 魔数 55 AA @0; 文件数 u32 @8; 文件表偏移 u32 @12; 数据段偏移 u32 @16;
//   条目: sequence(4) type(4) raw_size(4) comp_size(4) data_offset(4) crc32(4)
//   文件名(32B, ASCII 或 UTF-16LE 变体)）
static QByteArray put32(quint32 v)
{
    QByteArray b(4, 0);
    for (int i = 0; i < 4; ++i) b[i] = char((v >> (i * 8)) & 0xFF);
    return b;
}

static QByteArray buildMinimalApp()
{
    // 头 512B: 魔数 55 AA + 文件数(1) + 文件表偏移(512) + 数据段偏移(512+64)
    QByteArray hdr(512, 0);
    hdr[0] = char(0x55); hdr[1] = char(0xAA);
    hdr.replace(8, 4, put32(1));       // 文件数
    hdr.replace(12, 4, put32(512));    // 文件表偏移
    hdr.replace(16, 4, put32(512 + 64)); // 数据段偏移
    // 文件表条目 64B: type=0x02(boot), raw=8, comp=8, data_offset=0
    QByteArray entry(64, 0);
    entry.replace(4, 4, put32(0x02));
    entry.replace(8, 4, put32(8));
    entry.replace(12, 4, put32(8));
    entry.replace(16, 4, put32(0));
    const QByteArray name = QString("boot.img").toUtf8();
    for (int i = 0; i < name.size(); ++i) entry[32 + i] = name[i];
    QByteArray app = hdr + entry + QByteArray("BOOTDATA"); // 8B 数据, 与 raw/comp=8 一致
    return app;
}

class TestHuawei : public QObject
{
    Q_OBJECT
private slots:
    void detect();
    void parseMinimal();
    void extractFileData();
    void buildRoundTrip();
    void buildWithData();
    void parseUpdateBinL2();
    void buildWithSign();
    void loadSignConfigValid();
    void loadSignConfigInvalid();
};

void TestHuawei::detect()
{
    QVERIFY(imghw::isUpdateApp(QByteArray("\x55\xAA", 2)));
    QVERIFY(!imghw::isUpdateApp(QByteArray("CrAU")));
}

void TestHuawei::parseMinimal()
{
    QByteArray app = buildMinimalApp();
    QList<imghw::AppFile> files;
    QString err;
    QVERIFY(imghw::parseUpdateApp(app, files, &err));
    QCOMPARE(files.size(), 1);
    QCOMPARE(files[0].name, "boot.img");
    QCOMPARE(files[0].type, 0x02u);
}

void TestHuawei::extractFileData()
{
    QByteArray app = buildMinimalApp();
    QList<imghw::AppFile> files;
    QString err;
    QVERIFY(imghw::parseUpdateApp(app, files, &err));
    QByteArray data = imghw::extractFile(app, files[0], &err);
    QCOMPARE(data, QByteArray("BOOTDATA"));
}

// B5 骨架版修正: 容器仅含头+文件表, 无数据段 —— 只验证"解析骨架后字段一致"
// (type/name/rawSize/compSize 不变); extractFile 数据段断言由 B6 补全。
void TestHuawei::buildRoundTrip()
{
    QByteArray app = buildMinimalApp();
    QList<imghw::AppFile> files;
    QString err;
    QVERIFY(imghw::parseUpdateApp(app, files, &err));
    QByteArray rebuilt = imghw::buildUpdateApp(files);
    QList<imghw::AppFile> files2;
    QVERIFY(imghw::parseUpdateApp(rebuilt, files2, &err));
    QCOMPARE(files2.size(), 1);
    QCOMPARE(files2[0].name, "boot.img");
    QCOMPARE(files2[0].type, 0x02u);
    QCOMPARE(files2[0].rawSize, 8u);
    QCOMPARE(files2[0].compSize, 8u);
    // 骨架容器无数据段: extractFile 返回空为 buildUpdateApp 预期
    // （数据重打包走 buildUpdateAppWithData，见下方 B6 数据断言）
    QVERIFY(imghw::extractFile(rebuilt, files2[0], &err).isEmpty());
    // B6 数据断言: 8B 载荷 "BOOTDATA"（comp=8）重打包后逐字节一致
    const QByteArray payload = imghw::extractFile(app, files[0], &err);
    QCOMPARE(payload, QByteArray("BOOTDATA"));
    QByteArray rebuilt2 = imghw::buildUpdateAppWithData({{files[0], payload}});
    QList<imghw::AppFile> files3;
    QVERIFY(imghw::parseUpdateApp(rebuilt2, files3, &err));
    QCOMPARE(files3.size(), 1);
    QCOMPARE(files3[0].name, "boot.img");
    QCOMPARE(files3[0].compSize, 8u);
    QCOMPARE(imghw::extractFile(rebuilt2, files3[0], &err), QByteArray("BOOTDATA"));
}

void TestHuawei::buildWithData()
{
    imghw::AppFile f;
    f.name = "boot.img"; f.type = 0x02; f.rawSize = 8; f.compSize = 8;
    // 载荷与 compSize 一致（8B; brief 的 "BOOTDATA!" 为 9B 与 comp=8 不符, 同 B5 修正）
    QByteArray rebuilt = imghw::buildUpdateAppWithData({{f, QByteArray("BOOTDATA")}});
    QList<imghw::AppFile> files;
    QString err;
    QVERIFY(imghw::parseUpdateApp(rebuilt, files, &err));
    QCOMPARE(files.size(), 1);
    QCOMPARE(imghw::extractFile(rebuilt, files[0], &err), QByteArray("BOOTDATA"));
    // 数据长度与条目 compSize 不一致属非法输入: 返回空
    QVERIFY(imghw::buildUpdateAppWithData({{f, QByteArray("BOOTDATA!")}}).isEmpty());
}

void TestHuawei::parseUpdateBinL2()
{
    // 构造 L2 型: 178B 头 + 2B 长度 + 87B 分区信息(含 8B 长度 + 32B GUID) + 16B "update/info.bin"
    QByteArray bin(178, 0);
    // 2B 分区信息总长度 = 87
    QByteArray len2(2, 0); len2[0] = char(87);
    QByteArray partInfo(87, 0);
    auto put64 = [&](int off, quint64 v) { for (int i = 0; i < 8; ++i) partInfo[off + i] = char((v >> (i * 8)) & 0xFF); };
    // 87B 记录布局（按 unpack_huawei_package.py 核对）: 大小 8B @47-54, GUID 32B @55-86
    // （brief 原文 put64(48) 与 GUID@55 重叠 1 字节导致 4096 被 GUID 首字节覆盖, 修正为 @47）
    put64(47, 4096); // 分区长度 8B
    // GUID 最后 32B (55-86)
    QByteArray guid(32, '\xAB');
    partInfo.replace(55, 32, guid);
    // 尾部 16B: "update/info.bin" 名字段补零到 16B（brief 的裸字符串仅 15B, 与 16B 布局不符）
    bin.append(len2).append(partInfo)
       .append(QByteArray("update/info.bin").leftJustified(16, '\0'));
    QList<imghw::BinPartition> parts;
    QString err;
    QVERIFY(imghw::parseUpdateBin(bin, parts, &err));
    QCOMPARE(parts.size(), 1);
    QCOMPARE(parts[0].size, 4096ull);
    QCOMPARE(parts[0].guid.size(), 64); // 32B GUID → 64 位十六进制
    // 非法输入: 头不足 → false
    QVERIFY(!imghw::parseUpdateBin(QByteArray(10, 0), parts, &err));
    // 数据越界: 截掉尾部 16B "update/info.bin" → false
    QVERIFY(!imghw::parseUpdateBin(bin.left(178 + 2 + 87), parts, &err));
}

void TestHuawei::buildWithSign()
{
    imghw::AppFile f;
    f.name = "boot.img"; f.type = 0x02; f.rawSize = 8; f.compSize = 8;
    // 未提供签名配置 → 产物标记"未签名"（文件表无 type 0x05 条目）
    QByteArray plain = imghw::buildUpdateAppWithData({{f, QByteArray("BOOTDATA")}});
    QList<imghw::AppFile> files;
    QString err;
    QVERIFY(imghw::parseUpdateApp(plain, files, &err));
    QCOMPARE(files.size(), 1);
    // 提供签名配置 → 追加 type 0x05 签名占位条目（实际签名应用留扩展）
    imghw::SignConfig sc;
    sc.sigHeaderType = "08"; sc.sigLenOffset = 0; sc.haveKey = false;
    QByteArray signedApp = imghw::buildUpdateAppWithData({{f, QByteArray("BOOTDATA")}}, &sc);
    QList<imghw::AppFile> files2;
    QVERIFY(imghw::parseUpdateApp(signedApp, files2, &err));
    QCOMPARE(files2.size(), 2);
    QCOMPARE(files2[1].type, 0x05u);
    QCOMPARE(files2[1].name, QString("signature"));
    QVERIFY(imghw::extractFile(signedApp, files2[1], &err).isEmpty()); // 签名数据留空
    // 非法签名配置 → 返回空
    imghw::SignConfig bad = sc;
    bad.sigHeaderType.clear();
    QVERIFY(imghw::buildUpdateAppWithData({{f, QByteArray("BOOTDATA")}}, &bad).isEmpty());
}

void TestHuawei::loadSignConfigValid()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("sign.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{\"sigHeaderType\":\"08\",\"sigLenOffset\":0,\"haveKey\":true,\"keyPath\":\"/tmp/key.pem\"}");
    f.close();
    imghw::SignConfig sc;
    QString err;
    QVERIFY(imghw::loadSignConfig(path, sc, &err));
    QCOMPARE(sc.sigHeaderType, QString("08"));
    QCOMPARE(sc.sigLenOffset, 0u);
    QVERIFY(sc.haveKey);
    QCOMPARE(sc.keyPath, QString("/tmp/key.pem"));
    // "06" 类型 + 无密钥也可加载（haveKey 缺省为 false）
    const QString path2 = dir.filePath("sign2.json");
    QFile f2(path2);
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write("{\"sigHeaderType\":\"06\",\"sigLenOffset\":16}");
    f2.close();
    imghw::SignConfig sc2;
    QVERIFY(imghw::loadSignConfig(path2, sc2, &err));
    QCOMPARE(sc2.sigHeaderType, QString("06"));
    QCOMPARE(sc2.sigLenOffset, 16u);
    QVERIFY(!sc2.haveKey);
}

void TestHuawei::loadSignConfigInvalid()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("bad.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("{not valid json");
    f.close();
    imghw::SignConfig sc;
    QString err;
    QVERIFY(!imghw::loadSignConfig(path, sc, &err));          // JSON 损坏
    QVERIFY(!imghw::loadSignConfig(dir.filePath("nope.json"), sc, &err)); // 文件不存在
    // haveKey=true 但 keyPath 为空 → 非法
    const QString path2 = dir.filePath("bad2.json");
    QFile f2(path2);
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write("{\"sigHeaderType\":\"08\",\"haveKey\":true,\"keyPath\":\"\"}");
    f2.close();
    QVERIFY(!imghw::loadSignConfig(path2, sc, &err));
    // 未知签名头类型 → 非法
    const QString path3 = dir.filePath("bad3.json");
    QFile f3(path3);
    QVERIFY(f3.open(QIODevice::WriteOnly));
    f3.write("{\"sigHeaderType\":\"99\"}");
    f3.close();
    QVERIFY(!imghw::loadSignConfig(path3, sc, &err));
}

QTEST_APPLESS_MAIN(TestHuawei)
#include "test_huawei.moc"
