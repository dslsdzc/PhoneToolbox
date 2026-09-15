// tests/test_mtk_preloader_emi.cpp
//
// EMI 提取（LEGACY 切片）：合成夹具（MMM 分支 / 偏移 0 分支 / 失败路径 / 边界）
// + 真实 preloader（reference/mtk-samples/preloader.bin）。
// 真样本实测（直读文件复核）：MMM 魔术 @0、MTK_BLOADER_INFO_v @254392、MTK_BIN @254492、
// 版本字节 "35"、EMI 块 mlen=0x3EBB8 siglen=0x66C、dramsize=912；
// LEGACY 切片 = 文件末尾 800B（内层块 [254392, 255304) 的 912B 里 MTK_BIN+0xC 的相对偏移为 112）。
#include <QtTest>
#include <QFile>

#include "core/modes/mtk_preloader_emi.h"
#include "mtk_test_helpers.h"

class TestMtkPreloaderEmi : public QObject
{
    Q_OBJECT
private slots:
    void extractsLegacySliceFromOffsetZeroMarker();
    void handlesMmmBranch();
    void handlesMmmDramsizeZeroRetry();
    void rejectsMalformedMmmBranch();
    void failsWithoutMarkerOrMtkBin();
    void parsesVersionFromTwoAsciiBytes();
    // 真样本（reference/mtk-samples/，缺失时 SKIP；验证跑带 -DMTK_SAMPLES_REQUIRED=ON）
    void realPreloaderExtracts();
};

using namespace mtktest;

// 夹具自带的 MMM 魔术字面量（**不复用被测模块的常量**：夹具与解析器共用同一原语时，
// 方向性错误会自洽通过 —— 见 mtk_test_helpers.h 的头注释）
static const char kMmmMagic[] = "\x4D\x4D\x4D\x01\x38\x00\x00\x00";

// 造一个"偏移 0 处有 MTK_BLOADER_INFO_v 且含 MTK_BIN"的 preloader：
//   [0, 18)  = "MTK_BLOADER_INFO_v"
//   [18, 20) = "38"（版本 38）
//   [0x40, 0x47) = "MTK_BIN"（上游从 MTK_BIN+0xC = 0x4C 起取 EMI）
//   [0x4C, 0x4C+emiLen) = EMI 载荷（0xA1 起、16 周期递增）
static QByteArray buildPreloader(int emiLen)
{
    QByteArray d(0x4C + emiLen, '\0');
    d.replace(0, 18, QByteArray("MTK_BLOADER_INFO_v", 18));
    d.replace(18, 2, "38");
    d.replace(0x40, 7, QByteArray("MTK_BIN", 7));
    for (int i = 0; i < emiLen; ++i)
        d[0x4C + i] = char(0xA1 + (i % 16));
    return d;
}

// MMM 包装（正常路径）：[0,8) 魔术 + [0x40, 0x40+pay.size()) 内层载荷 + 尾部 4B dramsize。
// mlen = 载荷结束位置（0x40 + payLen + 4）、siglen = 0 —— 裁剪后 data[-dramsize-4:-4]
// 恰好取回 [0x40, 0x40+payLen) 的内层载荷。
static QByteArray wrapMmm(const QByteArray &pay, quint32 dramsize)
{
    const quint32 payOff = 0x40;
    const quint32 mlen = payOff + quint32(pay.size()) + 4;
    QByteArray w(int(mlen), '\0');
    w.replace(0, 8, QByteArray(kMmmMagic, 8));
    w.replace(int(payOff), pay.size(), pay);
    putLe32(w, 0x20, mlen);              // mlen（小端）
    putLe32(w, 0x2C, 0);                 // siglen = 0
    putLe32(w, int(mlen) - 4, dramsize);
    return w;
}

// dramsize==0 重读路径的布局（上游：末 4B 读得 0 → 砍掉 0x800 再读一次）：
//   L = payLen + 0x844，使重读后的 data[-dramsize-4:-4] 恰好取回 [0x40, 0x40+payLen) 的载荷。
static QByteArray wrapMmmZeroDramsize(const QByteArray &pay)
{
    const int L = pay.size() + 0x844;
    QByteArray w(L, '\0');
    w.replace(0, 8, QByteArray(kMmmMagic, 8));
    w.replace(0x40, pay.size(), pay);
    putLe32(w, 0x20, quint32(L));                 // mlen
    putLe32(w, 0x2C, 0);                          // siglen = 0
    putLe32(w, L - 0x804, quint32(pay.size()));   // 砍掉 0x800 后重读到的 dramsize
    // [L-4, L) 保持 0：首读 dramsize == 0，触发重读分支
    return w;
}

