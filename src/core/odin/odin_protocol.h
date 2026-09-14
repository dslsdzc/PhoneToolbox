// src/core/odin/odin_protocol.h
//
// 三星 Odin（Heimdall）协议**帧构造 + 应答判定**（纯函数，无 IO）。
// 本文件只负责"把字段摆到正确偏移"与"判断设备应答是否成功"，不碰 USB/传输（Task 7）、
// 不做会话编排（Task 6）。
//
// 三方出处（常量与布局逐条对照；行号为写作时的行号，漂移时以语义为准）：
//   * Heimdall：reference/heimdall/heimdall/source/{ControlPacket,SessionSetupPacket,
//     FileTransferPacket,PitFilePacket,EndSessionPacket,ResponsePacket,EndFileTransferPacket,
//     EndPhoneFileTransferPacket,EndModemFileTransferPacket,ReceiveFilePartPacket}.h
//   * odin4：reference/odin4-llucs/src/usb/odin_protocol.cpp（+ thor_protocol.h）
//   * Thor：reference/thor/TheAirBlow.Thor.Library/Protocols/Odin.cs + Extensions.cs
// 裁定与差异记录：docs/superpowers/plans/2026-09-14-samsung-heimdall-phase-c.md（D1…D14）。
//
// ⚠️ 与 spec §5 的一处不符（**参照裁定，非笔误**）：spec §5 写"结束帧（含整包 MD5）"，
//    但三份参照的结束序列包 32 字节里**没有任何 MD5 字段** —— 逐字段都能对上
//    dest / realSize / binaryType / deviceType / identifier / isLast / efsClear / bootUpdate
//    （Heimdall EndFileTransferPacket.h + EndPhoneFileTransferPacket.h；odin4:493-532；
//    Thor Odin.cs:371-397）。三星的整包 MD5 校验发生在**主机侧拆包阶段**（`.tar.md5` 的
//    校验行，Task 1/4），不在协议里。本期按参照实现，**不自造** MD5 字段（Task 10 记入事实报告）。
#pragma once
#include <QByteArray>
#include <QString>

#include "core/odin/pit.h"

