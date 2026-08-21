#pragma once

// 展锐 ResearchDownload — FDL 上传与存储命令（计划 F4-2）
//
// 独立实现声明：协议事实（命令序列/参数布局）来自公共领域协议行为观察。
//
// 核实要点（行为观察，2026-08-18）：
//   • FDL 上传：START_DATA(addr BE32 + size BE32) → MIDST_DATA×N（块 ≤2048）
//     → END_DATA → EXEC_DATA；每步发后收响应确认
//   • ERASE_FLASH(0x0A)：addr BE32 + size BE32
//   • READ_FLASH(0x06)：addr BE32 + n BE32 + offset BE32；响应 BSL_REP_READ_FLASH
//   • 分区 ID：BOOTLOADER=0x80000000 / NV=0x90000001 / FLASH=0x90000003 /
//     UDISK_IMG=0x90000006

#include <QByteArray>
#include <QString>
#include <QtGlobal>

#include "core/modes/spd_flash.h"

namespace spd {

// 分区 ID（行为观察）
constexpr quint32 kPartBootloader = 0x80000000;
constexpr quint32 kPartNv = 0x90000001;
constexpr quint32 kPartFlash = 0x90000003;
constexpr quint32 kPartUdiskImg = 0x90000006;

class SpdFlasher {
public:
    explicit SpdFlasher(SpdSession &session) : m_session(session) {}

    // FDL 上传：START_DATA → MIDST×N（step 2048）→ END_DATA → EXEC_DATA。
    // fdlPath 为 FDL 二进制（用户提供，自研提取为后续）；loadAddr 为芯片
    // 相关加载地址（FDL1 0x40004000 系 / FDL2 0x14000000 系）。
    bool uploadFdl(const QString &fdlPath, quint32 loadAddr, bool execAfter,
                   QString *error = nullptr);
    // ERASE_FLASH(0x0A)
    bool eraseFlash(quint32 addr, quint32 size, QString *error = nullptr);
    // READ_FLASH(0x06)：addr/len/offset；响应数据回填 out
    bool readFlash(quint32 addr, quint32 len, quint32 offset, QByteArray &out,
                   QString *error = nullptr);
    // NORMAL_RESET(0x05)
    bool resetDevice(QString *error = nullptr);

private:
    SpdSession &m_session;
};

} // namespace spd
