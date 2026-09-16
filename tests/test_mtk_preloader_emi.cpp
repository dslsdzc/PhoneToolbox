// tests/test_mtk_preloader_emi.cpp
//
// EMI 提取（LEGACY 切片）：合成夹具（MMM 分支 / 偏移 0 分支 / 失败路径 / 边界）
// + 真实 preloader（reference/mtk-samples/preloader.bin）。
// 真样本实测（直读文件复核）：MMM 魔术 @0、MTK_BLOADER_INFO_v @254392、MTK_BIN @254492、
// 版本字节 "35"、EMI 块 mlen=0x3EBB8 siglen=0x66C、dramsize=912；
// LEGACY 切片 = **dramsize 窗口** [254392, 255304)（912B；窗口末尾 255304 **不是** EOF）内
// MTK_BIN+0xC 之后的 800B（相对偏移 112 = 912 − 800）。**不是"到文件尾"** —— 到 EOF 会是 2704B。
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
    void rejectsNonNumericVersion();
    // 真样本（reference/mtk-samples/，缺失时 SKIP；验证跑带 -DMTK_SAMPLES_REQUIRED=ON）
    void realPreloaderExtracts();
    // XFlash 代（整块切片；与 LEGACY 在同一 preloader 上并存）
    void xflashSliceIsWholeWindow();
    void xflashAndLegacySlicesFromSyntheticFixture();
    void xflashRejectsMarkerNotAtWindowOrigin();
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
    // 分支名描述的是"**未命中 MMM**"（代码只判魔术在不在），不是"标记在偏移 0" ——
    // 标记不在偏移 0 时这个名字同样成立，故文案不得写"偏移0"。
    QCOMPARE(emi.branch, QStringLiteral("未命中MMM"));
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
    // 接受路径：版本 = 标记后**2 个 ASCII 字节**（上游 DC:138/143 的 int(...rstrip(b"\x00"))）——
    // **只去尾部 NUL**，去尾后全是数字才算读得懂。末两例是"合法的 0"：int("0")/int("00") 都是 0，
    // 而 emiver==0 在上游是**合法档位**（tier-0）—— 不得把合法 0 一起拒掉。
    struct Case {
        const char *verBytes;
        quint32 expect;
    };
    const Case cases[] = {
        { "38", 38 },      // 上游 Tools/preloader_to_dram.py 的样例
        { "35", 35 },      // 真样本 preloader.bin 的实测字节（**整体**两位，不是末位 5）
        { "5\0", 5 },      // 单字符 + NUL（上游 rstrip 去尾部 NUL）
        { "0\0", 0 },      // 合法档位 tier-0（单字符 "0"）
        { "00", 0 },       // 合法档位 tier-0（两位全数字："00" → 0）
    };
    for (const Case &c : cases) {
        QByteArray pre = buildPreloader(16);
        pre.replace(18, 2, QByteArray(c.verBytes, 2));
        mtkbrom::EmiData emi;
        QString err;
        QVERIFY2(mtkbrom::extractEmiLegacy(pre, emi, &err), qPrintable(err));
        QCOMPARE(emi.ver, c.expect);
        // 版本号的取值不影响切片
        QCOMPARE(emi.bytes, pre.mid(0x4C, 16));
    }
}

