#pragma once

// MTK BROM 协议自研（计划 F1-1）
//
// 协议细节对照 mtkclient v2.1.4-20-g71b0175 源码逐条核实（见计划文档"协议核实记录"）：
//   • 命令号/响应码 → Library/mtk_preloader.py Preloader.Cmd / Rsp
//   • 握手          → Library/Port.py run_handshake()
//   • echo/状态字   → Library/Port.py echo() / mtk_cmd()
//   • get_target_config / SEND_DA / JUMP_DA → mtk_preloader.py 对应函数
//   • 枚举白名单    → config/usb_ids.py default_ids
// 关键约定：多数字段大端；命令回显模式；状态字 2B BE <=0xFF 为成功。

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <memory>

namespace mtkbrom {

// ---- BROM 命令号（对照 Preloader.Cmd）----
enum Cmd : quint8 {
    CMD_SEND_PARTITION_DATA = 0x70,
    CMD_JUMP_TO_PARTITION = 0x71,
    CMD_READ16 = 0xD0,
    CMD_READ32 = 0xD1,
    CMD_WRITE16 = 0xD2,
    CMD_WRITE16_NO_ECHO = 0xD3,
    CMD_WRITE32 = 0xD4,
    CMD_JUMP_DA = 0xD5,
    CMD_JUMP_BL = 0xD6,
    CMD_SEND_DA = 0xD7,
    CMD_GET_TARGET_CONFIG = 0xD8,
    CMD_SEND_ENV_PREPARE = 0xD9,
    CMD_JUMP_DA64 = 0xDE,
    CMD_GET_HW_SW_VER = 0xFC,
    CMD_GET_HW_CODE = 0xFD,
    CMD_GET_BL_VER = 0xFE,
    CMD_GET_VERSION = 0xFF,
};

// ---- BROM 响应码（对照 Preloader.Rsp）----
enum Rsp : quint8 {
    RSP_CONF = 0x69,
    RSP_ACK = 0x5A,
    RSP_NACK = 0xA5,
    RSP_STOP = 0x96,
};

// 状态字特值（对照 send_da()）：0x1D0D = 需要 SLA 认证
constexpr quint16 kStatusSlaRequired = 0x1D0D;

struct BromDevice {
    int vid = 0;
    int pid = 0;
    QString portName; // usb-<bus>-<addr>，仅日志展示
};

// get_target_config 位域（对照 get_target_config()）
struct TargetConfig {
    bool sbc = false;      // bit0
    bool sla = false;      // bit1
    bool daa = false;      // bit2
    bool epp = false;      // bit3
    bool cert = false;     // bit4
    bool memread = false;  // bit5
    bool memwrite = false; // bit6
    bool cmdC8 = false;    // bit7
};

// get_hw_sw_ver(0xFC) 的 8B 响应（对照 mtk_preloader.py:928-930 的 unpack(">HHHH")；
// 末字段上游未使用 = 保留）
struct HwSwVer {
    quint16 hwSubCode = 0;
    quint16 hwVer = 0;
    quint16 swVer = 0;
};

// 抽象 USB 通道（协议层只依赖此接口；mock 注入单测，libusb 生产实现）
class IBromUsb {
public:
    virtual ~IBromUsb() = default;
    virtual bool open(QString *error) = 0;
    virtual bool write(const QByteArray &data, QString *error) = 0;
    virtual bool read(QByteArray &out, int maxLen, int timeoutMs, QString *error) = 0;
    virtual int maxPacketSize() const { return 0x400; } // SEND_DA 分块上限
    virtual bool close() = 0;
};

// libusb 枚举（对照 usb_ids.py 白名单）
bool enumerateUsb(QList<BromDevice> &out, QString *error);

// 打开 libusb 通道（ch 接管所有权；失败时 ch 保持 null）
bool openLibusbUsb(const BromDevice &dev, std::unique_ptr<IBromUsb> &ch, QString *error);

class BromSession {
public:
    BromSession(std::unique_ptr<IBromUsb> usb, const BromDevice &dev);
    ~BromSession();

    bool connect(QString *error);   // open + 握手
    bool isConnected() const { return m_connected; }

    bool echoCmd(quint8 cmd, QString *error);
    bool readStatus(quint16 &status, QString *error);
    bool getTargetConfig(TargetConfig &out, QString *error);

    // ---- D1: BROM 芯片信息（对照 mtk_preloader.py get_hwcode() / get_hw_sw_ver()）----
    // 0xFD：回 4B >I，hwCode = 高 16 位、hwVer = 低 16 位（上游 :190-191 的 regular 拆分）。
    // IoT 芯片上游改走 A2 寄存器读（:182-187 的 iot 分支）—— D1 不实现，调用方按芯片表 iot 位明确拒绝。
    bool getHwCode(quint16 &hwCode, quint16 &hwVer, QString *error);
    // 0xFC：回 8B >HHHH = (hwSubCode, hwVer, swVer, 保留)
    bool getHwSwVer(HwSwVer &out, QString *error);
    bool sendCommand(quint8 cmd, const QByteArray &payload, QByteArray &reply,
                     int replyMaxLen, QString *error);
    bool sendDa(quint32 address, quint32 length, quint32 sigLen,
                const QByteArray &data, QString *error);
    bool jumpDa(quint32 addr, QString *error);
    bool jumpDa64(quint32 addr, QString *error);

    // ---- F1-2: BROM 内存协议（对照 mtk_preloader.py read()/write()）----
    bool readMemory(quint32 addr, quint32 dwords, QByteArray &out, QString *error);
    bool writeMemory(quint32 addr, const QByteArray &data, QString *error);

    bool close(QString *error);
    bool isClosed() const { return m_closed; }

    // 纯函数（供单测/复用）
    static quint32 calcDaChecksum(const QByteArray &data);
    static QByteArray expectedHandshakeEcho();

    // F1-2 扩展点
    IBromUsb *usb() { return m_usb.get(); }

private:
    bool handshake(QString *error);
    bool echoBe32(quint32 v, QString *error); // 发 4B BE 并校验回显

    std::unique_ptr<IBromUsb> m_usb;
    BromDevice m_dev;
    bool m_connected = false;
    bool m_closed = false;
};

} // namespace mtkbrom
