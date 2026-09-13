// src/core/edl/sahara.h
#pragma once
#include <QByteArray>
#include <QString>
#include "edl_transport.h"

namespace edl {

// Sahara 命令
//
// 数值与既有实现一致（src/core/modes/edl_handler.h:47-68 的 SaharaCmd，来源
// edl/edlclient/Library/sahara_defs.py:20-39 cmd_t）。本枚举是其子集 + CMD_READY：
// CMD_READY 见下方服务循环说明（既有 edl_handler.cpp:409-415、bkerler sahara.py:741）。
enum SaharaCmd : quint32 {
    SAHARA_HELLO_REQ      = 0x01, SAHARA_HELLO_RSP   = 0x02,
    SAHARA_READ_DATA      = 0x03, SAHARA_END_OF_IMAGE = 0x04,
    SAHARA_DONE_REQ       = 0x05, SAHARA_DONE_RSP    = 0x06,
    SAHARA_CMD_READY      = 0x0B,
    SAHARA_READ_DATA_64   = 0x12
};

// 载入 programmer：等 HELLO_REQ → 回 HELLO_RSP（mode=Image transfer, ver 2, 兼容 1）
// → 循环服务 READ_DATA/READ_DATA_64（**用 quint64 偏移，不得截断**）→ END_OF_IMAGE → DONE_REQ/DONE_RSP
//
// 纯协议函数：不碰 libusb、不含 QObject、不 open/close/reset 设备（那是会话层的职责，
// spec §4：Sahara 阶段失败 → 中止 + 中文错误 + **不发 reset**）。失败一律返回 false 并写中文 *error。
//
// 帧布局：cmd(4B LE) + 总长(4B LE) + N×4B LE 参数；READ_DATA 参数 = {image_id(u32), offset(u32), length(u32)}，
// READ_DATA_64 参数 = {image_id(u64), offset(u64), length(u64)}（均为小端）。
// 参照：既有解析器 src/core/modes/edl_handler.cpp:201-254；edl/edlclient/Library/sahara.py:453-459（DONE）。
bool saharaLoadProgrammer(IEdlTransport &t, const QByteArray &programmer,
                          QString *error, int helloTimeoutMs = 30000);
} // namespace edl
