#include <QtTest>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <memory>

#include "core/resource_monitor.h"
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
// 夹具（合成包）来自 oppo_test_helpers.h + 本文件内的 opsPackage()：按格式事实拼字节流。
// 注: opsPackage() 并非"完全不调用被测代码"—— 它用被测模块的密码 helper
// （imgopp::opsEncrypt / opsKeyCandidates）产出 settings.xml 密文，以便 parseOPS
// 能真正解出尾页字段。密码实现的正确性由 test_oppo_ops::opsCipherAgainstPython
// （对拍参照实产密文向量）兜底；本文件的 detail 断言只依赖尾页字段与清单条目数，
// 与密码实现是否正确无关（密码错了这些用例只会走 parse 失败降级路径）。
// 线程模型：ImageWorker 在工作线程执行，结果经 queued 信号投递 → 必须有
// application 对象 —— 本文件用 QTEST_MAIN 建立 QCoreApplication
// （QTEST_APPLESS_MAIN 不建 application 对象，切回去等待会直接挂死；见文件末尾的
// 宏选择说明）。等待一律走 waitForEmission()（不要直接 QSignalSpy::wait()）。
// 负载稳健性（两道防线，见各自注释）：
//   1. waitForEmission()：按"计数"判定而非 wait() 的返回值 —— 这是负载下 flake 的
//      真正根因（高负载时 worker 可能先于主线程进入 wait 完成，wait() 会白等满整个
//      超时返回 false，spy 里其实已有结果）；
//   2. makeWorker()：切断 H1 优先级降级连接（防御性；本进程内监控未启动）。
// 等待余量 60s 仅约束"真挂死"场景，与 flake 无关（改前 10s、改后 60s 都会失败）。
class TestImageWorker : public QObject
{
    Q_OBJECT
private slots:
    void detectOpsPackageByTail();
    void detectOpsDetailCarriesTailFields();
    void detectOpsTailWithUnknownExtension();
    void detectQcPackage();
    void detectMtkPackage();
    void detectMtkDetailUtf8FieldSemantics();
    void detectTinyFileSkipsTailProbe();
    void detectUnknownBytesStaysUnknown();
    void detectGpt4096LbaLayout();
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

// 合成 OPS 包（可被 parseOPS 完整解析的最小形态）：settings.xml 密文 + 尾页 0x200，
// 尾页 +0x14 记 settings 扇区 / +0x18 记清单明文长度 / +0x1C/+0x2C 记 project id /
// firmware 名 —— 字段偏移与 test_oppo_ops.cpp 的 buildOpsPackage 及 oppo_ops.cpp 常量
// 同源（该文件的夹具另含下标越界等恶意形态，本文件只需要"能解出字段"的最小包）。
//   withEntry = true  → 清单含 1 条 Program 条目（boot.img@0，0x200B），settings 挪到扇区 1
//   withEntry = false → 清单仅 BasicInfo（条目数 0，验证"0 不拼"路径）
// 注: 包长取 0x1000（doDetect 只在 fileSize ≥ 0x1000 时才读尾页做二次探测）。
QByteArray opsPackage(const QString &projectId, const QString &firmwareName,
                      bool withEntry = true)
{
    const QByteArray mboxBlob = imgopp::opsKeyCandidates().at(0).mboxBlob; // mbox5
    QString xml = QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                                 "<ProFile>\n  <BasicInfo Project=\"%1\" Version=\"%2\"/>\n")
                      .arg(projectId, firmwareName);
    if (withEntry) {
        // 条目数据区在扇区 0（内容无关紧要：parseOPS 只按声明长度做越界校验）
        xml += QStringLiteral("  <Program>\n    <program filename=\"boot.img\""
                              " FileOffsetInSrc=\"0\" SizeInByteInSrc=\"512\"/>\n  </Program>\n");
    }
    xml += QStringLiteral("</ProFile>\n");
    const QByteArray xmlBytes = xml.toUtf8();
    // 参照的补齐式 (0x10 - len%0x10) 在已对齐时也补一整块（同 test_oppo_ops.cpp）
    const QByteArray padded = xmlBytes + QByteArray(0x10 - (xmlBytes.size() % 0x10), '\0');
    const QByteArray cipher = imgopp::opsEncrypt(padded, mboxBlob);

