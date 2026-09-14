// src/core/odin/odin_protocol.cpp
//
// 三星 Odin 协议帧构造与应答判定（纯函数）。
//
// 三方出处（本文件每个布局决策都能在这三份里找到出处；行号为写作时的行号，漂移时以语义为准）：
//   控制类型/请求号：
//     * Heimdall  reference/heimdall/heimdall/source/ControlPacket.h:33-39（类型枚举）、
//                 SessionSetupPacket.h:33-41（0x64 请求号）、FileTransferPacket.h:35-40（0x66 请求号）、
//                 PitFilePacket.h:33-38（0x65 请求号）、EndSessionPacket.h:32-36（0x67 请求号）、
//                 ResponsePacket.h:31-38（应答 id）、EndFileTransferPacket.h:33-37 与 :86-93（目的地 +
//                 destination/sequenceByteCount/unknown1/deviceType → 帧 8/12/16/20）、
//                 EndPhoneFileTransferPacket.h:78-83（identifier/isLast → 帧 24/28）、
//                 EndModemFileTransferPacket.h:48-52（isLast → 帧 24）
//     * odin4     reference/odin4-llucs/src/usb/odin_protocol.cpp:245-532（odin_command / 全部请求）、
//                 reference/odin4-llucs/src/protocol/thor_protocol.h:122（BOOTLOADER_FAIL = -1 = 0xFFFFFFFF）
//     * Thor      reference/thor/TheAirBlow.Thor.Library/Protocols/Odin.cs:38-215
//   1024 字节零填充控制包：Heimdall ControlPacket.h:52（OutboundPacket(1024)）→ Packet.h 构造函数
//     （new unsigned char[size] + memset 0）→ BridgeManager.cpp:699-711 按整包 GetSize()（=1024）发送；
//     odin4:269-281（std::vector<unsigned char> buf(1024, 0) + memcpy@8）；Thor Extensions.cs OdinAlign。
//   应答 8 字节 + 0xFFFFFFFF/负码失败：odin4:328-373（odin_fail_check）、Thor Extensions.cs:14-29。
//   PIT 分片 500：Heimdall ReceiveFilePartPacket.h:33；odin4:640；Thor Odin.cs:223。
//   版本 → 分片/超时：odin4:399-406、Thor Odin.cs:58-71。
#include "odin_protocol.h"

namespace odin {
namespace {

// PitEntry.binaryType：1 = CP（Modem）。libpit.h:55-80 的枚举；Heimdall FlashAction.cpp:253 按它选
// kDestinationModem，Thor Odin.cs:371 同款。**不**提出公共头 —— 会话层不直接用它（frameEndSequence 内部判定）。
constexpr quint32 kBinaryTypeCommunicationProcessor = 1;

// 小端读写原语：本文件自带一份（与 pit.cpp 同款 4 行），**不建共享头** ——
// 这是各自的字节级格式细节，提到公共层只会让两个模块耦合（任务书 Step 3 明确要求）。
quint32 rdU32(const QByteArray &d, int off)
{
    return quint32(quint8(d.at(off)))
         | (quint32(quint8(d.at(off + 1))) << 8)
         | (quint32(quint8(d.at(off + 2))) << 16)
         | (quint32(quint8(d.at(off + 3))) << 24);
}

void wrU32(QByteArray &d, int off, quint32 v)
{
    d[off]     = char(v & 0xFF);
    d[off + 1] = char((v >> 8) & 0xFF);
    d[off + 2] = char((v >> 16) & 0xFF);
    d[off + 3] = char((v >> 24) & 0xFF);
}

void wrU64(QByteArray &d, int off, quint64 v)
{
    wrU32(d, off, quint32(v & 0xFFFFFFFFu));
    wrU32(d, off + 4, quint32(v >> 32));
}

QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    wrU32(b, 0, v);
    return b;
}

QByteArray le64(quint64 v)
{
    QByteArray b(8, '\0');
    wrU64(b, 0, v);
    return b;
}

void setErr(QString *error, const QString &msg)
{
    if (error) *error = msg;
}

// 负码文案表（Thor Extensions.cs:19-26 与 odin4:351-359 同值）：
//   -2 WP / -3 Erase / -4 Write / -5 Auth / -6 Size / -7 Ext4
QString codeName(quint32 code)
{
    switch (static_cast<qint32>(code)) {
    case -2: return QStringLiteral("WP");
    case -3: return QStringLiteral("Erase");
    case -4: return QStringLiteral("Write");
    case -5: return QStringLiteral("Auth");
    case -6: return QStringLiteral("Size");
    case -7: return QStringLiteral("Ext4");
    default: return QString();
    }
}

// "code -4（Write）" / 表外码只给数值。
QString codeDesc(quint32 code)
{
    const QString name = codeName(code);
    const qint32 signedCode = static_cast<qint32>(code);
    if (name.isEmpty())
        return QStringLiteral("code %1").arg(signedCode);
    return QStringLiteral("code %1（%2）").arg(signedCode).arg(name);
}

} // namespace

