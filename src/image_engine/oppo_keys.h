#pragma once
// OPPO 系固件包密钥库（Phase A，设计 spec docs/superpowers/specs/2026-09-03-oppo-unpack-phase-a-design.md §3.4）
// 内置公开常量（QC triplet 表 / MTK triplet 表 + 直接常量 / OPS mbox blob）+ 外部 JSON 追加。
// 常量与派生规则出处：docs/superpowers/specs/oppo-format-notes.md（双源核对：
// bkerler/oppo_decrypt（MIT）+ Uotan-Dev/FirmwareKit.Oppo（MIT））。
// 纯函数模块：无 QObject，失败经 bool + QString *error 上报（opsDecrypt 密码原语除外）。
#include <QByteArray>
#include <QList>
#include <QString>

namespace imgopp {

// QC/MTK 候选密钥。key/iv 均为 16 字节——内容是 16 个 ASCII 十六进制字符
// （即 md5(...).toHex().left(16) 的字节本身），直接作为 AES-128-CFB 的 key/iv。
struct OppoKeyPair
{
    QString keyId;    // 速查表 KeyId（QC：V1.4.17…；MTK：MTK0…MTK8）
    QByteArray key;   // 16B
    QByteArray iv;    // 16B
};

// OPS 候选密钥。mboxBlob = 62 字节全量 asbox，作为 opsDecrypt() 的轮密钥材料
// （前 16B 素材；+0x3C = 轮数 0x0A）。
struct OpsKey
{
    QString keyId;          // mbox5 / mbox6 / mbox4
    QByteArray mboxBlob;    // 62B
};

// QC 尝试顺序（= ofp_qc_decrypt.py generatekey2() 表序）：
// V1.4.17 → V1.6.17(a3s) → V1.5.13 → V1.6.6 族 → V1.7.2 → V2.0.3。
// 判定由调用方做（解出 "<?xml" 即命中）。
QList<OppoKeyPair> qcKeyCandidates();

// MTK 尝试顺序（= ofp_mtk_decrypt.py keytables 表序）：MTK0..MTK8。
// MTK0-7 与 QC 同派生方案（XOR 与半字节交换可交换，见 .cpp 说明），MTK8 是直接 ASCII 常量。
QList<OppoKeyPair> mtkKeyCandidates();

// OPS 尝试顺序（= opscrypto.py mbox 定义序）：mbox5 → mbox6 → mbox4。
QList<OpsKey> opsKeyCandidates();

// 从 JSON 文件追加外部密钥（密钥库兜底：用户为新包导入密钥）。格式：
//   {"qc":[{"id":"...","mc":"hex16","userkey":"hex16","iv":"hex16"}],
//    "ops":[{"id":"...","key":"hex62"}]}
// qc 条目与内置表同款派生（给 mc/userkey/iv 三元素，key/iv 由本函数算出）→ 追加进 out；
// ops 条目按 A3 语义收 62B mbox blob → 追加进 opsOut，Task 5 可直接喂
// opsDecrypt(data, blob)。两路出参各自追加，均不清空调用方已有条目。
// 成功时 error 置空；文件不可读 / JSON 非法 / 字段缺失或 hex 长度不符（qc 16B、ops 62B）
// → 写 *error（中文）返回 false，且 out 与 opsOut 均保持不变（解析失败不产生半截列表）。
// error 允许为 nullptr。
bool loadOppoKeysJson(const QString &path, QList<OppoKeyPair> &out, QList<OpsKey> &opsOut,
                      QString *error);

// OPS 自定义流密码解密（非标准 AES）。data：密文；mboxBlob62：opsKeyCandidates()
// 返回的 62B blob（外部导入同理）。
// 契约：返回长度 == data 长度（参照 key_custom() 的输出在末块会补齐到 4/16 字节，
// 由两个调用方截断 —— decryptfile() L433 写 length 字节、extractxml() L417 写
// xmllength 字节 —— 本实现内部对齐后截断，等价于调用方实际写出的字节）。
// 空输入返回空；mboxBlob62 不足 62 字节返回空（非法密钥材料，同 aes128CfbDecrypt 的
// 失败契约，A9 豁免；调用方须在自己的中文错误分支收口）。
QByteArray opsDecrypt(const QByteArray &data, const QByteArray &mboxBlob62);

// 同密码的加密方向（key_custom(..., encrypt=True)）。与 opsDecrypt 在"等长"契约下
// 严格互逆（opsDecrypt(opsEncrypt(x)) == x，任意长度）。产品路径是只读解包，不使用
// 本函数；它的消费方是合成包测试夹具（tests/test_oppo_ops.cpp）—— 若把加密方向写进
// 测试，就得在测试里重抄 2048B 扩展 S-box 常量，等于制造第二份易漂移的密码实现。
QByteArray opsEncrypt(const QByteArray &data, const QByteArray &mboxBlob62);

} // namespace imgopp
