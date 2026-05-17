#ifndef LOCAL_IMPORTER_H
#define LOCAL_IMPORTER_H

#include "vuln_db/vuln_importer.h"

// Local JSON importer
// 从本地 vulndb.json 文件或 ZIP 包导入完整漏洞条目（含 exploit 脚本）
class LocalImporter : public VulnImporter
{
public:
    QString sourceName() const override { return "Local Exploit Pack"; }
    QString sourceKey() const override { return "local"; }

    ImportResult importFromUrl(const QString &url, QList<VulnEntry> &entries) override;
    ImportResult importFromFile(const QString &path, QList<VulnEntry> &entries) override;

private:
    ImportResult importFromJsonData(const QByteArray &data, QList<VulnEntry> &entries);
    ImportResult importFromZip(const QString &zipPath, QList<VulnEntry> &entries);
};

#endif // LOCAL_IMPORTER_H