    const qsizetype settingsOff = withEntry ? 0x200 : 0; // 条目数据区（扇区 0）之后
    QByteArray blob(0x1000, '\0');
    blob.replace(settingsOff, cipher.size(), cipher);
    const qsizetype tailBase = blob.size() - 0x200;
    ofptest::putLE32(blob, tailBase + 0x00, 2);              // version（A11 判据）
    ofptest::putLE32(blob, tailBase + 0x04, 1);              // flags（A11 判据）
    ofptest::putLE32(blob, tailBase + 0x10, 0x7CEF);         // 魔数
    ofptest::putLE32(blob, tailBase + 0x14, quint32(settingsOff / 0x200)); // settings 扇区
    ofptest::putLE32(blob, tailBase + 0x18, quint32(xmlBytes.size()));      // 清单明文长度
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

// 构造 ImageWorker 并切断 H1 优先级降级对本测试的影响（全用例统一入口）。
// 返回 unique_ptr：QObject 不可拷贝，且工厂集中保证"构造即切断"，避免今后新增用例
// 忘了这一步而再次引入负载相关 flake。
//
// 被切断的链路（产品行为，本文件不改产品代码）：CPU >80% → ResourceMonitor::cpuHigh
// → ImageWorker 构造函数里的 DirectConnection lambda 把工作线程降为 IdlePriority
// （SCHED_IDLE；H1 的既有设计取舍）。本文件断言的是探测顺序/解包语义，与优先级策略
// 无关，故断开该连接（注：本次 flake 的根因已验证为 wait() 语义，见 waitForEmission()，
// 不是优先级 —— 线程探针实测卡住时工作线程为 SCHED_NORMAL 且在睡眠）。
//
// 事实核查（2026-09-13）：本测试目标只链 image_worker.cpp + resource_monitor.cpp
// （CMakeLists.txt 的 test_image_worker 分支），而 ResourceMonitor::start() 的唯一
// 调用点在 MainWindow（main_window.cpp）→ 本进程内采样 QTimer 从未启动、cpuHigh
// 从未发射，故上述 lambda（全仓唯一调用 setPriority 处）实际不会执行。本行因此是
// 防御性接线：将来若测试里启动了监控（或 start 时机变化），用例仍不受优先级影响。
// disconnect 返回 false 会打印告警（连接本就存在，返回 false 说明 Qt 语义/构造变了）。
std::unique_ptr<ImageWorker> makeWorker()
{
    auto worker = std::make_unique<ImageWorker>();
    if (!QObject::disconnect(&ResourceMonitor::instance(), nullptr, worker.get(), nullptr))
        qWarning("test_image_worker: H1 优先级降级连接未按预期断开 —— 出现负载相关等待"
                 "超时时先查此处");
    return worker;
}

// 等待 QSignalSpy 收到至少一次发射（限 timeoutMs）。**必须用本函数，不要直接
// spy.wait()**：QSignalSpy 的槽是 DirectConnection（Qt 在跨线程发射时于发射线程内
// 直接记录，内部带 mutex），而 QSignalSpy::wait() 只认"等待期间新增的发射"
// （Qt 源码：进入前快照 origCount，返回 size() > origCount）。
// → 若 worker 在主线程调用 wait() **之前**就完成并发射（负载高时主线程被抢占即会
//   发生），信号其实已经记录在案，但 wait() 会白等满整个超时并返回 false。
// 本函数先查计数、超时未到就继续等，把"早到"与"迟到"都当成功（实测：这正是
// loadavg 25 下 detectFinished 整窗超时（且 spy.count()==1）、而改前 10s/改后 60s
// 都一样失败的根因；超时值再大也救不了"信号早于 wait"）。
bool waitForEmission(QSignalSpy &spy, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (spy.count() == 0) {
        const qint64 left = qint64(timeoutMs) - clock.elapsed();
        if (left <= 0)
            return false;
        spy.wait(int(left));   // 返回值不参与判定：循环条件已同时覆盖"早到/迟到"
    }
    return true;
}

// 驱动一次 runDetect；超时（含工作线程异常）返回 false 并填 why，由调用方 QVERIFY2。
// 等待余量 60s：正常路径为毫秒级，该值是"真挂死"与"仅投递时序异常"的分界。
bool detectFile(ImageWorker &worker, const QString &path, imgreg::Detected *out,
                QString *why)
{
    QSignalSpy spy(&worker, &ImageWorker::detectFinished);
    worker.runDetect(path);
    if (!waitForEmission(spy, 60000)) {
        *why = QStringLiteral("detectFinished 超时（60s）");
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
    // 同 detectFile：必须用 waitForEmission（spy.wait() 会漏掉"早于 wait 的发射"）
    outcome.delivered = waitForEmission(unpackSpy, 60000); // 超时（含工作线程异常）→ 空结果
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

    auto worker = makeWorker();
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(*worker, path, &r, &why), qPrintable(why));
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

    auto worker = makeWorker();
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(*worker, path, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::OPS);
    // spec §4「分区数」按条目数收口（措辞「条目」：Program/UFS_PROVISION 等组未必是分区）
    QCOMPARE(r.detail,
             QStringLiteral("OnePlus OPS 固件包 · 18801 · guacamoles_31_O.09_190820 · 1 个条目"));

    // 条目数 0（清单只有 BasicInfo）：不得拼出"0 个条目"（误导信息），尾页字段照常拼
    const QString noEntry = writeBlob(dir.filePath(QStringLiteral("noentry.ops")),
                                      opsPackage(QStringLiteral("18801"),
                                                 QStringLiteral("guacamoles_31_O.09_190820"),
                                                 false));
    QVERIFY(!noEntry.isEmpty());
    QVERIFY2(detectFile(*worker, noEntry, &r, &why), qPrintable(why));
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

    auto worker = makeWorker();
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(*worker, path, &r, &why), qPrintable(why));
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

