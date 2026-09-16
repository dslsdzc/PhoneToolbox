#include "core/modes/mtk_xflash_payload.h"

// 事实与行号出处见头文件注释（mtkclient GPL-3.0，只读参照，代码文本不进仓库）。

namespace mtkbrom {
namespace {

QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
    return b;
}

quint16 le16At(const QByteArray &b, int off)
{
    return quint16(quint8(b.at(off))) | (quint16(quint8(b.at(off + 1))) << 8);
}

quint32 le32At(const QByteArray &b, int off)
{
    return quint32(quint8(b.at(off))) | (quint32(quint8(b.at(off + 1))) << 8)
         | (quint32(quint8(b.at(off + 2))) << 16) | (quint32(quint8(b.at(off + 3))) << 24);
}

// 无参 devctrl 查询的统一形状：DEVICE_CTRL → status → 子命令 → status → 读回包
// →（**回包非空时**才）再读一次 status。空回包不读（上游同样按 `回包非空` 前置判断跳过，XFL:573）；
// 这条分支若多读一帧就会把下一个命令的 status 吃掉、后续全部后移。
// 尾部这一读是上游每条"有回包"的查询都做的
// （XFL:571-578 / :330-338 / :396-418 / :623-636 / :421-436），漏读会让该帧留在设备侧，
// 把之后每次读整体错位一帧 —— 所以**先读尾部 status、再校验回包内容**（判据同上游：非 0 即失败，
// 上游只把它当"取不到值"继续走，本层中止并给出文案）。
// 唯一例外见 xflashGetPartitionCata。
bool devCtrlQuery(XFlashSession &x, quint32 subcmd, QByteArray &reply, QString *error)
{
    if (!x.sendDevCtrl(subcmd, QByteArray(), &reply, error))
        return false;
    if (!reply.isEmpty() && !x.checkStatus(error))
        return false;
    return true;
}

} // namespace

// 七步握手（入口在 `0xC0` 已由 BROM 级校验之后，XFL:982-984）：上游整段只读 3 帧 ——
// 两个 setup 各经 send_param 读一次 status，最后读一次 SYNC 回包；裸 SYNC 命令**不读 status**
// （XFL:903-907、:986-994）。上游对两个 setup 的返回值不检查（XFL:989-990），本层检查（更严）。
bool xflashDa1Handshake(XFlashSession &x, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };

    // 1) SYNC：只发 0x434E5953（"SYNC" 字面量，XFP:3）
    if (!x.xsendInt(kXSync, error))
        return false;

    // 2) SETUP_ENVIRONMENT：命令帧 + 20B 载荷帧（XFL:909-924）
    QByteArray env;
    env += le32(0);            // da_log_level（上游 uartloglevel 默认 0）
    env += le32(1);            // log_channel = 1（UART；上游 config 默认 "UART"，mtk_config.py:81 → XFL:913-918）
    env += le32(1);            // system_os = OS_LINUX（XFP:83-85 FtSystemOSE）
    env += le32(0);            // ufs_provision
    env += le32(0);            // 第 5 个字段（上游恒 0）
    if (env.size() != 20) {    // 载荷长度是线协议契约：字段增删时在此失败，别把错长帧发出去
        if (error) *error = QStringLiteral("XFlash：内部错误：SETUP_ENVIRONMENT 载荷 %1 字节（应为 20）").arg(env.size());
        return false;
    }
    if (!x.xsendInt(X_CMD_SETUP_ENV, error))
        return false;
    if (!x.sendParam({env}, error))          // 载荷帧 + 一次 status
        return false;

    // 3) SETUP_HW_INIT_PARAMS：命令帧 + 4B 载荷帧（XFL:926-932）
    if (!x.xsendInt(X_CMD_SETUP_HW_INIT, error))
        return false;
    if (!x.sendParam({le32(0)}, error))      // 参数恒 0（"no config"）
        return false;

    // 4) 读回必须是 4 字节的 SYNC 值（XFL:991-994）
    QByteArray reply;
    if (!x.xread(reply, nullptr, error))
        return false;
    if (reply.size() != 4 || le32At(reply, 0) != kXSync) {
        if (error) *error = QStringLiteral("XFlash：DA1 未回 SYNC（读到 %1 字节 / 0x%2）")
                                .arg(reply.size())
                                .arg(reply.size() == 4 ? le32At(reply, 0) : 0u, 8, 16, QLatin1Char('0'));
        return false;
    }
    say(QStringLiteral("XFlash：七步握手完成（SYNC 应答）"));
    return true;
}

