#include "core/modes/mtk_xml_payload.h"

#include <QDateTime>

namespace mtkbrom {

namespace {

// SET-RUNTIME-PARAMETER 的**整段** XML（`XC:98-136`）：信封的通用形式（XmlSession::envelope）
// 只支持一个 <arg> 平铺字段列表，装不下上游**独立的 `<adv>` 块**，故按 create_cmd 的产物
// 逐字节拼（紧凑 XML、无换行；`XC:18-25` 的 create_cmd 就是拼字符串）。
// 注：`XC:101-115` 那段带 `</da>\x00` 结尾的 f-string 是**死代码**（`XC:135` 的 create_cmd 赋值把它
// 整个覆盖，`XC:136` 把该值 return 出去），线上的载荷不含尾部 NUL —— NUL 由 xsend 的 str 分支追加
// （XL:146-153，铁律 17）。
QString setRuntimeParameterXml()
{
    return QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><da><version>1.1</version>"
                          "<command>CMD:SET-RUNTIME-PARAMETER</command><arg>"
                          "<checksum_level>NONE</checksum_level>"
                          "<battery_exist>AUTO-DETECT</battery_exist>"
                          "<da_log_level>INFO</da_log_level>"
                          "<log_channel>UART</log_channel>"
                          "<system_os>LINUX</system_os>"
                          "</arg><adv><initialize_dram>YES</initialize_dram></adv></da>");
}

// HOST-SUPPORTED-COMMANDS 的能力串（`XC:139-140` 的默认值，也是 `XL:324-325` 传入的字面量）：
// caret 分隔能力与版本、`@` 结尾
QString hostCapabilities()
{
    return QStringLiteral("CMD:DOWNLOAD-FILE^1@CMD:FILE-SYS-OPERATION^1@"
                          "CMD:PROGRESS-REPORT^1@CMD:UPLOAD-FILE^1@");
}

// 失败文案：**单前缀 + 点名具体命令**（写法同 T5 的 SHUTDOWN）。`XmlSession` 的文案自带 "XML："
// 层名前缀，先剥掉再套，避免 "XML：… XML：…" 双前缀；内层措辞（实收内容、ERR! 码值等）逐字保留。
// 三条命令都过 `sendCommand`，不点名的话设备回的 "设备返回错误（…）" 无法归属到是哪一条。
QString stepFailure(const QString &step, const QString &inner)
{
    const QString layer = QStringLiteral("XML：");
    QString msg = inner;
    if (msg.startsWith(layer))
        msg.remove(0, layer.size());
    return QStringLiteral("XML：%1 失败（%2）").arg(step, msg);
}

} // namespace

// XL:167-186 setup_env：发 CMD:SET-RUNTIME-PARAMETER（**noack 默认 = false**，即走完
// OK → CMD:END → CMD:START 三步），返回值上游不检查（XL:311，调用处），我们检查。
bool xmlSetupEnv(XmlSession &x, QString *error)
{
    QString inner;
    if (!x.sendCommand(setRuntimeParameterXml(), nullptr, false, &inner)) {
        if (error) *error = stepFailure(QStringLiteral("setup_env 的 SET-RUNTIME-PARAMETER"), inner);
        return false;
    }
    return true;
}

// XL:323-327 setup_hw_init：两条顺序命令，都要求 OK（上游同样**不检查**返回值，且恒 return True）。
bool xmlSetupHwInit(XmlSession &x, QString *error)
{
    const QString cmd1 = XmlSession::envelope(
        QStringLiteral("HOST-SUPPORTED-COMMANDS"),
        {QStringLiteral("<host_capability>%1</host_capability>").arg(hostCapabilities())});
    QString inner;
    if (!x.sendCommand(cmd1, nullptr, false, &inner)) {
        if (error) *error = stepFailure(QStringLiteral("setup_hw_init 的 HOST-SUPPORTED-COMMANDS"), inner);
        return false;
    }
    // 上游 `cmd_notify_init_hw` 调 `create_cmd("NOTIFY-INIT-HW")` —— content=None，故 create_cmd
    // **不写 `<arg>`**（`XC:18-25` 的 `if content is not None`；`XC:32-42`）。该函数 docstring 里
    // 画的 `<arg></arg>` 与实现不符 —— 以**实现**为准（简令正文的"空 <arg></arg>"同理是笔误）。
    const QString cmd2 = XmlSession::envelope(QStringLiteral("NOTIFY-INIT-HW"));
    if (!x.sendCommand(cmd2, nullptr, false, &inner)) {
        if (error) *error = stepFailure(QStringLiteral("setup_hw_init 的 NOTIFY-INIT-HW"), inner);
        return false;
    }
    return true;
}

// XL:329-331 + XC:600-612：hostinfo 为空时用**本地时间** `%Y%m%dT%H%M%S`（如 20230901T234721）。
// Qt 的 'T' 不是格式符，原样输出（QDateTime::toString 对非格式字符逐字保留）。
bool xmlSetHostInfo(XmlSession &x, QString *error)
{
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMddThhmmss"));
    const QString cmd = XmlSession::envelope(QStringLiteral("SET-HOST-INFO"),
                                             {QStringLiteral("<info>%1</info>").arg(stamp)});
    QString inner;
    if (!x.sendCommand(cmd, nullptr, false, &inner)) {
        if (error) *error = stepFailure(QStringLiteral("set_host_info 的 SET-HOST-INFO"), inner);
        return false;
    }
    return true;
}

// XL:271-321 upload_da1 的尾部：jump_da 成功后 `get_command_result()`（XL:309），`cmd == "CMD:START"`
// 才算 DA1 起来（XL:310）；随后三步环境建立（XL:311-313 —— **上游不检查这三步的返回值**，
// 紧随其后就 `return True`，本层逐个检查）。
bool xmlDa1Handshake(XmlSession &x, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    XmlSession::Result first;
    if (!x.readCommandResult(first, log, error))
        return false;
    // 必须显式核对：XmlSession 对"具名但未列举"的命令（如 CMD:CUSTOM*）返回 true 而只置
    // out.command（`mtk_xml_session.h` 的约定），不核对就会把任意具名命令当成同步信号。
    if (first.command != QStringLiteral("CMD:START")) {
        if (error) *error = QStringLiteral("XML：DA1 后期待 CMD:START，收到 %1")
                                .arg(first.command.isEmpty() ? QStringLiteral("(空)") : first.command);
        return false;
    }
    say(QStringLiteral("XML：收到 CMD:START（DA1 就绪）"));
    if (!xmlSetupEnv(x, error))
        return false;
    if (!xmlSetupHwInit(x, error))
        return false;
    if (!xmlSetHostInfo(x, error))
        return false;
    say(QStringLiteral("XML：setup_env / setup_hw_init / set_host_info 完成"));
    return true;
}

} // namespace mtkbrom
