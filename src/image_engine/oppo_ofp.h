#pragma once
// OPPO OFP 包识别与解析（Phase A，设计 spec docs/superpowers/specs/2026-09-03-oppo-unpack-phase-a-design.md §3.2）
//
// 覆盖两变体：
//   - QC（高通机）: 末页 +0x10 魔数 0x7CEF，尾部加密 XML 清单（ProFile.xml）
//   - MTK（联发科机）: 首 16B 解密后以 "MMM" 起始，尾 0x6C 混淆头 + 0x60 条目文件表
// 格式事实/常量出处：docs/superpowers/specs/oppo-format-notes.md；参照实现
// reference/oppo_decrypt/{ofp_qc_decrypt,ofp_mtk_decrypt}.py（bkerler, MIT）与
// reference/FirmwareKit.Oppo（Uotan-Dev, MIT，C#）。
//
// 本模块只识别 + 解析（不落盘）；分区提取见 extractOFP（Task 4）。
// 纯函数模块：无 QObject；失败经 bool + QString *error 上报（中文文案，error 允许为 nullptr）。
#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace imgopp {

enum class OfpVariant {
    Qc,       // 高通变体（尾页 0x7CEF + 加密 XML 清单）
    Mtk,      // MTK 变体（首 16B "MMM" + 尾 0x6C 混淆头）
    Unknown,  // 未识别（detectOFP 失败时的初值/出参）
};

// 清单/文件表中的一个文件条目——字段语义即 Task 4 提取时的指令。
struct OfpFile
{
    QString name;               // 分区/文件名（QC 清单 Path 属性；MTK 文件表 filename 字段）
    QString group;              // QC 清单所属组（Sahara/Firmware/Config/…）；MTK 为空
    quint64 offset = 0;         // 包内绝对偏移（字节）
    quint64 size = 0;           // 落盘大小（字节；QC = SizeInByteInSrc，缺失时回退扇区数 ×页尺寸）
    quint64 encryptedSize = 0;  // 需解密的字节数（0 = 全明文）
    bool fullDecrypt = false;   // true = Sahara 类整段解密；false = 按 encryptedSize 解密前缀
    QString sha256Hex;          // 可选（QC 清单 sha256 属性）
    QString md5Hex;             // 可选（QC 清单 md5 属性）
    bool sparse = false;        // QC 清单 sparse="true"/"1"（仅标注，不转换）
};

struct OfpInfo
{
    OfpVariant variant = OfpVariant::Unknown;
    QString keyId;          // 命中的 key 候选 id（QC: V1.4.17…；MTK: MTK0…MTK8）
    QByteArray key;         // 命中的 16B AES key（Task 4 提取用；内容为 16 个 ASCII 十六进制字符）
    QByteArray iv;          // 命中的 16B IV（同上）
    quint32 pageSize = 0;   // QC: 0x200/0x1000；MTK 恒为 0（无页概念）
    QString projectName;    // MTK 尾头 prjname；QC 清单无此字段 → 留空
    QString version;        // MTK 尾头 flashtype（如 "UFS"）；QC 无 → 留空
    QList<OfpFile> files;
};

// 变体识别。
//   head: 文件前 16 字节（MTK 判据需完整 16B；不足则不试 MTK）
//   tail: 文件末至少 max(0x1000, 0x6C) 字节（A1 契约；实现只需覆盖末页，
//         因判定按 tail 末尾推算而不依赖 fileSize，传末 0x1000 字节或整包均可）
//   fileSize: 整个文件大小（字节），用于入参一致性校验
// 命中返回 true 并填 variant；否则返回 false 且不修改 variant。
// 判定顺序：先 MTK（试解首 16B == "MMM"）后 QC（尾页 +0x10 LE32 == 0x7CEF）。
bool detectOFP(const QByteArray &head, const QByteArray &tail, quint64 fileSize,
               OfpVariant &variant);

// 解析包元数据（含密钥试解），成功时填充 info（失败时 info 保持调用方原值）。
// QC: 尾页字段 → 清单解密（逐候选试解，命中条件 = 解出内容含 "<?xml"）→ QXmlStreamReader 解析
//     → 按组语义填 encryptedSize/fullDecrypt；MTK: 尾 0x6C 解混淆头 + 文件表。
// 错误文案（中文）覆盖：路径不可读 / 文件过小 / PK 老式 ZIP 包 / 非 OFP 包 /
// 清单越界或长度异常 / 密钥未知或文件损坏 / 文件表越界。
bool parseOFP(const QString &path, OfpInfo &info, QString *error);

} // namespace imgopp
