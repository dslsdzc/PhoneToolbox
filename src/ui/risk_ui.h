// src/ui/risk_ui.h
//
// 风险分级映射（纯函数，不碰 Widgets / libusb）—— W2「用户协议与风险分级门控」。
//
// 为什么单独抽出来：门控的正确性等价于"每个入口都落到正确的 tier" —— 返回 `None` 就不弹二次
// 确认，对应入口直接执行写入/绕过动作。把映射做成只依赖 `DeviceDetector::DeviceMode` 与
// `RiskFeature` 两个枚举的纯函数后，这张表可以被普通用例逐枚举钉住
// （tests/test_risk_tiers.cpp），不必构造窗口/设备 —— 与 `flashui::flashButtonLabelsFor` 同款分层。
//
// 分档（W2 设计 §3，本版本实际存在的功能）：
//   None            —— 不弹（MODE_UNKNOWN；以及将来新登记但未纳入分级的入口）
//   Tier2Boot       —— 底层引导与写操作（协议通道 / 刷写 / 擦除 / 解锁 / Root / Sideload）
//   Tier3Diagnostic —— 诊断与安全研究（漏洞扫描与利用 / 工程模式 / SafetyNet / 签名与 IMEI 相关）
// 1 留给首屏「使用须知」，故 Tier2/Tier3 的数值是 2/3（编号即用户可见的级别序）。
#pragma once

#include <QString>
#include <QStringList>

#include "core/device_detector.h"

namespace riskui {

enum class RiskTier { None = 0, Tier2Boot = 2, Tier3Diagnostic = 3 };

enum class RiskFeature {
    DieRepair, FrpClear, PartitionFlash, Unlock, Sideload, AdbRoot, MtkUnlock,   // → Tier2Boot
    VulnExploit, EngineerMode, SafetyNet, SignatureSpoof, ImeiTool              // → Tier3Diagnostic
};

// 设备模式 → tier。**全枚举覆盖**：除 MODE_UNKNOWN 外全部 Tier2Boot —— FlashPanel 在每个协议
// 通道（含 ADB / Recovery 模式下的刷入、擦除）都是底层写路径。
// 新增 DeviceMode 而未在本函数登记：**GCC/Clang 上是编译错误**（switch 无 default + `-Wswitch`
// 升级为 error，机制验证见 .superpowers/sdd/w2-task-1-mutation-logs/mutation-2-*），
// **MSVC 上不报错并静默返回 None**（= 新危险模式不弹二次确认）—— 详见 risk_ui.cpp 里的 ⚠️ 注释。
RiskTier   riskTierForMode(DeviceDetector::DeviceMode mode);

// 功能入口 → tier（分组见枚举注释）。新增 RiskFeature 同样必须显式登记（同款编译期机制，
// 同款 MSVC 边界）。
RiskTier   riskTierForFeature(RiskFeature f);

// 对话框标题：Tier2Boot="底层引导与写操作" Tier3Diagnostic="诊断与安全研究" None=""（空 = 不弹）。
QString    tierTitle(RiskTier t);

// 二次确认对话框里逐条展示的风险要点；None → 空表（不弹）。
// 文案是**唯一来源**：对话框直接展示，别处不要再抄一份（用例逐字钉住，改字会红）。
QStringList tierWarnings(RiskTier t);

} // namespace riskui
