#include <QtTest>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "ui/image_worker.h"
#include "image_engine/oppo_ofp.h"
#include "image_engine/oppo_ops.h"
#include "oppo_test_helpers.h"

// ImageWorker 接线测试（Task 6 / Phase A）。
//
// 覆盖点（都是"识别/解包接线"这一层，不重复引擎用例）：
//   1. **OFP/OPS 尾页探测顺序（修订 A11）**：OPS 尾页与 OFP-QC 共用 +0x10 的
//      0x7CEF，OPS 判据是 QC 判据的严格加强 → doDetect 必须"先 OPS 后 OFP"。
//      顺序反了的失效形态是静默的：真实 .ops 被判成 OFP-QC → 迟至 parse 阶段
//      才报"密钥未知或文件损坏"，用户看不出根因。故本文件用合成 OPS 包直接
//      断言 Format::OPS（detectOpsPackageByTail）—— 顺序反了该用例必红。
//   2. 扩展名兜底（.ofp/.ops）+ 未知扩展名靠尾页命中 + 小文件跳过尾页探测。
//   3. 解包分派：产物清单（outputs）与进度口径（中途封顶 99 / 收尾 100）。
//   4. **A10**：extractOFP 可 ok==true 且 *error 非空（部分条目被跳过）→ worker
//      必须把警告经 unpackFinished 回传，不得丢弃（静默部分解包是刷机场景大忌）。
//
// 夹具（合成包）来自 oppo_test_helpers.h + 本文件内的 opsPackage()：只按格式事实拼
// 字节流，不调用被测代码。
// 线程模型：ImageWorker 在工作线程执行，结果经 queued 信号投递 → 用 QSignalSpy
// 的 wait() 驱动事件循环，必须有 application 对象 —— 本文件用 QTEST_MAIN 建立
// QCoreApplication（QTEST_APPLESS_MAIN 不建 application 对象，切回去 spy.wait()
// 会直接挂死；见文件末尾的宏选择说明）。
class TestImageWorker : public QObject
{
    Q_OBJECT
private slots:
    void detectOpsPackageByTail();
    void detectOpsDetailCarriesTailFields();
    void detectOpsTailWithUnknownExtension();
    void detectQcPackage();
    void detectMtkPackage();
    void detectTinyFileSkipsTailProbe();
    void detectUnknownBytesStaysUnknown();
    void opsTailAlsoMatchesOfpQcJudge();
    void unpackOfpWritesOutputs();
    void unpackOfpPartialSuccessReportsWarning();
    void unpackMisnamedOfpReportsEngineError();
    void unpackMissingFileReportsError();
};

// ==================== 通用小工具 ====================

namespace {

// OPS 尾页形态：末 0x200 页 = version(2) / flags(1) / 0x7CEF@0x10
// （detectOPS 的全部输入；前导字节只把文件撑到一页以上以过 doDetect 的尾页门禁）
QByteArray opsShapedBlob(quint64 size = 0x4000)
{
    QByteArray blob(int(size), '\0');
    const qsizetype base = qsizetype(size) - 0x200;
    auto put32 = [&blob](qsizetype off, quint32 v) {
        for (int i = 0; i < 4; ++i)
            blob[off + i] = char((v >> (i * 8)) & 0xFF);
    };
    put32(base + 0x00, 2);
    put32(base + 0x04, 1);
    put32(base + 0x10, 0x7CEF);
    return blob;
}

// 合成 OPS 包（可被 parseOPS 完整解析的最小形态）：settings.xml（仅 BasicInfo 组、
// 无文件条目）密文 + 尾页 0x200，尾页 +0x14 记 settings 扇区 / +0x18 记清单明文长度
// / +0x1C/+0x2C 记 project id / firmware 名 —— 字段偏移与 test_oppo_ops.cpp 的
// buildOpsPackage 及 oppo_ops.cpp 常量同源（该文件的夹具另含文件条目，本用例只需要
// 尾页字段 → 不复用其完整夹具，避免跨文件耦合）。
// 注: 包长取 0x1000（doDetect 只在 fileSize ≥ 0x1000 时才读尾页做二次探测）。
QByteArray opsPackage(const QString &projectId, const QString &firmwareName)
{
    const QByteArray mboxBlob = imgopp::opsKeyCandidates().at(0).mboxBlob; // mbox5
    const QByteArray xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                          "<ProFile>\n  <BasicInfo Project=\"%1\" Version=\"%2\"/>\n"
                                          "</ProFile>\n").arg(projectId, firmwareName).toUtf8();
    // 参照的补齐式 (0x10 - len%0x10) 在已对齐时也补一整块（同 test_oppo_ops.cpp）
    const QByteArray padded = xml + QByteArray(0x10 - (xml.size() % 0x10), '\0');
    const QByteArray cipher = imgopp::opsEncrypt(padded, mboxBlob);

