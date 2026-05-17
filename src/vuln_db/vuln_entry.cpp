#include "vuln_entry.h"
#include <QJsonDocument>
#include <QJsonArray>

// ==================== AffectedRange ====================

QJsonObject affectedRangeToJson(const AffectedRange &r)
{
    QJsonObject obj;
    if (!r.androidVersionMin.isEmpty()) obj["android_version_min"] = r.androidVersionMin;
    if (!r.androidVersionMax.isEmpty()) obj["android_version_max"] = r.androidVersionMax;
    if (!r.patchLevelBefore.isEmpty())  obj["patch_level_before"] = r.patchLevelBefore;
    if (r.sdkMin > 0)  obj["sdk_min"] = r.sdkMin;
    if (r.sdkMax > 0)  obj["sdk_max"] = r.sdkMax;
    if (!r.platforms.isEmpty()) {
        QJsonArray arr;
        for (const auto &p : r.platforms) arr.append(p);
        obj["platforms"] = arr;
    }
    return obj;
}

AffectedRange affectedRangeFromJson(const QJsonObject &obj)
{
    AffectedRange r;
    r.androidVersionMin = obj["android_version_min"].toString();
    r.androidVersionMax = obj["android_version_max"].toString();
    r.patchLevelBefore  = obj["patch_level_before"].toString();
    r.sdkMin = obj["sdk_min"].toInt();
    r.sdkMax = obj["sdk_max"].toInt();
    for (const auto &v : obj["platforms"].toArray())
        r.platforms << v.toString();
    return r;
}

// ==================== PayloadFile ====================

QJsonObject payloadFileToJson(const ExploitScript::PayloadFile &pf)
{
    QJsonObject obj;
    obj["name"] = pf.name;
    obj["path"] = pf.remotePath;
    obj["mode"] = pf.mode;
    if (!pf.data.isEmpty())
        obj["data"] = QString::fromUtf8(pf.data.toBase64());
    if (!pf.platforms.isEmpty()) {
        QJsonArray arr;
        for (const auto &p : pf.platforms) arr.append(p);
        obj["platforms"] = arr;
    }
    return obj;
}

ExploitScript::PayloadFile payloadFileFromJson(const QJsonObject &obj)
{
    ExploitScript::PayloadFile pf;
    pf.name = obj["name"].toString();
    pf.remotePath = obj["path"].toString();
    pf.mode = obj["mode"].toString("755");
    QString b64 = obj["data"].toString();
    if (!b64.isEmpty())
        pf.data = QByteArray::fromBase64(b64.toUtf8());
    for (const auto &v : obj["platforms"].toArray())
        pf.platforms << v.toString();
    return pf;
}

// ==================== ExploitScript ====================

QJsonObject exploitScriptToJson(const ExploitScript &es)
{
    QJsonObject obj;
    obj["type"] = es.type;
    obj["requires_root"] = es.requiresRoot;
    obj["requires_file_push"] = es.requiresFilePush;
    obj["needs_reboot"] = es.needsReboot;
    obj["timeout_seconds"] = es.timeoutSeconds;

    auto listToArr = [](const QStringList &list) {
        QJsonArray arr;
        for (const auto &s : list) arr.append(s);
        return arr;
    };
    obj["detect"]  = listToArr(es.detect);
    obj["exploit"] = listToArr(es.exploit);
    obj["verify"]  = listToArr(es.verify);

    if (!es.payloadFiles.isEmpty()) {
        QJsonArray arr;
        for (const auto &pf : es.payloadFiles)
            arr.append(payloadFileToJson(pf));
        obj["payload_files"] = arr;
    }
    return obj;
}

ExploitScript exploitScriptFromJson(const QJsonObject &obj)
{
    ExploitScript es;
    es.type = obj["type"].toString("adb_shell");
    es.requiresRoot = obj["requires_root"].toBool();
    es.requiresFilePush = obj["requires_file_push"].toBool();
    es.needsReboot = obj["needs_reboot"].toBool();
    es.timeoutSeconds = obj["timeout_seconds"].toInt(30);

    auto arrToList = [](const QJsonArray &arr) {
        QStringList list;
        for (const auto &v : arr) list << v.toString();
        return list;
    };
    es.detect  = arrToList(obj["detect"].toArray());
    es.exploit = arrToList(obj["exploit"].toArray());
    es.verify  = arrToList(obj["verify"].toArray());

    for (const auto &v : obj["payload_files"].toArray())
        es.payloadFiles << payloadFileFromJson(v.toObject());
    return es;
}

// ==================== VulnEntry ====================

QJsonObject vulnEntryToJson(const VulnEntry &e)
{
    QJsonObject obj;
    obj["id"] = e.id;
    obj["summary"] = e.summary;
    if (!e.description.isEmpty()) obj["description"] = e.description;
    obj["severity"] = e.severity;
    obj["cvss_score"] = e.cvssScore;
    if (!e.cvssVector.isEmpty()) obj["cvss_vector"] = e.cvssVector;
    obj["affected"] = affectedRangeToJson(e.affected);
    obj["exploit"] = exploitScriptToJson(e.exploit);
    if (!e.references.isEmpty()) {
        QJsonArray arr;
        for (const auto &r : e.references) arr.append(r);
        obj["references"] = arr;
    }
    obj["source"] = e.source;
    if (!e.publishedDate.isEmpty()) obj["published_date"] = e.publishedDate;
    obj["is_android_specific"] = e.isAndroidSpecific;
    return obj;
}

VulnEntry vulnEntryFromJson(const QJsonObject &obj)
{
    VulnEntry e;
    e.id = obj["id"].toString();
    e.summary = obj["summary"].toString();
    e.description = obj["description"].toString();
    e.severity = obj["severity"].toString();
    e.cvssScore = obj["cvss_score"].toDouble();
    e.cvssVector = obj["cvss_vector"].toString();
    e.affected = affectedRangeFromJson(obj["affected"].toObject());
    e.exploit = exploitScriptFromJson(obj["exploit"].toObject());
    for (const auto &v : obj["references"].toArray())
        e.references << v.toString();
    e.source = obj["source"].toString();
    e.publishedDate = obj["published_date"].toString();
    e.isAndroidSpecific = obj["is_android_specific"].toBool(true);
    return e;
}
