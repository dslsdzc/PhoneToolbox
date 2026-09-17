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

// 数据通路助手（T10）：写描述符与 512 补零
constexpr quint32 kMemOffset = 0x8000000;            // XC:461 的默认 mem_offset

QString memDescriptor(quint32 length)
{
    return QStringLiteral("MEM://0x%1:0x%2").arg(kMemOffset, 0, 16).arg(length, 0, 16);
}

QByteArray padTo512(const QByteArray &data)
{
    if (data.size() % 512 == 0)
        return data;
    QByteArray out = data;
    out.append(QByteArray(512 - (data.size() % 512), '\0'));
    return out;
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

// —— Phase D2+D3 Task 10：WRITE-FLASH / READ-FLASH 数据通路 ——
//
// **与上游的分歧（有意）**：① 读路径要求"设备宣布长度 == 请求长度"（上游 `XL:517-518` 直接用宣布值覆盖请求值，
// 调用方拿到短包也发现不了；本层 fail-closed）。② 数据帧途中的 DT_MESSAGE 帧：上游把内容追加进 UART log，
// 本层**读掉但丢弃**（`XmlSession` 只提供 setLogSink、无 getter，载荷层拿不到 sink）——两者都不影响字节记账。

bool xmlWritePartition(XmlSession &x, const QString &partition, const QByteArray &data,
                       QStringList *log, QString *error, quint64 addr)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    if (data.isEmpty()) {
        if (error) *error = QStringLiteral("XML：WRITE-FLASH 分区 %1 的数据为空").arg(partition);
        return false;
    }
    const QByteArray padded = padTo512(data);          // 补齐后再宣布长度（XL:974-976 的 length += fill）
    const quint32 length = quint32(padded.size());

    // ① WRITE-FLASH（noack：只等首个 "OK"，收尾在 ⑦）
    // `<offset>` = **写入地址**（XC:452-462 的 `offset=addr`）：上游按 GPT 条目地址写分区
    // （`v6.py:1095-1097`、`mtk_da_handler.py:544-548` 都是 `partition.sector * pagesize`）。
    const QString cmd = XmlSession::envelope(
        QStringLiteral("WRITE-FLASH"),
        {QStringLiteral("<partition>%1</partition>").arg(partition),
         QStringLiteral("<offset>0x%1</offset>").arg(addr, 0, 16),
         QStringLiteral("<source_file>%1</source_file>").arg(memDescriptor(length))});
    if (!x.sendCommand(cmd, nullptr, /*noack=*/true, error))
        return false;

    // ② 设备的 FileSysOp 必须问 FILE-SIZE（XL:967-968）——否则长度契约不成立
    XmlSession::Result fs;
    if (!x.readCommandResult(fs, nullptr, error))
        return false;
    if (fs.command != QStringLiteral("CMD:FILE-SYS-OPERATION") || fs.info != QStringLiteral("FILE-SIZE")) {
        if (error) *error = QStringLiteral("XML：WRITE-FLASH 期待 FILE-SYS-OPERATION 且 key=FILE-SIZE（收到 %1/%2）")
                                .arg(fs.command.isEmpty() ? QStringLiteral("(空)") : fs.command, fs.info);
        return false;
    }
    // 设备索要的 file_path 应当指同一段区间（审查 Minor 7）：**只比数字，不比字面** ——
    // 上游压根不比对 file_path（只比对 key，`XL:967-968`），且设备完全可以补零/大写十六进制；
    // 逐字节比较会拒掉上游能正常写的请求。解析不出（或字段缺失）则跳过（同 DwnFile / 读路径的 OK@ 解析姿态）。
    if (!fs.file.isEmpty()) {
        const QString tail = fs.file.section(QLatin1Char(':'), 2);
        bool okLen = false;
        const quint32 asked = tail.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)
                                  ? tail.mid(2).toUInt(&okLen, 16) : tail.toUInt(&okLen, 16);
        if (okLen && asked != length) {
            if (error) *error = QStringLiteral("XML：WRITE-FLASH 设备索要的区间长度 0x%1 与宣布的 0x%2 不符 —— 拒绝")
                                    .arg(asked, 0, 16).arg(length, 0, 16);
            return false;
        }
    }

    // ③ ackValue(**length**) → ④ 设备回 DwnFile（带 packet_length）
    if (!x.ackValue(length, error))
        return false;
    XmlSession::Result dwn;
    if (!x.readCommandResult(dwn, nullptr, error))
        return false;
    if (dwn.command != QStringLiteral("CMD:DOWNLOAD-FILE") || !dwn.hasPacketLength) {
        if (error) *error = QStringLiteral("XML：WRITE-FLASH 期待 CMD:DOWNLOAD-FILE（收到 %1）")
                                .arg(dwn.command.isEmpty() ? QStringLiteral("(空)") : dwn.command);
        return false;
    }
    // ⚠️ 审查 Important（rev-t10）：`packet_length` 是**设备给的 quint32** —— 直接 `int(packet)` 在 ≥2^31 时变负数，
    //    步进为负 = 循环不前进（同"无进展守卫"的动机，另含有符号溢出 UB）。先**夹取到数据长度**再用。
    //    另：`packet_length == 0` 上游 `upload()` 会真死循环（`bytestowrite -= packet_length` 恒不减），
    //    而会话层已把 0 判失败（`mtk_xml_session.cpp:296-299`）→ 这里**不需要**兜底（原稿的三元是死代码，已删）。
    const quint32 packet = qMin<quint32>(dwn.packetLength, quint32(padded.size()));
    // 写路径的对称检查（审查 Minor 3）：上游从 DwnFile 的 `source_file` 回显里取长度（`XL:456-459`）。
    // 正常情况下它就是我们发去的 `MEM://0x8000000:0x<length>`；若回显不符（或压根没有），说明双方对"要写什么"理解不一致 → fail-closed。
    if (!dwn.file.isEmpty()) {
        const QString tail = dwn.file.section(QLatin1Char(':'), 2);        // "0x<len>"
        bool okLen = false;
        const quint32 echoed = tail.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)
                                   ? tail.mid(2).toUInt(&okLen, 16) : tail.toUInt(&okLen, 16);
        if (okLen && echoed != length) {
            if (error) *error = QStringLiteral("XML：WRITE-FLASH 设备回显的 source_file 长度 0x%1 与宣布的 0x%2 不符 —— 拒绝")
                                    .arg(echoed, 0, 16).arg(length, 0, 16);
            return false;
        }
    }

    // ⑤ `upload()` 内部**再**发一次长度 ack 并等一个 "OK"（XL:463-464）——漏掉这一发一读，后面每帧错位
    if (!x.ackValue(length, error))
        return false;
    {
        QString ackResp;
        if (!x.getResponse(ackResp, error))
            return false;
        if (!ackResp.contains(QStringLiteral("OK"))) {
            if (error) *error = QStringLiteral("XML：WRITE-FLASH 第二次长度 ack 未获 OK（收到 %1）").arg(ackResp);
            return false;
        }
    }

    // ⑥ 逐包：ackValue(0) → 读 "OK" → 发原始块 → 读 "OK"（XL:468-486）
    for (int pos = 0; pos < padded.size(); pos += int(packet)) {
        const QByteArray block = padded.mid(pos, int(packet));
        if (!x.ackValue(0, error))
            return false;
        QString resp;
        if (!x.getResponse(resp, error))
            return false;
        if (!resp.contains(QStringLiteral("OK"))) {
            if (error) *error = QStringLiteral("XML：WRITE-FLASH 偏移 0x%1 处的 ack(0) 未获 OK（%2）").arg(pos, 0, 16).arg(resp);
            return false;
        }
        if (!x.xsendBytes(block, 1, error))
            return false;
        if (!x.getResponse(resp, error))
            return false;
        if (!resp.contains(QStringLiteral("OK"))) {
            if (error) *error = QStringLiteral("XML：WRITE-FLASH 偏移 0x%1 处的数据未被接受（%2）").arg(pos, 0, 16).arg(resp);
            return false;
        }
    }

    // ⑦ 收尾（raw 分支 XL:487-488 → :490）：ack → CMD:END(OK) → ack → CMD:START
    //    注意：CMD:END 与 CMD:START **之间没有独立的 "OK" 帧** —— ack() 是写，它的"回应"就是下一条命令。
    if (!x.ack(error))
        return false;
    XmlSession::Result end;
    if (!x.readCommandResult(end, nullptr, error))
        return false;
    if (end.command != QStringLiteral("CMD:END") || end.text != QStringLiteral("OK")) {
        if (error) *error = QStringLiteral("XML：WRITE-FLASH 收尾期待 CMD:END(OK)（收到 %1/%2）")
                                .arg(end.command.isEmpty() ? QStringLiteral("(空)") : end.command, end.text);
        return false;
    }
    if (!x.ack(error))
        return false;
    XmlSession::Result start;
    if (!x.readCommandResult(start, nullptr, error))
        return false;
    if (start.command != QStringLiteral("CMD:START")) {
        if (error) *error = QStringLiteral("XML：WRITE-FLASH 收尾后未收到 CMD:START（收到 %1）")
                                .arg(start.command.isEmpty() ? QStringLiteral("(空)") : start.command);
        return false;
    }
    say(QStringLiteral("XML：WRITE-FLASH 分区 %1 共 %2 字节完成（packet_length = 0x%3）")
            .arg(partition).arg(padded.size()).arg(packet, 0, 16));
    return true;
}