namespace odin {

// ---------------------------------------------------------------------------
// 控制类型（ControlPacket.h:33-39 / odin4:245-532 / Thor Odin.cs:38-215 三方一致）
// ---------------------------------------------------------------------------
constexpr quint32 kControlSession      = 0x64;
constexpr quint32 kControlPitFile      = 0x65;
constexpr quint32 kControlFileTransfer = 0x66;
constexpr quint32 kControlEndSession   = 0x67;

// 0x64 会话请求（SessionSetupPacket.h:33-41；odin4:377-435）
constexpr quint32 kSessionBegin        = 0x00;
constexpr quint32 kSessionDeviceType   = 0x01;   // 机型查询；odin4 的 reset-flash-count 复用同一请求号
constexpr quint32 kSessionTotalBytes   = 0x02;
constexpr quint32 kSessionFilePartSize = 0x05;
constexpr quint32 kSessionEnableTFlash = 0x08;   // Heimdall kEnableTFlash；本期**不发送**（只登记）

// 0x65 PIT 请求（PitFilePacket.h:33-38；odin4:607-659）
constexpr quint32 kPitRequestFlash = 0x00;
constexpr quint32 kPitRequestDump  = 0x01;
constexpr quint32 kPitRequestPart  = 0x02;
constexpr quint32 kPitRequestEnd   = 0x03;

// 0x66 文件传输请求（FileTransferPacket.h:35-40；odin4:469-586）
constexpr quint32 kTransferRequestFlash = 0x00;  // 请求开始刷写
constexpr quint32 kTransferRequestPart  = 0x02;  // 请求一个序列（payload@8 = 对齐后字节数）
constexpr quint32 kTransferRequestEnd   = 0x03;  // 结束序列（payload 32 字节，见 frameEndSequence）

// 目的地（结束序列 payload@8；EndFileTransferPacket.h:33-37）
constexpr quint32 kDestinationPhone = 0x00;
constexpr quint32 kDestinationModem = 0x01;

// 应答 id（ResponsePacket.h:31-38）：分片应答回显 0x00
constexpr quint32 kResponseSendFilePart = 0x00;

// ---------------------------------------------------------------------------
// 尺寸（三方一致）
// ---------------------------------------------------------------------------
constexpr int kControlPacketSize = 1024;  // D1：控制包恒 1024 字节零填充（ControlPacket.h:52 的
                                          // OutboundPacket(1024)；odin4:269-281；Thor Extensions.cs OdinAlign）
constexpr int kAckSize           = 8;     // 应答恒 8 字节（id + code）
constexpr int kPitPartSize       = 500;   // D12：PIT/分片数据块 500（ReceiveFilePartPacket.h:33；odin4:640；Thor:223）

// 版本/失败
constexpr quint32 kMaxProtoVersion = 0x7FFFFFFFu; // D3：起会话时声明的最大协议版本
constexpr quint32 kBootloaderFail  = 0xFFFFFFFFu; // D11：id = BOOTLOADER_FAIL（odin4 thor_protocol.h:122）

// 版本 → 分片/序列/超时（odin4:399-406 / Thor Odin.cs:58-71；两边同值）
constexpr quint32 kPartSizeDefaultSmall = 131072;   // 128 KiB，version <= 1
constexpr quint32 kPartSizeDefaultLarge = 1048576;  // 1 MiB，version >= 2
constexpr int kSequenceCountSmall = 240;            // 240 * 128 KiB = 30 MiB
constexpr int kSequenceCountLarge = 30;             // 30 * 1 MiB = 30 MiB
constexpr int kFlashTimeoutSmallMs = 30000;
constexpr int kFlashTimeoutLargeMs = 120000;

// 一次会话协商出的传输档位。
struct TransferProfile {
    quint32 packetSize = kPartSizeDefaultSmall;  // 单个分片字节数
    int sequenceCount = kSequenceCountSmall;     // 一个"序列"分包数（决定序列字节数）
    int flashTimeoutMs = kFlashTimeoutSmallMs;   // 刷写类命令的超时
};

// 8 字节应答的两个 u32（原样小端读出，不做符号解释）。
struct Ack {
    quint32 id = 0;
    quint32 code = 0;
};

struct AckVerdict {
    bool ok = false;
    QString reason;   // 失败原因（成功时为空）；negative code 会附文案表里的名字
};

// 0x64/0x00 起会话的应答 —— ⚠️ 它的 `code` 字段**是版本号**，不是错误码：
//   version   = (code >> 16) & 0x7FFF      （odin4:394-395 的 `ack_upper & 0x7FFF`）
//   compressed= (code >> 31) != 0          （odin4:392-393 的 `ack_upper & 0x8000`；Thor 把 [6,7] 当 i16 版本）
// 因此**必须**用 judgeAckIdOnly（只判 id 回显 + BOOTLOADER_FAIL），若对它做 `code < 0` 判定，
// 会把"带压缩位的高位版本"（整个 u32 看起来是负数）误判成失败。
struct BeginSessionAck {
    quint32 id = 0;              // 回显 id（期望 0x64；0xFFFFFFFF = BOOTLOADER_FAIL）
    quint32 code = 0;            // 见上：版本号 + 压缩位
    quint32 version = 0;         // (code >> 16) & 0x7FFF
    bool compressedSupported = false;
};

// 版本档位（version <= 1 → 128 KiB/240/30s；>= 2 → 1 MiB/30/120s）。
TransferProfile profileForVersion(quint32 version);

// 解析 8 字节应答。数据不足 kAckSize → false + *error（*error 可为 nullptr）。
bool parseAck(const QByteArray &data, Ack &out, QString *error);
bool parseBeginSessionAck(const QByteArray &data, BeginSessionAck &out, QString *error);

// 只判 id：BOOTLOADER_FAIL 与 id 回显不符 → 失败。**不**看 code（起会话应答专用，见 BeginSessionAck）。
AckVerdict judgeAckIdOnly(const Ack &ack, quint32 expectedId);
// 通用判定：id 同上，另判 code —— 负码默认失败；`allowProgressCodes` 时放行 -7..-2
// （odin4:363-366：结束序列的"擦除/写入进度"码）。正码一律放行（odin4/Thor 只要求非负；
// Heimdall 要求"必须为 0" —— 采用前者，记日志）。
AckVerdict judgeAck(const Ack &ack, quint32 expectedId, bool allowProgressCodes);

// ---------------------------------------------------------------------------
// 帧构造：一律 1024 字节、前 8 字节 type/request、payload 从偏移 8 起、其余零填充、**不裁剪**（D1）。
// payload 必须 <= kControlPacketSize - 8（本模块所有调用方传的都是 <= 32 字节的定长结构）。
// ---------------------------------------------------------------------------
QByteArray frameControl(quint32 type, quint32 request, const QByteArray &payload = QByteArray());
// 0x64/0x00 + payload@8 = kMaxProtoVersion（D3；Heimdall 发全零，1:2 未被采纳）
QByteArray frameBeginSession();
// 0x64/0x05 + payload@8 = packetSize（D4：仅 version >= 2 时发）
QByteArray frameFilePartSize(quint32 packetSize);
// 0x64/0x02 + payload@8 = u64 总字节（D5；odin4:454-458 / Thor:101-106）
QByteArray frameTotalBytes(quint64 total);
// 0x64/0x01（机型查询；odin4:435-450 从应答 payload@8 取机型号）
QByteArray frameDeviceTypeQuery();
QByteArray framePitDumpRequest();               // 0x65/0x01
QByteArray framePitPartRequest(quint32 index);  // 0x65/0x02 + payload@8 = 分片下标
QByteArray framePitEndRequest();                // 0x65/0x03
QByteArray frameRequestFlash();                 // 0x66/0x00
QByteArray frameRequestSequence(quint32 alignedBytes);  // 0x66/0x02 + payload@8 = 对齐后字节数
// 0x66/0x03 + 32 字节 payload（D10，帧偏移）：
//   8=目的地(phone 0 / modem 1) 12=realBytes 16=binaryType 20=deviceType
//   phone：24=identifier 28=isLast 32=efsClear(0) 36=bootUpdate(0)   —— 三方一致
//   modem：24=isLast                                                —— Heimdall+Thor 2:1
//          （odin4:503-511/569-577 把 isLast 写在 payload 20 = 帧偏移 28、且 24 写 0；
//           三方里 2:1 取 Heimdall+Thor，此处并列记录。modem 分支不使用 identifier：
//           Heimdall SendFile 对 modem 传 fileIdentifier=0xFFFFFFFF 且不写进包）
// efsClear / bootUpdate 本期恒 0（参照的这两个开关本期不开放）。
QByteArray frameEndSequence(const PitEntry &entry, quint32 realBytes, bool isLast);
// 0x67：0=结束会话（kRequestEndSession） / 1=重启设备（kRequestRebootDevice）
// （EndSessionPacket.h:32-36；odin4:417/423）
QByteArray frameEndSession(bool reboot);

// 序列声明值 = realBytes 向上取整到 packetSize 的整数倍（三方一致：Heimdall SendFile 的
// sequenceTotalByteCount / odin4 flash_partition_stream 的 aligned_size / Thor Odin.cs:333-335）。
// **不做 512 对齐**（D14：odin4 的 large_partition 分支未采纳）；因此交给设备的
// frameEndSequence(realBytes) 与这里的对齐值可以不同 —— 前者是真实长度，后者只用于决定发几片。
// 前置：packetSize > 0（来自 profileForVersion，恒非 0）、realBytes <= UINT32_MAX
// （超过 u32 的序列由会话层拒绝，odin4:680 同款）。
quint32 alignedSequenceBytes(quint64 realBytes, quint32 packetSize);

} // namespace odin
