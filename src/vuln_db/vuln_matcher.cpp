#include "vuln_matcher.h"
#include <QDate>
#include <QVersionNumber>
#include <QRegularExpression>

VulnMatcher::VulnMatcher(const QList<VulnEntry> &entries)
    : m_entries(entries)
{
}

QList<MatchResult> VulnMatcher::matchDevice(const DeviceInfo &device) const
{
    QList<MatchResult> results;

    for (const auto &entry : m_entries) {
        MatchResult mr;
        mr.cveId = entry.id;
        mr.severity = entry.severity;
        mr.cvssScore = entry.cvssScore;
        mr.summary = entry.summary;

        if (!entry.affected.androidVersionMin.isEmpty() ||
            !entry.affected.androidVersionMax.isEmpty()) {
            if (!versionInRange(device.androidVersion, entry.affected)) {
                mr.reason = "Android 版本不在影响范围内";
                mr.isVulnerable = false;
                results.append(mr);
                continue;
            }
        }

        if (!entry.affected.patchLevelBefore.isEmpty()) {
            if (!patchBeforeDeadline(device.securityPatch, entry.affected.patchLevelBefore)) {
                mr.reason = QString("安全补丁 %1 ≥ %2，已修复")
                    .arg(device.securityPatch, entry.affected.patchLevelBefore);
                mr.isVulnerable = false;
                results.append(mr);
                continue;
            }
        }

        if (entry.affected.sdkMin > 0 || entry.affected.sdkMax > 0) {
            int sdk = device.sdkVersion.toInt();
            if ((entry.affected.sdkMin > 0 && sdk < entry.affected.sdkMin) ||
                (entry.affected.sdkMax > 0 && sdk > entry.affected.sdkMax)) {
                mr.reason = "SDK 版本不在影响范围内";
                mr.isVulnerable = false;
                results.append(mr);
                continue;
            }
        }

        if (!entry.affected.platforms.isEmpty()) {
            if (!platformMatches(device, entry.affected.platforms)) {
                mr.reason = "设备型号不匹配";
                mr.isVulnerable = false;
                results.append(mr);
                continue;
            }
        }

        // 所有检查通过
        mr.isVulnerable = true;
        mr.reason = QString("Android %1 补丁 %2 在 %3 之前，符合条件")
            .arg(device.androidVersion,
                 device.securityPatch,
                 entry.affected.patchLevelBefore);
        results.append(mr);
    }

    // 按严重级别排序
    std::sort(results.begin(), results.end(),
        [this](const MatchResult &a, const MatchResult &b) {
            return severityWeight(a.severity) > severityWeight(b.severity);
        });

    return results;
}

bool VulnMatcher::versionInRange(const QString &devVer, const AffectedRange &range) const
{
    // 简单的版本号比较
    auto toInt = [](const QString &v) {
        // "14" -> 14, "12.1" -> 12, "5.0.2" -> 5
        int dot = v.indexOf('.');
        return (dot > 0) ? v.left(dot).toInt() : v.toInt();
    };

    int dev = toInt(devVer);
    int minVer = range.androidVersionMin.isEmpty() ? 0 : toInt(range.androidVersionMin);
    int maxVer = range.androidVersionMax.isEmpty() ? 999 : toInt(range.androidVersionMax);

    return dev >= minVer && dev <= maxVer;
}

bool VulnMatcher::patchBeforeDeadline(const QString &devPatch,
                                       const QString &vulnPatchBefore) const
{
    if (devPatch.isEmpty()) return true; // 无补丁信息 = 假设受影响
    if (vulnPatchBefore.isEmpty()) return true;

    QDate devDate = QDate::fromString(devPatch.left(10), "yyyy-MM-dd");
    QDate vulnDate = QDate::fromString(vulnPatchBefore.left(10), "yyyy-MM-dd");

    if (!devDate.isValid() || !vulnDate.isValid()) return true;

    return devDate < vulnDate;
}

bool VulnMatcher::platformMatches(const DeviceInfo &device,
                                   const QStringList &patterns) const
{
    if (patterns.isEmpty()) return true;

    // 构建匹配字符串: "manufacturer/model"
    QString deviceId = device.manufacturer.toLower() + "/" + device.model.toLower();

    for (const auto &pattern : patterns) {
        QString p = pattern.toLower();
        // 支持通配符: google/pixel_* → 匹配 google/pixel_8, google/pixel_6a 等
        if (p.contains('*')) {
            QString regexStr = QRegularExpression::wildcardToRegularExpression(p);
            QRegularExpression re(regexStr);
            if (re.match(deviceId).hasMatch())
                return true;
            // Also match just the model
            if (re.match(device.model.toLower()).hasMatch())
                return true;
        } else {
            if (deviceId.contains(p))
                return true;
        }
    }
    return false;
}

int VulnMatcher::severityWeight(const QString &severity)
{
    QString s = severity.toUpper();
    if (s == "CRITICAL") return 5;
    if (s == "HIGH")     return 4;
    if (s == "MEDIUM")   return 3;
    if (s == "LOW")      return 2;
    return 1;
}

int VulnMatcher::calculateRiskScore(const QList<MatchResult> &results)
{
    if (results.isEmpty()) return 0;

    int score = 0;
    int maxScore = 0;

    for (const auto &r : results) {
        if (!r.isVulnerable) continue;
        double cvss = r.cvssScore;
        if (cvss <= 0) {
            // 没有 CVSS 分数时用严重级别估算
            if (r.severity.toUpper() == "CRITICAL") cvss = 9.0;
            else if (r.severity.toUpper() == "HIGH") cvss = 7.0;
            else if (r.severity.toUpper() == "MEDIUM") cvss = 5.0;
            else cvss = 3.0;
        }
        score += static_cast<int>(cvss * 10);
        maxScore += 100;
    }

    if (maxScore == 0) return 0;
    return qMin(100, score * 100 / maxScore);
}

QList<MatchResult> VulnMatcher::filterBySeverity(const QList<MatchResult> &results,
                                                   const QString &minSeverity)
{
    int minWeight = 1;
    QString s = minSeverity.toUpper();
    if (s == "CRITICAL") minWeight = 5;
    else if (s == "HIGH") minWeight = 4;
    else if (s == "MEDIUM") minWeight = 3;
    else if (s == "LOW") minWeight = 2;

    QList<MatchResult> filtered;
    for (const auto &r : results) {
        if (severityWeight(r.severity) >= minWeight)
            filtered.append(r);
    }
    return filtered;
}

int VulnMatcher::monthsBehindPatch(const QString &devicePatchDate,
                                    const QString &latestPatchDate)
{
    QDate devDate = QDate::fromString(devicePatchDate.left(10), "yyyy-MM-dd");
    if (!devDate.isValid()) return -1;

    QDate latestDate;
    if (latestPatchDate.isEmpty()) {
        latestDate = QDate::currentDate();
    } else {
        latestDate = QDate::fromString(latestPatchDate.left(10), "yyyy-MM-dd");
    }

    if (!latestDate.isValid()) return -1;

    // QDate::monthsTo removed in Qt6; manual calculation
    return (latestDate.year() - devDate.year()) * 12
         + (latestDate.month() - devDate.month());
}
