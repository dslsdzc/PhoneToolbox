#include "filename_parser.h"
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <QRegularExpression>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>

static const char *kCsvUrl = "https://raw.githubusercontent.com/KHwang9883/MobileModels-csv/main/models.csv";
static const char *kCsvCacheFile = "models.csv";

// ==================== CSV 加载 ====================

static QString cachedCsvPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
           + "/" + kCsvCacheFile;
}

static bool downloadCsv(const QString &destPath)
{
    QNetworkAccessManager mgr;
    QNetworkRequest req{QUrl(kCsvUrl)};
    req.setTransferTimeout(10000);

    QNetworkReply *reply = mgr.get(req);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        return false;
    }

    QByteArray data = reply->readAll();
    reply->deleteLater();

    QDir().mkpath(QFileInfo(destPath).absolutePath());
    QFile f(destPath);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(data);
    return true;
}

static QString loadCsvContent()
{
    QString path = cachedCsvPath();
    QFile f(path);

    // 尝试从缓存加载
    if (f.open(QIODevice::ReadOnly)) {
        return f.readAll();
    }

    // 缓存不存在，下载
    if (!downloadCsv(path))
        return {};

    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

// ==================== 数据库构建 ====================

#include <QMap>
using CodenameMap = QMap<QString, QPair<QString, QString>>; // codename -> (model, manufacturer)

// 检查字符串是否以拉丁字母开头（用于优先选择英文品牌名）
static bool isLatin(const QString &s)
{
    if (s.isEmpty()) return false;
    QChar c = s.at(0);
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static CodenameMap buildCodenameDb(const QString &csvContent)
{
    CodenameMap db;
    if (csvContent.isEmpty()) return db;

    QStringList lines = csvContent.split('\n', Qt::SkipEmptyParts);
    for (int i = 1; i < lines.size(); ++i) { // 跳过标题行
        QString line = lines[i].trimmed();
        if (line.isEmpty()) continue;

        // CSV 简单解析（不处理引号内逗号，但 models.csv 无此情况）
        QStringList fields;
        int start = 0;
        for (int j = 0; j < line.size(); ++j) {
            if (line[j] == ',') {
                fields.append(line.mid(start, j - start));
                start = j + 1;
            }
        }
        fields.append(line.mid(start)); // 最后一个字段

        // model,dtype,brand,brand_title,code,code_alias,model_name,ver_name
        // 0     1     2     3           4    5          6          7
        if (fields.size() < 8) continue;

        QString codename = fields[5].trimmed();
        QString modelName = fields[6].trimmed();
        QString brandTitle = fields[3].trimmed();

        if (codename.isEmpty() || modelName.isEmpty())
            continue;

        // 优先保留英文品牌名
        auto it = db.find(codename);
        if (it == db.end()) {
            db[codename] = {modelName, brandTitle};
        } else if (!isLatin(it.value().second) && isLatin(brandTitle)) {
            // 已有中文名，遇到英文名则替换
            db[codename] = {modelName, brandTitle};
        }
    }

    return db;
}

static const CodenameMap& codenameDb()
{
    static CodenameMap db = buildCodenameDb(loadCsvContent());
    return db;
}

// ==================== 模式匹配 ====================

RomInfo FilenameParser::parse(const QString &fileName)
{
    RomInfo info;
    QFileInfo fi(fileName);
    QString name = fi.fileName();

    // 支持的文件扩展名
    QString lower = name.toLower();
    if (lower.endsWith(".zip"))
        name = name.left(name.length() - 4);
    else if (lower.endsWith(".tar.md5"))
        name = name.left(name.length() - 8);
    else if (lower.endsWith(".tar.gz") || lower.endsWith(".tgz"))
        name = name.left(name.length() - (lower.endsWith(".tar.gz") ? 7 : 4));
    else if (lower.endsWith(".gz"))
        name = name.left(name.length() - 3);
    else if (lower.endsWith(".tar"))
        name = name.left(name.length() - 4);
    else if (lower.endsWith(".br"))
        name = name.left(name.length() - 3);
    else if (lower.endsWith(".img"))
        name = name.left(name.length() - 4);

    if (name.isEmpty())
        return info;

    info.romType = detectRomType(name);
    const auto &db = codenameDb();

    // 1. MIUI: miui_<codename>_<version>_<android>[_<hash>]
    {
        QRegularExpression re("^miui_(\\w+)_([\\d.]+)_(\\d+)(?:_[a-fA-F0-9]+)?$");
        auto m = re.match(name);
        if (m.hasMatch()) {
            info.codename = m.captured(1);
            info.buildVersion = m.captured(2);
            info.androidVersion = m.captured(3);
            info.manufacturer = lookupManufacturer(info.codename);
            info.model = lookupModel(info.codename);
            if (info.romType.isEmpty()) info.romType = "MIUI";
            info.valid = true;
            return info;
        }
    }

    // 2. LineageOS: lineage-<version>-<date>-<codename>
    {
        QRegularExpression re("^lineage[-_]([\\d.]+)?[-_]?(\\d{8})?[-_]?(\\w+)$");
        auto m = re.match(name);
        if (m.hasMatch()) {
            info.buildVersion = m.captured(1);
            QString dateStr = m.captured(2);
            if (info.buildVersion.isEmpty()) info.buildVersion = dateStr;
            info.codename = m.captured(3);
            info.manufacturer = lookupManufacturer(info.codename);
            info.model = lookupModel(info.codename);
            if (info.romType.isEmpty()) info.romType = "LineageOS";
            info.valid = true;
            return info;
        }
    }

    // 3. PixelExperience: PixelExperience_<codename>-<version>
    {
        QRegularExpression re("^PixelExperience_?(\\w+)[-_]([\\d.]+)");
        auto m = re.match(name);
        if (m.hasMatch()) {
            info.codename = m.captured(1);
            info.buildVersion = m.captured(2);
            info.manufacturer = lookupManufacturer(info.codename);
            info.model = lookupModel(info.codename);
            info.romType = "PixelExperience";
            info.valid = true;
            return info;
        }
    }

    // 4. crDroid: crDroidAndroid-<ver>-<date>-<codename>
    {
        QRegularExpression re("^crDroidAndroid[-_]([\\d.]+)[-_](\\d{8})[-_](\\w+)");
        auto m = re.match(name);
        if (m.hasMatch()) {
            info.buildVersion = m.captured(1);
            info.codename = m.captured(3);
            info.manufacturer = lookupManufacturer(info.codename);
            info.model = lookupModel(info.codename);
            info.romType = "crDroid";
            info.valid = true;
            return info;
        }
    }

    // 5. OnePlus: OnePlus<model>_<version>
    {
        QRegularExpression re("^OnePlus(\\w+)[-_]([\\d.]+)");
        auto m = re.match(name);
        if (m.hasMatch()) {
            info.model = QString("OnePlus %1").arg(m.captured(1));
            info.buildVersion = m.captured(2);
            info.manufacturer = "OnePlus";
            if (info.romType.isEmpty()) info.romType = "OxygenOS";
            info.valid = true;
            return info;
        }
    }

    // 6. Samsung: SM-<model>_<anything>_<android>
    {
        QRegularExpression re("^(SM-\\w+)[_-](.+?)[_-](\\d+)$");
        auto m = re.match(name);
        if (m.hasMatch()) {
            info.model = m.captured(1);
            info.buildVersion = m.captured(2);
            info.androidVersion = m.captured(3);
            info.manufacturer = "Samsung";
            if (info.romType.isEmpty()) info.romType = "OneUI";
            info.valid = true;
            return info;
        }
    }

    // 7. Oplus: oplus_<codename>_<version>
    {
        QRegularExpression re("^oplus[-_](\\w+)[-_]([\\d.]+)");
        auto m = re.match(name);
        if (m.hasMatch()) {
            info.codename = m.captured(1);
            info.buildVersion = m.captured(2);
            info.manufacturer = lookupManufacturer(info.codename);
            info.model = lookupModel(info.codename);
            if (info.manufacturer.isEmpty()) info.manufacturer = "OPPO";
            if (info.romType.isEmpty()) info.romType = "ColorOS";
            info.valid = true;
            return info;
        }
    }

    // 8. Generic: <codename>_<version>
    {
        QRegularExpression re("^(\\w+)[-_]([\\d.]+(?:[\\d.]+)?)$");
        auto m = re.match(name);
        if (m.hasMatch()) {
            info.codename = m.captured(1);
            info.buildVersion = m.captured(2);
            info.manufacturer = lookupManufacturer(info.codename);
            info.model = lookupModel(info.codename);
            if (info.manufacturer.isEmpty() && info.codename.startsWith("RMX"))
                info.manufacturer = "Realme";
            if (!info.manufacturer.isEmpty() || info.codename.startsWith("RMX")) {
                info.valid = true;
                return info;
            }
        }
    }

    // 9. Pure codename match
    if (db.contains(name)) {
        info.codename = name;
        info.manufacturer = lookupManufacturer(name);
        info.model = lookupModel(name);
        info.valid = true;
        return info;
    }

    // 10. Extract Android version from name
    {
        QRegularExpression re("(\\d{2})");
        auto m = re.match(name);
        if (m.hasMatch()) {
            info.androidVersion = m.captured(1);
        }
    }

    return info;
}

QString FilenameParser::detectRomType(const QString &name)
{
    QString lower = name.toLower();
    if (lower.startsWith("miui")) return "MIUI";
    if (lower.startsWith("lineage")) return "LineageOS";
    if (lower.startsWith("pixelexperience")) return "PixelExperience";
    if (lower.startsWith("crdroid")) return "crDroid";
    if (lower.startsWith("havoc")) return "HavocOS";
    if (lower.startsWith("derpfest")) return "DerpFest";
    if (lower.startsWith("evolution")) return "EvolutionX";
    if (lower.startsWith("arrow")) return "ArrowOS";
    if (lower.startsWith("aosp")) return "AOSP";
    if (lower.startsWith("paranoid")) return "ParanoidAndroid";
    if (lower.startsWith("dotos")) return "dotOS";
    if (lower.startsWith("projectelixir")) return "ProjectElixir";
    if (lower.startsWith("octavi")) return "OctaviOS";
    if (lower.startsWith("spark")) return "SparkOS";
    if (lower.startsWith("colt")) return "ColtOS";
    if (lower.startsWith("bliss")) return "BlissROM";
    if (lower.startsWith("xiaomi")) return "Xiaomi";
    if (lower.startsWith("oplus") || lower.startsWith("oneplus")) return "OxygenOS";
    if (lower.startsWith("sm-")) return "Samsung";
    return "";
}

QString FilenameParser::lookupModel(const QString &codename)
{
    const auto &db = codenameDb();
    auto it = db.find(codename);
    return it != db.end() ? it.value().first : "";
}

QString FilenameParser::lookupManufacturer(const QString &codename)
{
    const auto &db = codenameDb();
    auto it = db.find(codename);
    return it != db.end() ? it.value().second : "";
}