    QByteArray blob(0x1000, '\0');
    blob.replace(0, cipher.size(), cipher);
    const qsizetype tailBase = blob.size() - 0x200;
    ofptest::putLE32(blob, tailBase + 0x00, 2);              // version（A11 判据）
    ofptest::putLE32(blob, tailBase + 0x04, 1);              // flags（A11 判据）
    ofptest::putLE32(blob, tailBase + 0x10, 0x7CEF);         // 魔数
    ofptest::putLE32(blob, tailBase + 0x14, 0);              // settings 扇区位置 = 0
    ofptest::putLE32(blob, tailBase + 0x18, quint32(xml.size())); // 清单明文长度
    ofptest::putFixed(blob, tailBase + 0x1C, 16, projectId);
    ofptest::putFixed(blob, tailBase + 0x2C, 32, firmwareName);
    return blob;
}

QString writeBlob(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size())
        return QString();
    return path;
}

// 驱动一次 runDetect；超时（含工作线程异常）返回 false 并填 why，由调用方 QVERIFY2
bool detectFile(ImageWorker &worker, const QString &path, imgreg::Detected *out,
                QString *why)
{
    QSignalSpy spy(&worker, &ImageWorker::detectFinished);
    worker.runDetect(path);
    if (!spy.wait(10000)) {
        *why = QStringLiteral("detectFinished 超时（10s）");
        return false;
    }
    *out = qvariant_cast<imgreg::Detected>(spy.at(0).at(1));
    return true;
}

struct UnpackOutcome
{
    bool delivered = false; // 信号是否到达
    bool ok = false;
    QStringList outputs;
    QString error;
    QList<int> percents; // progress 信号序列（口径检查用）
};

UnpackOutcome unpackFile(ImageWorker &worker, const QString &path,
                         const imgreg::Detected &detected, const QString &outDir)
{
    UnpackOutcome outcome;
    QSignalSpy unpackSpy(&worker, &ImageWorker::unpackFinished);
    QSignalSpy progressSpy(&worker, &ImageWorker::progress);
    worker.runUnpack(path, outDir, detected);
    outcome.delivered = unpackSpy.wait(60000); // 超时（含工作线程异常）→ 空结果
    if (!outcome.delivered)
        return outcome;
    outcome.ok = unpackSpy.at(0).at(0).toBool();
    outcome.outputs = unpackSpy.at(0).at(1).toStringList();
    outcome.error = unpackSpy.at(0).at(2).toString();
    for (const QList<QVariant> &args : progressSpy)
        outcome.percents << args.at(0).toInt();
    return outcome;
}

} // namespace

// ==================== 1. 尾页探测（A11） ====================

// A11 实现级验证：OPS 形态的包必须判成 OPS。若 doDetect 按"先 OFP 后 OPS"探测，
// 同一份尾页会被 detectOFP 的 QC 分支先认领 → 本用例得到 Format::OFP 而失败。
// 兼作"信息卡字段解析失败优雅降级"用例：opsShapedBlob 只有尾页标记、没有可解析的
// settings.xml（长度字段为 0）→ 追加字段的 parseOPS 必然失败 → 文案须保持基础形态
// （既不得拼出空分隔符，也不得把已命中的探测结果降级成 Unknown）。
void TestImageWorker::detectOpsPackageByTail()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeBlob(dir.filePath(QStringLiteral("firmware.ops")),
                                   opsShapedBlob());
    QVERIFY(!path.isEmpty());

    ImageWorker worker;
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(worker, path, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::OPS);
    QCOMPARE(r.detail, QStringLiteral("OnePlus OPS 固件包"));
}

