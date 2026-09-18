// src/legal/consent_store.cpp
//
// 留痕的读写实现。语义与理由见头文件；本文件只做三件事：选存储、比版本、把写入落盘。
//
// 为什么每次写都 sync()：这是**同意记录**，不是缓存。用户点了"同意"就该落盘再返回 ——
// 留痕只活在 QSettings 的内存缓冲里、进程被杀（或崩溃）就没了，等于没收过这次确认：
// 下次启动会再弹一次，而用户以为自己已经同意过了。sync() 的开销只发生在用户动作之后
// （acceptAgreement/acceptTier），不在轮询/启动路径上。
#include "legal/consent_store.h"

#include <QCoreApplication>
#include <QSettings>

namespace legal {
namespace {

// 键名（逐字契约，见头文件）。用函数返回而不是全局 QString：省掉静态初始化/析构顺序问题。
QString agreementKey() { return QStringLiteral("legal/agreementVersion"); }
QString tier2Key()     { return QStringLiteral("legal/tier2Version"); }
QString tier3Key()     { return QStringLiteral("legal/tier3Version"); }

} // namespace

ConsentStore::ConsentStore(QSettings *settings)
    : m_settings(settings)
    , m_owned(false)
{
    if (!m_settings) {
        // 生产的默认分支：与 src/main.cpp 的 setOrganizationName("PhoneToolbox") 配套。
        m_settings = new QSettings(QSettings::IniFormat, QSettings::UserScope,
                                   QCoreApplication::organizationName(),
                                   QCoreApplication::applicationName());
        m_owned = true;
    }
}

ConsentStore::~ConsentStore()
{
    if (m_owned)
        delete m_settings;   // 注入的 settings 归调用方所有，不在此删
}

bool ConsentStore::hasAcceptedAgreement() const
{
    // 无留痕 → value() 取默认 0 → 0 >= kAgreementVersion（= 1）为 false。
    // **不写**任何东西：同意只能来自用户动作，不能由"读过一次"产生。
    return m_settings->value(agreementKey(), 0).toInt() >= kAgreementVersion;
}

void ConsentStore::acceptAgreement()
{
    m_settings->setValue(agreementKey(), kAgreementVersion);
    m_settings->sync();
}

// 两个 tier 各有一把键是**有意**的：接受 Tier2（底层写操作）不该顺带放行 Tier3（诊断/绕过），
// 反之亦然 —— 用户在两次不同的场景下做了两次不同的判断。
QString ConsentStore::keyForTier(riskui::RiskTier t)
{
    // 无 default 是故意的：新增 RiskTier 而未登记时，`-Wswitch` 点名未处理的枚举值，
    // 这里把它升级为编译错误（漏登记的档会落进"空串键"→ 判据恒 false → 每次操作都弹窗，
    // 属可察觉的失败，但不如编译期就拦住）。⚠️ 与 risk_ui.cpp 同款边界：**该机制只在
    // GCC/Clang 生效**（MSVC 上静默走到函数末尾的 return QString()）。
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic error "-Wswitch"
#endif
    switch (t) {
    case riskui::RiskTier::Tier2Boot:       return tier2Key();
    case riskui::RiskTier::Tier3Diagnostic: return tier3Key();
    case riskui::RiskTier::None:            return QString();    // 无需确认的档：没有留痕键
    }
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
    return QString();   // 不可达（枚举已穷尽）；保留以免 UB
}

bool ConsentStore::hasAcceptedTier(riskui::RiskTier t) const
{
    if (t == riskui::RiskTier::None)
        return true;   // 该档本来就无需确认 —— 门控据此不弹窗
    return m_settings->value(keyForTier(t), 0).toInt() >= kAgreementVersion;
}

void ConsentStore::acceptTier(riskui::RiskTier t)
{
    if (t == riskui::RiskTier::None)
        return;        // None 不是一次"确认"：不写留痕（写进去只会让"用户同意过什么"失真）
    m_settings->setValue(keyForTier(t), kAgreementVersion);
    m_settings->sync();
}

} // namespace legal
