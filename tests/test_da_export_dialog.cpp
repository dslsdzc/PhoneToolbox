// tests/test_da_export_dialog.cpp
//
// W8-T6：DA 切分另存的 UI 入口（`DaExportDialog`）—— T4 交付的 `writeExtractedDAs` 的第一个调用点。
// offscreen（CMakeLists 的 ENVIRONMENT 与其余对话框用例同款），**无设备依赖**：本入口读的是用户显式
// 给的 DA 文件 + 芯片码（这正是它"不依赖连接设备"的形态）。
//
// 断言强度的取舍（本目标的易恒真点逐条点明）：
//   * 期望字节**独立于被测路径**：夹具 `mtktest::buildDa` 的填充规则是 `char(1 + i*3 + r)`（见夹具注释），
//     用例据此写死期望值，而**不**从 `DaSelection.da1Bytes` 反推 —— 反推会让"取错 region/取错长度"
//     自洽地通过。DA1（32 字节、填 2）与 DA2（64 字节、填 3）长度与内容都不同 ⇒ 互换当场红。
//   * "用的是 **dacode** 而不是 hw_code"这一条**必须**有一条反向用例（`dacodeDrivesSelectionNotHwCode`）：
//     拿 hw_code 当键时"恰好两者相同的芯片"上照样能过 —— 只有拿一对**不同**的值才分辨得出来
//     （0x0321 → 0x6735 就是这样的芯片：89 条里 31 条两者不同）。
//   * 覆盖语义用**先写大文件再导出**钉住：只查"文件在那"会让"追加/不截断"实现蒙混过关。
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>

#include "core/modes/mtk_da_file.h"
#include "mtk_test_helpers.h"      // mtktest::buildDa（共享合成夹具；与解析器不共用读写原语）
#include "ui/da_export_dialog.h"

namespace {

// 芯片表里的这一对：hw_code 0x0321 → **dacode 0x6735**（两者不同，见 mtk_chip_table.h 的说明）。
const quint16 kHwCode = 0x0321;
const quint16 kDaCode = 0x6735;

constexpr int kDa1Len = 32;
constexpr int kDa2Len = 64;

mtktest::EntrySpec daEntry(quint16 hwCode, int da1Len = kDa1Len, int da2Len = kDa2Len)
{
    mtktest::EntrySpec e;
    e.hwCode = hwCode;
    e.hwSubCode = 0;
    e.hwVersion = 0;
    e.swVersion = 0;
    e.pagesize = 4096;
    mtktest::RegionSpec r0; r0.len = 16;                  // region[0] = EMI/env（**不该被导出**）
    mtktest::RegionSpec r1; r1.len = quint32(da1Len);     // region[1] = DA1（硬编码）
    r1.startAddr = 0x2007000;
    mtktest::RegionSpec r2; r2.len = quint32(da2Len);     // region[2] = DA2（硬编码）
    r2.startAddr = 0x80000000;
    r2.sigLen = 0x10;
    e.regions << r0 << r1 << r2;
    return e;
}

// 夹具填充规则：第 i 个条目第 r 个 region 的内容 = len 个 `char(1 + i*3 + r)` 字节。
QByteArray expectedRegion(int entryIndex, int regionIndex, int len)
{
    return QByteArray(len, char(1 + entryIndex * 3 + regionIndex));
}

QString writeDaFile(const QString &dir, const QList<mtktest::EntrySpec> &entries, bool v6 = false)
{
    const QString path = dir + QStringLiteral("/da.bin");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(mtktest::buildDa(entries, v6));
    f.close();
    return path;
}

QByteArray readAllBytes(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray();
    return f.readAll();
}

bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    const bool ok = (f.write(bytes) == bytes.size());
    f.close();
    return ok;
}

// 驱动对话框走「同一个入口」：设三个输入 → exportNow（按钮点击路径也走它）。
bool runExport(DaExportDialog &dlg, const QString &daPath, const QString &hwCode, const QString &outDir,
               QString *error = nullptr)
{
    dlg.setDaFilePath(daPath);
    dlg.setHwCodeText(hwCode);
    dlg.setOutputDir(outDir);
    return dlg.exportNow(error);
}

QString statusOf(const DaExportDialog &dlg)
{
    auto *label = dlg.findChild<QLabel *>(QStringLiteral("daStatusLabel"));
    return label ? label->text() : QString();
}

} // namespace

class TestDaExportDialog : public QObject
{
    Q_OBJECT

private slots:
    void exportsDa1AndDa2ByteForByte();
    void dacodeDrivesSelectionNotHwCode();
    void skipsEntryWithEmptyDa1AndTakesTheNext();
    void overwritesExistingOutputsCompletely();
    void v6FileIsFlaggedInStatus();
    void rejectsChipCodeOutsideTable();
    void rejectsUnparsableOrEmptyChipCode();
    void rejectsMissingInputsWithoutWritingAnything();
    void rejectsWhenNoEntryMatchesAndWritesNothing();
};

