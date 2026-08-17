#include "core/modes/mtk_payload.h"

namespace mtkbrom {

bool sendPayload(BromSession &s, const QByteArray &daBinary, QString *error)
{
    // 对照规格 §2.4 SEND_DA 完整时序：echo 0xD7 → addr/size/sig_len（4B BE 回显）
    // → 状态字（0x1D0D=SLA）→ 数据上传 → checksum+status；随后 JUMP_DA：
    // echo 0xD5 → addr 回显 → status==0 跳转成功。
    // 地址默认 0x0、签名长度默认 0（DA 载荷头部解析为后续任务）。
    if (daBinary.isEmpty()) {
        if (error) *error = QStringLiteral("DA 二进制为空（需调用方提供，自研提取为后续任务）");
        return false;
    }
    if (!s.sendDa(0, quint32(daBinary.size()), 0, daBinary, error))
        return false;
    if (!s.jumpDa(0, error))
        return false;
    return true;
}

bool patchPreloaderSecurity(QByteArray &preloader)
{
    // SBC 修补字节模式（GPLv3 子模块来源标注；规格 §2.3 SBC 位背景，§4.2 历史绕过路径）：
    // ram blacklist / seclib_sec_usbdl_enabled / Patched loader msg / sec_img_auth /
    // get_vfy_policy。模式为十六进制字节串，全文替换（可多次出现）。
    struct Patch { const char *hexFrom; const char *hexTo; const char *name; };
    static const Patch kPatches[] = {
        {"10B50C680268", "10B5012010BD", "ram blacklist"},
        {"08B5104B7B441B681B68", "00207047000000000000", "seclib_sec_usbdl_enabled"},
        {"5072656C6F61646572205374617274", "50617463686564204C205374617274", "Patched loader msg"},
        {"F0B58BB002AE20250C460746", "002070470000000000205374617274", "sec_img_auth"},
        {"FFC0F3400008BD", "FF4FF0000008BD", "get_vfy_policy"},
    };
    bool any = false;
    for (const Patch &p : kPatches) {
        const QByteArray from = QByteArray::fromHex(p.hexFrom);
        const QByteArray to = QByteArray::fromHex(p.hexTo);
        int idx = 0;
        while ((idx = preloader.indexOf(from, idx)) != -1) {
            preloader.replace(idx, from.size(), to);
            any = true;
        }
    }
    return any;
}

bool flashPartition(BromSession &s, DaStorage &st, const QString &name,
                    const QByteArray &image, QString *error)
{
    // listPartitions（规格 §3.6 分区表）查名 → emmcWrite（规格 §3.5 Legacy 命令集）
    QList<EmPartition> parts;
    if (!st.listPartitions(parts, error))
        return false;
    for (const EmPartition &p : parts) {
        if (p.name == name) {
            if (!st.emmcWrite(p.offsetBytes, image, kEmmcPartUser, error))
                return false;
            return true;
        }
    }
    if (error) *error = QStringLiteral("未找到分区 %1（分区表 %2 条）")
                            .arg(name).arg(parts.size());
    return false;
}

} // namespace mtkbrom
