#include "core/modes/mtk_xml_session.h"

namespace mtkbrom {
namespace {

constexpr quint32 kXmlMagic = 0xFEEEEEEF;   // XC:15
constexpr quint32 kDtProtocolFlow = 1;      // 协议流（文本响应 / 数据块）
constexpr quint32 kDtMessage = 2;           // DA 日志（16B 头，XL:124-127）
constexpr int kTimeoutMs = 5000;            // 传输层约定（上游 usbread 自带 maxtimeout，本层无对应常量）
constexpr quint32 kMaxFrameLength = 1u << 22;   // 单帧上限 4 MB（防御性；上游无校验）

QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
    return b;
}

quint32 le32At(const QByteArray &b, int off)
{
    return quint32(quint8(b.at(off))) | (quint32(quint8(b.at(off + 1))) << 8)
         | (quint32(quint8(b.at(off + 2))) << 16) | (quint32(quint8(b.at(off + 3))) << 24);
}

// 通道未设置（构造时传 nullptr）—— 唯一文案来源；与 XFlashSession 同姿态
bool noUsb(QString *error)
{
    if (error) *error = QStringLiteral("XML：USB 通道未设置");
    return false;
}

// XL:374 `int(tmp[2:], 16)` / XL:422 `int(get_field(data,"packet_length"), 16)`：
// Python hex() 输出**小写 0x 前缀、不补零**；上游 [2:] 是**无条件**剥前两位。
// 本实现容忍**有无** `0x` 两种写法（带前缀之外上游会 ValueError 崩溃），并按 Python int() 的
// 姿态去空白；解析失败返回 false（不改写 out）。
bool parseHexU32(const QString &s, quint32 &out)
{
    QString hex = s.trimmed();
    if (hex.startsWith(QLatin1String("0x"), Qt::CaseInsensitive))
        hex = hex.mid(2);
    bool ok = false;
    const quint32 v = hex.toUInt(&ok, 16);
    if (!ok)
        return false;
    out = v;
    return true;
}

} // namespace

// XC:18-25 create_cmd：`<da><version>v</version><command>CMD:<name></command>[<arg>…</arg>]</da>`
// （content 为空则**不写** <arg> —— 对应 XC 的 `if content is not None`）
QString XmlSession::envelope(const QString &command, const QStringList &argItems, const QString &version)
{
    QString out = QStringLiteral("<?xml version=\"1.0\" encoding=\"utf-8\"?><da><version>%1</version>"
                                 "<command>CMD:%2</command>").arg(version, command);
    if (!argItems.isEmpty())
        out += QStringLiteral("<arg>") + argItems.join(QString()) + QStringLiteral("</arg>");
    out += QStringLiteral("</da>");
    return out;
}

// XL:35-43 get_field：首个 `<name>…</name>` 的内容；缺开/闭标签均返回空串
QString XmlSession::field(const QString &xml, const QString &name)
{
    const QString open = QStringLiteral("<%1>").arg(name);
    const int i = xml.indexOf(open);
    if (i < 0)
        return QString();
    const int j = xml.indexOf(QStringLiteral("</%1>").arg(name), i + open.size());
    if (j < 0)
        return QString();
    return xml.mid(i + open.size(), j - i - open.size());
}

// XL:146-153：str 载荷 length = len(data)+1，实际写 bytes(data,'utf-8') + b"\x00"（铁律 17）
bool XmlSession::xsendText(const QString &text, QString *error)
{
    if (!m_usb)
        return noUsb(error);
    const QByteArray body = text.toUtf8() + QByteArray(1, '\0');    // **NUL 结尾**
    if (!m_usb->write(le32(kXmlMagic) + le32(kDtProtocolFlow) + le32(quint32(body.size())), error))
        return false;
    return m_usb->write(body, error);
}

// XL:151-153 的 bytes 分支：length = len(data)，**不追加 NUL**
bool XmlSession::xsendBytes(const QByteArray &data, quint32 datatype, QString *error)
{
    if (!m_usb)
        return noUsb(error);
    if (!m_usb->write(le32(kXmlMagic) + le32(datatype) + le32(quint32(data.size())), error))
        return false;
    return data.isEmpty() ? true : m_usb->write(data, error);
}