TransferProfile profileForVersion(quint32 version)
{
    // odin4:399-406 / Thor Odin.cs:58-71：version <= 1 走小档，>= 2 走大档（两边同值）。
    TransferProfile p;
    if (version <= 1) {
        p.packetSize = kPartSizeDefaultSmall;
        p.sequenceCount = kSequenceCountSmall;
        p.flashTimeoutMs = kFlashTimeoutSmallMs;
    } else {
        p.packetSize = kPartSizeDefaultLarge;
        p.sequenceCount = kSequenceCountLarge;
        p.flashTimeoutMs = kFlashTimeoutLargeMs;
    }
    return p;
}

bool parseAck(const QByteArray &data, Ack &out, QString *error)
{
    if (data.size() < kAckSize) {
        setErr(error, QStringLiteral("应答不足 %1 字节（实际 %2）").arg(kAckSize).arg(data.size()));
        return false;
    }
    out.id = rdU32(data, 0);
    out.code = rdU32(data, 4);
    return true;
}

bool parseBeginSessionAck(const QByteArray &data, BeginSessionAck &out, QString *error)
{
    Ack raw;
    if (!parseAck(data, raw, error))
        return false;
    out.id = raw.id;
    out.code = raw.code;
    // code 是"版本号 + 压缩位"，不是错误码（odin4:392-395）：高 16 位取低 15 位为版本，bit31 为压缩支持位。
    // 等价写法：odin4 的 ack_upper = (u16)(code >> 16); version = ack_upper & 0x7FFF;
    //           compressed = (ack_upper & 0x8000) != 0;
    out.version = (raw.code >> 16) & 0x7FFFu;
    out.compressedSupported = (raw.code >> 31) != 0;
    return true;
}

AckVerdict judgeAckIdOnly(const Ack &ack, quint32 expectedId)
{
    AckVerdict v;
    if (ack.id == kBootloaderFail) {
        // odin4 thor_protocol.h:122 的 BOOTLOADER_FAIL；code 一并带出（-4 = Write 等）
        v.reason = QStringLiteral("设备返回 BOOTLOADER_FAIL（id 0xFFFFFFFF，%1）").arg(codeDesc(ack.code));
        return v;
    }
    if (ack.id != expectedId) {
        v.reason = QStringLiteral("应答 id 不符：收到 0x%1，期望 0x%2")
                       .arg(ack.id, 8, 16, QLatin1Char('0'))
                       .arg(expectedId, 8, 16, QLatin1Char('0'));
        return v;
    }
    v.ok = true;
    return v;
}

AckVerdict judgeAck(const Ack &ack, quint32 expectedId, bool allowProgressCodes)
{
    AckVerdict v = judgeAckIdOnly(ack, expectedId);
    if (!v.ok)
        return v;

    const qint32 code = static_cast<qint32>(ack.code);
    if (code < 0) {
        // 结束序列的"擦除/写入进度"码：-7..-2（odin4:363-366）。放行区间之外一律失败 ——
        // 表外负码（如 -8）即便放行也不认。
        if (allowProgressCodes && code >= -7 && code <= -2)
            return v;   // ok = true；进度码不是失败
        v.ok = false;
        v.reason = QStringLiteral("设备返回失败码 %1").arg(codeDesc(ack.code));
        return v;
    }
    // 正码不拒：odin4/Thor 只要求非负；Heimdall 要求"必须为 0"——采用前者，非零由会话层记账（Task 6）。
    return v;
}

QByteArray frameControl(quint32 type, quint32 request, const QByteArray &payload)
{
    // D1：控制包恒 1024 字节零填充；type@0 / request@4 / payload@8，**不裁剪**（尾部保留零填充）。
    // 前置：payload.size() <= kControlPacketSize - 8（调用方传的都是 <= 32 字节的定长结构）。
    QByteArray f(kControlPacketSize, '\0');
    wrU32(f, 0, type);
    wrU32(f, 4, request);
    if (!payload.isEmpty())
        f.replace(8, payload.size(), payload);
    return f;
}

QByteArray frameBeginSession()
{
    // D3：payload@8 = 0x7FFFFFFF（odin4:377-379 / Thor Odin.cs:44-45）；Heimdall 发全零（1:2 未采纳）。
    return frameControl(kControlSession, kSessionBegin, le32(kMaxProtoVersion));
}

