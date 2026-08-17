#pragma once

// MTK payload 上传与刷写集成（计划 F1-3）
//
// 协议细节对照洁净室规格文档（lx/02-联发科-BROM-DA-协议.md）：
//   • SEND_DA/JUMP_DA → 规格 §2.4 SEND_DA 完整时序（F1-1 已实现帧）
//   • SBC 修补字节模式 → GPLv3 子模块来源标注（5 个核心模式，见 mtk_payload.cpp）
//   • EMMC 写入        → 规格 §3.5 Legacy 命令集 + §3.6 分区表（F1-2 DaStorage）
//
// 诚实边界：
//   • DA 二进制由调用方提供（官方固件/设备提取的自研工具为后续任务）
//   • SLA（0x1D0D）检测到返回明确错误；RSA 响应生成（规格 §2.5）为后续任务
//   • V6 新平台 BROM 已修补（规格 §1.2），无专用检测 —— 以 SEND_DA/JUMP_DA 失败
//     形式传播，V6 检测标后续

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include "core/modes/mtk_brom.h"
#include "core/modes/mtk_emmc.h"

namespace mtkbrom {

// SEND_DA 上传（地址默认 0x0、签名长度默认 0；DA 载荷头部解析为后续任务）→ JUMP_DA。
// DA 二进制需调用方提供。
bool sendPayload(BromSession &s, const QByteArray &daBinary, QString *error = nullptr);

// SBC 修补：preloader 字节模式替换（GPLv3 子模块来源标注，5 个核心模式）。
// 返回是否发生任何替换；不匹配时原样返回。
bool patchPreloaderSecurity(QByteArray &preloader);

// 刷写分区：listPartitions 查名 → emmcWrite(分区偏移, 镜像)。未找到返回明确错误。
bool flashPartition(BromSession &s, DaStorage &st, const QString &name,
                    const QByteArray &image, QString *error = nullptr);

} // namespace mtkbrom