// spec §4 信息卡：探测命中 OPS 后，detail 须带上尾页 +0x1C 的 project id 与
// +0x2C 的 firmware 名（parseOPS 成功时）。字段值由夹具显式给定，断言其确实来自
// 尾页字段而非常量文案。
void TestImageWorker::detectOpsDetailCarriesTailFields()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeBlob(dir.filePath(QStringLiteral("firmware.ops")),
                                   opsPackage(QStringLiteral("18801"),
                                              QStringLiteral("guacamoles_31_O.09_190820")));
    QVERIFY(!path.isEmpty());

    ImageWorker worker;
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(worker, path, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::OPS);
    QCOMPARE(r.detail,
             QStringLiteral("OnePlus OPS 固件包 · 18801 · guacamoles_31_O.09_190820"));
}

// 未知扩展名（.bin）：无头魔数、扩展名兜底也给不出候选 → 只能靠尾页探测命中
void TestImageWorker::detectOpsTailWithUnknownExtension()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeBlob(dir.filePath(QStringLiteral("mystery.bin")),
                                   opsShapedBlob());
    QVERIFY(!path.isEmpty());

    ImageWorker worker;
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(worker, path, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::OPS);
}

// OFP-QC 合成包（尾页 0x7CEF @ +0x10，version/flags 非 2/1）→ OFP (QC)
void TestImageWorker::detectQcPackage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Firmware"), QStringLiteral("boot.img"),
          QByteArray(0x1200, 'F'), 0},
         {QStringLiteral("Sahara"), QStringLiteral("xbl.img"),
          QByteArray(0x2000, 'S'), 0}});
    QVERIFY(pkg.isValid());
    const QString path = writeBlob(dir.filePath(QStringLiteral("qc.ofp")), pkg.blob);
    QVERIFY(!path.isEmpty());

    ImageWorker worker;
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(worker, path, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::OFP);
    QCOMPARE(r.detail, QStringLiteral("OPPO/realme OFP 固件包 (QC)"));
}

// MTK 合成包（首 16B 试解出 "MMM"）→ OFP (MTK)。注：包体须 ≥ 0x1000 才会走尾页
// 探测（doDetect 对小文件跳过尾页读取），故载荷取 0x2000。
// spec §4 信息卡：detail 须带尾头项目名（prjname）与版本（flashtype）—— 值为夹具
// 显式给定（非默认值），断言其确实来自解析结果。
void TestImageWorker::detectMtkPackage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ofptest::MtkBuildOptions opts;
    opts.prjname = QStringLiteral("CPH1827");
    const ofptest::MtkPackage pkg = ofptest::buildMtkPackage(
        {{QStringLiteral("boot"), QStringLiteral("boot.img"), QByteArray(0x2000, 'M')}}, opts);
    QVERIFY(pkg.isValid());
    const QString path = writeBlob(dir.filePath(QStringLiteral("mtk.ofp")), pkg.blob);
    QVERIFY(!path.isEmpty());

    ImageWorker worker;
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(worker, path, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::OFP);
    QCOMPARE(r.detail, QStringLiteral("OPPO/realme OFP 固件包 (MTK) · CPH1827 · UFS"));
}

// 不足一页（0x200B）：读不到完整尾页 → 跳过尾页探测，仅扩展名兜底（不崩不误判）
void TestImageWorker::detectTinyFileSkipsTailProbe()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeBlob(dir.filePath(QStringLiteral("tiny.ops")),
                                   QByteArray(0x200, '\0'));
    QVERIFY(!path.isEmpty());

    ImageWorker worker;
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(worker, path, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::OPS); // 扩展名给出候选
    QCOMPARE(r.detail, QStringLiteral("按扩展名识别"));
}

// 无关内容不得误报：满 0x1000+ 的填充字节 + 小文本文件都保持 Unknown
void TestImageWorker::detectUnknownBytesStaysUnknown()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString filler = writeBlob(dir.filePath(QStringLiteral("filler.bin")),
                                     QByteArray(0x2000, '\x5A'));
    const QString text = writeBlob(dir.filePath(QStringLiteral("note.txt")),
                                   QByteArray("hello, not a firmware image"));
    QVERIFY(!filler.isEmpty() && !text.isEmpty());

    ImageWorker worker;
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(worker, filler, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::Unknown);
    QVERIFY2(detectFile(worker, text, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::Unknown);
}