QByteArray frameFilePartSize(quint32 packetSize)
{
    // D4：只有 version >= 2 才发（odin4:407-410 / Thor Odin.cs:90-102）—— 是否发送由会话层决定。
    return frameControl(kControlSession, kSessionFilePartSize, le32(packetSize));
}

QByteArray frameTotalBytes(quint64 total)
{
    // D5：u64 小端（odin4:454-458 / Thor Odin.cs:101-106）；Heimdall 只发 u32，< 4 GiB 时字节相容。
    return frameControl(kControlSession, kSessionTotalBytes, le64(total));
}

QByteArray frameDeviceTypeQuery()
{
    return frameControl(kControlSession, kSessionDeviceType);
}

QByteArray framePitDumpRequest()
{
    return frameControl(kControlPitFile, kPitRequestDump);
}

QByteArray framePitPartRequest(quint32 index)
{
    return frameControl(kControlPitFile, kPitRequestPart, le32(index));
}

QByteArray framePitEndRequest()
{
    return frameControl(kControlPitFile, kPitRequestEnd);
}

QByteArray frameRequestFlash()
{
    return frameControl(kControlFileTransfer, kTransferRequestFlash);
}

QByteArray frameRequestSequence(quint32 alignedBytes)
{
    return frameControl(kControlFileTransfer, kTransferRequestPart, le32(alignedBytes));
}

QByteArray frameEndSequence(const PitEntry &entry, quint32 realBytes, bool isLast)
{
    // D10：32 字节 payload（帧偏移 8..39）。三方逐字段对照 ——
    //   Heimdall：EndFileTransferPacket::Pack 写 destination/sequenceByteCount/unknown1/deviceType
    //             到帧偏移 8/12/16/20；EndPhoneFileTransferPacket::Pack 续写 fileIdentifier/endOfFile 到 24/28；
    //             EndModemFileTransferPacket::Pack 只续写 endOfFile 到 24。
    //   Thor Odin.cs:371-397：phone 8/12/16/20/24/28/32/36，modem 只写到 24。
    //   odin4:493-532（结束序列）/ 557-586（压缩结束序列）：字段同序，但 **modem 的 isLast 在 payload 20
    //             = 帧偏移 28**、帧偏移 24 写 0（与上面两家不同）。
    // 2:1 → 取 Heimdall+Thor；odin4 的写法在下面 modem 分支并列注明。
    const bool modem = (entry.binaryType == kBinaryTypeCommunicationProcessor);

    QByteArray payload(32, '\0');
    wrU32(payload, 0, modem ? kDestinationModem : kDestinationPhone);  // 帧 8
    wrU32(payload, 4, realBytes);                                      // 帧 12 真实长度（非对齐值）
    wrU32(payload, 8, entry.binaryType);                               // 帧 16
    wrU32(payload, 12, entry.deviceType);                              // 帧 20
    if (modem) {
        wrU32(payload, 16, isLast ? 1u : 0u);                          // 帧 24
        // 帧 28 / 32 / 36 保持 0：odin4 在帧 28 写 isLast、帧 24 写 0 —— 未采纳（见上注释）。
        // modem 分支不使用 identifier：Heimdall SendFile 给 modem 传 fileIdentifier=0xFFFFFFFF 且不写进包。
    } else {
        wrU32(payload, 16, entry.identifier);                          // 帧 24
        wrU32(payload, 20, isLast ? 1u : 0u);                          // 帧 28
        // 帧 32 = efsClear、帧 36 = bootUpdate：本期恒 0（参照的这两个开关本期不开放）。
    }
    return frameControl(kControlFileTransfer, kTransferRequestEnd, payload);
}

QByteArray frameEndSession(bool reboot)
{
    // EndSessionPacket.h:32-36 的 kRequestEndSession=0 / kRequestRebootDevice=1；
    // odin4:417（0x67/0x00 结束会话）、:423（0x67/0x01 重启）同款。
    return frameControl(kControlEndSession, reboot ? 1u : 0u);
}

quint32 alignedSequenceBytes(quint64 realBytes, quint32 packetSize)
{
    // 三方一致：序列声明值 = realBytes 向上取整到 packetSize 的整数倍
    // （Heimdall SendFile 的 sequenceTotalByteCount；odin4:695-700 的 aligned_size；
    //  Thor Odin.cs:333-335）。**不做 512 对齐**（D14：odin4 的 large_partition 分支未采纳）。
    const quint64 rem = realBytes % packetSize;
    if (rem == 0)
        return static_cast<quint32>(realBytes);
    return static_cast<quint32>(realBytes + packetSize - rem);
}

} // namespace odin
