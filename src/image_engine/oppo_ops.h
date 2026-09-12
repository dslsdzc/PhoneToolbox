#pragma once
// OPPO OPS 包识别与解析（Phase A，设计 spec docs/superpowers/specs/2026-09-03-oppo-unpack-phase-a-design.md
// §2.3 OPS 变体 / §3.3 oppo_ops）
//
// .ops（OnePlus MSM 包）与 .ofp-QC 是兄弟族，尾页共用 +0x10 的 0x7CEF 魔数；OPS 的判据是
// QC 判据的严格加强（额外要求 +0x00 version==2 与 +0x04 flags==1）→ 探测顺序必须先 OPS
// 后 OFP（修订 A11，Task 6 的 worker 按此序接线；本模块只负责判据精确）。
//
// 结构（spec 速查表 §OPS / opscrypto.py）:
//   尾页末 0x200: +0x00 version=2, +0x04 flags=1, +0x10 0x7CEF, +0x14 settings.xml 扇区位置,
//                 +0x18 settings.xml 密文长度(明文长度), +0x1C project id[16], +0x2C firmware 名
//   物理布局:     [SAHARA 组(加密)] [UFS_PROVISION(明文)] [Program 组(明文)] [settings.xml(加密)] [尾页]
//   settings.xml: 分区清单 XML（根 ProFile），密钥试解判定 = 解出内容含 "<?xml" 或 "xml "
//   密码:         非标准流密码 → opsDecrypt()（oppo_keys.h，A3）
//
// 出处（双源核对）: reference/oppo_decrypt/opscrypto.py（bkerler, MIT；extractxml() L404-419
// 与 main() L555-642）与 reference/FirmwareKit.Oppo/FirmwareKit.OpsReader（Uotan-Dev, MIT；
// OppOpsParser.cs / OpsFormatParser.cs / Models/OppHeader.cs 的 OpsTailPage）。
//
// 本模块只识别 + 解析（不落盘）；分区提取见 extractOPS（oppo_extract.h）。
// 纯函数模块：无 QObject；失败经 bool + QString *error 上报（中文文案，error 允许为 nullptr）。
#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace imgopp {

// settings.xml 清单中的一个文件条目 —— 字段语义即 extractOPS 搬运时的指令。
// 组语义出处 opscrypto.py main() L589-638 的三个分支。
struct OpsEntry
{
    QString name;          // Path 属性（SAHARA/UFS_PROVISION）/ filename 属性（Program）
    quint64 offset = 0;    // 包内绝对偏移（字节）= FileOffsetInSrc × 0x200（L594 等）
    quint64 size = 0;      // 落盘长度（字节）= SizeInByteInSrc（缺失时回退 SizeInSectorInSrc × 0x200）
    bool decrypt = false;  // true = SAHARA 组整段解密（L590-597）；false = 原样拷贝（L598-638）
    QString sha256Hex;     // Program 的 Sha256 属性；空 = 不校验（SAHARA/UFS_PROVISION 恒空）
    bool sparse = false;   // Program 的 sparse="true" → 跳过 sha256 校验（L612+622）
};

struct OpsInfo
{
    QString projectId;                 // 尾页 +0x1C 16B ASCII（首个 0x00 截断）
    QString firmwareName;              // 尾页 +0x2C 32B ASCII（C# OpsTailPage 口径）
    quint64 settingsOffset = 0;        // settings.xml 密文起点（字节）= 尾页 +0x14 × 0x200
    quint32 settingsLength = 0;        // 尾页 +0x18：settings.xml 明文长度（未按 16/0x200 对齐）
    QString keyId;                     // 命中的 mbox 候选 id（mbox5/mbox6/mbox4）
    QByteArray mboxBlob;               // 命中的 62B 轮密钥材料（extractOPS 解密用；同 OfpInfo::key/iv 角色）
    QList<OpsEntry> entries;           // 清单条目（文件序 = XML 文档序；同 OfpInfo::files 角色）
};

// OPS 尾页识别（A11：判据不得放宽）。
//   tail: 文件末至少 0x200 字节（传整包或末若干页均可，只取末 0x200 页判定）
//   fileSize: 整个文件大小（字节），用于入参一致性校验
// 判据（OpsTailPage.Size=0x200；opscrypto.py extractxml() L408-410；
// FirmwareKit FormatDetector.detect() L114-131）：末 0x200 页
//   +0x00 LE32 == 2 && +0x04 LE32 == 1 && +0x10 LE32 == 0x7CEF
// 命中返回 true。
bool detectOPS(const QByteArray &tail, quint64 fileSize);

// 解析 OPS 包元数据（尾页字段 + settings.xml 试解 + 清单解析）。
// 成功时填充 info（失败时 info 保持调用方原值）。
// 错误文案（中文）覆盖：路径不可读 / 文件过小 / 非 OPS 包（尾页标记不匹配）/
// settings.xml 长度字段为 0 / settings.xml 区域越界 / 读取不完整 /
// 密钥未知或文件损坏（三个 mbox 候选均试解失败）/ 清单 XML 非法或条目越界。
// 注: 清单里"缺少 FileOffsetInSrc"的条目按跳过处理，其提示追加进 *error
//     （返回 true 且 *error 非空 = 部分条目被跳过，调用方须落日志，见 A10）。
bool parseOPS(const QString &path, OpsInfo &info, QString *error);

} // namespace imgopp
