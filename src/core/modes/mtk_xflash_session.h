#pragma once

// MTK XFlash 帧层（Phase D2 Task 2）：12B 帧 / status 判定 / send_param / ack 0x6781 特例
//
// 协议细节对照 mtkclient v2.1.4-20-g71b0175（GPL-3.0，**只读参照，代码文本不进仓库**）：
//   • XFL = mtkclient/Library/DA/xflash/xflash_lib.py
//   • XFP = mtkclient/Library/DA/xflash/xflash_param.py（事实报告 .superpowers/sdd/mtk-d2d3-facts-report.md
//     里的 "XFP" 简写即指此文件；报告 §3.1-§3.3 是本层的事实底稿）
//
// 跨代铁律（docs/superpowers/plans/2026-09-16-mtk-xflash-xml.md "Global Constraints"）：
//   铁律 3 —— 12B 帧头 = pack("<III", 0xFEEEEEEF, datatype, length)，**全小端**；
//              datatype 1=协议流 / 2=DA 日志；**帧头一次写、载荷第二次写**（XFL:112-115）。
//   铁律 4 —— ack()：dacode == 0x6781 **一次写 16 字节**（头 12B + 载荷 4B 合并），
//              其余芯片**两次写**；**不存在"4 字节短帧"**（XFL:85-100）。
//   铁律 5 —— send_param：载荷按 **0x200** 分块、每个参数一个独立帧、全部写完**读一次 status**；
//              **帧头一次写 + 载荷分块循环写**（不整段写，XFL:163-177 / :273-281）；
//              status == 0xC0040050（EMI 版本不匹配）→ **判失败**（上游 XFL:180-188 只是跳过错误打印
//              与 sys.exit，但**仍以失败返回**；显式 preloader 路径 XFL:1147-1149 据此中止整链）；
//              0xC0020053（anti-rollback）/ 0xC0020004（DL forbidden）→ 明确报错
//              （上游这两条直接 sys.exit(1)；本实现改为返回 false + 中文文案）。
//              （2026-09-16 控制方裁决：本条原写作"0xC0040050 容忍、不算错误"，已按上游更正为失败。）
//   铁律 6 —— status 判据（XFL:138-158）：读 12B 头（magic 必须 0xFEEEEEEF）→ 读 length 字节 →
//              length==2 取 <H、**为 0 才成功**；length==4 取 <I、**0 或 0xFEEEEEEF 都算成功**；
//              其它长度取载荷首个 u32（XFL:157）。
//
// 诚实边界：
//   • 时间参数（超时）上游无对应常量（usbread 由传输层决定）—— 本层用 D1 传输层约定值，
//     真机拆包行为未验证（见 mtk_brom.h 的 readExact 注释）
//   • ack() 只写帧、不读 status（= 上游 ack(rstatus=False) 写法，XFL:755 的 readflash 用法）；
//     需要确认状态时由调用方显式 checkStatus()（上游 ack() 默认还会读一次 status，XFL:95-97）
//   • 上游 0xC0010004（devctrl 的"不支持"码）会**抑制错误日志**（XFL:202-203）；本实现统一
//     由 checkStatus 给文案，返回语义（失败）一致

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

#include "core/modes/mtk_brom.h"

namespace mtkbrom {

// ---- XFlash 命令常量（XFP；devctrl 子命令另见 Task 4）----
enum XCmd : quint32 {
    X_CMD_FORMAT = 0x010003, X_CMD_WRITE_DATA = 0x010004, X_CMD_READ_DATA = 0x010005,
    X_CMD_SHUTDOWN = 0x010007, X_CMD_BOOT_TO = 0x010008, X_CMD_DEVICE_CTRL = 0x010009,
    X_CMD_INIT_EXT_RAM = 0x01000A, X_CMD_SETUP_ENV = 0x010100, X_CMD_SETUP_HW_INIT = 0x010101,
};
constexpr quint32 kXMagic = 0xFEEEEEEF;         // XFP:2
constexpr quint32 kXSync = 0x434E5953;          // "SYNC"（XFP:3 SYNC_SIGNAL；小端帧里即 ASCII SYNC）
constexpr quint32 kXDataProtocolFlow = 1;       // DT_PROTOCOL_FLOW（XFP:89）
constexpr quint32 kXDataMessage = 2;            // DT_MESSAGE（XFP:90）
constexpr quint32 kXEmitVersionMismatch = 0xC0040050;   // EMI 版本不匹配：**判失败**（XFL:180-188）

// XFlash 12B 帧层（跑在 D1 的 IBromUsb 上；不持有通道所有权）
class XFlashSession {
public:
    XFlashSession(IBromUsb *usb, quint16 dacode)
        : m_usb(usb), m_dacode(dacode) {}

    bool xsend(const QByteArray &payload, QString *error = nullptr);            // 帧头+载荷两次写
    bool xsendInt(quint32 v, QString *error = nullptr);                         // pack("<I", v)
    bool xsendInt64(quint64 v, QString *error = nullptr);                       // pack("<Q", v)
    // 读一帧应答：12B 头（magic 校验）+ 载荷；datatype 可空（不需要时）
    bool xread(QByteArray &payload, quint32 *datatype = nullptr, QString *error = nullptr);
    // 铁律 6：length==2 → <H（为 0 才成功）；length==4 → <I（0 或 magic 都成功）；其它 → 载荷首 u32。
    // 返回 false **仅表示帧读取/解析失败**；非 0 的 code 由 checkStatus 判定。
    bool readStatus(quint32 &code, QString *error = nullptr);
    bool checkStatus(QString *error = nullptr);                                 // readStatus + 错误文案
    bool ack(QString *error = nullptr);                                         // 0x6781 一次 16B；其余两次
    bool sendParam(const QList<QByteArray> &params, QString *error = nullptr);  // 0x200 分块 + 一次 status
    bool sendData(const QByteArray &data, QString *error = nullptr);            // 帧 + wMaxPacketSize 分块 + 一次 status
    bool sendDevCtrl(quint32 subcmd, const QByteArray &param, QByteArray *reply, QString *error = nullptr);

private:
    // 只写 12B 帧头（长度 = 载荷字节数）。sendParam/sendData 需要"头一次写 + 载荷自行分块"
    // （XFL:163-177 / :273-281），而 xsend 是"头 + 整段载荷"——两者共用此函数。
    bool xsendHeader(quint32 length, QString *error = nullptr);

    IBromUsb *m_usb;
    quint16 m_dacode;
};

} // namespace mtkbrom