// XL:112-135 xread 的头部分：magic 校验 → datatype/length。
// DT_MESSAGE 的**16B 头** = 本层"12B 头 + 独立读 priority:u32"（XL:124-127 宣布 length = 日志字节数 + 4）。
bool XmlSession::xreadHeader(quint32 &datatype, quint32 &length, QString *error)
{
    if (!m_usb)
        return noUsb(error);
    QByteArray h;
    if (!m_usb->readExact(h, 12, kTimeoutMs, error))
        return false;
    if (le32At(h, 0) != kXmlMagic) {
        if (error) *error = QStringLiteral("XML：应答帧 magic 不符（读到 0x%1）")
                                .arg(le32At(h, 0), 8, 16, QLatin1Char('0'));
        return false;
    }
    datatype = le32At(h, 4);
    length = le32At(h, 8);
    if (length > kMaxFrameLength) {
        if (error) *error = QStringLiteral("XML：应答帧长度异常（%1）").arg(length);
        return false;
    }
    if (datatype == kDtMessage) {
        QByteArray prio;
        if (!m_usb->readExact(prio, 4, kTimeoutMs, error))
            return false;
        // 宣布长度含这 4B priority。XL:126 是无条件 `length -= 4`；上游 usbread(负数) 实际读 0 字节，
        // 故此处对 <4 的畸形宣布值取 0（读 0 字节），避免"读 2 字节"式错位。
        length = (length >= 4) ? length - 4 : 0;
    }
    return true;
}

bool XmlSession::readPayload(quint32 length, QByteArray &out, QString *error)
{
    out.clear();
    if (length == 0)
        return true;
    if (!m_usb)
        return noUsb(error);
    return m_usb->readExact(out, int(length), kTimeoutMs, error);
}

// XL:221-232 get_response + XL:107-132 xread：xread 是**循环** —— DT_MESSAGE（DA 日志）帧的载荷被
// 读掉、追加进 UART log，然后**继续读下一帧**；只有 DT_PROTOCOL_FLOW 才返回给调用方。
// 本层同样跳过日志帧（文本交给 logSink），但限定连续跳过次数（上游无上限）。
bool XmlSession::getResponse(QString &text, QString *error)
{
    text.clear();
    for (int skipped = 0; skipped < kMaxLogFramesToSkip; ++skipped) {
        quint32 dt = 0, len = 0;
        if (!xreadHeader(dt, len, error))
            return false;
        QByteArray payload;
        if (!readPayload(len, payload, error))
            return false;
        if (dt == kDtProtocolFlow) {
            // XL:228 是 rstrip(b"\x00")（只去尾部）；本实现去掉**全部** NUL —— 文本应答中段不该有 NUL，
            // 去掉更稳（超集，不影响 NUL 结尾的正常帧）
            text = QString::fromUtf8(payload).remove(QChar('\0'));
            return true;
        }
        if (m_logSink)
            m_logSink(QString::fromUtf8(payload).remove(QChar('\0')));   // DA 日志：进 UART log，不打断协议
    }
    if (error) *error = QStringLiteral("XML：连续 %1 帧都不是协议流响应（DA 日志刷屏？）")
                            .arg(kMaxLogFramesToSkip);
    return false;
}

// XL:158-159 ack()。**逐字节复刻上游**（2026-09-17 控制方裁决）：上游字面量是 Python str `"OK\0"` ——
// 该 str **自带**一个 NUL（`len() == 3`），xsend 的 str 分支再追加一个 → 实写 `4F 4B 00 00`、宣布 4 字节。
// 故本层传 `"OK\0"`（QStringLiteral 保留内嵌 NUL），由 xsendText 追加第二个 NUL。
bool XmlSession::ack(QString *error) { return xsendText(QStringLiteral("OK\0"), error); }

// XL:161-163 ack_value：`f"OK@{hex(length)}\0"` —— **小写十六进制、0x 前缀、不补零**；
// 同样自带 NUL + xsend 追加的 NUL → 实写 `OK@0x<hex>\0\0`（9+2 = 11 字节 @ 0x1000）、宣布 = 字符数 + 2
bool XmlSession::ackValue(quint32 length, QString *error)
{
    return xsendText(QStringLiteral("OK@0x%1\0").arg(length, 0, 16), error);
}

