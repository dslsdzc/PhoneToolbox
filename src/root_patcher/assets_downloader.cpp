#include "assets_downloader.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QMetaObject>

namespace patcher {
namespace {

const QString kPartialSuffix = QStringLiteral(".part");

// key 承载缓存子目录名：拒绝空串、路径分隔符与相对路径穿越
bool validKey(const QString &key)
{
    if (key.isEmpty() || key == QStringLiteral(".") || key == QStringLiteral(".."))
        return false;
    return !key.contains('/') && !key.contains('\\');
}

} // namespace

AssetsDownloader::AssetsDownloader(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_cacheDir(defaultCacheDir())
{
}

void AssetsDownloader::setCacheDir(const QString &dir)
{
    m_cacheDir = dir;
}

QString AssetsDownloader::cacheDir() const
{
    return m_cacheDir;
}

QString AssetsDownloader::defaultCacheDir() const
{
    // 全局契约：QStandardPaths::AppDataLocation + "/patcher"；
    // 版本化子目录由 key（如 "magisk-v30.7"）承载
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::home().filePath(QStringLiteral(".local/share/PhoneToolbox"));
    return base + QStringLiteral("/patcher");
}

QString AssetsDownloader::keyDir(const QString &key) const
{
    return m_cacheDir + QLatin1Char('/') + key;
}

void AssetsDownloader::setManualFile(const QString &key, const QString &path)
{
    if (!validKey(key))
        return;
    if (path.isEmpty())
        m_manualFiles.remove(key);
    else
        m_manualFiles.insert(key, path);
}

QString AssetsDownloader::manualFile(const QString &key) const
{
    return m_manualFiles.value(key);
}

bool AssetsDownloader::hasCached(const QString &key) const
{
    if (!validKey(key))
        return false;
    if (!m_manualFiles.value(key).isEmpty())
        return true; // 手动指定视为可用
    return QFile::exists(cachedPath(key));
}

QString AssetsDownloader::cachedPath(const QString &key) const
{
    if (!validKey(key))
        return {};
    const QString manual = m_manualFiles.value(key);
    if (!manual.isEmpty())
        return manual;
    // 下载时记录的文件名优先（确定性）
    const QString name = m_keyFileNames.value(key);
    if (!name.isEmpty()) {
        const QString p = keyDir(key) + QLatin1Char('/') + name;
        if (QFile::exists(p))
            return p;
    }
    // 回退：扫描 key 目录（预置缓存 / 旧版本下载的缓存）
    const QDir dir(keyDir(key));
    if (!dir.exists())
        return {};
    const QStringList files = dir.entryList(QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &f : files) {
        if (f.endsWith(kPartialSuffix))
            continue;
        return dir.filePath(f);
    }
    return {};
}

void AssetsDownloader::finishQueued(const QString &key, const QString &path)
{
    QMetaObject::invokeMethod(this, [this, key, path]() {
        emit downloadFinished(key, path);
    }, Qt::QueuedConnection);
}

void AssetsDownloader::failQueued(const QString &key, const QString &error)
{
    QMetaObject::invokeMethod(this, [this, key, error]() {
        emit downloadFailed(key, error);
    }, Qt::QueuedConnection);
}

void AssetsDownloader::downloadAsync(const QUrl &url, const QString &key)
{
    if (!validKey(key)) {
        failQueued(key, QStringLiteral("非法的 key"));
        return;
    }
    // 手动指定优先：直接视为完成
    const QString manual = m_manualFiles.value(key);
    if (!manual.isEmpty()) {
        finishQueued(key, manual);
        return;
    }
    // 缓存命中：不发网络请求
    const QString cached = cachedPath(key);
    if (!cached.isEmpty()) {
        finishQueued(key, cached);
        return;
    }
    if (m_replies.contains(key)) {
        failQueued(key, QStringLiteral("该 key 已有下载进行中"));
        return;
    }
    if (url.isEmpty()) {
        failQueued(key, QStringLiteral("URL 为空"));
        return;
    }
    const QString fileName = url.fileName();
    if (fileName.isEmpty() || fileName == QStringLiteral(".") || fileName == QStringLiteral("..")) {
        failQueued(key, QStringLiteral("URL 缺少文件名"));
        return;
    }
    const QString dir = keyDir(key);
    if (!QDir().mkpath(dir)) {
        failQueued(key, QStringLiteral("无法创建缓存目录: %1").arg(dir));
        return;
    }
    m_keyFileNames.insert(key, fileName);
    const QString dest = dir + QLatin1Char('/') + fileName;
    const QString partial = dest + kPartialSuffix;
    QFile::remove(partial); // 清理上次失败残留

    auto *out = new QFile(partial, this);
    if (!out->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        const QString err = out->errorString();
        out->deleteLater();
        failQueued(key, QStringLiteral("无法打开缓存文件: %1").arg(err));
        return;
    }

    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy); // GitHub release 资产有重定向
    auto *reply = m_nam->get(req);
    m_replies.insert(key, reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, key, dest, partial, out]() {
        m_replies.remove(key);
        QString err;
        if (reply->error() != QNetworkReply::NoError) {
            err = reply->errorString();
        } else {
            const QByteArray data = reply->readAll();
            if (out->write(data) != data.size() || !out->flush())
                err = QStringLiteral("写入缓存文件失败");
        }
        out->close();
        if (err.isEmpty()) {
            QFile::remove(dest); // 覆盖旧缓存
            if (QFile::rename(partial, dest)) {
                emit downloadFinished(key, dest);
            } else {
                QFile::remove(partial);
                emit downloadFailed(key, QStringLiteral("缓存文件落盘失败"));
            }
        } else {
            QFile::remove(partial); // 失败不留 .part 残留
            emit downloadFailed(key, err);
        }
        out->deleteLater();
        reply->deleteLater();
    });
}

void AssetsDownloader::cancel()
{
    for (auto it = m_replies.cbegin(); it != m_replies.cend(); ++it) {
        if (QNetworkReply *r = it.value())
            r->abort(); // finished 处理器负责清理部分文件并以失败信号收尾
    }
    m_replies.clear();
}

} // namespace patcher
