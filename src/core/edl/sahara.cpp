// src/core/edl/sahara.cpp
//
// Sahara 协议（纯协议层，走 IEdlTransport）——语义来源：既有**已工作**实现
// src/core/modes/edl_handler.cpp（重写前 515cc57）:131-446（HELLO 应答 / 按设备请求分片回吐 programmer /
// END_OF_IMAGE → DONE 序列），差异只有三点（Task 4 brief Step 3）：
//   1. 走 IEdlTransport（不内联 libusb）；
//   2. SAHARA_READ_DATA_64 用 quint64 偏移（修既有 edl_handler.cpp（重写前 515cc57）:302-312 的 32 位截断缺陷）；
//   3. 失败文案中文且带阶段名。
// 另注（方向陷阱，不属上面三点差异）：DONE 对的方向是 **host 主动**（发 DONE_REQ、收 DONE_RSP，与
// 其余四条"设备主动"相反），细节见下方 sendDoneAndWait() 注释。
// 协议参照：edl/edlclient/Library/sahara.py:104-108（cmd_hello：HELLO_RSP 布局与 version 默认值）、
// :650-747（服务循环）、:453-459（DONE_REQ/DONE_RSP）；Qualcomm Sahara 协议。
#include "sahara.h"

namespace edl {
namespace {

// 服务循环单次读超时（照 edl_handler.cpp（重写前 515cc57）:23 TIMEOUT_MS）
const int kDataTimeoutMs = 10000;
// 单次 IN 传输的读取上限：设备→主机的 Sahara 帧很小（HELLO_REQ 0x30、END_OF_IMAGE 0x10），
// 4 KiB 足够一次收下；半帧/多帧由 PacketReader 自行合并拆分。
// 不用 IEdlTransport::maxPacketSize()——那是 OUT 端点的 ZLP 判据（Task 5 数据面用）。
const int kReadChunkBytes = 4096;
// HELLO_RSP 固定 0x30 字节：8 头 + 10×u32（bkerler sahara.py:105-109 的 pack("<IIIIIIIIIIII")）
const quint32 kHelloResponseLen = 0x30;
// HELLO_REQ 载荷最小长度：version/version_supported/cmd_packet_length/mode 四字。
// 真机为 0x28（10×u32），这里只要求够解析这四字，其余保留字缺失时不解释。
const quint32 kMinHelloPayload = 16;
// 单包总长上限：防伪造长度导致大分配（真机最大也就 HELLO_REQ 的 0x30）
const quint32 kMaxPacketBytes = 1u << 20;
// HELLO_RSP 里声明的"主机支持的最低协议版本"（兼容 1；bkerler sahara.py:104 形参 version_min=1）
const quint32 kVersionSupportedMin = 1;
// 本项目主机协议版本（bkerler sahara.py:104 形参 version 默认 2）
const quint32 kHostVersion = 2;
const quint32 kStatusSuccess = 0x00;

bool fail(QString *error, const QString &msg)
{
    if (error) *error = msg;
    return false;
}

quint32 readLe32(const QByteArray &buf, int off)
{
    return quint32(quint8(buf.at(off))) |
           (quint32(quint8(buf.at(off + 1))) << 8) |
           (quint32(quint8(buf.at(off + 2))) << 16) |
           (quint32(quint8(buf.at(off + 3))) << 24);
}

quint64 readLe64(const QByteArray &buf, int off)
{
    return quint64(readLe32(buf, off)) | (quint64(readLe32(buf, off + 4)) << 32);
}

void appendLe32(QByteArray &buf, quint32 v)
{
    buf.append(char(v & 0xFF));
    buf.append(char((v >> 8) & 0xFF));
    buf.append(char((v >> 16) & 0xFF));
    buf.append(char((v >> 24) & 0xFF));
}

// 帧：cmd(4B LE) + 总长(4B LE) + 载荷（总长含 8 字节头）——与 tests/edl_test_helpers.h 的
// saharaFrame() 及 edl_handler.cpp（重写前 515cc57）:134-157 sendSaharaCmd 同布局。
QByteArray buildFrame(quint32 cmd, const QByteArray &payload)
{
    QByteArray frame;
    frame.reserve(8 + payload.size());
    appendLe32(frame, cmd);
    appendLe32(frame, quint32(8 + payload.size()));
    frame.append(payload);
    return frame;
}

// 读帧器：与 edl_handler.cpp（重写前 515cc57）:159-199 同构（先收 8 字节帧头，再按 len-8 收载荷），
// 额外维护跨读缓冲——真机 bulk IN 一次可能只给半帧、或一次给多帧，按 totalLen 切分后
// 余量留给下一帧。测试的 MockEdlTransport 每次 read() 给整帧，走同一路径。
class PacketReader
{
public:
    explicit PacketReader(IEdlTransport &t) : m_t(t) {}