void TestMtkPreloaderEmi::extractsLegacySliceFromOffsetZeroMarker()
{
    const QByteArray pre = buildPreloader(64);
    QCOMPARE(pre.indexOf(QByteArray("MTK_BIN")), 0x40);   // 夹具自检：标记确实在 0x40

    mtkbrom::EmiData emi;
    QString err;
    QVERIFY2(mtkbrom::extractEmiLegacy(pre, emi, &err), qPrintable(err));
    QCOMPARE(emi.branch, QStringLiteral("偏移0"));
    QCOMPARE(emi.ver, quint32(38));
    // **LEGACY 切片**：从 MTK_BIN+0xC 起（0x40 + 0xC = 0x4C），长度 64 —— 不是整块
    QCOMPARE(emi.bytes.size(), 64);
    QCOMPARE(emi.bytes, pre.mid(0x4C, 64));
}

void TestMtkPreloaderEmi::handlesMmmBranch()
{
    // MMM 分支（上游 m_extract_emi 的 1-4 步，**全小端**）：
    //   1) data = data[mmm:]            2) mlen/siglen @ +0x20/+0x2C → data = data[:mlen-siglen]
    //   3) dramsize = <u32 @ data[-4:]  4) data = data[-dramsize-4:-4]
    const QByteArray pay = buildPreloader(32);            // 内层 0x6C 字节，两个标记齐备
    const QByteArray wrapped = wrapMmm(pay, quint32(pay.size()));

    mtkbrom::EmiData emi;
    QString err;
    QVERIFY2(mtkbrom::extractEmiLegacy(wrapped, emi, &err), qPrintable(err));
    QCOMPARE(emi.branch, QStringLiteral("MMM"));
    QCOMPARE(emi.ver, quint32(38));
    // 第 4 步取回的正是内层载荷 → LEGACY 切片 = 内层载荷的 MTK_BIN+0xC 起
    QCOMPARE(emi.bytes, pay.mid(0x4C, 32));
}

void TestMtkPreloaderEmi::handlesMmmDramsizeZeroRetry()
{
    // 上游 DC:128-130：末 4B 的 dramsize == 0 → data = data[:-0x800] 后重读 dramsize。
    const QByteArray pay = buildPreloader(32);
    const QByteArray wrapped = wrapMmmZeroDramsize(pay);

    mtkbrom::EmiData emi;
    QString err;
    QVERIFY2(mtkbrom::extractEmiLegacy(wrapped, emi, &err), qPrintable(err));
    QCOMPARE(emi.branch, QStringLiteral("MMM"));
    QCOMPARE(emi.ver, quint32(38));
    QCOMPARE(emi.bytes, pay.mid(0x4C, 32));
}

void TestMtkPreloaderEmi::rejectsMalformedMmmBranch()
{
    // MMM 分支的边界：上游是 Python 切片（越界静默 clamp），本实现 fail-closed 报错
    // —— 逐条钉住"拒收"而不是"喂出垃圾"。
    mtkbrom::EmiData emi;
    QString err;

    // (1) 数据太短：连 mlen/siglen 都读不全（需要 [0x2C, 0x30)）
    const QByteArray tiny = QByteArray(kMmmMagic, 8) + QByteArray(0x20, '\0');
    QVERIFY(!mtkbrom::extractEmiLegacy(tiny, emi, &err));
    QVERIFY(!err.isEmpty());

    // (2) mlen < siglen（头部自相矛盾；上游会按负数切片静默留前段）
    QByteArray bad = QByteArray(kMmmMagic, 8) + QByteArray(0x40, '\0');
    putLe32(bad, 0x20, 0x100);           // mlen
    putLe32(bad, 0x2C, 0x200);           // siglen > mlen
    err.clear();
    QVERIFY(!mtkbrom::extractEmiLegacy(bad, emi, &err));
    QVERIFY(!err.isEmpty());

    // (3) dramsize == 0 且裁剪后 ≤ 0x800（上游此处 data[-4:] 会抛异常 → EMI 置 None）
    QByteArray zero = QByteArray(kMmmMagic, 8) + QByteArray(0x1F8, '\0');   // 共 0x200
    putLe32(zero, 0x20, quint32(zero.size()));   // mlen = 全长
    putLe32(zero, 0x2C, 0);                      // siglen = 0
    err.clear();
    QVERIFY(!mtkbrom::extractEmiLegacy(zero, emi, &err));
    QVERIFY(!err.isEmpty());

    // (4) dramsize 越界（尾部字段是垃圾）→ 拒收，不按上游静默 clamp 成 data[:-4]
    QByteArray huge = QByteArray(kMmmMagic, 8) + QByteArray(0xF8, '\0');    // 共 0x100
    putLe32(huge, 0x20, quint32(huge.size()));
    putLe32(huge, 0x2C, 0);
    putLe32(huge, int(huge.size()) - 4, 0x1000);   // > 可用数据
    err.clear();
    QVERIFY(!mtkbrom::extractEmiLegacy(huge, emi, &err));
    QVERIFY(!err.isEmpty());

    // 失败路径不得留下半份结果（fail-closed：调用方拿到 false 就不会用 bytes）
    QVERIFY(emi.bytes.isEmpty());
    QVERIFY(emi.ver == 0);
}

