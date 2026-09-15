// tests/test_mtk_preloader_fetch.cpp
//
// preloader 两路径解析（Phase D1 Task 8）：① 显式/固件目录自动导入 ② 网络获取（**默认关闭**）。
// 全用例只用 QTemporaryDir 合成输入（无真样本、无网络）：下载器一律是**注入的 lambda**，
// 且用调用计数钉住"该调几次"——**没有任何用例会真的联网**；生产下载器
// （src/core/modes/mtk_preloader_download_qt.cpp，Qt Network）本测试目标**不编译**。
//
// 语义（spec §7，四条易错点各有用例）：
//   · 缺 preloader 永不返回 false（走 skipReason，不中止刷写）；
//   · allowNetwork == false 时下载器一次都不许被调用；
//   · fail-closed：来源缺 64 位 sha256 → 不下载；下载后 sha256 不符 → 拒绝且**不落盘**；
//   · 自动导入**唯一命中才用**，多个候选 → 不猜，列出候选。
#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "core/modes/mtk_preloader_fetch.h"

using mtkbrom::PreloaderOrigin;
using mtkbrom::PreloaderOptions;
using mtkbrom::PreloaderResult;
using mtkbrom::PreloaderSource;

namespace {
QString writeFile(const QDir &dir, const QString &name, const QByteArray &data)
{
    const QString path = dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    f.write(data);
    f.close();
    return path;
}
QString sha256Of(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
} // namespace

class TestMtkPreloaderFetch : public QObject
{
    Q_OBJECT
private slots:
    // XDG_CONFIG_HOME 是"配置路径重定向"用例组唯一的开关：进来先存原值，**每个用例之后**恢复
    // （cleanup 而非 cleanupTestCase —— 任一用例中途 QVERIFY 早退也不会把 env 漏给下一个用例）。
    void initTestCase()
    {
        m_xdgWasSet = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
        m_savedXdg = qgetenv("XDG_CONFIG_HOME");
    }
    void cleanup()
    {
        if (m_xdgWasSet)
            qputenv("XDG_CONFIG_HOME", m_savedXdg);
        else
            qunsetenv("XDG_CONFIG_HOME");
    }

