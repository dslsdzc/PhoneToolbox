#pragma once

// MTK preloader 两路径解析（Phase D1 Task 8；spec §7）
//
// 职责：**拿到 preloader 字节**（EMI 的来源）—— 提取在 mtk_preloader_emi.h（T5），发送在 T6/T9。
// 三条去向：
//   ① 显式路径 → 读；读不到/为空 = **响亮失败**（用户以为会做 DRAM 初始化，不能悄悄跳过）
//   ① 固件目录自动导入 → `preloader*.bin`（大小写不敏感）；**唯一命中才用**，多个 → 不猜、列候选
//   ② 网络获取（**默认关闭**）→ 按用户配置的来源清单下载 → **必须 sha256 通过**才采用 → 可缓存
//   ③ 都不可用 → 如实告警"跳过 DRAM 初始化（DA2 可能起不来）"，**不中止刷写**
//
// **不变量（spec §7 + 上游同姿态）**：`resolvePreloader` 只在"①显式路径"失败时返回 false；
// 其余一切情况（没找到 / 多候选 / 读不到自动导入的候选 / 网络关着 / 没清单 / 全部来源被拒 /
// 缓存写不进去）都返回 true，把原因写进 `skipReason`（非空）与 `log`，由调用方按 warning 落日志。
// 出处：mtkclient `Library/DA/xflash/xflash_lib.py:1144`
//   "No preloader given. Operation may fail due to missing dram setup."（告警而非中止）。
//
// 许可/诚实边界（计划期裁决，用户 2026-09-15 认可）：**不内置任何第三方镜像 URL 与 sha256** ——
// 无法核实其真实性与内容，而错误的 preloader 有**砖机**风险；凭空写 URL/哈希等于假装有可信来源。
// 来源清单由用户经 `configuredSourcesPath()`（默认 `mtk_preloader_sources.json`）提供；
// **缺清单 → 走 skip 分支并如实告警**（与"都不可用"同一姿态）。机制（解析/校验/缓存/日志/拒绝）
// 全部实现并测试；用户后续要加来源只需往 JSON 里加条目，代码不改。
//
// 依赖：**只依赖 Qt Core** —— 下载器是本模块的**注入点**（`PreloaderDownloader`），
// 生产实现（Qt Network）在 mtk_preloader_download_qt.h，单独文件使测试目标不必链 Network。

#include <functional>
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace mtkbrom {

// 一条网络来源。`sha256` 缺失/长度不对 = **该来源**被拒绝（fail-closed），不影响其它条目，
// 也不是"清单非法"（清单合法性只要求 url 非空）。
struct PreloaderSource {
    QString name;
    QString url;
    QString sha256;   // 64 位十六进制（大小写不敏感）
};

enum class PreloaderOrigin { None, Explicit, AutoImport, Network };

struct PreloaderOptions {
    QString explicitPath;       // ① 显式优先
    QStringList firmwareDirs;   // ① 自动导入：在这些目录里找 preloader（唯一命中才用）
    bool allowNetwork = false;  // ② 默认关闭（关闭时下载器一次都不会被调用）
    QString cacheDir;           // ② 命中后落盘目录（空 = 只留在内存）
};

struct PreloaderResult {
    PreloaderOrigin origin = PreloaderOrigin::None;
    QString path;               // 命中文件路径（Network = 缓存路径；缓存失败/未启用 = 空）
    QByteArray bytes;           // origin != None 时非空
    QStringList log;            // 需落日志（来源/风险/校验）；调用方逐条 emit
    QString skipReason;         // origin == None 时的原因（非空 → 调用方按 warning 落日志）
};

// 注入点：返回 false 或给空数据 = 该来源不可用（调用方继续试下一条）。
using PreloaderDownloader = std::function<bool(const PreloaderSource &, QByteArray *out, QString *error)>;

// 语义见文件头不变量。sources 为空/downloader 为空都不算错误（走 skipReason）。
bool resolvePreloader(const PreloaderOptions &opt, const QList<PreloaderSource> &sources,
                      const PreloaderDownloader &downloader, PreloaderResult &out,
                      QString *error = nullptr);

// 扫目录找 `preloader*.bin`（**大小写不敏感**、按绝对路径**去重后字典序**排序）。
// 不存在的目录直接跳过（不算错误）；不去目录的子目录里递归。
QStringList findPreloaderCandidates(const QStringList &dirs);

// 解析来源清单 JSON：`{"sources":[{"name","url","sha256"}, ...]}`。
// 失败（false + error）：不是合法 JSON 对象 / 条目缺 url / 清单为空 —— **整份拒绝**。
bool parsePreloaderSources(const QByteArray &json, QList<PreloaderSource> &out, QString *error = nullptr);
QString configuredSourcesPath();                                    // 标准配置路径（UI 展示/用户填写）
QList<PreloaderSource> loadConfiguredSources(QStringList *log = nullptr);  // 缺失/为空 → 空表 + log 说明

// 64 位十六进制、大小写不敏感；长度或字符不合法 → false（不抛异常、不静默当真）。
bool verifySha256(const QByteArray &bytes, const QString &expectedHex);

} // namespace mtkbrom
