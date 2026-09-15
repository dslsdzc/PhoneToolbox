// tests/test_mtk_da_file.cpp
//
// AllInOne DA 解析：合成夹具（覆盖 P1-P12 的边界）+ 真样本对拍
// （6 个真实文件 / 161 条目，与 reference/mtk-samples/da_parse_report.json 逐字段一致）。
#include <QtTest>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "core/modes/mtk_da_file.h"
#include "mtk_test_helpers.h"

class TestMtkDaFile : public QObject
{
    Q_OBJECT
private slots:
    void parsesHeaderAndEntries();
    void detectsV6ByMarkerNotBanner();
    void probesOldFormatWithCrossValidation();
    void rejectsBadInputs();
    void toleratesRealWorldShapes();
    // 真样本（reference/mtk-samples/，缺失时 SKIP；验证跑带 -DMTK_SAMPLES_REQUIRED=ON）
    void realSamplesMatchIndependentReport();
};

using namespace mtktest;

static EntrySpec entryWith3Regions(quint16 hwCode = 0x6765)
{
    EntrySpec e;
    e.hwCode = hwCode;
    RegionSpec r0; r0.startAddr = 0x200000; r0.len = 16;
    RegionSpec r1; r1.startAddr = 0x2007000; r1.len = 16;
    RegionSpec r2; r2.startAddr = 0x80000000; r2.len = 16; r2.sigLen = 0x100;
    e.regions << r0 << r1 << r2;
    return e;
}

void TestMtkDaFile::parsesHeaderAndEntries()
{
    const QByteArray data = buildDa({entryWith3Regions(), entryWith3Regions(0x6752)});
    mtkbrom::DaFile f;
    QString err;
    QVERIFY2(mtkbrom::parseDaFile(data, f, &err), qPrintable(err));
    QCOMPARE(f.count, quint32(2));
    QCOMPARE(f.entries.size(), 2);
    QVERIFY(!f.isV6);
    QVERIFY(!f.oldFormat);
    const mtkbrom::DaEntry &e = f.entries.at(0);
    QCOMPARE(e.magic, quint16(0xDADA));
    QCOMPARE(e.hwCode, quint16(0x6765));
    QCOMPARE(e.hwSubCode, quint16(0x8A00));
    QCOMPARE(e.hwVersion, quint16(0xCA00));
    QCOMPARE(e.pagesize, quint16(4096));
    QCOMPARE(e.regionCount, quint16(3));
    QCOMPARE(e.regions.size(), 3);
    QCOMPARE(e.regions.at(2).startAddr, quint32(0x80000000));
    QCOMPARE(e.regions.at(2).sigLen, quint32(0x100));
    // m_buf 指向真实载荷：按它切片能读回 builder 写入的填充字节
    const mtkbrom::DaRegion &r0 = e.regions.at(0);
    QCOMPARE(data.mid(int(r0.fileOffset), int(r0.len)), QByteArray(16, char(1)));
    QCOMPARE(f.entries.at(1).hwCode, quint16(0x6752));
}

void TestMtkDaFile::detectsV6ByMarkerNotBanner()
{
    // P10：判别式是**负向**的（含 "MTK_DA_v6" 才算 V6）；横幅版本号不得用于逻辑
    mtkbrom::DaFile f;
    QString err;
    QVERIFY2(mtkbrom::parseDaFile(buildDa({entryWith3Regions()}, /*v6=*/true), f, &err), qPrintable(err));
    QVERIFY(f.isV6);
    // 横幅写着 v5.1624 但**不含** "MTK_DA_v6" → 仍是老代（真实 iot.bin 就是这种）
    QVERIFY2(mtkbrom::parseDaFile(
                 buildDa({entryWith3Regions()}, false, false,
                         QByteArray("MTK_AllInOne_DA_v5.1624.00.00_viperbjk")), f, &err),
             qPrintable(err));
    QVERIFY(!f.isV6);
}

