#include "vuln_db.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QDateTime>

VulnDb::VulnDb(QObject *parent)
    : QObject(parent)
{
}

void VulnDb::addEntry(const VulnEntry &entry)
{
    m_entries[entry.id] = entry;
    emit changed();
}

void VulnDb::addEntries(const QList<VulnEntry> &entries)
{
    for (const auto &e : entries)
        m_entries[e.id] = e;
    emit changed();
}

void VulnDb::removeEntry(const QString &cveId)
{
    m_entries.remove(cveId);
    emit changed();
}

void VulnDb::clear()
{
    m_entries.clear();
    emit changed();
}

VulnEntry VulnDb::getEntry(const QString &cveId) const
{
    return m_entries.value(cveId);
}

QList<VulnEntry> VulnDb::getAllEntries() const
{
    return m_entries.values();
}

QList<VulnEntry> VulnDb::getEntriesBySeverity(const QString &severity) const
{
    QList<VulnEntry> result;
    QString s = severity.toUpper();
    for (const auto &e : m_entries) {
        if (e.severity.toUpper() == s)
            result.append(e);
    }
    return result;
}

bool VulnDb::hasEntry(const QString &cveId) const
{
    return m_entries.contains(cveId);
}

bool VulnDb::loadFromFile(const QString &filePath)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly))
        return false;

    QByteArray data = f.readAll();
    f.close();
    return loadFromJson(data);
}

bool VulnDb::saveToFile(const QString &filePath) const
{
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly))
        return false;

    f.write(toJson());
    f.close();
    return true;
}

bool VulnDb::loadFromJson(const QByteArray &json)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (err.error != QJsonParseError::NoError)
        return false;

    QJsonObject root = doc.object();
    QJsonArray arr = root["entries"].toArray();
    QList<VulnEntry> entries;
    for (const auto &v : arr) {
        QJsonObject obj = v.toObject();
        // Support both flat entries and { "entry": {...} } wrappers
        if (obj.contains("entry"))
            obj = obj["entry"].toObject();
        entries.append(vulnEntryFromJson(obj));
    }

    addEntries(entries);
    return true;
}

QByteArray VulnDb::toJson() const
{
    QJsonObject root;
    root["vuln_db_version"] = "2.0.0";
    root["created"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);

    QJsonObject meta;
    meta["name"] = "PhoneToolbox Vulnerability Database";
    meta["entry_count"] = m_entries.size();
    root["metadata"] = meta;

    QJsonArray arr;
    for (const auto &e : m_entries)
        arr.append(vulnEntryToJson(e));
    root["entries"] = arr;

    QJsonDocument doc(root);
    return doc.toJson(QJsonDocument::Indented);
}

int VulnDb::entryCount() const
{
    return m_entries.size();
}

QStringList VulnDb::severityCounts() const
{
    QMap<QString, int> counts;
    for (const auto &e : m_entries)
        counts[e.severity.toUpper()]++;

    QStringList result;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
        result << QString("%1: %2").arg(it.key()).arg(it.value());
    return result;
}
