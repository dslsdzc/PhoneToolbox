// src/core/edl/flash_plan.h
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace edl {

struct PlanEntry {
    enum class Action { Program, Erase, Patch };
    Action  action = Action::Program;
    QString partitionName;      // rawprogram: label；patch: filename（供日志）
    QString imageFile;          // 绝对路径；Patch 为 "DISK" 时表示下发设备
    quint32 lun = 0;            // physical_partition_number
    quint64 startSector = 0;    // 纯十进制时填此值
    QString startSectorExpr;    // start_sector 非纯十进制时原样保留（firehose 表达式，如 "NUM_DISK_SECTORS-5."）；
                                // 非空时 startSector==0，且发送方必须原样透传该串
                                // （reference/qdl/src/firehose.c:874-879 明确"解析会写错地址"）
    quint64 numSectors = 0;     // Program/Erase：sparse 展开后的 raw 扇区数
    quint32 sectorSize = 4096;  // 逐条目 SECTOR_SIZE_IN_BYTES
    bool    sparse = false;
    quint64 rawBytes = 0;
    QString sha256;             // 可选（rawprogram 无此属性；OPS 元数据有）
    quint64 byteOffset = 0;     // Patch 专用
    quint32 sizeInBytes = 0;    // Patch 专用
    QString value;              // Patch：原样透传（表达式不解释）
    QString what;               // Patch：只进日志，不进 XML
};

struct FlashPlan {
    QString source;             // 计划来源描述
    QString storageType;        // "ufs"/"emmc" → configure 的 MemoryName
    QList<PlanEntry> entries;   // 已排序：先 Erase，再 Program（按 lun、start_sector），最后 Patch
    QStringList warnings;
    quint64 totalBytes = 0;     // Program 条目 rawBytes 之和（进度分母）
};

struct StorageInfo { quint32 lun = 0; quint64 totalBlocks = 0; quint32 blockSize = 4096; };
struct PlanCheck { bool ok = false; QStringList errors; QStringList warnings; };

// 来源①：解析单个 rawprogram{N}.xml / patch{N}.xml（lun 由调用方从文件名序号得出，见 Task 3）
bool parseRawprogramXml(const QString &xmlPath, quint32 lun,
                        QList<PlanEntry> &out, QStringList &warnings, QString *error);
bool parsePatchXml(const QString &xmlPath, quint32 lun,
                   QList<PlanEntry> &out, QStringList &warnings, QString *error);

// 排序 + 统计（spec §3.3/§3.5）：Erase 一律在前，Program 按 (lun, startSector) 升序，Patch 一律最后；
// 同键保持解析顺序（stable_sort）。填 totalBytes = Program 条目 rawBytes（为 0 时退化为
// numSectors × sectorSize）之和 —— 进度分母，Patch/Erase 不计入。
// **调用顺序**：validatePlan 会就地修正 sparse 条目的 numSectors/rawBytes，故应由
// "finalizePlan → validatePlan → finalizePlan（或校验通过后重算）"保证 totalBytes 与最终下发值一致。
void finalizePlan(FlashPlan &plan);

// 刷前校验（spec §3.5 七条规则；在 getstorageinfo 之后、进入写入之前调用）。
// **会就地修正 plan**：sparse 条目按文件头回填 rawBytes、并在与 XML 不符时以头为准修正 numSectors
// （Task 1 的模型契约：rawBytes 由校验步骤填充，flash_plan.cpp:155 注释）。
// 因此参数是**非 const** 引用 —— 调用方传入的必须是可写的 FlashPlan。
// errors 非空 → ok=false（拒刷，绝不放行）；warnings 只进预览与日志。
PlanCheck validatePlan(FlashPlan &plan, const QList<StorageInfo> &device);

} // namespace edl