void TestMtkDaFile::probesOldFormatWithCrossValidation()
{
    mtkbrom::DaFile f;
    QString err;
    // P8：0xD8 老格式**无真实样本** —— 只合成夹具，用例注释里如实标注。
    // 夹具必须 >= 2 条目：0xD8 的探测点 0x6C+0xD8 只有落在 entry[1] 起始处才读到 magic
    // （单条目时那里是尾部载荷首字节 —— 探测语义本身即"entry[1] 是否紧跟其后"）。
    QVERIFY2(mtkbrom::parseDaFile(buildDa({entryWith3Regions(), entryWith3Regions(0x6752)}, false,
                                          /*oldFormat=*/true), f, &err),
             qPrintable(err));
    QVERIFY(f.oldFormat);
    QCOMPARE(f.entries.size(), 2);
    QCOMPARE(f.entries.at(0).magic, quint16(0xDADA));
}

void TestMtkDaFile::rejectsBadInputs()
{
    mtkbrom::DaFile f;
    QString err;
    // 太短
    QVERIFY(!mtkbrom::parseDaFile(QByteArray(0x40, '\0'), f, &err));
    QVERIFY(!err.isEmpty());
    // 头部标记不符
    QByteArray noMagic = buildDa({entryWith3Regions()});
    noMagic.replace(0, 4, "XXXX");
    err.clear();
    QVERIFY(!mtkbrom::parseDaFile(noMagic, f, &err));
    QVERIFY(!err.isEmpty());
    // P9：count_da 与 EOF 不符（声称 1000 条，文件只够 1 条）
    QByteArray badCount = buildDa({entryWith3Regions()});
    putLe32(badCount, 0x68, 1000);
    err.clear();
    QVERIFY(!mtkbrom::parseDaFile(badCount, f, &err));
    QVERIFY(err.contains(QStringLiteral("截断")) || !err.isEmpty());
    // P11：m_buf + m_len 越界（把 region[0] 的 m_len 改到超大）
    QByteArray oob = buildDa({entryWith3Regions()});
    putLe32(oob, 0x6C + 0x14 + 4, 0x7FFFFFFF);
    err.clear();
    QVERIFY(!mtkbrom::parseDaFile(oob, f, &err));
    QVERIFY(!err.isEmpty());
    // 条目 magic 不是 0xDADA（条目边界唯一的锚点 → fail-closed）
    QByteArray badMagic = buildDa({entryWith3Regions()});
    putLe16(badMagic, 0x6C, 0x0000);
    err.clear();
    QVERIFY(!mtkbrom::parseDaFile(badMagic, f, &err));
    QVERIFY(!err.isEmpty());
}

void TestMtkDaFile::toleratesRealWorldShapes()
{
    // P1/P7：pagesize 0/1、m_len==0、hw_code==0 占位条目都必须容忍（真实文件里都有）
    EntrySpec placeholder;
    placeholder.hwCode = 0x0000;
    placeholder.pagesize = 0;
    RegionSpec zeroLen; zeroLen.len = 0; zeroLen.startAddr = 0;
    placeholder.regions << zeroLen;
    EntrySpec normal = entryWith3Regions();
    normal.pagesize = 1;

    mtkbrom::DaFile f;
    QString err;
    QVERIFY2(mtkbrom::parseDaFile(buildDa({placeholder, normal}), f, &err), qPrintable(err));
    QCOMPARE(f.entries.size(), 2);
    QCOMPARE(f.entries.at(0).hwCode, quint16(0));
    QCOMPARE(f.entries.at(0).pagesize, quint16(0));
    QCOMPARE(f.entries.at(0).regions.at(0).len, quint32(0));
    QCOMPARE(f.entries.at(1).pagesize, quint16(1));
}