    auto worker = makeWorker();
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(*worker, path, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::OFP);
    // QC 无 project/version 字段，但清单文件表有条目 → detail 尾部带条目数
    QCOMPARE(r.detail, QStringLiteral("OPPO/realme OFP 固件包 (QC) · 2 个条目"));
}

// MTK 合成包（首 16B 试解出 "MMM"）→ OFP (MTK)。注：包体须 ≥ 0x1000 才会走尾页
// 探测（doDetect 对小文件跳过尾页读取），故载荷取 0x2000。
// spec §4 信息卡：detail 须带尾头项目名（prjname）/版本（flashtype）/条目数 —— 前两者
// 为夹具显式给定（非默认值）、后者用两条目（数量非 1，断言来自真实计数），确认其
// 确实来自解析结果。
void TestImageWorker::detectMtkPackage()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    ofptest::MtkBuildOptions opts;
    opts.prjname = QStringLiteral("CPH1827");
    const ofptest::MtkPackage pkg = ofptest::buildMtkPackage(
        {{QStringLiteral("boot"), QStringLiteral("boot.img"), QByteArray(0x2000, 'M')},
         {QStringLiteral("system"), QStringLiteral("system.img"), QByteArray(0x2000, 'S')}},
        opts);
    QVERIFY(pkg.isValid());
    const QString path = writeBlob(dir.filePath(QStringLiteral("mtk.ofp")), pkg.blob);
    QVERIFY(!path.isEmpty());

    auto worker = makeWorker();
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(*worker, path, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::OFP);
    QCOMPARE(r.detail,
             QStringLiteral("OPPO/realme OFP 固件包 (MTK) · CPH1827 · UFS · 2 个条目"));
}

