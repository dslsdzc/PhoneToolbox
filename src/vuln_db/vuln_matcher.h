#ifndef VULN_MATCHER_H
#define VULN_MATCHER_H

#include <QList>
#include <QString>
#include "vuln_db/vuln_entry.h"
#include "core/device_info.h"

struct MatchResult {
    QString cveId;
    QString severity;
    double cvssScore = 0.0;
    QString summary;
    bool isVulnerable = false;
    QString reason;               // 匹配依据描述
};

class VulnMatcher
{
public:
    explicit VulnMatcher(const QList<VulnEntry> &entries);

    // 对单个设备执行全套匹配
    QList<MatchResult> matchDevice(const DeviceInfo &device) const;

    // 风险评分 (0-100)
    static int calculateRiskScore(const QList<MatchResult> &results);

    // 按严重级别筛选
    static QList<MatchResult> filterBySeverity(const QList<MatchResult> &results,
                                                const QString &minSeverity);

    // 补丁延期月数计算
    static int monthsBehindPatch(const QString &devicePatchDate,
                                 const QString &latestPatchDate = {});

private:
    const QList<VulnEntry> &m_entries;

    bool versionInRange(const QString &devVer, const AffectedRange &range) const;
    bool patchBeforeDeadline(const QString &devPatch, const QString &vulnPatchBefore) const;
    bool platformMatches(const DeviceInfo &device, const QStringList &patterns) const;
    static int severityWeight(const QString &severity);
};

#endif // VULN_MATCHER_H