    // ① 显式路径优先于自动导入
    void explicitPathWins()
    {
        QTemporaryDir tmp; QDir d(tmp.path());
        const QString autoHit = writeFile(d, QStringLiteral("preloader_auto.bin"), QByteArray("AUTO"));
        const QString explicitPath = writeFile(d, QStringLiteral("my_preloader.bin"), QByteArray("EXPLICIT"));
        QVERIFY(!autoHit.isEmpty() && !explicitPath.isEmpty());

        PreloaderOptions opt;
        opt.explicitPath = explicitPath;
        opt.firmwareDirs << tmp.path();
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {}, {}, res, &err), qPrintable(err));
        QCOMPARE(res.origin, PreloaderOrigin::Explicit);
        QCOMPARE(res.path, explicitPath);
        QCOMPARE(res.bytes, QByteArray("EXPLICIT"));
    }

    // 显式路径读不到 = 响亮失败（用户以为会做 DRAM 初始化）
    void explicitPathUnreadableFailsLoudly()
    {
        PreloaderOptions opt;
        opt.explicitPath = QStringLiteral("/nonexistent/preloader.bin");
        PreloaderResult res; QString err;
        QVERIFY(!mtkbrom::resolvePreloader(opt, {}, {}, res, &err));
        QVERIFY(!err.isEmpty());
    }

    // 显式路径的**另一半**：文件在但为空 —— 同样响亮失败（"读不到/为空"都算用户给的东西不可用）
    void explicitPathEmptyFileFailsLoudly()
    {
        QTemporaryDir tmp; QDir d(tmp.path());
        const QString emptyPath = writeFile(d, QStringLiteral("empty_preloader.bin"), QByteArray());
        QVERIFY(!emptyPath.isEmpty());

        PreloaderOptions opt;
        opt.explicitPath = emptyPath;
        PreloaderResult res; QString err;
        QVERIFY(!mtkbrom::resolvePreloader(opt, {}, {}, res, &err));
        QVERIFY(!err.isEmpty());
    }

    // ① 自动导入：目录里唯一命中即用
    void autoImportUsesSingleCandidate()
    {
        QTemporaryDir tmp; QDir d(tmp.path());
        QVERIFY(!writeFile(d, QStringLiteral("preloader_k65v1_64_bsp.bin"), QByteArray("PRE")).isEmpty());
        QVERIFY(!writeFile(d, QStringLiteral("boot.img"), QByteArray("BOOT")).isEmpty());
        PreloaderOptions opt;
        opt.firmwareDirs << tmp.path();
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {}, {}, res, &err), qPrintable(err));
        QCOMPARE(res.origin, PreloaderOrigin::AutoImport);
        QVERIFY(res.path.endsWith(QStringLiteral("preloader_k65v1_64_bsp.bin")));
        QCOMPARE(res.bytes, QByteArray("PRE"));
    }

    // 语义 #1 的另一半：自动导入**唯一命中但读不到/为空** → 与"没找到"同姿态（不中止），
    // 但必须点名是哪个文件、为什么不可用（brief 的样例代码此处 return false —— 见报告 §偏差）
    void autoImportUnreadableCandidateSkipsInsteadOfFailing()
    {
        QTemporaryDir tmp; QDir d(tmp.path());
        QVERIFY(!writeFile(d, QStringLiteral("preloader_empty.bin"), QByteArray()).isEmpty());

        PreloaderOptions opt;                      // allowNetwork 默认 false
        opt.firmwareDirs << tmp.path();
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {}, {}, res, &err), qPrintable(err));
        QCOMPARE(res.origin, PreloaderOrigin::None);
        QVERIFY(res.bytes.isEmpty());

        const QString all = res.log.join(QLatin1Char('\n')) + QLatin1Char('\n') + res.skipReason;
        QVERIFY2(all.contains(QStringLiteral("preloader_empty.bin")), qPrintable(all));
        QVERIFY2(!res.skipReason.isEmpty(), qPrintable(all));
    }

    // 多个候选：不猜 —— 跳过 + 把候选列进日志（**不返回 false**）
    void multipleCandidatesAreReportedNotGuessed()
    {
        QTemporaryDir tmp; QDir d(tmp.path());
        QVERIFY(!writeFile(d, QStringLiteral("preloader_a.bin"), QByteArray("A")).isEmpty());
        QVERIFY(!writeFile(d, QStringLiteral("preloader_b.bin"), QByteArray("B")).isEmpty());
        PreloaderOptions opt;
        opt.firmwareDirs << tmp.path();
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {}, {}, res, &err), qPrintable(err));
        QCOMPARE(res.origin, PreloaderOrigin::None);
        QVERIFY2(res.skipReason.contains(QStringLiteral("2")), qPrintable(res.skipReason));
        const QString all = res.log.join('\n');
        QVERIFY2(all.contains(QStringLiteral("preloader_a.bin")) && all.contains(QStringLiteral("preloader_b.bin")),
                 qPrintable(all));
    }

    // 没有任何 preloader 且网络关闭 → 跳过 DRAM 初始化但**不中止**
    void noPreloaderSkipsWithoutFailing()
    {
        PreloaderOptions opt;
        opt.allowNetwork = false;
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {}, {}, res, &err), qPrintable(err));
        QCOMPARE(res.origin, PreloaderOrigin::None);
        QVERIFY(!res.skipReason.isEmpty());
        QVERIFY(res.bytes.isEmpty());
    }

    // 网络默认关闭：即使有来源清单，下载器一次都不许被调
    void networkOffNeverCallsDownloader()
    {
        int calls = 0;
        PreloaderSource src{QStringLiteral("t"), QStringLiteral("https://example.invalid/p.bin"),
                            QString(64, QLatin1Char('a'))};
        PreloaderOptions opt;                    // allowNetwork 默认 false
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {src},
                                           [&calls](const PreloaderSource &, QByteArray *, QString *) {
                                               ++calls; return true;
                                           }, res, &err), qPrintable(err));
        QCOMPARE(calls, 0);
        QCOMPARE(res.origin, PreloaderOrigin::None);
    }

    // ② 显式开启 + sha256 匹配 → 采用并落盘 + 日志写清来源
    void networkVerifiesSha256AndCaches()
    {
        const QByteArray payload("NETPRELOADER");
        PreloaderSource src{QStringLiteral("t"), QStringLiteral("https://example.invalid/p.bin"), sha256Of(payload)};
        QTemporaryDir tmp;
        PreloaderOptions opt;
        opt.allowNetwork = true;
        opt.cacheDir = tmp.path();
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {src},
                                           [&payload](const PreloaderSource &, QByteArray *out, QString *) {
                                               *out = payload; return true;
                                           }, res, &err), qPrintable(err));
        QCOMPARE(res.origin, PreloaderOrigin::Network);
        QCOMPARE(res.bytes, payload);
        QVERIFY(!res.path.isEmpty());
        QFile cached(res.path);
        QVERIFY(cached.exists() && cached.open(QIODevice::ReadOnly));
        QCOMPARE(cached.readAll(), payload);
        QVERIFY2(res.log.join('\n').contains(QStringLiteral("https://example.invalid/p.bin")),
                 qPrintable(res.log.join('\n')));
    }

    // ② sha256 不匹配 → 拒绝使用（不落盘）
    void networkRejectsHashMismatch()
    {
        const QByteArray payload("TAMPERED");
        PreloaderSource src{QStringLiteral("t"), QStringLiteral("https://example.invalid/p.bin"),
                            QString(64, QLatin1Char('0'))};
        QTemporaryDir tmp;
        PreloaderOptions opt;
        opt.allowNetwork = true;
        opt.cacheDir = tmp.path();
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {src},
                                           [&payload](const PreloaderSource &, QByteArray *out, QString *) {
                                               *out = payload; return true;
                                           }, res, &err), qPrintable(err));
        QCOMPARE(res.origin, PreloaderOrigin::None);
        // sha256 细节在 log（逐条失败原因）；skipReason 是汇总口径 —— 两者合起来必须能看到"为什么拒绝"
        const QString all = res.log.join('\n') + QLatin1Char('\n') + res.skipReason;
        QVERIFY2(all.contains(QStringLiteral("sha256")), qPrintable(all));
        QCOMPARE(QDir(tmp.path()).entryList(QDir::Files).size(), 0);   // 未落盘
    }

    // ② 第一条被校验拒绝 → **继续试下一条**（拒绝的是来源，不是整次获取）
    void networkFallsBackToSecondSource()
    {
        const QByteArray payload("SECOND");
        const PreloaderSource bad{QStringLiteral("bad"), QStringLiteral("https://example.invalid/bad.bin"),
                                  QString(64, QLatin1Char('0'))};
        const PreloaderSource good{QStringLiteral("good"), QStringLiteral("https://example.invalid/good.bin"),
                                   sha256Of(payload)};
        int calls = 0;
        QTemporaryDir tmp;
        PreloaderOptions opt;
        opt.allowNetwork = true;
        opt.cacheDir = tmp.path();
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {bad, good},
                                           [&calls, &payload](const PreloaderSource &, QByteArray *out, QString *) {
                                               ++calls; *out = payload; return true;
                                           }, res, &err), qPrintable(err));
        QCOMPARE(calls, 2);                       // 两条都试过（第一条被 sha256 校验拒绝）
        QCOMPARE(res.origin, PreloaderOrigin::Network);
        QCOMPARE(res.bytes, payload);
    }

    // ② cacheDir 留空 = 只留在内存（合法的 opt；不因"没法缓存"而失败）
    void networkWithoutCacheDirKeepsBytesInMemory()
    {
        const QByteArray payload("NOCACHE");
        PreloaderSource src{QStringLiteral("t"), QStringLiteral("https://example.invalid/p.bin"), sha256Of(payload)};
        PreloaderOptions opt;
        opt.allowNetwork = true;                  // cacheDir 留空
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {src},
                                           [&payload](const PreloaderSource &, QByteArray *out, QString *) {
                                               *out = payload; return true;
                                           }, res, &err), qPrintable(err));
        QCOMPARE(res.origin, PreloaderOrigin::Network);
        QCOMPARE(res.bytes, payload);
        QVERIFY(res.path.isEmpty());
    }

    // ② 缓存目录建不出来：**已校验通过的字节仍可用**（缓存只是便利，不中止、不丢数据）
    void cacheWriteFailureKeepsVerifiedBytes()
    {
        const QByteArray payload("CACHEFAIL");
        PreloaderSource src{QStringLiteral("t"), QStringLiteral("https://example.invalid/p.bin"), sha256Of(payload)};
        QTemporaryDir tmp; QDir d(tmp.path());
        const QString blocker = writeFile(d, QStringLiteral("blocker"), QByteArray("x"));
        QVERIFY(!blocker.isEmpty());

        PreloaderOptions opt;
        opt.allowNetwork = true;
        opt.cacheDir = QDir(blocker).filePath(QStringLiteral("sub"));   // blocker 是普通文件 → mkpath 必失败
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {src},
                                           [&payload](const PreloaderSource &, QByteArray *out, QString *) {
                                               *out = payload; return true;
                                           }, res, &err), qPrintable(err));
        QCOMPARE(res.origin, PreloaderOrigin::Network);
        QCOMPARE(res.bytes, payload);
        QVERIFY(res.path.isEmpty());               // 没落盘 → 路径为空
        QVERIFY2(res.log.join('\n').contains(QStringLiteral("缓存")), qPrintable(res.log.join('\n')));
    }

    // 缓存文件名只取**基名**：来源 name 来自用户配置的 JSON，不得借 `../` 写到 cacheDir 之外
    void cacheNameCannotEscapeCacheDir()
    {
        const QByteArray payload("ESCAPE");
        PreloaderSource src{QStringLiteral("../../escaped_preloader.bin"),
                            QStringLiteral("https://example.invalid/p.bin"), sha256Of(payload)};
        QTemporaryDir tmp; QDir d(tmp.path());
        QVERIFY(d.mkdir(QStringLiteral("cache")));
        PreloaderOptions opt;
        opt.allowNetwork = true;
        opt.cacheDir = d.filePath(QStringLiteral("cache"));
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {src},
                                           [&payload](const PreloaderSource &, QByteArray *out, QString *) {
                                               *out = payload; return true;
                                           }, res, &err), qPrintable(err));
        QCOMPARE(QFileInfo(res.path).fileName(), QStringLiteral("escaped_preloader.bin"));
        QCOMPARE(QFileInfo(res.path).absolutePath(), QFileInfo(opt.cacheDir).absoluteFilePath());
        QVERIFY(!QFile::exists(d.filePath(QStringLiteral("escaped_preloader.bin"))));   // 没逃到上层
    }

    // M1：期望哈希**可判定无效**（64 字符但含非十六进制）→ 与"缺失"同口径，在**发请求之前**拒绝
    // （判据与 verifySha256 共用 isHexSha256；原来只查长度 → 64 个 'z' 也会真发一次下载请求）
    void networkRefusesSourceWithNonHexSha256()
    {
        int calls = 0;
        PreloaderSource src{QStringLiteral("t"), QStringLiteral("https://example.invalid/p.bin"),
                            QString(64, QLatin1Char('z'))};
        PreloaderOptions opt;
        opt.allowNetwork = true;
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {src},
                                           [&calls](const PreloaderSource &, QByteArray *, QString *) {
                                               ++calls; return true;
                                           }, res, &err), qPrintable(err));
        QCOMPARE(calls, 0);                 // 不是"下载后再拒"——一次请求都不发
        QCOMPARE(res.origin, PreloaderOrigin::None);
        QVERIFY2((res.log.join('\n') + res.skipReason).contains(QStringLiteral("sha256")),
                 qPrintable(res.log.join('\n') + res.skipReason));
    }

    // ② 来源没有 sha256 → fail-closed：不下载、告警
    void networkRefusesSourceWithoutSha256()
    {
        int calls = 0;
        PreloaderSource src{QStringLiteral("t"), QStringLiteral("https://example.invalid/p.bin"), QString()};
        PreloaderOptions opt;
        opt.allowNetwork = true;
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {src},
                                           [&calls](const PreloaderSource &, QByteArray *, QString *) {
                                               ++calls; return true;
                                           }, res, &err), qPrintable(err));
        QCOMPARE(calls, 0);
        QCOMPARE(res.origin, PreloaderOrigin::None);
        QVERIFY2((res.log.join('\n') + res.skipReason).contains(QStringLiteral("sha256")),
                 qPrintable(res.log.join('\n') + res.skipReason));
    }

    // ② 允许联网但没配置任何来源 → 跳过 + 说明（不是崩溃、不是静默）
    void networkOnWithoutSourcesSkipsWithReason()
    {
        PreloaderOptions opt;
        opt.allowNetwork = true;
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {}, {}, res, &err), qPrintable(err));
        QCOMPARE(res.origin, PreloaderOrigin::None);
        QVERIFY2(res.skipReason.contains(QStringLiteral("来源")), qPrintable(res.skipReason));
    }

    // ② 开启了网络、也有清单，但**没有下载实现**（注入点为空）→ 跳过 + 说明（不是崩溃、不是静默）
    void networkWithoutDownloaderSkipsWithReason()
    {
        PreloaderSource src{QStringLiteral("t"), QStringLiteral("https://example.invalid/p.bin"),
                            QString(64, QLatin1Char('a'))};
        PreloaderOptions opt;
        opt.allowNetwork = true;
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {src}, {}, res, &err), qPrintable(err));
        QCOMPARE(res.origin, PreloaderOrigin::None);
        QVERIFY(res.bytes.isEmpty());
        QVERIFY2(res.skipReason.contains(QStringLiteral("下载实现")), qPrintable(res.skipReason));
    }

    // ② 下载"成功"但**零字节** → 不是合法 preloader：拒绝（fail-closed），并把原因写进日志
    void networkRejectsEmptyDownload()
    {
        PreloaderSource src{QStringLiteral("t"), QStringLiteral("https://example.invalid/p.bin"),
                            QString(64, QLatin1Char('a'))};
        PreloaderOptions opt;
        opt.allowNetwork = true;
        PreloaderResult res; QString err;
        QVERIFY2(mtkbrom::resolvePreloader(opt, {src},
                                           [](const PreloaderSource &, QByteArray *, QString *) {
                                               return true;      // 谎报成功、什么都没给
                                           }, res, &err), qPrintable(err));
        QCOMPARE(res.origin, PreloaderOrigin::None);
        QVERIFY(res.bytes.isEmpty());
        QVERIFY2((res.log.join('\n') + res.skipReason).contains(QStringLiteral("为空")),
                 qPrintable(res.log.join('\n') + res.skipReason));
    }

    // 清单 JSON：合法可解析；坏输入明确失败
    void parseSourcesJson()
    {
        const QByteArray good = R"({"sources":[{"name":"a","url":"https://h/p.bin",)"
                                R"("sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}]})";
        QList<PreloaderSource> out; QString err;
        QVERIFY2(mtkbrom::parsePreloaderSources(good, out, &err), qPrintable(err));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).name, QStringLiteral("a"));
        QCOMPARE(out.at(0).url, QStringLiteral("https://h/p.bin"));

        out.clear(); err.clear();
        QVERIFY(!mtkbrom::parsePreloaderSources(QByteArray("not json"), out, &err));
        QVERIFY(!err.isEmpty());
        out.clear(); err.clear();
        QVERIFY(!mtkbrom::parsePreloaderSources(QByteArray(R"({"sources":[]})"), out, &err));
        QVERIFY(!err.isEmpty());        // 空清单 = 明确失败（而不是"成功但没来源"）
    }

    // 清单坏输入的另两档：根不是对象 / 条目缺 url → 明确失败；
    // **缺 sha256 的条目是合法清单**（fail-closed 在下载前拒绝"该来源"，不是拒绝整份清单）
    void parseSourcesJsonRejectsMalformedEntries()
    {
        QList<PreloaderSource> out; QString err;
        QVERIFY(!mtkbrom::parsePreloaderSources(QByteArray("[1,2]"), out, &err));
        QVERIFY(!err.isEmpty());

        out.clear(); err.clear();
        QVERIFY(!mtkbrom::parsePreloaderSources(QByteArray(R"({"sources":[{"name":"a"}]})"), out, &err));
        QVERIFY(!err.isEmpty());

        // 注：JSON 先落到变量再进宏 —— 空定界符的 raw string（R"(...)") 里带 "//"（URL）时
        // **moc 会误词法化**（Qt 6.11.2 实测：raw string 内联在 QtTest 宏里 → "missing ')' in
        // macro usage"）。brief 的用例同样是把 JSON 先赋给变量的。
        const QByteArray missingSha = R"({"sources":[{"name":"a","url":"https://h/p.bin"}]})";
        out.clear(); err.clear();
        QVERIFY2(mtkbrom::parsePreloaderSources(missingSha, out, &err), qPrintable(err));
        QCOMPARE(out.size(), 1);
        QVERIFY(out.at(0).sha256.isEmpty());
    }

    // I1①：配置清单**不存在** → 空表 + log 说明（真实调用方的默认处境：没配就如实说，不是静默空表）
    void configuredSourcesMissingFileIsReported()
    {
        QTemporaryDir cfg;
        QVERIFY(cfg.isValid());
        const QString path = redirectConfigTo(cfg);
        QCOMPARE(QFileInfo(path).absolutePath(), QFileInfo(cfg.path()).absoluteFilePath());  // 没跑去用户真实配置目录
        QVERIFY(!QFile::exists(path));

        QStringList log;
        const QList<PreloaderSource> out = mtkbrom::loadConfiguredSources(&log);
        QVERIFY(out.isEmpty());
        QCOMPARE(log.size(), 1);
        QVERIFY2(log.first().contains(QStringLiteral("不存在")), qPrintable(log.join('\n')));
        QVERIFY(mtkbrom::loadConfiguredSources().isEmpty());    // log 出参可省（默认 nullptr）
    }

    // I1②：合法清单 → **真的解析出条目**。loadConfiguredSources 是真实调用方唯一的 sources 来源：
    // 它若静默退化成空表（永远走 skip 分支），此前没有任何用例能抓 —— 本用例就是防这个。
    void configuredSourcesLoadsValidList()
    {
        QTemporaryDir cfg;
        QVERIFY(cfg.isValid());
        const QString path = redirectConfigTo(cfg);
        QCOMPARE(QFileInfo(path).absolutePath(), QFileInfo(cfg.path()).absoluteFilePath());

        // JSON 先落变量再进宏（内联 R"(...)" 含 "//" 会被 moc 误词法化 —— 见文件头注释）
        const QByteArray json = R"({"sources":[{"name":"a","url":"https://h/p.bin",)"
                                R"("sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"}]})";
        QFile f(path);
        QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(path));
        QCOMPARE(f.write(json), qint64(json.size()));
        f.close();

        QStringList log;
        const QList<PreloaderSource> out = mtkbrom::loadConfiguredSources(&log);
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.at(0).name, QStringLiteral("a"));
        QCOMPARE(out.at(0).url, QStringLiteral("https://h/p.bin"));
        QCOMPARE(out.at(0).sha256.size(), 64);
        QVERIFY2(log.join('\n').contains(QStringLiteral("1 条")), qPrintable(log.join('\n')));
    }

    // I1③：清单**非法**（坏 JSON）→ 空表 + log 写明无效（与"文件不存在"必须能区分开）
    void configuredSourcesRejectsBrokenJson()
    {
        QTemporaryDir cfg;
        QVERIFY(cfg.isValid());
        const QString path = redirectConfigTo(cfg);
        QCOMPARE(QFileInfo(path).absolutePath(), QFileInfo(cfg.path()).absoluteFilePath());

        QFile f(path);
        QVERIFY2(f.open(QIODevice::WriteOnly), qPrintable(path));
        QCOMPARE(f.write(QByteArray("not json")), qint64(8));
        f.close();

        QStringList log;
        const QList<PreloaderSource> out = mtkbrom::loadConfiguredSources(&log);
        QVERIFY(out.isEmpty());
        QVERIFY2(log.join('\n').contains(QStringLiteral("无效")), qPrintable(log.join('\n')));
    }

    // 候选扫描：大小写不敏感、只认 preloader*.bin、顺序确定
    void findCandidatesIsCaseInsensitiveAndSorted()
    {
        QTemporaryDir tmp; QDir d(tmp.path());
        QVERIFY(!writeFile(d, QStringLiteral("PRELOADER_X.BIN"), QByteArray("X")).isEmpty());
        QVERIFY(!writeFile(d, QStringLiteral("preloader.bin"), QByteArray("Y")).isEmpty());
        QVERIFY(!writeFile(d, QStringLiteral("boot.img"), QByteArray("Z")).isEmpty());
        const QStringList cand = mtkbrom::findPreloaderCandidates({tmp.path()});
        QCOMPARE(cand.size(), 2);
        QVERIFY(cand.at(0) < cand.at(1));           // 排序确定（去重后字典序）
    }

    // 去重：同一目录给两次不重复；不存在的目录不炸（不是每个目录都必须存在）
    void findCandidatesDeduplicatesDirs()
    {
        QTemporaryDir tmp; QDir d(tmp.path());
        QVERIFY(!writeFile(d, QStringLiteral("preloader.bin"), QByteArray("P")).isEmpty());
        QVERIFY(!writeFile(d, QStringLiteral("PRELOADER_2.BIN"), QByteArray("Q")).isEmpty());
        const QStringList cand = mtkbrom::findPreloaderCandidates(
            {tmp.path(), tmp.path(), QStringLiteral("/nonexistent-dir-for-test")});
        QCOMPARE(cand.size(), 2);
        QVERIFY(cand.at(0) < cand.at(1));
    }

    void verifySha256IsCaseInsensitive()
    {
        const QByteArray d("abc");
        QVERIFY(mtkbrom::verifySha256(d, sha256Of(d).toUpper()));
        QVERIFY(!mtkbrom::verifySha256(d, QString(64, QLatin1Char('f'))));
        QVERIFY(!mtkbrom::verifySha256(d, QStringLiteral("short")));
    }

private:
    // 配置路径重定向：XDG_CONFIG_HOME → 临时目录（用例组专用；env 由 cleanup() 恢复）。
    // **调用方必须先 QVERIFY(cfg.isValid())**：空的 XDG 会被 Qt 当成"未设置"，用例就会去动
    // 用户真实的 ~/.config（我们只读不写，但也不必冒这个险）。返回清单文件的完整路径。
    QString redirectConfigTo(const QTemporaryDir &cfg)
    {
        qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8());
        return mtkbrom::configuredSourcesPath();
    }

    QByteArray m_savedXdg;
    bool m_xdgWasSet = false;
};
QTEST_APPLESS_MAIN(TestMtkPreloaderFetch)
#include "test_mtk_preloader_fetch.moc"
