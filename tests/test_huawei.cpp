#include <QtTest>
#include <QByteArray>
#include <QList>
#include <QString>
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
    // 骨架容器无数据段: extractFile 返回空为 B5 预期（数据段由 B6 buildUpdateAppWithData 填充）
    QVERIFY(imghw::extractFile(rebuilt, files2[0], &err).isEmpty());
}

QTEST_APPLESS_MAIN(TestHuawei)
#include "test_huawei.moc"