// 前提断言（"为什么顺序敏感"钉在测试里；完整版见 test_registry.cpp 的
// oppoTailProbeOrderPremise）：同一份 OPS 尾页若按 OFP 判据看同样成立。
void TestImageWorker::opsTailAlsoMatchesOfpQcJudge()
{
    QByteArray opsTail(0x200, '\0');
    const auto put32 = [&opsTail](qsizetype off, quint32 v) {
        for (int i = 0; i < 4; ++i)
            opsTail[off + i] = char((v >> (i * 8)) & 0xFF);
    };
    put32(0x00, 2);
    put32(0x04, 1);
    put32(0x10, 0x7CEF);
    QVERIFY(imgopp::detectOPS(opsTail, 0x2000));
    imgopp::OfpVariant variant = imgopp::OfpVariant::Unknown;
    QVERIFY(imgopp::detectOFP(QByteArray(16, '\0'), opsTail, 0x2000, variant));
    QCOMPARE(variant, imgopp::OfpVariant::Qc); // ← 探测顺序若反，OPS 包会走到这里
}

// ==================== 2. 解包分派 ====================

// OFP-QC 包解包：产物清单来自引擎回调（worker 用输出目录快照差集统计），
// 进度口径 = 中途封顶 99、收尾恰好 100。
void TestImageWorker::unpackOfpWritesOutputs()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Firmware"), QStringLiteral("boot.img"),
          QByteArray(0x1200, 'B'), 0},
         {QStringLiteral("Sahara"), QStringLiteral("xbl.img"),
          QByteArray(0x2000, 'S'), 0}});
    QVERIFY(pkg.isValid());
    const QString path = writeBlob(dir.filePath(QStringLiteral("pkg.ofp")), pkg.blob);
    QVERIFY(!path.isEmpty());

    ImageWorker worker;
    imgreg::Detected detected;
    QString why;
    QVERIFY2(detectFile(worker, path, &detected, &why), qPrintable(why));
    QCOMPARE(detected.format, imgreg::Format::OFP);

    const QString outDir = dir.filePath(QStringLiteral("out"));
    const UnpackOutcome outcome = unpackFile(worker, path, detected, outDir);
    QVERIFY2(outcome.delivered, "unpackFinished 超时（60s）");
    QVERIFY2(outcome.ok, qPrintable(outcome.error));
    QVERIFY(outcome.error.isEmpty()); // 完整成功：无警告文案
    QCOMPARE(outcome.outputs.size(), 2);
    QVERIFY(QFileInfo::exists(QDir(outDir).filePath(QStringLiteral("boot.img"))));
    QVERIFY(QFileInfo::exists(QDir(outDir).filePath(QStringLiteral("xbl.img"))));
    // 进度口径：除收尾的 100 外，中途值一律 ≤ 99（面板在 100 时隐藏进度条）
    QVERIFY(!outcome.percents.isEmpty());
    QCOMPARE(outcome.percents.last(), 100);
    for (int i = 0; i + 1 < outcome.percents.size(); ++i)
        QVERIFY(outcome.percents.at(i) <= 99);
}

