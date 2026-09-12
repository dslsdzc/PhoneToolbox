#pragma once
// OPPO 固件包解包（Phase A，设计 spec docs/superpowers/specs/2026-09-03-oppo-unpack-phase-a-design.md
// §3.2 提取 / §4 sparse 标注 / §5 错误与边界）。
//
// 本对文件承载 OFP 与 OPS 两个解包入口：Task 4 实现 extractOFP，Task 5 在同一 .cpp 追加
// extractOPS —— 路径守卫 / 分块搬运 / 摘要校验这些通用构件放在 oppo_extract.cpp 的匿名
// 命名空间内，两入口共用，勿把本节实现写成只服务 OFP 的形状。
//
// 提取策略（由 parseOFP 归一到 OfpFile 字段，语义出处 ofp_qc_decrypt.py main() L340-347）:
//   - fullDecrypt == true                → 整段 CFB 解密（Sahara 组）
//   - encryptedSize > 0 且 !fullDecrypt  → 仅前 min(encryptedSize, size) 字节解密，其余原样拷贝
//   - encryptedSize == 0                 → 原样拷贝（Firmware/DigestsToSign/… 明文组）
// 三种策略统一按 1 MiB 分块流式读写（copysub() L153-165 同款），内存 O(1 MiB)，不整包读入。
//
// 纯函数模块：无 QObject；失败经 bool + QString *error 上报（中文文案，error 允许为 nullptr）。
#include <QString>

#include <functional>

namespace imgopp {

// 进度回调：当前文件名 + 0-100。每完成一个文件回调一次，百分比 = 已完成字节 / 计划提取
// 总字节（仅统计会被提取的条目）× 100，最后一个文件完成后必为 100。
// sparse 条目在文件名后带标注 "<name>（sparse 镜像，原样输出）"（spec §4，仅标注不转 raw）。
using ExtractProgress = std::function<void(const QString &name, int percent)>;

// 解包整个包到 outDir。产物: outDir/<file.name>（同名产物覆盖；outDir 不存在时自动创建）。
//   - 清单/文件表中 md5、sha256 非空即校验（写出的字节增量喂 QCryptographicHash，不二次读盘；
//     十六进制比较大小写不敏感）。不匹配 → *error = "校验失败: <name> sha256 不匹配" 并返回
//     false；已写产物保留（spec §5：不回滚，可重跑）
//   - 条目名不安全（空 / 含 '/'、'\\' / ".." 成分 / 绝对路径）→ 跳过该条目并把中文提示写入
//     *error，其余条目继续提取（恶意输入防护；同 tar_image.cpp safeTarName() 的拒绝语义）。
//     此时只要还有条目被提取成功即返回 true —— 返回 true 且 *error 非空表示"部分条目被
//     跳过"，调用方应把 *error 一并展示；一个条目都没提取成功则返回 false
//   - 产物路径与输入包路径相同 → 拒绝（spec §5 流式写保护；先判后开，包本体不被截断）
//   - 清单为空（无可提取条目）→ 拒绝
bool extractOFP(const QString &path, const QString &outDir,
                const ExtractProgress &progress, QString *error);

// 解包整个 OPS 包到 outDir（条目来自 settings.xml 清单，见 oppo_ops.h 的 OpsInfo）。
// 搬运策略按组（opscrypto.py main() L589-638）:
//   - SAHARA 组          → 整段 opsDecrypt() 解密（decryptfile() L423-437 同为整段读入）
//   - UFS_PROVISION 组   → 原样拷贝（copyfile() L487-491）
//   - Program 组（含两层 <program><Image/></program>）→ 原样拷贝 + Sha256 校验
// 与 extractOFP 的差异（同口径处不复述）:
//   - 摘要口径: Program 的 Sha256 按"整段 + 补零到 0x1000 边界"计算（calc_digest() L462-470，
//     A10 附注），故不用 verifyHashes() 的整段口径；sparse="true" 跳过校验（L622）
//   - 进度回调对 sparse 条目的文件名后加"（sparse 镜像，原样输出）"标注（spec §4）
//   - 条目名不安全（空/含路径成分/".."）→ 跳过 + 中文 *error + 其余继续（A10）；
//     于是同样可能出现"ok==true 且 *error 非空"（部分条目被跳过，调用方须落日志）
//   - 校验失败 → false 且不回滚已写产物（spec §5）
bool extractOPS(const QString &path, const QString &outDir,
                const ExtractProgress &progress, QString *error);

} // namespace imgopp
