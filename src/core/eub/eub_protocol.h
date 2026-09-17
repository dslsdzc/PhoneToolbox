// src/core/eub/eub_protocol.h
//
// EUB 下载帧：`[4B 头字段][u32 小端 = 数据长 + 10][数据][2B 尾]`（facts §B3，三实现一致）。
// 头字段与尾 2 字节**语义未定**：三个可用实现互相矛盾（facts §B4/§B5 的对照表）——
//   * exynos-usbdl：头 00 00 00 00、尾 00 00（calloc 全零，从不赋值）
//   * dltool：头 = 下载地址（默认 0xFFFFFFFE）、尾 = 数据字节 16 位累加和
//   * hubble：头 1B 44 4E 57（ASCII ESC+"DNW"）、尾 FF FF（常量）
// 本层只提供**照抄**能力：两种内置风格 + 任意自定义风格；注释与 UI 必须说明"照抄自哪个实现"，
// 不得声称理解其语义（facts §B6）。dltool 的"地址 + 累加和"风格本仓**不采用**（它服务的是
// 另一条 Windows 流程，facts §C10），需要时可用自定义 EubFrameStyle 表达。
//
// ⚠️ 真机路径未验证：本机没有任何 Exynos 设备（facts §F1），下述字节值只到"照抄自参照实现"
// 这一层证据；设备是否校验这两个字段留持机人。
#pragma once
#include <QByteArray>
#include <QString>

#include "eub_transport.h"

namespace eub {

// 头 4 字节 + 尾 2 字节（尺寸由 buildEubFrame 校验）
struct EubFrameStyle {
    QByteArray header;
    QByteArray trailer;
};

// 帧开销：4(头) + 4(长度) + 2(尾) —— 长度字段恒为 数据长 + kFrameOverhead（facts §B3）
constexpr int kFrameOverhead = 10;

EubFrameStyle zeroStyle();   // exynos-usbdl 路径（8890/8895 的表用它，facts §B4/§B5）
EubFrameStyle dnwStyle();    // hubble 路径（其余 6 个 SoC 的表用它）

// 构造一帧。前提不满足（载荷为空 / 风格尺寸不为 4+2）→ 返回空 + 中文 *error（fail-closed）。
QByteArray buildEubFrame(const QByteArray &payload, const EubFrameStyle &style, QString *error);

// 发送一段：构造帧 → **一次** writeBulk（facts §B7：hubble/dltool 都是整帧一次写）。
// 空载荷/风格非法/写失败 → false + *error；失败时不发出任何字节。
bool sendSegment(IEubTransport &t, const QByteArray &payload,
                 const EubFrameStyle &style, QString *error);

} // namespace eub