void TestDaExportDialog::exportsDa1AndDa2ByteForByte()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString daPath = writeDaFile(tmp.path(), {daEntry(kDaCode)});
    QVERIFY(!daPath.isEmpty());
    const QString outDir = tmp.filePath(QStringLiteral("out"));
    QVERIFY(QDir().mkpath(outDir));

    DaExportDialog dlg;
    QString err;
    QVERIFY2(runExport(dlg, daPath, QStringLiteral("0x0321"), outDir, &err), qPrintable(err));

    const QString da1 = outDir + QStringLiteral("/da1.bin");
    const QString da2 = outDir + QStringLiteral("/da2.bin");
    QVERIFY2(QFile::exists(da1), "da1.bin 没落盘");
    QVERIFY2(QFile::exists(da2), "da2.bin 没落盘");

    // 逐字节 + 长度（**不**从 DaSelection 反推，见文件头）。
    QCOMPARE(readAllBytes(da1), expectedRegion(0, 1, kDa1Len));
    QCOMPARE(readAllBytes(da2), expectedRegion(0, 2, kDa2Len));
    QCOMPARE(QFileInfo(da1).size(), qint64(kDa1Len));
    QCOMPARE(QFileInfo(da2).size(), qint64(kDa2Len));

    // 状态行要给出字节数（用户据此确认"切出来的不是空的"）——数字必须来自**实际导出**的那两段。
    const QString status = statusOf(dlg);
    QVERIFY2(status.contains(QStringLiteral("32")), qPrintable(status));
    QVERIFY2(status.contains(QStringLiteral("64")), qPrintable(status));
}

void TestDaExportDialog::dacodeDrivesSelectionNotHwCode()
{
    // 反向用例：夹具条目的 hw_code 写 **0x0321**（= 面板输入的 hw_code，而**不是** dacode）。
    // 若实现拿 hw_code 当选择键，这条会成功挑选 —— 正确实现必须**选不中**（dacode 是 0x6735）。
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString daPath = writeDaFile(tmp.path(), {daEntry(kHwCode)});
    QVERIFY(!daPath.isEmpty());
    const QString outDir = tmp.filePath(QStringLiteral("out"));
    QVERIFY(QDir().mkpath(outDir));

    DaExportDialog dlg;
    QString err;
    QVERIFY2(!runExport(dlg, daPath, QStringLiteral("0x0321"), outDir, &err), "拿 hw_code 当选择键了？");
    // 错误里要出现**查表用的那个 dacode**（用户据此知道该去核对什么）。
    QVERIFY2(err.contains(QStringLiteral("6735")), qPrintable(err));
    QVERIFY(!QFile::exists(outDir + QStringLiteral("/da1.bin")));
    QVERIFY(!QFile::exists(outDir + QStringLiteral("/da2.bin")));
}

void TestDaExportDialog::skipsEntryWithEmptyDa1AndTakesTheNext()
{
    // 真实文件里大量 region[1].len == 0（DA1 缺位）—— 选择层跳过它们并记 warning（P7：
    // **不得**"上传 0 字节后静默成功"）。本槽钉住：跳过后取**下一条**，且跳过原因浮到状态行。
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString daPath = writeDaFile(tmp.path(), {daEntry(kDaCode, /*da1Len=*/0), daEntry(kDaCode)});
    QVERIFY(!daPath.isEmpty());
    const QString outDir = tmp.filePath(QStringLiteral("out"));
    QVERIFY(QDir().mkpath(outDir));

    DaExportDialog dlg;
    QString err;
    QVERIFY2(runExport(dlg, daPath, QStringLiteral("0x0321"), outDir, &err), qPrintable(err));
    QCOMPARE(readAllBytes(outDir + QStringLiteral("/da1.bin")), expectedRegion(1, 1, kDa1Len));
    QCOMPARE(readAllBytes(outDir + QStringLiteral("/da2.bin")), expectedRegion(1, 2, kDa2Len));
    QVERIFY2(statusOf(dlg).contains(QStringLiteral("跳过")), qPrintable(statusOf(dlg)));
}

void TestDaExportDialog::overwritesExistingOutputsCompletely()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString daPath = writeDaFile(tmp.path(), {daEntry(kDaCode)});
    const QString outDir = tmp.filePath(QStringLiteral("out"));
    QVERIFY(QDir().mkpath(outDir));

    // 先放两份**比导出结果大**的旧文件：不截断的实现会留下尾巴，逐字节比对当场红。
    const QString da1 = outDir + QStringLiteral("/da1.bin");
    const QString da2 = outDir + QStringLiteral("/da2.bin");
    QVERIFY(writeBytes(da1, QByteArray(200, 'X')));
    QVERIFY(writeBytes(da2, QByteArray(200, 'Y')));

    DaExportDialog dlg;
    QString err;
    QVERIFY2(runExport(dlg, daPath, QStringLiteral("0x0321"), outDir, &err), qPrintable(err));

    QCOMPARE(QFileInfo(da1).size(), qint64(kDa1Len));
    QCOMPARE(readAllBytes(da1), expectedRegion(0, 1, kDa1Len));
    QCOMPARE(QFileInfo(da2).size(), qint64(kDa2Len));
    QCOMPARE(readAllBytes(da2), expectedRegion(0, 2, kDa2Len));
}

