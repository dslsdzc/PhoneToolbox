#ifndef VULN_DB_H
#define VULN_DB_H

#include <QObject>
#include <QMap>
#include <QList>
#include <QString>
#include <QByteArray>
#include "vuln_entry.h"

class VulnDb : public QObject
{
    Q_OBJECT

public:
    explicit VulnDb(QObject *parent = nullptr);

    // 增删改
    void addEntry(const VulnEntry &entry);
    void addEntries(const QList<VulnEntry> &entries);
    void removeEntry(const QString &cveId);
    void clear();

    // 查询
    VulnEntry getEntry(const QString &cveId) const;
    QList<VulnEntry> getAllEntries() const;
    QList<VulnEntry> getEntriesBySeverity(const QString &severity) const;
    bool hasEntry(const QString &cveId) const;

    // 持久化 (JSON)
    bool loadFromFile(const QString &filePath);
    bool saveToFile(const QString &filePath) const;
    bool loadFromJson(const QByteArray &json);
    QByteArray toJson() const;

    // 统计
    int entryCount() const;
    QStringList severityCounts() const;

signals:
    void changed();

private:
    QMap<QString, VulnEntry> m_entries;
};

#endif // VULN_DB_H