void TestMtkDaFile::realSamplesMatchIndependentReport()
{
    if (!mtktest::samplesAvailable()) {
#if MTK_SAMPLES_REQUIRED
        QFAIL("真样本目录不存在，但本次构建要求真样本（MTK_SAMPLES_REQUIRED=ON）");
#else
        QSKIP("真样本目录不存在（reference/ 为 gitignored）");
#endif
    }
    const QString dir = mtktest::samplesDir();
    // 独立解析器产物（由 reference/mtk-samples/parse_da.py 生成，非本模块产出）
    QFile jf(dir + QStringLiteral("/da_parse_report.json"));
    QVERIFY2(jf.open(QIODevice::ReadOnly), "缺 da_parse_report.json（样本侦察产物）");
    const QJsonArray report = QJsonDocument::fromJson(jf.readAll()).array();
    QVERIFY2(report.size() >= 6, qPrintable(QStringLiteral("报告条目文件数 %1").arg(report.size())));

    int totalEntries = 0;
    for (const QJsonValue &fv : report) {
        const QJsonObject fileObj = fv.toObject();
        const QString path = dir + QLatin1Char('/') + fileObj.value("file").toString();
        QFile df(path);
        QVERIFY2(df.open(QIODevice::ReadOnly), qPrintable(path));
        const QByteArray data = df.readAll();
        // 与报告独立算出的 sha256 比对（证明读的是同一个文件）
        QCOMPARE(fileObj.value("sha256").toString(),
                 QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex()));

        mtkbrom::DaFile f;
        QString err;
        QVERIFY2(mtkbrom::parseDaFile(data, f, &err), qPrintable(path + ": " + err));
        QCOMPARE(f.count, quint32(fileObj.value("count_da").toInt()));
        QVERIFY(f.isV6 == fileObj.value("v6_flag_mtkclient").toBool());
        QCOMPARE(f.oldFormat, fileObj.value("old_ldr").toBool());

        const QJsonArray entries = fileObj.value("entries").toArray();
        QCOMPARE(f.entries.size(), entries.size());
        for (int i = 0; i < entries.size(); ++i) {
            const QJsonObject eo = entries.at(i).toObject();
            const mtkbrom::DaEntry &e = f.entries.at(i);
            QCOMPARE(int(e.magic), eo.value("magic").toInt());
            QCOMPARE(int(e.hwCode), eo.value("hw_code").toInt());
            QCOMPARE(int(e.hwSubCode), eo.value("hw_sub_code").toInt());
            QCOMPARE(int(e.hwVersion), eo.value("hw_version").toInt());
            QCOMPARE(int(e.swVersion), eo.value("sw_version").toInt());
            QCOMPARE(int(e.pagesize), eo.value("pagesize").toInt());
            QCOMPARE(int(e.entryRegionIndex), eo.value("entry_region_index").toInt());
            QCOMPARE(int(e.regionCount), eo.value("entry_region_count").toInt());
            const QJsonArray regions = eo.value("regions").toArray();
            QCOMPARE(e.regions.size(), regions.size());
            for (int r = 0; r < regions.size(); ++r) {
                const QJsonObject ro = regions.at(r).toObject();
                QCOMPARE(int(e.regions.at(r).fileOffset), ro.value("m_buf").toInt());
                QCOMPARE(int(e.regions.at(r).len), ro.value("m_len").toInt());
                // toInt() 对 >= 0x80000000 返回 0（实测 0xf1000000→0），而真样本 m_start_addr
                // 多为 0xf0000000/0xf1000000 → 按 quint32 精确比较（判据不变：逐字段与报告相等）
                QCOMPARE(e.regions.at(r).startAddr, quint32(ro.value("m_start_addr").toDouble()));
                QCOMPARE(int(e.regions.at(r).startOffset), ro.value("m_start_offset").toInt());
                QCOMPARE(int(e.regions.at(r).sigLen), ro.value("m_sig_len").toInt());
            }
            ++totalEntries;
        }
    }
    // 样本侦察实测：161 条；少于 150 说明样本或报告有问题（不静默放过）
    QVERIFY2(totalEntries >= 150, qPrintable(QStringLiteral("对拍条目数 %1").arg(totalEntries)));
}

QTEST_APPLESS_MAIN(TestMtkDaFile)
#include "test_mtk_da_file.moc"
