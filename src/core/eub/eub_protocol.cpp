// src/core/eub/eub_protocol.cpp
//
// 帧构造（纯函数）与段发送（只经 IEubTransport 碰设备）。字节值出处见头文件注释与
// facts §B3/§B4/§B5；本文件不解释这两个字段的语义（facts §B6：未定）。
#include "eub_protocol.h"

#include <limits>

namespace eub {
namespace {
void setErr(QString *error, const QString &msg) { if (error) *error = msg; }

QByteArray le32(quint32 v)
{
    QByteArray out(4, Qt::Uninitialized);
    for (int i = 0; i < 4; ++i)
        out[i] = char((v >> (8 * i)) & 0xFF);
    return out;
}
} // namespace

EubFrameStyle zeroStyle()
{
    return {QByteArray::fromHex("00000000"), QByteArray::fromHex("0000")};   // facts §B4/§B5
}

EubFrameStyle dnwStyle()
{
    return {QByteArray::fromHex("1b444e57"), QByteArray::fromHex("ffff")};   // facts §B4/§B5
}

bool payloadFitsFrame(quint64 payloadBytes)
{
    // 长度字段 = 数据长 + kFrameOverhead，要与 u32 上限比 —— 先把常量提到 quint64 再减，
    // 免得 `quint32::max() - 10` 与 quint64 比较时的整型提升把边界搞错（原来就是 quint32 减法）。
    return payloadBytes <= std::numeric_limits<quint32>::max() - quint64(kFrameOverhead);
}

QByteArray buildEubFrame(const QByteArray &payload, const EubFrameStyle &style, QString *error)
{
    if (payload.isEmpty()) {
        setErr(error, QStringLiteral("EUB 帧载荷为空：无处可发（fail-closed）"));
        return {};
    }
    if (style.header.size() != 4 || style.trailer.size() != 2) {
        setErr(error, QStringLiteral("EUB 帧风格非法：头必须 4 字节（当前 %1）、尾必须 2 字节（当前 %2）")
                          .arg(style.header.size()).arg(style.trailer.size()));
        return {};
    }
    if (!payloadFitsFrame(quint64(payload.size()))) {
        setErr(error, QStringLiteral("EUB 帧载荷过大：%1 字节超出长度字段上限").arg(payload.size()));
        return {};
    }
    QByteArray frame;
    frame.reserve(4 + 4 + payload.size() + 2);
    frame.append(style.header);
    frame.append(le32(quint32(payload.size()) + quint32(kFrameOverhead)));   // facts §B3
    frame.append(payload);
    frame.append(style.trailer);
    return frame;
}

bool sendSegment(IEubTransport &t, const QByteArray &payload,
                 const EubFrameStyle &style, QString *error)
{
    const QByteArray frame = buildEubFrame(payload, style, error);
    if (frame.isEmpty())
        return false;    // buildEubFrame 已置 error（空载荷/风格非法）
    return t.writeBulk(frame, error);
}

} // namespace eub