// XL:369-449 get_command_result 的 C++ 形态
bool XmlSession::readCommandResult(Result &out, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    out = Result{};
    QString data;
    if (!getResponse(data, error))
        return false;
    QString cmd = field(data, QStringLiteral("command"));

    // 裸数据路径：无 <command> 但含 "OK@"（XL:373-388）
    if (cmd.isEmpty() && data.contains(QStringLiteral("OK@"))) {
        const int at = data.indexOf(QLatin1Char('@'));
        quint32 len = 0;
        if (!parseHexU32(data.mid(at + 1), len)) {
            if (error) *error = QStringLiteral("XML：OK@ 长度解析失败（%1）").arg(data.mid(at + 1));
            return false;
        }
        if (!ack(error))                                       // XL:377
            return false;
        QString done;
        if (!getResponse(done, error))                         // XL:378
            return false;
        if (!done.contains(QStringLiteral("OK"))) {            // XL:379
            if (error) *error = QStringLiteral("XML：数据路径的确认响应不是 OK（%1）").arg(done);
            return false;
        }
        if (!ack(error))                                       // XL:380
            return false;
        // XL:381-385：按宣布长度**逐帧**收数据，收满后才 ack **一次**。
        // （上游 `download_raw`（XL:508-589，T10 的读路径）是"每帧 ack → 读 OK → 再 ack"的逐帧节奏；
        //   本函数对应的是 `get_command_result`，其数据分支**不**逐帧 ack —— 两者不要混。）
        QByteArray bytes;
        quint32 got = 0;
        while (got < len) {
            quint32 dt = 0, flen = 0;
            if (!xreadHeader(dt, flen, error))
                return false;
            QByteArray chunk;
            if (!readPayload(flen, chunk, error))
                return false;
            if (chunk.isEmpty()) {                             // 零进展守卫（上游此处会原地死循环）
                if (error) *error = QStringLiteral("XML：数据帧为空（已收 %1/%2 字节）").arg(got).arg(len);
                return false;
            }
            bytes += chunk;
            got += quint32(chunk.size());
        }
        if (!ack(error))                                       // XL:386
            return false;
        if (bytes.size() != int(len)) {                        // 宣布长度与实际不符 → 失败（比上游严格）
            if (error) *error = QStringLiteral("XML：数据长度不符（要 %1，得 %2）").arg(len).arg(bytes.size());
            return false;
        }
        out.command = QString();
        out.bytes = bytes;
        return true;
    }

    // 保活：PROGRESS-REPORT 直到 "OK!EOT"（XL:390-407）
    if (cmd == QStringLiteral("CMD:PROGRESS-REPORT")) {
        say(QStringLiteral("XML：DA 上报进度（保活）"));
        if (!ack(error))                                       // XL:399
            return false;
        QString line;
        do {
            if (!getResponse(line, error))                     // XL:402
                return false;
            if (!ack(error))                                   // XL:403（末次 ack 也不可省）
                return false;
        } while (line != QStringLiteral("OK!EOT"));
        if (!getResponse(data, error))                         // XL:404：**不** ack，直接读下一条命令
            return false;
        cmd = field(data, QStringLiteral("command"));
    }

    out.command = cmd;
    if (cmd == QStringLiteral("CMD:START")) {                  // XL:405-407
        if (!ack(error))
            return false;
        out.text = QStringLiteral("START");
        return true;
    }
    if (cmd == QStringLiteral("CMD:DOWNLOAD-FILE")) {          // XL:408-424
        out.info = field(data, QStringLiteral("info"));
        out.file = field(data, QStringLiteral("source_file"));
        const QString pl = field(data, QStringLiteral("packet_length"));
        // 上游 `DwnFile.checksum` 解析后从不校验（XL:413 示例恒为 CHK_NO）—— 本层不设该字段
        if (!parseHexU32(pl, out.packetLength) || out.packetLength == 0) {
            if (error) *error = QStringLiteral("XML：DOWNLOAD-FILE 的 packet_length 非法（%1）").arg(pl);
            return false;
        }
        out.hasPacketLength = true;
        if (!ack(error))                                       // XL:423
            return false;
        return true;
    }
    if (cmd == QStringLiteral("CMD:UPLOAD-FILE")) {            // XL:425-431
        out.info = field(data, QStringLiteral("info"));
        out.file = field(data, QStringLiteral("target_file"));
        // 上游把 packet_length 当**字符串**存（不转 int，XL:427）且回包无该项 —— 本层不设 hasPacketLength
        if (!ack(error))
            return false;
        return true;
    }
    if (cmd == QStringLiteral("CMD:FILE-SYS-OPERATION")) {     // XL:432-443
        out.info = field(data, QStringLiteral("key"));
        out.file = field(data, QStringLiteral("file_path"));
        if (!ack(error))
            return false;
        return true;
    }
    if (cmd == QStringLiteral("CMD:END")) {                    // XL:444-448：**不** ack（由 sendCommand 收尾）
        out.text = field(data, QStringLiteral("result"));
        if (out.text != QStringLiteral("OK")) {
            // XL:445 判据是 `"message" in data`（空 <message></message> 也会命中，结果为 ""）；
            // 本实现只在 message **非空**时替换 —— 空 message 时保留 <result> 原文，信息量更大
            const QString msg = field(data, QStringLiteral("message"));
            if (!msg.isEmpty())
                out.text = msg;
        }
        return true;
    }
    return true;                                               // 其它命令：原样返回 command/text
}