// bring-up 四步，顺序固定（XFL:1103-1107）
bool xflashBringUpSteps(XFlashSession &x, QByteArray *connectionAgent, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };

    // get_expire_date：无参、回包为文本（XFL:571-578）
    QByteArray date;
    if (!devCtrlQuery(x, X_CTRL_GET_EXPIRE_DATE, date, error))
        return false;
    say(QStringLiteral("XFlash：expire_date = %1").arg(QString::fromLatin1(date)));

    // set_reset_key(0x68)：<I 参数（XFL:1104；默认值即 0x68，XFL:206-209）
    if (!x.sendDevCtrl(X_CTRL_SET_RESET_KEY, le32(0x68), nullptr, error))
        return false;

    // set_checksum_level(0) = PLAIN：上游恒设 0（XFL:1106；XFL:241-244；枚举见 XFP:77-80）
    if (!x.sendDevCtrl(X_CTRL_SET_CHECKSUM_LEVEL, le32(0x0), nullptr, error))
        return false;

    // get_connection_agent：无参、回包为 b"brom" / b"preloader"（XFL:330-338）
    QByteArray agent;
    if (!devCtrlQuery(x, X_CTRL_GET_CONNECTION_AGENT, agent, error))
        return false;
    if (connectionAgent) *connectionAgent = agent;
    say(QStringLiteral("XFlash：connection_agent = %1").arg(QString::fromLatin1(agent)));
    return true;
}

// GET_CHIP_ID：5×u16（XFL:396-418，解析前的尾部 status 见 devCtrlQuery）。
// 回包长于 10 字节**照上游截断**（只取前 5×u16，不判失败 —— 未知硬件可能多带填充），
// 但把截断写进 log（截断不再是静默行为）。
bool xflashGetChipId(XFlashSession &x, XChipId &out, QString *error, QStringList *log)
{
    QByteArray r;
    if (!devCtrlQuery(x, X_CTRL_GET_CHIP_ID, r, error))
        return false;
    if (r.size() < 10) {
        if (error) *error = QStringLiteral("XFlash：GET_CHIP_ID 回包长度不符（%1 字节，应 ≥ 10）").arg(r.size());
        return false;
    }
    if (r.size() != 10 && log)
        *log << QStringLiteral("XFlash：GET_CHIP_ID 回包 %1 字节（超出 10 字节的部分按上游忽略）").arg(r.size());
    out.hwCode = le16At(r, 0);
    out.hwSubCode = le16At(r, 2);
    out.hwVersion = le16At(r, 4);
    out.swVersion = le16At(r, 6);
    out.chipEvolution = le16At(r, 8);
    return true;
}

// GET_PACKET_LENGTH：<II（XFL:623-636）
bool xflashGetPacketLength(XFlashSession &x, XPacketLength &out, QString *error)
{
    QByteArray r;
    if (!devCtrlQuery(x, X_CTRL_GET_PACKET_LENGTH, r, error))
        return false;
    if (r.size() < 8) {
        if (error) *error = QStringLiteral("XFlash：GET_PACKET_LENGTH 回包长度不符（%1 字节，应 ≥ 8）").arg(r.size());
        return false;
    }
    out.writeLength = le32At(r, 0);
    out.readLength = le32At(r, 4);
    return true;
}

// GET_PARTITION_TBL_CATA：<I，0x64=GPT / 0x65=PMT / 其它=Unknown（XFL:612-621）。
// **本层唯一不读尾部 status 的查询** —— 上游这一条拿到回包就直接返回（XFL:612-621），
// 本层照做（"上游 wins"）。若以后把它夹在别的 devctrl 之间调用，上游同样的写法会留下
// 一帧未读的 status 把后续读错位；届时需要控制方裁决是否比上游多读这一帧。
bool xflashGetPartitionCata(XFlashSession &x, PartitionCata &out, QString *error)
{
    QByteArray r;
    if (!x.sendDevCtrl(X_CTRL_GET_PARTITION_CATA, QByteArray(), &r, error))
        return false;
    if (r.size() < 4) {
        if (error) *error = QStringLiteral("XFlash：GET_PARTITION_TBL_CATA 回包长度不符（%1 字节，应 ≥ 4）").arg(r.size());
        return false;
    }
    const quint32 v = le32At(r, 0);
    out = (v == 0x64) ? PartitionCata::Gpt : (v == 0x65) ? PartitionCata::Pmt : PartitionCata::Unknown;
    return true;
}

// GET_RAM_INFO：24B（6×u32）/ 48B（6×u64）原样返回（XFL:421-436）。
// 其它长度上游只是静默不解析（返回空 sram/dram），本层明确报错（更严；解析留给 D4/诊断）。
bool xflashGetRamInfo(XFlashSession &x, QByteArray *raw, QString *error)
{
    QByteArray r;
    if (!devCtrlQuery(x, X_CTRL_GET_RAM_INFO, r, error))
        return false;
    if (r.size() != 24 && r.size() != 48) {
        if (error) *error = QStringLiteral("XFlash：GET_RAM_INFO 回包长度异常（%1 字节，应为 24 或 48）").arg(r.size());
        return false;
    }
    if (raw) *raw = r;
    return true;
}

} // namespace mtkbrom
