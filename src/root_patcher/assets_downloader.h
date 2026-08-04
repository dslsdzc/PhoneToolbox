#pragma once
#include <QObject>
#include <QHash>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace patcher {

// 注入物下载器：负责 Magisk/KernelSU/APatch 等 root 注入资产的下载与本地缓存。
// - 缓存布局：<cacheDir>/<key>/<basename>；key 自带版本（如 "magisk-v30.7"），
//   构成版本化子目录；默认 cacheDir 为 QStandardPaths::AppDataLocation + "/patcher"。
// - 手动指定（setManualFile）优先于缓存与下载，且视为已可用（全局契约：失败
//   只发 downloadFailed 信号，绝不崩溃/抛异常）。
class AssetsDownloader : public QObject
{
    Q_OBJECT
public:
    explicit AssetsDownloader(QObject *parent = nullptr);

    // 覆盖默认缓存目录（默认 QStandardPaths::AppDataLocation + "/patcher"）
    void setCacheDir(const QString &dir);
    QString cacheDir() const;

    // 手动指定 key → 本地文件（不校验存在性，调用方保证）；空路径清除该指定
    void setManualFile(const QString &key, const QString &path);
    QString manualFile(const QString &key) const;

    // 手动指定或缓存文件存在即视为已缓存
    bool hasCached(const QString &key) const;
    // 手动路径 / 缓存文件路径；未命中返回空串
    QString cachedPath(const QString &key) const;

    // 异步下载到 <cacheDir>/<key>/<basename>（basename 取 URL 文件名）；
    // 已有缓存或手动指定时直接成功完成（不发网络请求）。
    // key → URL 的映射由调用方维护（如 C3/C4/C5 各注入器）。
    void downloadAsync(const QUrl &url, const QString &key);

    // 取消所有进行中的下载（清理部分文件；对应 key 以失败信号收尾）
    void cancel();

signals:
    void downloadFinished(const QString &key, const QString &filePath);
    void downloadFailed(const QString &key, const QString &error);

private:
    QString defaultCacheDir() const;
    QString keyDir(const QString &key) const;
    void finishQueued(const QString &key, const QString &path);
    void failQueued(const QString &key, const QString &error);

    QNetworkAccessManager *m_nam = nullptr;
    QString m_cacheDir;
    QHash<QString, QString> m_manualFiles;   // key -> 手动指定路径
    QHash<QString, QString> m_keyFileNames;  // key -> 缓存文件名（下载时记录）
    QHash<QString, QNetworkReply *> m_replies; // key -> 进行中的 reply
};

} // namespace patcher
