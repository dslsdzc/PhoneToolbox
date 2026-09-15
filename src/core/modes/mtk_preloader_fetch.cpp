// src/core/modes/mtk_preloader_fetch.cpp
//
// 出处（mtkclient v2.1.4-20-g71b0175，GPL-3.0，**只读引用不复制代码**）：
//   Library/DA/xflash/xflash_lib.py:1144  "No preloader given. Operation may fail due to missing
//     dram setup." —— 缺 preloader 只告警、不中止（本模块同姿态）。
//   Library/DA/xflash/xflash_lib.py:1121  "No preloader given. Searching for preloader" —— 上游另有
//     "自动找 preloader"的补救路径；本模块**不实现**（项目自有决策：只做显式路径 + 固件目录唯一命中，
//     不做"按 draminfo 猜"；见计划 2026-09-15-mtk-brom-d1.md 的歧义登记表）。
//
// 与 brief/计划样例代码的**有意偏差**（两处都落在不变量"只有**显式路径**失败才返回 false"上）：
//   1. 自动导入的唯一候选**读不到/为空**：样例 `return false` → 本实现**不中止**（与"没找到"同姿态，
//      spec §7 的"都不可用 → 跳过 EMI"），但把文件名与失败原因写进 log/skipReason（不静默）。
//   2. 缓存写不进去：样例 `return false` → 本实现保留**已 sha256 校验通过**的字节（缓存只是便利，
//      不该因为缓存目录不可写而丢掉可用数据），只告警 + `path` 留空。
//   偏差依据：Team Lead 对该任务的四条语义第 1 条（"只有显式给了路径却读不到/为空才 false"）
//   + spec §7"都不可用 → 不中止"。两处都有用例钉住（autoImportUnreadableCandidateSkipsInsteadOfFailing
//   / cacheWriteFailureKeepsVerifiedBytes）。
#include "core/modes/mtk_preloader_fetch.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStandardPaths>
#include <utility>   // std::as_const（遍历 Qt 容器，不得用 qAsConst）

namespace mtkbrom {
namespace {

QString sha256Hex(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

// 64 位十六进制谓词（大小写不敏感）。**唯一实现**：`verifySha256` 与"下载前的 fail-closed 拒绝"
// 共用同一口径 —— 两处各写一份会漂移（M1：原来下载前只查长度，64 个 'z' 也会真发一次请求）。
bool isHexSha256(const QString &hex)
{
    const QString t = hex.trimmed().toLower();
    if (t.size() != 64)
        return false;
    for (const QChar c : std::as_const(t))
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))))
            return false;
    return true;
}

bool readWholeFile(const QString &path, QByteArray &out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法读取 preloader：%1").arg(path);
        return false;
    }
    out = f.readAll();
    if (out.isEmpty()) {
        if (error) *error = QStringLiteral("preloader 文件为空：%1").arg(path);
        return false;
    }
    return true;
}

// 落盘。cacheDir 为空 = 合法（只留在内存）→ *outPath 留空并返回 true。
// 文件名的目录部分一律剥掉（`name` 来自用户配置的 JSON，不得借此写到 cacheDir 之外）。
bool writeCache(const QString &cacheDir, const QString &name, const QByteArray &data,
                QString *outPath, QString *error)
{
    if (cacheDir.isEmpty()) {
        *outPath = QString();
        return true;
    }
    QDir dir(cacheDir);
    if (!dir.mkpath(QStringLiteral("."))) {
        if (error) *error = QStringLiteral("无法创建缓存目录：%1").arg(cacheDir);
        return false;
    }
    const QString fileName = QFileInfo(name).fileName();
    const QString path = dir.filePath(fileName.isEmpty() ? QStringLiteral("preloader.bin") : fileName);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("无法写入缓存文件：%1").arg(path);
        return false;
    }
    if (f.write(data) != data.size()) {
        if (error) *error = QStringLiteral("缓存写入不完整：%1").arg(path);
        return false;
    }
    *outPath = path;
    return true;
}

} // namespace

bool verifySha256(const QByteArray &bytes, const QString &expectedHex)
{
    const QString expected = expectedHex.trimmed().toLower();
    return isHexSha256(expected) && sha256Hex(bytes) == expected;
}