    bool read(quint32 &outCmd, QByteArray &outPayload, int timeoutMs, QString *error)
    {
        while (m_buf.size() < 8) {
            if (!refill(timeoutMs, error)) return false;
        }

        const quint32 cmd = readLe32(m_buf, 0);
        const quint32 totalLen = readLe32(m_buf, 4);
        if (totalLen < 8 || totalLen > kMaxPacketBytes) {
            return fail(error, QStringLiteral("包长度非法（cmd=0x%1 len=%2）")
                        .arg(cmd, 2, 16, QChar('0')).arg(totalLen));
        }

        while (quint32(m_buf.size()) < totalLen) {
            if (!refill(timeoutMs, error)) return false;
        }

        outCmd = cmd;
        outPayload = m_buf.mid(8, int(totalLen) - 8);
        m_buf.remove(0, int(totalLen));
        return true;
    }

private:
    bool refill(int timeoutMs, QString *error)
    {
        QString readError;
        const QByteArray chunk = m_t.read(kReadChunkBytes, timeoutMs, &readError);
        if (chunk.isEmpty()) {
            return fail(error, readError.isEmpty()
                        ? QStringLiteral("传输层未返回数据") : readError);
        }
        m_buf.append(chunk);
        return true;
    }

    IEdlTransport &m_t;
    QByteArray m_buf;
};

// 发送一帧（cmd + 总长 + 载荷），失败写阶段名 + 传输层错误
bool writeFrame(IEdlTransport &t, quint32 cmd, const QByteArray &payload,
                const QString &stage, QString *error)
{
    QString writeError;
    if (!t.write(buildFrame(cmd, payload), &writeError)) {
        return fail(error, QStringLiteral("%1: %2").arg(stage, writeError));
    }
    return true;
}

// HELLO_RSP：cmd=0x02、总长 0x30、载荷 10×u32 =
// {version, version_supported, cmd_packet_length, mode, reserved[0..5]}
// （edl_handler.cpp（重写前 515cc57）:236-254 sendHelloResp；bkerler sahara.py:105-109；
//  mode 取 SAHARA_MODE_IMAGE_TX_PENDING=0，见 sahara_defs.py:78-82）
bool sendHelloResponse(IEdlTransport &t, quint32 version, QString *error)
{
    QByteArray payload;
    payload.reserve(int(kHelloResponseLen) - 8);
    appendLe32(payload, version);              // version（回设备上报值，见调用处）
    appendLe32(payload, kVersionSupportedMin); // version_supported
    appendLe32(payload, 0);                    // cmd_packet_length
    appendLe32(payload, 0);                    // mode = SAHARA_MODE_IMAGE_TX_PENDING
    for (quint32 i = 0; i < 6; ++i)            // reserved[6]：既有实现填 0..5
        appendLe32(payload, i);
    return writeFrame(t, SAHARA_HELLO_RSP, payload,
                      QStringLiteral("Sahara 阶段 HELLO_RSP 发送失败"), error);
}

// 按设备请求回吐 programmer 切片。越界 → 中文错误（既有实现只查了 offset+len > size 的
// 32 位形式，edl_handler.cpp（重写前 515cc57）:427-433；这里用 quint64 且写成防溢出的减法形式）。
bool serveProgrammerChunk(IEdlTransport &t, const QByteArray &programmer,
                          quint32 imageId, quint64 offset, quint64 len, QString *error)
{
    const quint64 total = quint64(programmer.size());
    if (offset > total || len > total - offset) {
        return fail(error, QStringLiteral(
            "Sahara 阶段 programmer 请求越界: image_id=%1 offset=%2 len=%3 总大小=%4")
            .arg(imageId).arg(offset).arg(len).arg(total));
    }

    const QByteArray chunk = programmer.mid(int(offset), int(len));
    QString writeError;
    if (!t.write(chunk, &writeError)) {
        return fail(error, QStringLiteral("Sahara 阶段发送 programmer 数据失败: %1")
                    .arg(writeError));
    }
    return true;
}

// DONE 序列：**host 主动** —— host 先发 DONE_REQ（cmd=0x05、总长 8、无载荷），设备再回 DONE_RSP（0x06）。
// ⚠️ 方向与 HELLO_REQ / READ_DATA / READ_DATA_64 / END_OF_IMAGE **相反**（那四条都是设备主动发）：
//    把 DONE_REQ 误当成"设备发来的完成信号"就会少发或多发一帧，是这条链上最易记反的一处。
// 两源一致：
//   - 既有可工作实现 src/core/modes/edl_handler.cpp（重写前 515cc57）:566-584：sendDoneReq() 之后 recvDoneResp()
//     明确要求收到 DONE_RSP(0x06)（`sendDoneReq`/`recvDoneResp` 本体见 :338-364）；
//   - edl/edlclient/Library/sahara.py:453-459（cmd_done）：host 写 `pack("<II", DONE_REQ, 0x8)`
//     后要求 `cmd == SAHARA_DONE_RSP`。
// 本条由 lead 复核确认（brief 修订提交 313a7c5）。
bool sendDoneAndWait(IEdlTransport &t, PacketReader &reader, QString *error)
{
    if (!writeFrame(t, SAHARA_DONE_REQ, QByteArray(),
                    QStringLiteral("Sahara 阶段 DONE_REQ 发送失败"), error)) {
        return false;
    }

    quint32 cmd = 0;
    QByteArray payload;
    QString detail;
    if (!reader.read(cmd, payload, kDataTimeoutMs, &detail)) {
        return fail(error, QStringLiteral("Sahara 阶段未收到 DONE_RSP: %1").arg(detail));
    }
    if (cmd != SAHARA_DONE_RSP) {
        return fail(error, QStringLiteral("Sahara 阶段未收到 DONE_RSP（收到 0x%1）")
                    .arg(cmd, 2, 16, QChar('0')));
    }
    if (payload.size() >= 4) {
        const quint32 status = readLe32(payload, 0);
        if (status != kStatusSuccess) {
            return fail(error, QStringLiteral("Sahara 阶段 DONE 状态错误: 0x%1")
                        .arg(status, 2, 16, QChar('0')));
        }
    }
    return true;
}

} // namespace

bool saharaLoadProgrammer(IEdlTransport &t, const QByteArray &programmer,
                          QString *error, int helloTimeoutMs)
{
    PacketReader reader(t);
    quint32 cmd = 0;
    QByteArray payload;
    QString detail;

    // 第 1 步：等设备 HELLO_REQ（Sahara 由设备先发话，主机在此之前不发任何字节）
    // detail 既可能是"读超时/传输失败"也可能是"包长度非法"，故文案取"未收到有效请求"
    if (!reader.read(cmd, payload, helloTimeoutMs, &detail)) {
        return fail(error, QStringLiteral("Sahara 阶段未收到有效的 HELLO 请求: %1").arg(detail));
    }
    if (cmd != SAHARA_HELLO_REQ) {
        return fail(error, QStringLiteral("Sahara 阶段首个数据包不是 HELLO_REQ（收到 0x%1）")
                    .arg(cmd, 2, 16, QChar('0')));
    }
    if (payload.size() < int(kMinHelloPayload)) {
        return fail(error, QStringLiteral("Sahara 阶段 HELLO_REQ 载荷过短（%1 字节，期望 ≥%2）")
                    .arg(payload.size()).arg(kMinHelloPayload));
    }

    // HELLO_RSP 的 version 字段回设备上报的版本（既有 edl_handler.cpp（重写前 515cc57）:549 与
    // bkerler sahara.py:656 传的都是设备版本）；设备报 0（异常/空字段）时退回主机 v2。
    const quint32 deviceVersion = readLe32(payload, 0);
    const quint32 version = deviceVersion >= 1 ? deviceVersion : kHostVersion;

    // 第 2 步：回 HELLO_RSP（mode = IMAGE_TX_PENDING）
    if (!sendHelloResponse(t, version, error))
        return false;

    // 第 3 步：循环服务设备的数据请求，直到 END_OF_IMAGE
    for (;;) {
        if (!reader.read(cmd, payload, kDataTimeoutMs, &detail)) {
            return fail(error, QStringLiteral("Sahara 阶段读取设备请求失败: %1").arg(detail));
        }

        if (cmd == SAHARA_READ_DATA) {
            if (payload.size() < 12) {
                return fail(error, QStringLiteral("Sahara 阶段 READ_DATA 载荷过短（%1 字节，期望 ≥12）")
                            .arg(payload.size()));
            }
            const quint32 imageId = readLe32(payload, 0);
            const quint64 offset = readLe32(payload, 4);
            const quint64 len = readLe32(payload, 8);
            if (!serveProgrammerChunk(t, programmer, imageId, offset, len, error))
                return false;
        } else if (cmd == SAHARA_READ_DATA_64) {
            if (payload.size() < 24) {
                return fail(error, QStringLiteral("Sahara 阶段 READ_DATA_64 载荷过短（%1 字节，期望 ≥24）")
                            .arg(payload.size()));
            }
            // 64 位：image_id(u64) offset(u64) length(u64) —— 不得截断成 32 位
            // （既有 edl_handler.cpp（重写前 515cc57）:302-312 的缺陷，Task 4 修复点）
            const quint32 imageId = quint32(readLe64(payload, 0));
            const quint64 offset = readLe64(payload, 8);
            const quint64 len = readLe64(payload, 16);
            if (!serveProgrammerChunk(t, programmer, imageId, offset, len, error))
                return false;
        } else if (cmd == SAHARA_END_OF_IMAGE) {
            // 参数 {image_id(u32), image_tx_status(u32)}；非 SUCCESS 即失败
            // （edl_handler.cpp（重写前 515cc57）:394-408、bkerler sahara.py:719-740）
            const quint32 status = payload.size() >= 8 ? readLe32(payload, 4) : kStatusSuccess;
            if (status != kStatusSuccess) {
                return fail(error, QStringLiteral("Sahara 阶段 programmer 传输失败: status=0x%1")
                            .arg(status, 2, 16, QChar('0')));
            }
            return sendDoneAndWait(t, reader, error);
        } else if (cmd == SAHARA_DONE_RSP || cmd == SAHARA_CMD_READY) {
            // 设备直接宣告完成（多阶段/XML 配置流程）：参照 bkerler sahara.py:741
            // （CMD_READY/DONE_RSP → "All images from XML uploaded."）与
            // edl_handler.cpp（重写前 515cc57）:409-415（同样按成功退出）。此时设备不需要我们的 DONE_REQ。
            return true;
        } else if (cmd == SAHARA_HELLO_REQ) {
            // 中途重握手：既有实现按错误处理（edl_handler.cpp（重写前 515cc57）:287-290 "Unexpected HELLO_REQ
            // during data transfer"）。bkerler 只在多阶段 XML 流程里重发 HELLO_RSP
            // （sahara.py:670-672，is_xml_config 分支）——本项目的单 programmer 流程不做，
            // fail-closed 报错（宁可停也不装看不见）。
            return fail(error, QStringLiteral(
                "Sahara 阶段数据传输中意外再次收到 HELLO_REQ（设备要求重新握手）"));
        } else {
            return fail(error, QStringLiteral(
                "Sahara 阶段收到非预期命令 0x%1（期望 READ_DATA/READ_DATA_64/END_OF_IMAGE）")
                .arg(cmd, 2, 16, QChar('0')));
        }
    }
}

} // namespace edl