bool xmlReadDataFrames(XmlSession &x, quint32 length, QByteArray &out, QString *error)
{
    // **上游 `download_raw` 形状**（XL:508-559）：裸 OK@ → ack → 读 "OK" → ack → 逐帧{收帧 → ack → 读 "OK" → ack}
    QString head;
    if (!x.getResponse(head, error))
        return false;
    if (!head.startsWith(QStringLiteral("OK@"))) {
        if (error) *error = QStringLiteral("XML：READ-FLASH 期待裸 OK@ 长度帧（收到 %1）").arg(head);
        return false;
    }
    const QString after = head.section(QLatin1Char('@'), 1).trimmed();
    bool ok = false;
    const quint32 announced = after.startsWith(QLatin1String("0x"), Qt::CaseInsensitive)
                                  ? after.mid(2).toUInt(&ok, 16) : after.toUInt(&ok, 16);
    if (!ok) {
        if (error) *error = QStringLiteral("XML：READ-FLASH OK@ 长度解析失败（%1）").arg(after);
        return false;
    }
    // 分歧①（有意）：设备宣布长度必须等于请求长度（上游会静默接受短包）
    if (announced != length) {
        if (error) *error = QStringLiteral("XML：READ-FLASH 设备宣布 0x%1 字节、请求 0x%2 字节 —— 不符（拒绝）")
                                .arg(announced, 0, 16).arg(length, 0, 16);
        return false;
    }
    if (!x.ack(error))
        return false;
    QString ok1;
    if (!x.getResponse(ok1, error))
        return false;
    if (!ok1.contains(QStringLiteral("OK"))) {
        if (error) *error = QStringLiteral("XML：READ-FLASH 长度 ack 未获 OK（收到 %1）").arg(ok1);
        return false;
    }
    if (!x.ack(error))
        return false;

    out.clear();
    int skippedLogs = 0;                                // 连续日志帧上限（审查 Minor 4：与 kMaxLogFramesToSkip 同姿态，防日志刷屏死循环）
    while (quint32(out.size()) < length) {
        quint32 dt = 0, flen = 0;
        if (!x.xreadHeader(dt, flen, error))
            return false;
        QByteArray chunk;
        if (!x.readPayload(flen, chunk, error))
            return false;
        if (dt == 2) {                                  // DT_MESSAGE：读掉但丢弃（分歧②）
            if (++skippedLogs > 64) {
                if (error) *error = QStringLiteral("XML：READ-FLASH 连续收到超过 64 帧 DA 日志仍未推进数据 —— 中止");
                return false;
            }
            continue;
        }
        skippedLogs = 0;                                // 有数据进展即清零（连续计数，非累计）
        if (dt != 1) {
            if (error) *error = QStringLiteral("XML：READ-FLASH 数据帧 datatype 异常（%1）").arg(dt);
            return false;
        }
        out += chunk;
        if (!x.ack(error))
            return false;
        QString okN;
        if (!x.getResponse(okN, error))
            return false;
        if (!okN.contains(QStringLiteral("OK"))) {
            if (error) *error = QStringLiteral("XML：READ-FLASH 逐帧 ack 未获 OK（收到 %1，已收 %2/%3 字节）")
                                    .arg(okN).arg(out.size()).arg(length);
            return false;
        }
        if (!x.ack(error))
            return false;
    }
    if (quint32(out.size()) != length) {
        if (error) *error = QStringLiteral("XML：READ-FLASH 数据长度不符（要 %1，得 %2）").arg(length).arg(out.size());
        return false;
    }
    return true;
}