void TestDaExportDialog::v6FileIsFlaggedInStatus()
{
    // V6 文件的 DA**只能推向 XML 路径**（P10 的负向判别式；isXmlForced 由 selectDaEntry 填）。
    // 这件事必须让用户看见 —— 否则他会拿这份 DA 去走 LEGACY 流程。
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString daPath = writeDaFile(tmp.path(), {daEntry(kDaCode)}, /*v6=*/true);
    const QString outDir = tmp.filePath(QStringLiteral("out"));
    QVERIFY(QDir().mkpath(outDir));

    DaExportDialog dlg;
    QString err;
    QVERIFY2(runExport(dlg, daPath, QStringLiteral("0x0321"), outDir, &err), qPrintable(err));
    QVERIFY2(statusOf(dlg).contains(QStringLiteral("MTK_DA_v6")), qPrintable(statusOf(dlg)));
}

void TestDaExportDialog::rejectsChipCodeOutsideTable()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString daPath = writeDaFile(tmp.path(), {daEntry(kDaCode)});
    const QString outDir = tmp.filePath(QStringLiteral("out"));
    QVERIFY(QDir().mkpath(outDir));

    DaExportDialog dlg;
    QString err;
    QVERIFY(!runExport(dlg, daPath, QStringLiteral("0x9999"), outDir, &err));
    QVERIFY2(err.contains(QStringLiteral("不在芯片表内")), qPrintable(err));
    QVERIFY(statusOf(dlg).startsWith(QStringLiteral("✘")));
    QVERIFY(!QFile::exists(outDir + QStringLiteral("/da1.bin")));
}

void TestDaExportDialog::rejectsUnparsableOrEmptyChipCode()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString daPath = writeDaFile(tmp.path(), {daEntry(kDaCode)});
    const QString outDir = tmp.filePath(QStringLiteral("out"));
    QVERIFY(QDir().mkpath(outDir));

    DaExportDialog dlg;
    QString err;
    QVERIFY(!runExport(dlg, daPath, QString(), outDir, &err));
    QVERIFY2(err.contains(QStringLiteral("芯片码")), qPrintable(err));

    err.clear();
    QVERIFY(!runExport(dlg, daPath, QStringLiteral("0x67g5"), outDir, &err));
    QVERIFY2(err.contains(QStringLiteral("非十六进制")), qPrintable(err));
    QVERIFY(!QFile::exists(outDir + QStringLiteral("/da1.bin")));
}

void TestDaExportDialog::rejectsMissingInputsWithoutWritingAnything()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString daPath = writeDaFile(tmp.path(), {daEntry(kDaCode)});
    const QString outDir = tmp.filePath(QStringLiteral("out"));
    QVERIFY(QDir().mkpath(outDir));

    DaExportDialog dlg;
    QString err;
    // ① DA 文件不存在
    QVERIFY(!runExport(dlg, tmp.filePath(QStringLiteral("nope.bin")), QStringLiteral("0x0321"), outDir, &err));
    QVERIFY2(err.contains(QStringLiteral("打不开")), qPrintable(err));
    // ② 输出目录不存在（writeExtractedDAs **不替调用方建目录** —— 猜错地方比失败更糟）
    QVERIFY(!runExport(dlg, daPath, QStringLiteral("0x0321"), tmp.filePath(QStringLiteral("no-such-dir")), &err));
    QVERIFY(!err.isEmpty());
    // ③ 输出目录是个**文件**（不是目录）
    const QString fileAsDir = tmp.filePath(QStringLiteral("file-as-dir"));
    QVERIFY(writeBytes(fileAsDir, QByteArray("not a dir")));
    QVERIFY(!runExport(dlg, daPath, QStringLiteral("0x0321"), fileAsDir, &err));
    QVERIFY(!err.isEmpty());
    // 三种失败都不该在输出目录里留下任何东西
    QVERIFY(!QFile::exists(outDir + QStringLiteral("/da1.bin")));
    QVERIFY(!QFile::exists(outDir + QStringLiteral("/da2.bin")));
}

void TestDaExportDialog::rejectsWhenNoEntryMatchesAndWritesNothing()
{
    // DA 文件里的条目是**另一颗芯片**（0x6752）：dacode 0x6735 找不到候选 ⇒ 失败 + 列出文件里的 hw_code。
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString daPath = writeDaFile(tmp.path(), {daEntry(0x6752)});
    const QString outDir = tmp.filePath(QStringLiteral("out"));
    QVERIFY(QDir().mkpath(outDir));

    DaExportDialog dlg;
    QString err;
    QVERIFY(!runExport(dlg, daPath, QStringLiteral("0x0321"), outDir, &err));
    QVERIFY2(err.contains(QStringLiteral("选条目失败")), qPrintable(err));
    QVERIFY2(err.contains(QStringLiteral("6752")), qPrintable(err));   // 诊断里带上文件内的 hw_code
    QVERIFY(!QFile::exists(outDir + QStringLiteral("/da1.bin")));
    QVERIFY(!QFile::exists(outDir + QStringLiteral("/da2.bin")));
}

QTEST_MAIN(TestDaExportDialog)
#include "test_da_export_dialog.moc"
