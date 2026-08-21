#pragma once

// 展锐 ResearchDownload 刷写通道 — 连接与帧层（计划 F4-1）
//
// 独立实现声明：协议事实（帧格式/命令字/checksum 算法）来自对公共领域
// 协议行为的观察整理（Unlicense 公领域实现，reference/spreadtrum-flash/）。
// 实现代码为本项目独立撰写。
//
// 核实要点（行为观察，2026-08-18）：
//   • 帧：0x7E | type(2B BE) + len(2B BE) + data + checksum(2B BE) | 0x7E
//   • checksum：16-bit 求和（小端字累加 + 进位折叠 + 取反 + BE 交换）
//   • BSL 命令：CONNECT=0x00/START_DATA=0x01/MIDST_DATA=0x02/END_DATA=0x03/
//     EXEC_DATA=0x04/NORMAL_RESET=0x05/READ_FLASH=0x06/CHANGE_BAUD=0x09/
//     ERASE_FLASH=0x0A/REPARTITION=0x0B
//   • 响应：同帧结构，checksum 校验；READ_FLASH 响应 type=0x93（行为观察核实）

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QtGlobal>
#include <memory>

namespace spd {

// ---- BSL 命令（type 2B BE，行为观察）----
enum BslCmd : quint16 {
    BSL_CMD_CONNECT = 0x00,
    BSL_CMD_START_DATA = 0x01,
    BSL_CMD_MIDST_DATA = 0x02,
    BSL_CMD_END_DATA = 0x03,
    BSL_CMD_EXEC_DATA = 0x04,
    BSL_CMD_NORMAL_RESET = 0x05,
    BSL_CMD_READ_FLASH = 0x06,
    BSL_CMD_READ_CHIP_TYPE = 0x07,
    BSL_CMD_CHANGE_BAUD = 0x09,
    BSL_CMD_ERASE_FLASH = 0x0A,
    BSL_CMD_REPARTITION = 0x0B,
    BSL_CMD_READ_FLASH_TYPE = 0x0C,
    BSL_CMD_READ_FLASH_INFO = 0x0D,
    BSL_CMD_READ_SECTOR_SIZE = 0x0F,
    BSL_CMD_READ_START = 0x10,
    BSL_CMD_POWER_OFF = 0x17, // 行为观察：关机（FDL2 阶段）
    // 行为观察核实：0x7E（与帧头同值，发送时整帧填 0x7E；0x25 是 SET_DEBUGINFO）
    BSL_CMD_CHECK_BAUD = 0x7E,
};

// 响应 type（行为观察核实）
enum BslReply : quint16 {
    BSL_REP_ACK = 0x80, // 行为观察：命令响应一律为 ACK（发后强制校验 ACK 响应）
    BSL_REP_VER = 0x81, // 行为观察：FDL 握手版本响应（CHECK_BAUD 应答，如 "SPRD3"）
    BSL_REP_READ_FLASH = 0x93,
    BSL_REP_LOG = 0xFF, // log 帧：接收循环跳过（行为观察：接收循环跳过 log 帧）
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

// libusb 枚举（VID 0x1782 展锐）
bool enumerateUsb(QList<QPair<int, int>> &out, QString *error);

// 打开 libusb 通道（ch 接管所有权；失败时 ch 保持 null）
bool openLibusbUsb(int vid, int pid, std::unique_ptr<IUsbChannel> &ch, QString *error);

// ---- 纯函数（供单测/复用）----
// 16-bit 求和 checksum：每 2 字节小端字累加 + 进位折叠 + 取反 + BE 交换
quint16 sumChecksum(const QByteArray &data);
// CRC-16（poly 0x11021 非反射，init 0；行为观察核实 init 0）
quint16 crc16(const QByteArray &data);

class SpdSession {
public:
    // transcode=true 时帧内 0x7E/0x7D 转义（行为观察 TRANSCODE 标志）
    SpdSession(std::unique_ptr<IUsbChannel> usb, int vid, int pid, bool transcode = false);
    ~SpdSession();

    bool connect(QString *error);
    bool isConnected() const { return m_connected; }

    // 帧封装 + 发送 + 响应解析（type/len/data + checksum 校验）。
    // 成功返回 true 并填充 reply（响应 data 区）；checksum 不符失败。
    // replyType 可选：回传响应 type（sendCommand 不校验 type，供调用方断言，
    // 如 readFlash 校验 BSL_REP_READ_FLASH）。
    // BSL_REP_LOG(0xFF) log 帧自动跳过（连续上限 16，行为观察：接收循环跳过 log 帧）；
    // timeoutMs 为单帧响应超时（EXEC_DATA 设备执行耗时可达 15s，其余 2000ms）。
    bool sendCommand(quint16 type, const QByteArray &payload, QByteArray &reply,
                     int replyMaxLen, QString *error, quint16 *replyType = nullptr,
                     int timeoutMs = 2000);

    // FDL 握手（行为观察核实线序）：CHECK_BAUD（全 0x7E 帧，checkBaudLen 字节）
    // → 响应须为 REP_VER(0x81) → CONNECT → 响应须为 ACK(0x80)。
    // checkBaudLen：FDL1 用 1、FDL2 就绪用 4（行为观察核实）；
    // maxAttempts：FDL1 传 1（单次尝试）；FDL2 就绪等待传 10（无响应重试 ≤10 次，
    // 参照仅无数据超时重试，其余失败立即报错）。
    // 注意（真机待验证）：参照 FDL1 阶段（含本握手与 FDL1 上传）帧校验为 CRC16
    // 模式（FLAGS_CRC16），本项目统一 16-bit 求和校验——真机 FDL1 校验不符时需补齐。
    bool handshake(int checkBaudLen, int maxAttempts, QString *error);

    bool close(QString *error);
    bool isClosed() const { return m_closed; }

private:
    bool buildFrame(quint16 type, const QByteArray &payload, QByteArray &frame) const;

    std::unique_ptr<IUsbChannel> m_usb;
    int m_vid;
    int m_pid;
    bool m_transcode;
    bool m_connected = false;
    bool m_closed = false;
};

} // namespace spd
