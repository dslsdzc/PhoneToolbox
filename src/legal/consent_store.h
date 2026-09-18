// src/legal/consent_store.h
//
// 同意留痕 —— W2「用户协议与风险分级门控」。
//
// 为什么单独抽出来：这是"用户同意过什么"的**唯一记录**，也是二次确认门控的**唯一依据** ——
// 判据放松一处（版本比较变成"非零即同意"、两个 tier 共用一把键），用户就会在没确认过的情况下
// 进入写操作。把它做成只依赖 QSettings 的半纯类（settings 可注入）后，全部语义都能用临时 ini
// 离线钉住（tests/test_consent_store.cpp），不必碰真实用户配置、也不必构造窗口。
//
// 存储：QSettings（本仓首次引入）。默认 UserScope + IniFormat（跨平台路径由 Qt 决定），
// **不联网、不上传** —— 协议正文第七条已向用户承诺，改这里之前先看那一节。
// 键（三把，逐字是契约：用例直写同名键、文档按此说明）：
//   legal/agreementVersion —— 首屏「使用须知」的留痕（值为接受时的 kAgreementVersion）
//   legal/tier2Version     —— Tier2「底层引导与写操作」的留痕
//   legal/tier3Version     —— Tier3「诊断与安全研究」的留痕
// 判据一律是 `存储值 >= kAgreementVersion`：协议升版（kAgreementVersion 增大）→ 三处都要重新确认；
// 存储值更高（更严格地说：>=）不回退——旧版程序读到未来版本写下的留痕时应视为已接受，不该反过来
// 把用户已经同意过的更严版本降级成"未接受"。
#pragma once

#include <QString>

#include "ui/risk_ui.h"    // 只取 RiskTier 枚举；risk_ui 不反向依赖 legal

class QSettings;

namespace legal {

// 使用须知版本。**必须与 resources/legal/user-agreement.md 正文里的「版本 N」一致** ——
// 三路比对（spec 正文 ↔ 资源文件 ↔ 本常量）由 Task 3 的用例钉住。
constexpr int kAgreementVersion = 1;

class ConsentStore
{
public:
    // settings == nullptr → 自建 UserScope 的 IniFormat 存储（生产的默认分支）。
    // 传入非空指针时**不接管所有权**（用例指向临时 ini；调用方保证其生命周期覆盖本对象）。
    explicit ConsentStore(QSettings *settings = nullptr);
    ~ConsentStore();
    ConsentStore(const ConsentStore &) = delete;
    ConsentStore &operator=(const ConsentStore &) = delete;

    bool hasAcceptedAgreement() const;   // 存储值 >= kAgreementVersion（无留痕 → false）
    void acceptAgreement();              // 写入当前版本并落盘

    // None → true（该档无需确认，不弹窗）。其余档同款 `>= kAgreementVersion`。
    bool hasAcceptedTier(riskui::RiskTier t) const;
    // None → **不写**（None 不是一次"确认"，留痕无从谈起；调用方也不该为它弹窗）。
    void acceptTier(riskui::RiskTier t);

private:
    // Tier2Boot → "legal/tier2Version"，Tier3Diagnostic → "legal/tier3Version"，None → 空串
    // （空串永不落到 QSettings —— 两个公开入口都先短路 None）。
    static QString keyForTier(riskui::RiskTier t);

    QSettings *m_settings = nullptr;
    bool m_owned = false;
};

} // namespace legal
