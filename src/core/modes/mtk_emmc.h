#pragma once

// MTK DA 存储命令层（计划 F1-2）
//
// 帧结构对照 mtkclient v2.1.4：
//   • 内存读写   → Library/mtk_preloader.py read()/write()
//   • EMMC 写    → Library/DA/legacy/dalegacy_lib.py sdmmc_write_data()
//   • EMMC 读    → dalegacy_lib.py readflash() emmc 分支 + sdmmc_switch_part()
//   • 分区表     → dalegacy_lib.py read_pmt()（0x60/0x58/0x4C 三种条目）
//   • 命令号     → Library/DA/legacy/dalegacy_param.py Cmd
//
// 诚实边界：EMMC/UFS 命令属于 DA 阶段（BROM 仅内存协议）。未 setDaActive(true)
// 时存储命令返回明确错误，不假装支持。

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

#include "core/modes/mtk_brom.h"

namespace mtkbrom {

// DA 阶段命令号（对照 dalegacy_param.py Cmd）
enum DaCmd : quint8 {
    DA_CMD_SDMMC_SWITCH_PART = 0x60,
    DA_CMD_SDMMC_WRITE_DATA = 0x62,
    DA_CMD_SDMMC_READ_PMT = 0xA5,
    DA_CMD_READ = 0xD6,
    DA_CMD_FINISH = 0xD9,   // dalegacy_param.py:91 FINISH_CMD
};

// EMMC 分区枚举（对照 sdmmc_switch_part 注释：EMMC_Part_User = 0x8）
constexpr quint8 kEmmcPartUser = 0x08;

struct EmPartition {
    QString name;
    quint64 offsetBytes = 0;
    quint64 sizeBytes = 0;
};

// BROM 内存协议（BromSession 成员，F1-1 头文件追加声明）
// （声明追加在 mtk_brom.h 的 public 区，见 Step 3 注）

// DA 存储命令层（需 DA 运行）
class DaStorage {
public:
    explicit DaStorage(BromSession &session) : m_session(session) {}

    void setDaActive(bool active) { m_daActive = active; }
    bool daActive() const { return m_daActive; }

    // 对照 readflash() emmc 分支 + sdmmc_switch_part()
    bool emmcRead(quint64 addr, quint64 length, QByteArray &out,
                  int partType = kEmmcPartUser, QString *error = nullptr);
    // 对照 sdmmc_write_data()
    bool emmcWrite(quint64 addr, const QByteArray &data,
                   int partType = kEmmcPartUser, QString *error = nullptr);
    // 对照 read_pmt()
    bool listPartitions(QList<EmPartition> &out, QString *error = nullptr);

    // DA 收尾（dalegacy_param.py:91 命令号；dalegacy_lib.py:972-980 finish() 的帧）：
    //   write 0xD9 → 读 1B 须 ACK → write >I value → 读 1B 须 ACK
    // 上游只在交互式 `reset` 命令里调它（dalegacy_lib.py:903-905 shutdown → mtk_da_handler.py:1382），
    // **上游刷写完不复位**；我们在刷写成功末尾调用它作为收尾（value = 0 → ShutDownModes.NORMAL，
    // mtk_daloader.py:307-310），失败**只告警**（数据已落盘，不把成功报成失败 —— 调用方口径）。
    bool finishFlash(quint32 value, QString *error = nullptr);

private:
    bool switchPart(int partType, QString *error);
    bool ensureDa(QString *error) const;

    BromSession &m_session;
    bool m_daActive = false;
};

} // namespace mtkbrom