QStringList findPreloaderCandidates(const QStringList &dirs)
{
    QSet<QString> seen;
    QStringList out;
    for (const QString &dirPath : std::as_const(dirs)) {
        if (dirPath.isEmpty())
            continue;
        const QDir dir(dirPath);
        const QStringList files = dir.entryList({QStringLiteral("*.bin")}, QDir::Files, QDir::Name);
        for (const QString &name : files) {
            if (!name.startsWith(QLatin1String("preloader"), Qt::CaseInsensitive))
                continue;
            const QString abs = QFileInfo(dir.filePath(name)).absoluteFilePath();
            if (seen.contains(abs))
                continue;
            seen.insert(abs);
            out << abs;
        }
    }
    out.sort();                       // 顺序确定（去重后字典序）——UI/日志/用例都依赖它
    return out;
}

bool parsePreloaderSources(const QByteArray &json, QList<PreloaderSource> &out, QString *error)
{
    out.clear();
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &perr);
    if (doc.isNull() || !doc.isObject()) {
        if (error) *error = QStringLiteral("来源清单不是合法 JSON 对象：%1").arg(perr.errorString());
        return false;
    }
    const QJsonArray arr = doc.object().value(QStringLiteral("sources")).toArray();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        PreloaderSource s;
        s.name = o.value(QStringLiteral("name")).toString();
        s.url = o.value(QStringLiteral("url")).toString();
        s.sha256 = o.value(QStringLiteral("sha256")).toString();
        if (s.url.isEmpty()) {
            if (error) *error = QStringLiteral("来源清单有条目缺少 url");
            out.clear();
            return false;
        }
        out << s;
    }
    if (out.isEmpty()) {
        if (error) *error = QStringLiteral("来源清单为空（sources 数组没有任何条目）");
        return false;
    }
    return true;
}

QString configuredSourcesPath()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    return dir.isEmpty() ? QStringLiteral("mtk_preloader_sources.json")
                         : QDir(dir).filePath(QStringLiteral("mtk_preloader_sources.json"));
}

QList<PreloaderSource> loadConfiguredSources(QStringList *log)
{
    const QString path = configuredSourcesPath();
    if (!QFile::exists(path)) {
        if (log)
            *log << QStringLiteral("未配置 preloader 来源清单（%1 不存在）—— 网络获取不可用").arg(path);
        return {};
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (log)
            *log << QStringLiteral("无法读取 preloader 来源清单：%1").arg(path);
        return {};
    }
    QList<PreloaderSource> out;
    QString err;
    if (!parsePreloaderSources(f.readAll(), out, &err)) {
        if (log)
            *log << QStringLiteral("preloader 来源清单无效：%1（%2）").arg(err, path);
        return {};
    }
    if (log)
        *log << QStringLiteral("已加载 %1 条 preloader 来源（%2）").arg(out.size()).arg(path);
    return out;
}

