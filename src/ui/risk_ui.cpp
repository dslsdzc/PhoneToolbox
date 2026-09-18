// src/ui/risk_ui.cpp
//
// 分级映射与文案的**唯一来源**。映射表另有一份独立手写的期望表钉在
// tests/test_risk_tiers.cpp 里：这里改一档、那里没跟着改 → 用例红。
#include "ui/risk_ui.h"

namespace riskui {

RiskTier riskTierForMode(DeviceDetector::DeviceMode mode)
{
    // 无 default 是**故意的**：新增 DeviceMode 而未登记时，`-Wswitch` 点名未处理的枚举值，
    // 且下面把它升级为**编译错误**（漏登记 = 该入口静默不弹确认，正是本特性要防的缺陷，
    // 不能让它在运行期才显形）。变异②（.superpowers/sdd/w2-task-1-mutation-logs/）就是对
    // "临时追加一个模式取值 → 编译失败"的实测留档。
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic error "-Wswitch"
#endif
    switch (mode) {
    case DeviceDetector::MODE_UNKNOWN:            return RiskTier::None;         // 无设备 = 无操作
    case DeviceDetector::MODE_ADB:                return RiskTier::Tier2Boot;    // 面板在 ADB 下也刷入/擦除
    case DeviceDetector::MODE_FASTBOOT:           return RiskTier::Tier2Boot;
    case DeviceDetector::MODE_FASTBOOTD:          return RiskTier::Tier2Boot;
    case DeviceDetector::MODE_EDL_9008:           return RiskTier::Tier2Boot;
    case DeviceDetector::MODE_MTK_DA:             return RiskTier::Tier2Boot;
    case DeviceDetector::MODE_RECOVERY:           return RiskTier::Tier2Boot;
    case DeviceDetector::MODE_MTK_BROM:           return RiskTier::Tier2Boot;
    case DeviceDetector::MODE_HUAWEI_USB_UPDATE:  return RiskTier::Tier2Boot;
    case DeviceDetector::MODE_SPD:                return RiskTier::Tier2Boot;
    case DeviceDetector::MODE_SAMSUNG_ODIN:       return RiskTier::Tier2Boot;
    case DeviceDetector::MODE_SAMSUNG_EUB:        return RiskTier::Tier2Boot;    // 救援也是底层写路径
    }
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
    // 不可达（上面的 switch 已穷尽枚举）；保留以免"漏登记 + 编译器未把 -Wswitch 当错"时 UB。
    // 走到这里 = 有模式没登记，保守取 None 会静默放行，故这条路径只应出现在下面的变异实验里。
    return RiskTier::None;
}

RiskTier riskTierForFeature(RiskFeature f)
{
    // 与 riskTierForMode 同款编译期防线（新增 RiskFeature 未登记 → 编译错误）。
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic error "-Wswitch"
#endif
    switch (f) {
    // 写操作族
    case RiskFeature::DieRepair:      return RiskTier::Tier2Boot;
    case RiskFeature::FrpClear:       return RiskTier::Tier2Boot;
    case RiskFeature::PartitionFlash: return RiskTier::Tier2Boot;
    case RiskFeature::Unlock:         return RiskTier::Tier2Boot;
    case RiskFeature::Sideload:       return RiskTier::Tier2Boot;
    case RiskFeature::AdbRoot:        return RiskTier::Tier2Boot;
    case RiskFeature::MtkUnlock:      return RiskTier::Tier2Boot;
    // 诊断与安全研究族（绕过安全机制的脚本可执行 —— 计划正文第四条已明写其法律边界）
    case RiskFeature::VulnExploit:    return RiskTier::Tier3Diagnostic;
    case RiskFeature::EngineerMode:   return RiskTier::Tier3Diagnostic;
    case RiskFeature::SafetyNet:      return RiskTier::Tier3Diagnostic;
    case RiskFeature::SignatureSpoof: return RiskTier::Tier3Diagnostic;
    case RiskFeature::ImeiTool:       return RiskTier::Tier3Diagnostic;
    }
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
    return RiskTier::None;   // 不可达（枚举已穷尽）
}

QString tierTitle(RiskTier t)
{
    switch (t) {
    case RiskTier::None:           return QString();                        // 空 = 该档不弹窗
    case RiskTier::Tier2Boot:      return QStringLiteral("底层引导与写操作");
    case RiskTier::Tier3Diagnostic: return QStringLiteral("诊断与安全研究");
    }
    return QString();   // 不可达（枚举已穷尽）
}

QStringList tierWarnings(RiskTier t)
{
    switch (t) {
    case RiskTier::None:
        return QStringList();          // 不弹窗 → 无要点
    case RiskTier::Tier2Boot:
        return {
            QStringLiteral("这些操作直接写设备存储或底层引导，失败可能导致设备变砖、数据不可恢复。"),
            QStringLiteral("请先确认机型与固件匹配，并确认设备属于你或已获授权。"),
            QStringLiteral("部分通道未经真机验证（见项目文档「诚实边界」），实际行为可能与预期不同。"),
        };
    case RiskTier::Tier3Diagnostic:
        return {
            QStringLiteral("漏洞扫描与利用脚本会尝试绕过设备安全机制。"),
            QStringLiteral("未经设备所有者同意对他人设备使用，可能违法。"),
            QStringLiteral("脚本来自第三方数据库，本工具不对其效果与后果负责。"),
        };
    }
    return QStringList();   // 不可达（枚举已穷尽）
}

} // namespace riskui