// cleanCString 的 UTF-8 口径回归钉（Task 终审复审）：此前全仓夹具只造 ASCII 字段名，
// 该改动零覆盖。两条子用例：
//   (1) 非 ASCII prjname（"测试项目" UTF-8 共 12 字节）→ detail 须原样显示；按 Latin-1
//       解码会得到 "æµ‹è¯•é¡¹ç›®" 一类乱码 → 本断言必红；
//   (2) 字段中部含 0x00（"AB\0CD"）→ 参照 replace(b"\x00", b"").decode('utf-8') 去掉
//       全部 NUL，故 detail = "ABCD"；旧的"首个 NUL 截断 + Latin-1"实现只会给出 "AB"
//       → 本断言钉住"内部 NUL 之后内容不再丢弃"的语义变化。
void TestImageWorker::detectMtkDetailUtf8FieldSemantics()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ofptest::MtkBuildOptions utf8Opts;
    utf8Opts.prjname = QStringLiteral("测试项目");
    const ofptest::MtkPackage utf8Pkg = ofptest::buildMtkPackage(
        {{QStringLiteral("boot"), QStringLiteral("boot.img"), QByteArray(0x2000, 'M')}}, utf8Opts);
    QVERIFY(utf8Pkg.isValid());
    const QString utf8Path = writeBlob(dir.filePath(QStringLiteral("utf8.ofp")), utf8Pkg.blob);
    QVERIFY(!utf8Path.isEmpty());

    auto worker = makeWorker();
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(*worker, utf8Path, &r, &why), qPrintable(why));
    QCOMPARE(r.detail,
             QStringLiteral("OPPO/realme OFP 固件包 (MTK) · 测试项目 · UFS · 1 个条目"));

    // 内部 NUL：显式长度构造（保留嵌入的 U+0000），夹具按 UTF-8 写出 5 字节 "AB\0CD"
    ofptest::MtkBuildOptions nulOpts;
    nulOpts.prjname = QString::fromLatin1("AB\0CD", 5);
    const ofptest::MtkPackage nulPkg = ofptest::buildMtkPackage(
        {{QStringLiteral("boot"), QStringLiteral("boot.img"), QByteArray(0x2000, 'M')}}, nulOpts);
    QVERIFY(nulPkg.isValid());
    const QString nulPath = writeBlob(dir.filePath(QStringLiteral("nul.ofp")), nulPkg.blob);
    QVERIFY(!nulPath.isEmpty());
    QVERIFY2(detectFile(*worker, nulPath, &r, &why), qPrintable(why));
    QCOMPARE(r.detail, QStringLiteral("OPPO/realme OFP 固件包 (MTK) · ABCD · UFS · 1 个条目"));
}

// 不足一页（0x200B）：读不到完整尾页 → 跳过尾页探测，仅扩展名兜底（不崩不误判）
void TestImageWorker::detectTinyFileSkipsTailProbe()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeBlob(dir.filePath(QStringLiteral("tiny.ops")),
                                   QByteArray(0x200, '\0'));
    QVERIFY(!path.isEmpty());

    auto worker = makeWorker();
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(*worker, path, &r, &why), qPrintable(why));
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

    auto worker = makeWorker();
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(*worker, filler, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::Unknown);
    QVERIFY2(detectFile(*worker, text, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::Unknown);
}

// ==================== 1b. GPT 双 LBA 布局的探测缓冲（②-b） ====================