bool resolvePreloader(const PreloaderOptions &opt, const QList<PreloaderSource> &sources,
                      const PreloaderDownloader &downloader, PreloaderResult &out, QString *error)
{
    out = PreloaderResult{};

    // ---- ① 显式路径（用户指定 → 读不到就是响亮失败，不能悄悄跳过 DRAM 初始化）----
    if (!opt.explicitPath.isEmpty()) {
        if (!readWholeFile(opt.explicitPath, out.bytes, error))
            return false;
        out.origin = PreloaderOrigin::Explicit;
        out.path = opt.explicitPath;
        out.log << QStringLiteral("使用显式指定的 preloader：%1（%2 字节，sha256 %3）")
                       .arg(opt.explicitPath).arg(out.bytes.size()).arg(sha256Hex(out.bytes));
        return true;
    }

    // ---- ① 固件目录自动导入（唯一命中才用；多个 → 不猜）----
    const QStringList candidates = findPreloaderCandidates(opt.firmwareDirs);
    QString importNote;                 // 候选不可用的说明（并入最终 skipReason —— 不静默，见偏差 1）
    if (candidates.size() == 1) {
        QString readErr;
        if (readWholeFile(candidates.first(), out.bytes, &readErr)) {
            out.origin = PreloaderOrigin::AutoImport;
            out.path = candidates.first();
            out.log << QStringLiteral("从固件目录自动导入 preloader：%1（%2 字节）")
                           .arg(candidates.first()).arg(out.bytes.size());
            return true;
        }
        // 不变量（见文件头）：只有"显式路径"失败才中止。自动导入的唯一候选读不到/为空
        // → 与"没找到"同姿态（spec §7"都不可用 → 跳过 EMI"），但必须点名是哪个文件、为什么。
        out.bytes.clear();
        out.log << QStringLiteral("固件目录里唯一的 preloader 候选不可用：%1").arg(readErr);
        importNote = QStringLiteral("；固件目录里的候选 preloader 不可用（%1）").arg(readErr);
    }
    if (candidates.size() > 1) {
        out.log << QStringLiteral("固件目录里有 %1 个 preloader 候选：%2")
                       .arg(candidates.size()).arg(candidates.join(QStringLiteral("、")));
        out.skipReason = QStringLiteral("有 %1 个 preloader 候选、未指定用哪个 —— 不猜，跳过 DRAM 初始化"
                                        "（DA2 可能起不来；可显式指定 preloader 后重试）")
                             .arg(candidates.size());
        return true;
    }

    // ---- ② 网络获取（默认关闭）----
    // 跳过原因统一出口：把①里"候选不可用"的说明并入，避免"为什么跳过"丢信息
    const auto skipReasonText = [&importNote](const QString &reason) {
        return importNote.isEmpty() ? reason : reason + importNote;
    };
    if (!opt.allowNetwork) {
        out.skipReason = skipReasonText(QStringLiteral("未提供 preloader，且网络获取默认关闭 —— 跳过 DRAM 初始化"
                                                       "（DA2 可能起不来）"));
        return true;
    }
    if (sources.isEmpty()) {
        out.skipReason = skipReasonText(QStringLiteral("已允许网络获取 preloader，但没有可用的来源清单（%1）"
                                                       "—— 跳过 DRAM 初始化")
                                            .arg(configuredSourcesPath()));
        return true;
    }
    if (!downloader) {
        out.skipReason = skipReasonText(
            QStringLiteral("已允许网络获取 preloader，但没有可用的下载实现 —— 跳过 DRAM 初始化"));
        return true;
    }

    QStringList failures;
    for (const PreloaderSource &src : std::as_const(sources)) {
        const QString label = src.name.isEmpty() ? src.url : src.name;   // 日志里点名的口径
        // fail-closed：**可判定无效**的期望哈希（缺失/长度不对/含非十六进制字符）在**发请求之前**就拒
        // —— 判据与 verifySha256 共用 isHexSha256（M1：原来只查长度，64 个 'z' 也会真下载一次）
        if (!isHexSha256(src.sha256)) {
            failures << QStringLiteral("%1：来源未提供 64 位十六进制 sha256，拒绝下载（fail-closed）")
                            .arg(label);
            continue;
        }
        QByteArray data;
        QString derr;
        if (!downloader(src, &data, &derr)) {
            failures << QStringLiteral("%1：下载失败（%2）").arg(label, derr);
            continue;
        }
        if (data.isEmpty()) {
            failures << QStringLiteral("%1：下载结果为空 —— 拒绝使用").arg(label);
            continue;
        }
        if (!verifySha256(data, src.sha256)) {
            failures << QStringLiteral("%1：sha256 不匹配（期望 %2，实得 %3）—— 拒绝使用")
                            .arg(label, src.sha256.trimmed().toLower(), sha256Hex(data));
            continue;
        }
        QString cachePath;
        QString cacheErr;
        if (!writeCache(opt.cacheDir, src.name, data, &cachePath, &cacheErr)) {
            // 偏差 2（见文件头）：缓存只是便利 —— 字节已 sha256 校验通过，不因缓存目录不可写而丢弃
            out.log << QStringLiteral("preloader 已获取但缓存失败（%1）—— 本次仅内存可用").arg(cacheErr);
            cachePath.clear();
        }
        out.origin = PreloaderOrigin::Network;
        out.bytes = data;
        out.path = cachePath;
        out.log << QStringLiteral("已从网络获取 preloader：%1（sha256 校验通过）").arg(src.url);
        out.log << QStringLiteral("⚠️ 该文件来自第三方来源（%1）—— 来源不可信时刷入有砖机风险，请自行核实")
                       .arg(src.url);
        return true;
    }

    out.log << failures;
    out.skipReason = skipReasonText(QStringLiteral("网络获取的 %1 条来源都不可用 —— 跳过 DRAM 初始化"
                                                   "（DA2 可能起不来）")
                                        .arg(sources.size()));
    return true;
}

} // namespace mtkbrom
