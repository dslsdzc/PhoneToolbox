// src/core/eub/samsung_mode.h
//
// 三星设备的三态认领：**认领顺序在这里固化**，检测层只消费结论。
//
// 为什么值得单独抽一层：EUB 的 PID 0x1234 不在 Odin 的兜底 PID 表
// （0x6601/0x685D/0x68C3，odin_libusb_transport.cpp:108-112）里，但它同样满足 Odin 的
// "接口类 0x0A + 批量 in/out" 判据 —— 顺序写反就会把 EUB 设备认成 Download 模式设备，
// 用户会被引到一条注定失败的刷写链上（facts §E2）。抽成纯函数后这条优先级被单测钉死。
#pragma once
#include <QList>
#include <QString>

namespace eub {

enum class SamsungMode { NotSamsung, Eub, Odin };

// 纯函数：不碰 libusb。参数与 odin::LibusbOdinTransport::isOdinDevice 同形
// （interfaceClasses = 全部接口的 bInterfaceClass 扁平表；hasBulkInOut = 设备级事实）。
SamsungMode samsungModeFor(quint16 vid, quint16 pid,
                           const QList<quint8> &interfaceClasses, bool hasBulkInOut);

} // namespace eub
