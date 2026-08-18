#pragma once

// 华为 Kirin USB Update 命令层与刷写流程（计划 F2-2）
//
// 独立实现声明：协议事实（命令码/帧结构/刷写时序）来自公共领域协议行为
// 观察；实现代码为本项目独立撰写。
//
// 核实要点（行为观察，2026-08-18）：
//   • HEAD(0x41) → DATA×N(0x0F) → TAIL(0x43) 逐分区刷写
//   • DATA 块：0x20000 字节原始数据 → zlib 压缩（0x78 01 + Deflate + Adler32 BE）
//     → 帧体 (fileSeqInt+addr) BE32 + origLen BE32 + 压缩数据
//   • addr 从 0 起按原始长度累加；fileSeq = 分区头偏移 20 的 4 字节（大端）
//   • DATA 超时 = max(1, min(8, 压缩后 MB × 1.5)) 秒
//   • TAIL 超时 = max(35, min(180, 15 + 镜像 MB/10)) 秒（TAIL 等设备落盘提交，
//     固定 8s 会假失败）
//   • HEAD/TAIL 发送前头变换：92-93 两字节置零 + 追加 1 字节 0x00（变换后头
//     长度 = headerLen + 1；两命令共用同一份变换后头）

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <functional>

#include "hisi_update.h"

namespace hisi {

// zlib 压缩：0x78 01 头 + Deflate + Adler32 BE 尾（协议要求）
QByteArray zlibCompress(const QByteArray &data);

// 集成路由：update.app → 枚举 → 会话 → 逐分区刷写（诚实边界见 cpp）
bool runHisiFlash(const QString &updateAppPath,
                  std::function<void(const QString &name, int percent)> progress,
                  QString *error = nullptr);
// 纯函数（单测）：xloader/preloader 分区名判定（诚实边界用）
bool isXloaderPartition(const QString &partitionName);

class HisiFlasher {
public:
    explicit HisiFlasher(HisiSession &session) : m_session(session) {}

    // UNLOCK（0x0B + unlockcode）
    bool unlock(const QByteArray &unlockCode, QString *error = nullptr);

    // 逐分区刷写：HEAD → DATA×N（0x20000 块，zlib 压缩）→ TAIL。
    // imagePath 为未压缩分区镜像文件；header 为分区头（98+ 字节，含
    // fileSeq@20..24 大端）。progress 回调已发送原始字节数（可为 null）。
    bool flashPartition(const QString &name, const QByteArray &header,
                        const QString &imagePath,
                        std::function<void(qint64)> progress = nullptr,
                        QString *error = nullptr);

    // REBOOT（0x0A）→ FORCE_REBOOT（0x32）
    bool reboot(QString *error = nullptr);

private:
    bool sendDataBlocks(const QString &name, const QByteArray &header, const QString &imagePath,
                        std::function<void(qint64)> progress, QString *error);

    HisiSession &m_session;
};

} // namespace hisi