void TestMtkPreloaderEmi::failsWithoutMarkerOrMtkBin()
{
    mtkbrom::EmiData emi;
    QString err;

    // 无 MTK_BLOADER_INFO_v
    const QByteArray noMarker(0x100, '\0');
    QVERIFY(!mtkbrom::extractEmiLegacy(noMarker, emi, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(emi.bytes.isEmpty());

    // 有标记但抹掉 MTK_BIN（LEGACY 切片起点未知）
    QByteArray noBin = buildPreloader(32);
    noBin.replace(0x40, 7, QByteArray(7, '\0'));
    err.clear();
    QVERIFY(!mtkbrom::extractEmiLegacy(noBin, emi, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(emi.bytes.isEmpty());

    // 空输入
    err.clear();
    QVERIFY(!mtkbrom::extractEmiLegacy(QByteArray(), emi, &err));
    QVERIFY(!err.isEmpty());

    // 标记与 MTK_BIN 都在，但 MTK_BIN+0xC 越过末尾 → 切片为空（0x58 + 0xC > 0x60）
    QByteArray tail(0x60, '\0');
    tail.replace(0, 18, QByteArray("MTK_BLOADER_INFO_v", 18));
    tail.replace(18, 2, "38");
    tail.replace(0x58, 7, QByteArray("MTK_BIN", 7));
    err.clear();
    QVERIFY(!mtkbrom::extractEmiLegacy(tail, emi, &err));
    QVERIFY(!err.isEmpty());
}

void TestMtkPreloaderEmi::parsesVersionFromTwoAsciiBytes()
{
    // 版本 = 标记后**2 个 ASCII 字节**（上游 DC:138/143 的 int(...rstrip(b"\x00"))）。
    // 末例为非数字：上游 int() 抛异常 → 整个提取失败（emi=None）；本实现按 brief 判据继续
    // （ver=0）且提取仍成功 —— 发送端（D2）需自行决定 ver==0 是否可发。
    struct Case {
        const char *verBytes;
        quint32 expect;
    };
    const Case cases[] = {
        { "38", 38 },      // 上游 Tools/preloader_to_dram.py 的样例
        { "35", 35 },      // 真样本 preloader.bin 的实测字节
        { "5\0", 5 },      // 单字符 + NUL（上游 rstrip 去尾部 NUL）
        { "\0\0", 0 },     // 非数字 → ver = 0
    };
    for (const Case &c : cases) {
        QByteArray pre = buildPreloader(16);
        pre.replace(18, 2, QByteArray(c.verBytes, 2));
        mtkbrom::EmiData emi;
        QString err;
        QVERIFY2(mtkbrom::extractEmiLegacy(pre, emi, &err), qPrintable(err));
        QCOMPARE(emi.ver, c.expect);
        // 版本解析失败不影响切片（切片判据与版本号无关）
        QCOMPARE(emi.bytes, pre.mid(0x4C, 16));
    }
}

void TestMtkPreloaderEmi::realPreloaderExtracts()
{
    if (!mtktest::sampleFileAvailable(QStringLiteral("preloader.bin"))) {
#if MTK_SAMPLES_REQUIRED
        QFAIL("preloader.bin 缺失，但本次构建要求真样本（MTK_SAMPLES_REQUIRED=ON）");
#else
        QSKIP("preloader.bin 缺失（reference/ 为 gitignored）");
#endif
    }
    QFile f(mtktest::samplesDir() + QStringLiteral("/preloader.bin"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray pre = f.readAll();
    QVERIFY(!pre.isEmpty());

    // 样本锚点（实测复核）—— 三个标记的偏移就是本用例的"钉住真值"部分
    QCOMPARE(pre.indexOf(QByteArray(kMmmMagic, 8)), 0);
    QCOMPARE(pre.indexOf(QByteArray("MTK_BLOADER_INFO_v")), 254392);
    QCOMPARE(pre.indexOf(QByteArray("MTK_BIN")), 254492);
    QCOMPARE(pre.mid(254392 + 18, 2), QByteArray("35"));   // 版本字节（ASCII，非数字 5）

    mtkbrom::EmiData emi;
    QString err;
    QVERIFY2(mtkbrom::extractEmiLegacy(pre, emi, &err), qPrintable(err));
    // 该样本 MMM 魔术在偏移 0 → 走 MMM 分支（832 个真实 preloader 里仅 3 个含此魔术）
    QCOMPARE(emi.branch, QStringLiteral("MMM"));
    QCOMPARE(emi.ver, quint32(35));
    // LEGACY 切片：内层块 [254392, 255304)（912B = XFlash 的整块取法）里，MTK_BIN 在块内
    // 相对偏移 100，故切片 = 块起点 + 112 起的 800B —— 两代取不同切片（912 vs 800）
    QCOMPARE(emi.bytes.size(), 800);
    QCOMPARE(emi.bytes, pre.mid(254392 + 112, 800));
    QCOMPARE(emi.bytes.left(4), pre.mid(254492 + 0xC, 4));
    // 反向锚点：LEGACY 切片必须与 XFlash 整块不同（防"退回整块"的静默错法）
    QVERIFY(emi.bytes != pre.mid(254392, 912));
}

QTEST_APPLESS_MAIN(TestMtkPreloaderEmi)
#include "test_mtk_preloader_emi.moc"