// 4096 字节 LBA 的整盘 GPT：头签名 `EFI PART` 在 0x1000（UFS/真包形态）。doDetect 的
// 探测缓冲**必须 ≥ 0x1008 字节**才看得到它 —— 否则 registry 只按 0x200 判、落到扩展名
// 兜底（.img → RawImage），UI 的 DiskGpt 分支永远进不去（改前 buffer=4096 时正是如此：
// 症状是"已识别: RawImage"而非"GPT 解析失败"）。
// 夹具尺寸 0x1200 = 4608 是**刻意**的：> 4096（改前缓冲）且 < 8192（改后缓冲）⇒
// 缓冲若退回 4096，本用例必红（这就是它的判别力所在）。
// 内容只需签名：探测不解析 GPT（解析器另由 tests/test_disk.cpp 的 4096 用例守护，
// 对账另由 test_flash_plan 的两条用例守护），保护 MBR 写上只为形态真实。
void TestImageWorker::detectGpt4096LbaLayout()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QByteArray disk4096(0x1200, 0);
    disk4096[446 + 4] = char(0xEE);
    disk4096[510] = char(0x55); disk4096[511] = char(0xAA);
    disk4096.replace(0x1000, 8, "EFI PART");
    const QString path4096 = writeBlob(dir.filePath(QStringLiteral("disk4096.img")), disk4096);
    QVERIFY(!path4096.isEmpty());

    auto worker = makeWorker();
    imgreg::Detected r;
    QString why;
    QVERIFY2(detectFile(*worker, path4096, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::DiskGpt);
    QCOMPARE(r.detail, QStringLiteral("GPT 磁盘镜像"));

    // 对照：512 字节 LBA（签名在 0x200）= 既有行为，不被拓宽影响
    QByteArray disk512(1024, 0);
    disk512[446 + 4] = char(0xEE);
    disk512[510] = char(0x55); disk512[511] = char(0xAA);
    disk512.replace(512, 8, "EFI PART");
    const QString path512 = writeBlob(dir.filePath(QStringLiteral("disk512.img")), disk512);
    QVERIFY(!path512.isEmpty());
    QVERIFY2(detectFile(*worker, path512, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::DiskGpt);

    // 负向对照：同样尺寸但签名不在两处 → 仍是扩展名兜底（.img → RawImage），不误报成 GPT
    QByteArray notGpt(0x1200, '\x5A');
    const QString pathNot = writeBlob(dir.filePath(QStringLiteral("plain.img")), notGpt);
    QVERIFY(!pathNot.isEmpty());
    QVERIFY2(detectFile(*worker, pathNot, &r, &why), qPrintable(why));
    QCOMPARE(r.format, imgreg::Format::RawImage);
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

    auto worker = makeWorker();
    imgreg::Detected detected;
    QString why;
    QVERIFY2(detectFile(*worker, path, &detected, &why), qPrintable(why));
    QCOMPARE(detected.format, imgreg::Format::OFP);

    const QString outDir = dir.filePath(QStringLiteral("out"));
    const UnpackOutcome outcome = unpackFile(*worker, path, detected, outDir);
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

    auto worker = makeWorker();
    imgreg::Detected detected;
    QString why;
    QVERIFY2(detectFile(*worker, path, &detected, &why), qPrintable(why));

    const QString outDir = dir.filePath(QStringLiteral("out"));
    const UnpackOutcome outcome = unpackFile(*worker, path, detected, outDir);
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

    auto worker = makeWorker();
    imgreg::Detected detected;
    QString why;
    QVERIFY2(detectFile(*worker, path, &detected, &why), qPrintable(why));
    // 尾页探测不命中 → 仅扩展名给出候选（这是"误命名/损坏包"的典型识别结果）
    QCOMPARE(detected.format, imgreg::Format::OFP);
    QCOMPARE(detected.detail, QStringLiteral("按扩展名识别"));

    const QString outDir = dir.filePath(QStringLiteral("out"));
    const UnpackOutcome outcome = unpackFile(*worker, path, detected, outDir);
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

    auto worker = makeWorker();
    const UnpackOutcome outcome = unpackFile(
        *worker, dir.filePath(QStringLiteral("nope.ofp")), forged,
        dir.filePath(QStringLiteral("out")));
    QVERIFY2(outcome.delivered, "unpackFinished 超时（60s）");
    QVERIFY(!outcome.ok);
    QVERIFY(!outcome.error.isEmpty());
    QVERIFY2(outcome.error.contains(QStringLiteral("无法打开")), qPrintable(outcome.error));
    QVERIFY(outcome.outputs.isEmpty());
}

QTEST_MAIN(TestImageWorker)
#include "test_image_worker.moc"