void TestMtkPreloaderEmi::rejectsNonNumericVersion()
{
    // 非数字版本 → **整体提取失败**（不是"ver=0 的成功"）：上游 int() 抛异常时是
    // `except Exception: self.emiver = 0; self.emi = None`（**DC:162-164**；注意 157-160 是
    // `self.error(...)/exit(1)` 的**硬中止**分支，别引错）→ 后面
    // `if self.daconfig.emi is not None:` 整段跳过、**根本不发 DRAM 配置**。
    // 而 ver==0 在上游是**合法档位**（tier-0）—— 把"读不懂"混进"合法的 0"会让伪造版本
    // 驱动协议分档，属本仓"静默错误值"家族。
    const char *bad[] = {
        "XX",       // 双字母
        "\0X",      // 前导 NUL + 字母（只去**尾部** NUL，故这是 2 字节非法）
        "3X",       // 数字 + 字母（不得"前缀能转就收"）
        "\0\0",     // 全 NUL（去尾部 NUL 后为空）
        " X",       // 空格 + 字母（toInt() 会 trim，故判据必须是"**全**数字"）
        "\0" "5",   // **内嵌** NUL + 数字：上游 int(rstrip 后的 b"\x005") 抛异常 → emi=None
                    // （相邻字面量拼接，**不能**写 "\x005" —— 那是十六进制转义 0x05）
    };
    for (const char *verBytes : bad) {
        QByteArray pre = buildPreloader(16);
        pre.replace(18, 2, QByteArray(verBytes, 2));
        mtkbrom::EmiData emi;
        QString err;
        QVERIFY2(!mtkbrom::extractEmiLegacy(pre, emi, &err), qPrintable(QByteArray(verBytes, 2).toHex()));
        QVERIFY2(err.contains(QStringLiteral("数字")), qPrintable(err));
        QVERIFY(emi.bytes.isEmpty());     // 失败即无 EMI（与上游 emi=None 净效果一致）
        QVERIFY(emi.ver == 0);
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

// XFlash：整块切片（真样本 912 B），与 LEGACY 的 800 B 在同一 preloader 上并存
void TestMtkPreloaderEmi::xflashSliceIsWholeWindow()
{
    // 用共享 helper 判"文件在不在"（目录在而样本缺失时应 SKIP 而非误 FAIL）—— 与真样本用例同一套
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

    mtkbrom::EmiData legacy, xflash;
    QString err;
    QVERIFY2(mtkbrom::extractEmiLegacy(pre, legacy, &err), qPrintable(err));
    err.clear();
    QVERIFY2(mtkbrom::extractEmiXflash(pre, xflash, &err), qPrintable(err));

    QCOMPARE(legacy.ver, quint32(35));
    QCOMPARE(xflash.ver, quint32(35));
    QCOMPARE(legacy.bytes.size(), 800);       // MTK_BIN+0xC 起
    QCOMPARE(xflash.bytes.size(), 912);       // **整块**（dramsize 窗口）
    QCOMPARE(legacy.bytes.size() + 112, xflash.bytes.size());
    QVERIFY(xflash.bytes.mid(112) == legacy.bytes);      // LEGACY 切片 = XFlash 切片去掉前 112 字节
}

// 合成：两种切片在同一夹具上的关系（不依赖真样本）
void TestMtkPreloaderEmi::xflashAndLegacySlicesFromSyntheticFixture()
{
    const QByteArray pre = buildPreloader(64);            // 既有夹具（含标记 + MTK_BIN + 尾部 EMI）
    QVERIFY(pre.indexOf(QByteArray(kMmmMagic, 8)) == -1); // 夹具自检：无 MMM ⇒ **窗口 = 整块输入**
    const int bin = pre.indexOf(QByteArray("MTK_BIN"));
    QCOMPARE(bin, 0x40);

    mtkbrom::EmiData legacy, xflash;
    QString err;
    QVERIFY2(mtkbrom::extractEmiLegacy(pre, legacy, &err), qPrintable(err));
    err.clear();
    QVERIFY2(mtkbrom::extractEmiXflash(pre, xflash, &err), qPrintable(err));
    // 两条都要有，才钉得住"XFlash = **整块窗口**"而不是"LEGACY 的弱式扩展"：
    // 长度断言钉住窗口长度；逐字节断言钉住窗口内 MTK_BIN+0xC 之后与 LEGACY 相同。
    // （只看 `xflash.bytes.endsWith(legacy.bytes)` 是判别不出退化的 —— 把 XFlash 换成 LEGACY
    //   切片时它照样为真；判别力证明见 task-3 报告 "Fix wave 1" 一节。）
    QCOMPARE(xflash.bytes.size(), pre.size());            // 窗口 = 整块输入（未命中 MMM 分支）
    QCOMPARE(xflash.bytes.mid(bin + 0xC), legacy.bytes);  // 且窗口的该偏移之后 == LEGACY 切片
}

// XFlash 的整块取法**不是无条件的**：上游 DC:137 把整块返回系于 `idx == 0`（标记正在窗口起点），
// 标记不在起点时落 else 分支、改取 MTK_BIN+0xC 切片（DC:141-144）—— 即**换成另一代的切片**。
// 本实现不静默换切片（否则发出的是上游在此情形不会发的字节）：明确失败，并把替代调用写进 error。
// 832 个真实 preloader **全部**满足标记在窗口偏移 0（见 task-3 报告的对拍统计），故该分支只有合成夹具能钉。
void TestMtkPreloaderEmi::xflashRejectsMarkerNotAtWindowOrigin()
{
    const QByteArray pre = QByteArray(0x10, '\x5A') + buildPreloader(16);   // 标记被推到窗口偏移 0x10
    QCOMPARE(pre.indexOf(QByteArray("MTK_BLOADER_INFO_v")), 0x10);          // 夹具自检：确实不在 0

    mtkbrom::EmiData legacy, xflash;
    QString err;
    // LEGACY 不受此条件约束（上游 LEGACY 分支只查 MTK_BIN 在不在）
    QVERIFY2(mtkbrom::extractEmiLegacy(pre, legacy, &err), qPrintable(err));
    QVERIFY(!legacy.bytes.isEmpty());

    err.clear();
    QVERIFY(!mtkbrom::extractEmiXflash(pre, xflash, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY2(err.contains(QStringLiteral("extractEmiLegacy")), qPrintable(err));   // 诊断须给出替代调用
    QVERIFY(xflash.bytes.isEmpty());      // fail-closed：失败不留半份结果
    // 实现细节，**不是契约**：失败后 ver 不可读（头文件只保证 bytes 恒空）；此断言只钉当前
    // fail-closed 行为（重置后守卫先于赋值），不得被调用方当作可依赖的取值
    QVERIFY(xflash.ver == 0);
}

QTEST_APPLESS_MAIN(TestMtkPreloaderEmi)
#include "test_mtk_preloader_emi.moc"