// XL:188-219 send_command
bool XmlSession::sendCommand(const QString &xml, Result *out, bool noack, QString *error)
{
    if (!xsendText(xml, error))
        return false;
    QString resp;
    if (!getResponse(resp, error))
        return false;
    if (resp == QStringLiteral("ERR!UNSUPPORTED")) {           // XL:212-217
        // 上游：读一条结果 → ack → **再读一条**（期望 CMD:START 收尾），随后一律 `return False`。
        // 这里照样把两帧读掉，保持流干净（否则下一条命令的 "OK" 会被残留的 CMD:START 顶掉）。
        // 两次读都 best-effort：读不到只说明设备没按序收尾，最终文案仍是 ERR!UNSUPPORTED。
        Result r;
        readCommandResult(r, nullptr, nullptr);
        ack(nullptr);                                          // XL:214
        Result s;
        readCommandResult(s, nullptr, nullptr);                // XL:215
        if (error) *error = QStringLiteral("XML：设备不支持该命令（ERR!UNSUPPORTED）");
        return false;                                          // 与上游一致：该分支一律 False
    }
    // 铁律 18①：上游此处是 `return result`（**非空字符串**，调用方按"非 False 即成功"处理）；本层改判失败
    if (resp.contains(QStringLiteral("ERR!"))) {               // XL:218-219
        if (error) *error = QStringLiteral("XML：设备返回错误（%1）").arg(resp);
        return false;
    }
    if (resp != QStringLiteral("OK")) {
        if (error) *error = QStringLiteral("XML：命令未被接受（响应 %1）").arg(resp);
        return false;
    }
    if (noack)
        return true;
    Result r;
    if (!readCommandResult(r, nullptr, error))                 // XL:194
        return false;
    if (r.command == QStringLiteral("CMD:END")) {              // XL:195
        if (r.text != QStringLiteral("OK")) {                  // XL:196-198
            if (error) *error = QStringLiteral("XML：命令以 CMD:END 结束但结果非 OK（%1）").arg(r.text);
            return false;
        }
        if (!ack(error))                                       // XL:199
            return false;
        Result s;
        if (!readCommandResult(s, nullptr, error))             // XL:201
            return false;
        if (s.command != QStringLiteral("CMD:START")) {        // XL:202：上游拿 sresult 但比的是**上一个** result
            if (error) *error = QStringLiteral("XML：CMD:END 之后未收到 CMD:START（收到 %1）").arg(s.command);
            return false;
        }
        if (out) *out = r;
        return true;
    }
    if (out) *out = r;
    return true;
}

} // namespace mtkbrom
