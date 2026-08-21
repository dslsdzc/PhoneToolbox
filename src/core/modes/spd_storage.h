#pragma once

// 展锐 ResearchDownload — FDL 上传、存储命令与刷写集成路由（计划 F4-2/F4-3）
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
//   • 分区选择包（行为观察：分区选择/加载/擦除流程）：name 36×UTF-16LE + size LE32（mode64 时
//     + size_hi LE32 + dummy 8B；载荷 76/88B）——擦除与写分区共用
//   • 按名擦除（行为观察：分区选择/加载/擦除流程）：ERASE_FLASH(0x0A) + 选择包（size=0）
//   • 按名写分区（行为观察：分区选择/加载/擦除流程）：START_DATA(0x01) + 选择包 → ACK →
//     MIDST_DATA×N（块 ≤4096，逐块 ACK，15s 超时）→ END_DATA → ACK；
//     分区写无 EXEC_DATA（区别于 FDL 上传）。真机验证待后续（诚实边界）。
//   • 机型范围：展锐芯片系（实测前不承诺具体型号）。

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <functional>

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
    // 按名擦除分区（行为观察：分区选择/加载/擦除流程）：ERASE_FLASH(0x0A) + 分区选择包
    // （name 36×UTF-16LE + size LE32=0，载荷 76B）→ ACK
    bool erasePartition(const QString &name, QString *error = nullptr);
    // 按名写分区（行为观察：分区选择/加载/擦除流程）：START_DATA(0x01) + 分区选择包
    // （name 36×UTF-16LE + size LE32 + size_hi LE32 + dummy 8B，mode64 时载荷
    // 88B 否则 76B）→ ACK → MIDST_DATA×N（块 ≤4096，逐块 ACK，15s 超时）→
    // END_DATA → ACK。分区写无 EXEC_DATA（行为观察核实）；真机验证待后续
    bool writePartition(const QString &name, const QByteArray &data,
                        QString *error = nullptr);

private:
    // 发送命令并强制校验响应 type == BSL_REP_ACK（行为观察：发后强制校验 ACK 响应；
    // 设备错误响应如 0x84 不得当成功）。timeoutMs 默认 2000，EXEC_DATA 传 15000。
    bool sendAndExpectAck(quint16 type, const QByteArray &payload, QString *error,
                          int timeoutMs = 2000);
    // 分区选择包（行为观察：分区选择/加载/擦除流程）：以 cmd 携带该包并强制 ACK
    bool selectPartition(quint16 cmd, const QString &name, quint64 size,
                         QString *error);
    SpdSession &m_session;
};

// 集成路由：pac → 枚举 → 会话 → FDL 上传 → 逐分区刷写（诚实边界见 cpp）
bool runSpdFlash(const QString &pacPath, const QString &fdl1Path, const QString &fdl2Path,
                 std::function<void(const QString &name, int percent)> progress,
                 QString *error = nullptr);
// 纯函数（单测）：fdl/fdl1/fdl2 分区名判定（诚实边界用——FDL 单独上传不参与普通刷写）
bool isFdlPartition(const QString &partitionName);
// 分区选择包（行为观察：分区选择/加载/擦除流程）：name 36×UTF-16LE + size LE32
// （mode64 时另含 size_hi LE32 + dummy 8B）——载荷 76B 或 88B。
// 纯函数（单测覆盖 mode64 88B 布局）；mode64 由调用方按 size 高位判定
QByteArray selectPartitionPacket(const QString &name, quint64 size, bool mode64);

} // namespace spd
