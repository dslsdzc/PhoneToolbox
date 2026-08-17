#pragma once

// 华为 Kirin USB Update（VCOM）连接与帧层（计划 F2-1）
//
// 独立实现声明：本模块协议事实（帧格式/命令字/CRC 算法）来自对公共领域
// 协议行为的观察整理（HiSilicon USB Update 下载协议）。实现代码为本项目
// 独立撰写，不复制任何参照实现的源表达。
//
// 核实要点（行为观察，2026-08-18）：
//   • 帧：0x7E | escaped(payload + CRC16-X25 LE) | 0x7E
//   • 转义：0x7E→0x7D 0x5E、0x7D→0x7D 0x5D
//   • CRC16-X25：init 0xFFFF、反射多项式 0x8408、结果取反、小端输出
//   • 握手：26 00 00 25 A7 00 06 …（19B）+ CRC LE + 0x7E（无前导 0x7E），
//     期望响应含前缀 7E 26 00 00 25 A7，3 次重试
//   • 成功响应帧：7E 02 6A D3 7E；错误帧 payload 首字节 0x03

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <memory>

namespace hisi {

// ---- 命令码（协议事实）----
enum FrameCmd : quint8 {
    FRAME_UNLOCK = 0x0B,
    FRAME_DATA = 0x0F,
    FRAME_HEAD = 0x41,
    FRAME_TAIL = 0x43,
    FRAME_REBOOT = 0x0A,
    FRAME_FORCE_REBOOT = 0x32,
};

// 设备描述
struct HisiDevice {
    int vid = 0;
    int pid = 0;
    QString portName; // usb-<bus>-<addr>，仅日志展示
};

// 抽象传输通道（mock 注入单测；libusb 生产实现）
class IUsbChannel {
public:
    virtual ~IUsbChannel() = default;
    virtual bool open(QString *error) = 0;
    virtual bool write(const QByteArray &data, QString *error) = 0;
    virtual bool read(QByteArray &out, int maxLen, int timeoutMs, QString *error) = 0;
    virtual int maxPacketSize() const { return 0x400; }
    virtual bool close() = 0;
};

// libusb 枚举（VID 0x12D1 华为）
bool enumerateUsb(QList<HisiDevice> &out, QString *error);

// 打开 libusb 通道（ch 接管所有权；失败时 ch 保持 null）
bool openLibusbUsb(const HisiDevice &dev, std::unique_ptr<IUsbChannel> &ch, QString *error);

// ---- 纯函数（供单测/复用）----
// CRC16-X25：init 0xFFFF、反射多项式 0x8408、逐位右移、结果取反
quint16 crc16X25(const QByteArray &data);
// HDLC 风格转义：0x7E→0x7D 0x5E、0x7D→0x7D 0x5D
QByteArray escapePayload(const QByteArray &data);
// 帧封装：0x7E | escaped(payload + CRC16-X25 LE) | 0x7E
QByteArray buildFrame(quint8 cmd, const QByteArray &payload);

class HisiSession {
public:
    HisiSession(std::unique_ptr<IUsbChannel> usb, const HisiDevice &dev);
    ~HisiSession();

    // 打开 USB + CDC line coding(9600) + 握手（3 次重试）
    bool connect(QString *error);
    bool isConnected() const { return m_connected; }

    // 命令帧发送 + 响应解析：成功帧（payload 首字节 0x02）→ true；
    // 错误帧（0x03）→ false + error；超时/无响应 → false + error
    bool sendCommand(quint8 cmd, const QByteArray &payload, double timeoutSec,
                     QString *error);

    // 握手（供单测/复用）：发握手帧，期望响应含前缀
    bool handshake(QString *error);

    bool close(QString *error);
    bool isClosed() const { return m_closed; }

    // 协议常量
    static const QByteArray kHandshakeCommand; // 19B 握手命令（无前导 0x7E）
    static const QByteArray kAckResponse;      // 7E 02 6A D3 7E
    static const QByteArray kHandshakePrefix;  // 7E 26 00 00 25 A7

private:
    bool readFrame(QByteArray &out, int timeoutMs, QString *error);

    std::unique_ptr<IUsbChannel> m_usb;
    HisiDevice m_dev;
    bool m_connected = false;
    bool m_closed = false;
};

} // namespace hisi