// A10：条目名不安全 → 跳过该条目 + 中文 *error + 其余继续（引擎返回 true 且
// *error 非空）→ worker 必须把警告经 unpackFinished 回传（ok 仍为 true），
// 面板据此落日志。丢弃即"静默部分解包"。
void TestImageWorker::unpackOfpPartialSuccessReportsWarning()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QByteArray legit = QByteArray(0x180, 'L');
    const ofptest::QcPackage pkg = ofptest::buildQcPackage(
        {{QStringLiteral("Firmware"), QStringLiteral("../evil.img"),
          QByteArray(0x40, 'E'), 0},
         {QStringLiteral("Firmware"), QStringLiteral("boot.img"), legit, 0}});
    QVERIFY(pkg.isValid());
    const QString path = writeBlob(dir.filePath(QStringLiteral("unsafe.ofp")), pkg.blob);
    QVERIFY(!path.isEmpty());

    ImageWorker worker;
    imgreg::Detected detected;
    QString why;
    QVERIFY2(detectFile(worker, path, &detected, &why), qPrintable(why));

    const QString outDir = dir.filePath(QStringLiteral("out"));
    const UnpackOutcome outcome = unpackFile(worker, path, detected, outDir);
    QVERIFY2(outcome.delivered, "unpackFinished 超时（60s）");
    QVERIFY(outcome.ok); // 部分成功不得被误报成致命失败
    QVERIFY2(outcome.error.contains(QStringLiteral("文件名")), qPrintable(outcome.error));
    QCOMPARE(outcome.outputs.size(), 1); // 只统计合法产物
    QCOMPARE(outcome.outputs.first(), QDir(outDir).filePath(QStringLiteral("boot.img")));
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral("evil.img")))); // 未逃出输出目录
}

// 注: 必须用 QTEST_MAIN（而非本仓其它测试的 QTEST_APPLESS_MAIN）—— 本文件经
// QSignalSpy::wait() 驱动事件循环等 worker 的 queued 结果，无 QCoreApplication
// 时 QEventLoop 无法使用（本目标未链 Qt6 Gui/Widgets → QTEST_MAIN 建 QCoreApplication）。
// 失败路径 (1)：扩展名兜底判成 OFP，但包体不是 OFP（尾页探测不命中）→ 解包必须
// 以 ok=false + 引擎的中文错误上抛（面板显示"解包失败: <reason>"），不得静默产空目录。
void TestImageWorker::unpackMisnamedOfpReportsEngineError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeBlob(dir.filePath(QStringLiteral("garbage.ofp")),
                                   QByteArray(0x2000, '\x5A'));
    QVERIFY(!path.isEmpty());

    ImageWorker worker;
    imgreg::Detected detected;
    QString why;
    QVERIFY2(detectFile(worker, path, &detected, &why), qPrintable(why));
    // 尾页探测不命中 → 仅扩展名给出候选（这是"误命名/损坏包"的典型识别结果）
    QCOMPARE(detected.format, imgreg::Format::OFP);
    QCOMPARE(detected.detail, QStringLiteral("按扩展名识别"));

    const QString outDir = dir.filePath(QStringLiteral("out"));
    const UnpackOutcome outcome = unpackFile(worker, path, detected, outDir);
    QVERIFY2(outcome.delivered, "unpackFinished 超时（60s）");
    QVERIFY(!outcome.ok);
    QVERIFY(!outcome.error.isEmpty()); // 引擎中文文案原样上抛（§5：不得空 error）
    // 断言引擎原文（"不是有效的 OFP 包…"）：只查子串 "OFP" 无区分力 —— worker
    // 兜底文案 "OFP 解包失败" 也含 OFP，引擎错误被吞掉时用例仍会绿
    QVERIFY2(outcome.error.contains(QStringLiteral("不是有效的 OFP 包")),
             qPrintable(outcome.error));
    QVERIFY(outcome.outputs.isEmpty()); // 失败即无产物清单
}

// 失败路径 (2)：路径不存在（面板拖入后文件被删/移动等防御路径）→ 不崩溃、不空 error。
// 不走 runDetect（识别本身也会失败），直接投喂伪造的 Detected 走 doUnpack 分派。
void TestImageWorker::unpackMissingFileReportsError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    imgreg::Detected forged;
    forged.format = imgreg::Format::OFP;

    ImageWorker worker;
    const UnpackOutcome outcome = unpackFile(
        worker, dir.filePath(QStringLiteral("nope.ofp")), forged,
        dir.filePath(QStringLiteral("out")));
    QVERIFY2(outcome.delivered, "unpackFinished 超时（60s）");
    QVERIFY(!outcome.ok);
    QVERIFY(!outcome.error.isEmpty());
    QVERIFY2(outcome.error.contains(QStringLiteral("无法打开")), qPrintable(outcome.error));
    QVERIFY(outcome.outputs.isEmpty());
}

QTEST_MAIN(TestImageWorker)
#include "test_image_worker.moc"
