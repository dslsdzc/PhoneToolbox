#pragma once

// MTK GPT 解析（Phase D2/D3）—— **纯函数**，只依赖 Qt Core；设备数据经 ReadFn 注入。
//
// 与上游（mtkclient `Library/Partitions/gpt.py`、`Library/partition.py`、`Library/realtime.py`）的**有意差异**：
//   ① **头部 CRC 与条目表 CRC 都校验**（上游 `gpt.py:48` 解析但从不比较）→ 不符即 fail-closed
//   ② **备份 GPT 兜底真的生效**（上游 `partition.py:45` 的 seek 被 `gpt.py:161` 的绝对 seek 覆盖）
//   ③ **扇区大小可探测**（512→4096）—— 上游 eMMC 恒 512（`XFL:448` 读出的 `emmc.block_size` 从未赋给它）
//   ④ **空条目判据 = type GUID 全 0 且逐条 continue**（UEFI 规范；上游 `gpt.py:192-193` 用 unique
//      GUID 且**遇空即 break** 停止扫描 → 表中间有空槽时会漏掉其后的分区）
//   ⑤ 条目区起点只认头部的 `part_entry_start_lba × sectorSize`（上游命令行覆盖项被当**字节偏移**用）
//
// 真样本布局（实测，`reference/mtk-samples/`）：
//   PGPT.img = 8×4096 字节，"EFI PART"@4096（LBA1），条目@8192（LBA2），双 CRC OK
//   SGPT.img = 8×4096 字节，"EFI PART"@28672（**窗口最后一扇区**），part_entry_start_lba=124960760
//             （= 窗口起始 LBA）→ **条目在窗口开头、头在末尾**（UEFI 备份布局）

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <functional>

namespace mtkgpt {

struct Partition {
    QString name;
    quint64 firstLba = 0;
    quint64 lastLba = 0;      // 含端点
    QByteArray typeGuid;      // 16 字节原始
    QByteArray uniqueGuid;
    quint64 flags = 0;
};

struct Table {
    quint32 sectorSize = 512;
    QList<Partition> partitions;
    bool usedBackup = false;
    quint64 diskSectors = 0;
    QByteArray diskGuid;      // 16 字节
};

// 读回调契约：把 [byteOffset, byteOffset + length) 读进 *out，成功返回 true。
// **允许短读**（*out 少于 length，如读到设备/文件末尾）；但"这个范围读不了"必须返回 false
// 并填 error —— 两者不得混用：readTable 靠 false 区分"读不到"与"读到的就这么长"，
// 并据此决定是报错还是让 parsePrimary 判尺寸。length 恒为正。
using ReadFn = std::function<bool(quint64 byteOffset, int length, QByteArray *out, QString *error)>;

// 读主 GPT；主 GPT 不可用（签名/revision/双 CRC/尺寸任一不符）时读末尾备份 GPT 兜底。
// diskSectors = 磁盘总扇区数（0 = 未知 → 备份兜底不可用，只报主 GPT 的错）。
// 失败路径 out 的**内容不保证**（可能仍是上一次的内容），调用方只以返回值判成败。
bool readTable(const ReadFn &read, quint64 diskSectors, Table &out,
               QStringList *log = nullptr, QString *error = nullptr);

// raw = **从字节 0（保护 MBR）起**的缓冲，至少含 LBA1 头 + 整张条目表（parsePrimary 会按
// 头部的 part_entry_start_lba × sectorSize 定位条目区，并校验两个 CRC）。
bool parsePrimary(const QByteArray &raw, quint32 sectorSize, Table &out, QString *error = nullptr);

// raw = **磁盘末尾若干扇区**的窗口（长度须为扇区整数倍）；windowFirstLba = 该窗口第一扇区的
// LBA。窗口**最后一扇区**必须是备份头；条目区位置取头部字段 part_entry_start_lba（真样本
// SGPT.img 的窗口 = 8 扇区：条目在窗口开头、头在末尾；table.diskSectors 按窗口推算）。
bool parseBackup(const QByteArray &raw, quint32 sectorSize, quint64 windowFirstLba, Table &out,
                 QString *error = nullptr);

quint64 offsetBytes(const Partition &p, quint32 sectorSize);
quint64 sizeBytes(const Partition &p, quint32 sectorSize);

// **用例辅助**（生产 .cpp 里，理由见计划 Step 3 注）：造一份含单分区 "boot"（LBA 34..35）的合法 GPT
QByteArray testBuildSyntheticGpt(quint32 sectorSize, quint32 sectorCount);

} // namespace mtkgpt
