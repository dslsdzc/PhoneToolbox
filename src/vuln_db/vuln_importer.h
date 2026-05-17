#ifndef VULN_IMPORTER_H
#define VULN_IMPORTER_H

#include <QString>
#include <QStringList>
#include "vuln_entry.h"

struct ImportResult {
    bool success = false;
    int count = 0;
    QString error;
};

class VulnImporter
{
public:
    virtual ~VulnImporter() = default;
    virtual QString sourceName() const = 0;
    virtual QString sourceKey() const = 0;
    virtual ImportResult importFromUrl(const QString &url, QList<VulnEntry> &entries) = 0;
    virtual ImportResult importFromFile(const QString &path, QList<VulnEntry> &entries) = 0;
};

#endif // VULN_IMPORTER_H
