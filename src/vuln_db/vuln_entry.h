#ifndef VULN_ENTRY_H
#define VULN_ENTRY_H

#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>
#include <QByteArray>

// 利用脚本 — 三级 ADB shell 脚本
struct ExploitScript {
    QString type = "adb_shell";
    bool requiresRoot = false;
    bool requiresFilePush = false;
    bool needsReboot = false;
    int timeoutSeconds = 30;

    QStringList detect;   // 检测脚本: 最后一行必须输出 VULNERABLE / NOT_VULNERABLE
    QStringList exploit;  // 利用脚本
    QStringList verify;   // 验证脚本: 最后一行必须输出 EXPLOIT_SUCCESS / EXPLOIT_FAILED

    struct PayloadFile {
        QString name;
        QString remotePath;
        QString mode = "755";
        QByteArray data;
        QStringList platforms; // e.g. ["arm64-v8a", "armeabi-v7a"]
    };
    QList<PayloadFile> payloadFiles;
};

// 受影响范围
struct AffectedRange {
    QString androidVersionMin;
    QString androidVersionMax;
    QString patchLevelBefore;   // yyyy-MM-dd
    int sdkMin = 0;
    int sdkMax = 0;
    QStringList platforms;      // 通配符匹配: "google/pixel_*"
};

// 漏洞条目
struct VulnEntry {
    QString id;                 // CVE-2025-12345
    QString summary;
    QString description;
    QString severity;           // CRITICAL / HIGH / MEDIUM / LOW
    double cvssScore = 0.0;
    QString cvssVector;
    AffectedRange affected;
    ExploitScript exploit;
    QStringList references;
    QString source;             // "ASB", "NVD", "local"
    QString publishedDate;      // yyyy-MM-dd
    bool isAndroidSpecific = true;

    enum State { UNKNOWN, VULNERABLE, NOT_VULNERABLE, EXPLOITED, FAILED };
    State state = UNKNOWN;
};

// JSON 序列化
QJsonObject affectedRangeToJson(const AffectedRange &r);
AffectedRange affectedRangeFromJson(const QJsonObject &obj);

QJsonObject payloadFileToJson(const ExploitScript::PayloadFile &pf);
ExploitScript::PayloadFile payloadFileFromJson(const QJsonObject &obj);

QJsonObject exploitScriptToJson(const ExploitScript &es);
ExploitScript exploitScriptFromJson(const QJsonObject &obj);

QJsonObject vulnEntryToJson(const VulnEntry &e);
VulnEntry vulnEntryFromJson(const QJsonObject &obj);

#endif // VULN_ENTRY_H