bool xmlReadPartition(XmlSession &x, const QString &partition, quint64 offset, quint32 length,
                      QByteArray &out, QString *error)
{
    const QString cmd = XmlSession::envelope(
        QStringLiteral("READ-FLASH"),
        {QStringLiteral("<partition>%1</partition>").arg(partition),
         QStringLiteral("<offset>0x%1</offset>").arg(offset, 0, 16),
         QStringLiteral("<length>0x%1</length>").arg(length, 0, 16),
         QStringLiteral("<target_file>ROM_0</target_file>")});      // XC:474-484
    if (!x.sendCommand(cmd, nullptr, /*noack=*/true, error))
        return false;
    XmlSession::Result up;
    if (!x.readCommandResult(up, nullptr, error))                  // 消费 UPLOAD-FILE（内部会 ack，XL:430）
        return false;
    if (up.command != QStringLiteral("CMD:UPLOAD-FILE")) {
        if (error) *error = QStringLiteral("XML：READ-FLASH 期待 CMD:UPLOAD-FILE（收到 %1）")
                                .arg(up.command.isEmpty() ? QStringLiteral("(空)") : up.command);
        return false;
    }
    if (!xmlReadDataFrames(x, length, out, error))
        return false;
    XmlSession::Result start;
    if (!x.readCommandResult(start, nullptr, error))
        return false;
    if (start.command != QStringLiteral("CMD:START")) {
        if (error) *error = QStringLiteral("XML：READ-FLASH 收尾未收到 CMD:START（收到 %1）")
                                .arg(start.command.isEmpty() ? QStringLiteral("(空)") : start.command);
        return false;
    }
    return true;
}

bool xmlReboot(XmlSession &x, bool disconnect, QString *error)
{
    const QString cmd = XmlSession::envelope(
        QStringLiteral("REBOOT"),
        {QStringLiteral("<action>%1</action>").arg(disconnect ? QStringLiteral("DISCONNECT") : QStringLiteral("IMMEDIATE"))});
    return x.sendCommand(cmd, nullptr, false, error);              // 默认节奏：OK → CMD:END → CMD:START
}

} // namespace mtkbrom
