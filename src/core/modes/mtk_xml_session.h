#pragma once

// MTK XML（D3）帧层（Phase D2+D3 Task 8）：文本帧 / OK / OK@0x<len> / OK!EOT 保活 / command_result 分派
//
// 协议细节对照 mtkclient v2.1.4-20-g71b0175（GPL-3.0，**只读参照，代码文本不进仓库**）：
//   • XL = mtkclient/Library/DA/xmlflash/xml_lib.py
//   • XC = mtkclient/Library/DA/xmlflash/xml_cmd.py（命令信封构造器）
//   事实底稿：.superpowers/sdd/mtk-d2d3-facts-report.md §5（"XL" 简写即指 xml_lib.py）
//
// 帧格式（与 XFlash 同族，铁律 3）：12B 头 = pack("<III", 0xFEEEEEEF, datatype, length)，全小端。
//   datatype 1 = DT_PROTOCOL_FLOW（协议流；载荷是 UTF-8 文本）、2 = DT_MESSAGE（DA 日志 —— **16B 头**：
//   多一个 priority:u32，且宣布的 length = 日志字节数 + 4，XL:124-127）。
//   **文本载荷**（XL:146-153，铁律 17）：length = utf8 字节数 + 1，实际写 utf-8 字节 + **NUL**；
//   字节载荷（写数据块用）：length = 字节数，不追加 NUL。
//
// 铁律 18（计划期裁决的两处姿态，均写在下方各入口的注释里）：
//   ① **不复刻**上游 `send_command` 对含 "ERR!" 的响应 `return result`（非空字符串，XL:218-219）——
//      上游调用方普遍按"非 False 即成功"判断，本层改为返回 false + 中文文案；
//   ② 日志帧（DT_MESSAGE）：上游 `xread` 是**循环**跳过、只把 DT_PROTOCOL_FLOW 返回给调用方
//      （XL:112-135）。本层同样跳过（文本交给 logSink）—— **不复刻**的是"收到日志帧即失败"；
//      另加跳过次数上限（kMaxLogFramesToSkip）防设备刷屏，上游无此上限。
//
// 诚实边界：
//   • 超时是传输层约定值（上游 usbread 自带 maxtimeout，本层无对应常量）
//   • 不拥有通道：IBromUsb* 由调用方持有；未设置时所有入口立即失败（与 XFlashSession 同姿态）
//   • 只做**帧收发与响应解析**：不发业务命令、不落分区表、不碰 EMI/代际路由（T9/T10）

#include <functional>
#include <utility>

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "core/modes/mtk_brom.h"

namespace mtkbrom {

class XmlSession {
public:
    explicit XmlSession(IBromUsb *usb) : m_usb(usb) {}
    IBromUsb *usb() { return m_usb; }

    // 发送：str 载荷 → 12B 头（magic, DT_PROTOCOL_FLOW, len+1）+ utf8 字节 + **NUL**（XL:146-153）
    bool xsendText(const QString &text, QString *error = nullptr);
    // 发送：原始字节（写数据块用；length = 字节数，不加 NUL）（XL:151-153 的 bytes 分支）
    bool xsendBytes(const QByteArray &data, quint32 datatype = 1, QString *error = nullptr);
    // 读一帧：头 12B（DT_MESSAGE 为 16B → 额外读 priority:u32 且 length -= 4）→ 载荷由调用方读
    // （XL:112-135：上游 xread 不消费 DT_PROTOCOL_FLOW 的载荷，由调用方读）
    bool xreadHeader(quint32 &datatype, quint32 &length, QString *error = nullptr);
    bool readPayload(quint32 length, QByteArray &out, QString *error = nullptr);
    // 读一条**文本**响应（DT_PROTOCOL_FLOW → 去 NUL → utf8）（XL:222-232）。
    // 途中的 DA 日志帧（DT_MESSAGE）被**跳过**并交给 logSink（上游同姿态：日志帧不打断协议，只记 UART log）
    using LogSink = std::function<void(const QString &)>;
    void setLogSink(LogSink sink) { m_logSink = std::move(sink); }
    bool getResponse(QString &text, QString *error = nullptr);
    bool ack(QString *error = nullptr);                        // xsend("OK\0") → 实写 4 字节（XL:158-159）
    bool ackValue(quint32 length, QString *error = nullptr);   // xsend("OK@0x<hex>\0") → 字符数+2（XL:161-163）

    struct Result {
        QString command;            // "CMD:START" / "CMD:END" / "CMD:DOWNLOAD-FILE" / ""（裸 OK@ 数据路径）
        QString text;               // END 的 result / 错误文本
        QByteArray bytes;           // 裸数据路径的字节
        quint32 packetLength = 0;   // DOWNLOAD-FILE 的 packet_length（**十六进制解析**，XL:422）
        QString info, file;         // info / source_file / target_file
        bool hasPacketLength = false;
    };
    // XL:369-449 的 C++ 形态：读响应 → 解析 <command> → 分派（含 PROGRESS-REPORT 的 OK!EOT 保活）
    bool readCommandResult(Result &out, QStringList *log = nullptr, QString *error = nullptr);
    // ⚠️ **调用方契约**：`command` 为空的 `Result` 可能是"无名帧"（已被 sendCommand 拦成失败），也可能是
    //    **具名但未列举**的命令（如 `CMD:CUSTOM*`，上游 `get_command_result` 返回 `(cmd,"")` 交调用方判，`XL:449`）。
    //    因此**调用方必须自己查 `out.command`**，不能只看返回值。
    //    ⚠️ 且：**调用方传 `out == nullptr` 时**（setup_env/setup_hw_init/set_host_info/xmlReboot 五处），本层对**所有非 `CMD:END`
    //    的结构化响应**（含 `CMD:START` / `DOWNLOAD-FILE` / `UPLOAD-FILE` / `FILE-SYS-OPERATION`）一律**判失败** —— 因为
    //    调用方结构上无法核验，而这些形状对"期待 END+START 收尾"的调用都是协议意外。**比上游更严**（上游对这几类返回真值）。
    //    ⇒ 需要"允许这些形状"的调用方请传 `Result*` 并自行判定。
    // XL:188-219：xsend → 响应必须 "OK" → （非 noack）readCommandResult → CMD:END/CMD:START 收尾
    bool sendCommand(const QString &xml, Result *out, bool noack = false, QString *error = nullptr);

    // 纯函数辅助（可单测）：信封与字段
    static QString envelope(const QString &command, const QStringList &argItems = {},
                            const QString &version = QStringLiteral("1.0"));
    static QString field(const QString &xml, const QString &name);

private:
    static constexpr int kMaxLogFramesToSkip = 64;   // 连续日志帧上限（防御设备刷屏；上游无上限，我们只加上界）

    LogSink m_logSink;
    IBromUsb *m_usb;
};

} // namespace mtkbrom
