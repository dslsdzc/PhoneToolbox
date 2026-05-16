#ifndef FILENAME_PARSER_H
#define FILENAME_PARSER_H

#include <QString>

struct RomInfo
{
    QString manufacturer;
    QString model;
    QString codename;
    QString androidVersion;
    QString buildVersion;
    QString romType;
    bool valid = false;
};

class FilenameParser
{
public:
    static RomInfo parse(const QString &fileName);

private:
    static QString detectRomType(const QString &name);
    static QString lookupModel(const QString &codename);
    static QString lookupManufacturer(const QString &codename);
};

#endif // FILENAME_PARSER_H
