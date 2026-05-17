#include "local_importer.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QByteArray>

ImportResult LocalImporter::importFromUrl(const QString &url, QList<VulnEntry> &entries)
{
    ImportResult result;

    QNetworkAccessManager mgr;
    QNetworkRequest req(url);
    req.setTransferTimeout(30000);

    QNetworkReply *reply = mgr.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        result.error = reply->errorString();
        reply->deleteLater();
        return result;
    }

    QByteArray data = reply->readAll();
    reply->deleteLater();

    return importFromJsonData(data, entries);
}

ImportResult LocalImporter::importFromFile(const QString &path, QList<VulnEntry> &entries)
{
    QString lower = path.toLower();
    if (lower.endsWith(".zip"))
        return importFromZip(path, entries);

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        ImportResult r;
        r.error = "无法打开文件: " + path;
        return r;
    }
    QByteArray data = f.readAll();
    f.close();
    return importFromJsonData(data, entries);
}

ImportResult LocalImporter::importFromJsonData(const QByteArray &data, QList<VulnEntry> &entries)
{
    ImportResult result;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError) {
        result.error = "JSON 解析错误: " + err.errorString();
        return result;
    }

    QJsonObject root = doc.object();
    QJsonArray arr = root["entries"].toArray();
    if (arr.isEmpty()) {
        result.error = "JSON 中未找到 entries 数组";
        return result;
    }

    for (const auto &v : arr) {
        QJsonObject obj = v.toObject();
        // unwrap { "entry": {...} } if present
        if (obj.contains("entry"))
            obj = obj["entry"].toObject();
        entries.append(vulnEntryFromJson(obj));
    }

    result.success = true;
    result.count = entries.size();
    return result;
}

ImportResult LocalImporter::importFromZip(const QString &zipPath, QList<VulnEntry> &entries)
{
    ImportResult result;
    result.error = "ZIP 导入暂未实现，请使用 JSON 文件";
    return result;
}
