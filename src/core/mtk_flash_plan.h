#pragma once

// MTK 刷写计划层（Phase D1 Task 7；spec §6）
//
// 职责：把"用户选的镜像文件"匹配到"分区参照表" → 计划（按参照表顺序）+ 告警 + 进度分母。
// **不碰设备、不读 USB**：参照表有两个来源，本模块不关心来源 ——
//   • 预览：scatter（用户提供，可选）或空表（按镜像文件名推导目标分区名）
//   • 写入：设备实读分区表（EmPartition → PartitionRef 由调用方转换）—— **写入判据以设备为准**
// 两类不匹配（镜像 > 分区 / 参照表里没这个分区）只告警不中止；**镜像 > 分区 = 跳过不写**
// （写超分区会覆盖相邻分区数据，不可逆）。
//
// 只依赖 Qt Core：**不得** include mtk_emmc.h / mtk_brom.h（否则测试目标会被拖进 libusb）。
#ifndef MTK_FLASH_PLAN_H
#define MTK_FLASH_PLAN_H

#include <QList>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "core/modes/mtk_gpt.h"   // 只依赖 Qt Core 的纯函数模块（不拖进 libusb）

namespace mtkplan {

struct PartitionRef {
    QString name;
    quint64 sizeBytes = 0;   // 0 = 未知（不参与大小校验）
};

struct PlanEntry {
    QString partition;            // 目标分区名（参照表原样 / 推导）
    QString imagePath;
    quint64 imageSize = 0;
    quint64 partitionSize = 0;    // 0 = 未知
    QString matchRule;            // "exact" | "prefix" | "derived"
};

struct MtkFlashPlan {
    QList<PlanEntry> entries;     // 按参照表顺序（无参照表时按镜像选择顺序）
    QStringList warnings;
    quint64 totalBytes = 0;       // 匹配到的镜像字节和（进度分母）
    int skippedOversize = 0;
};

// 参照表为空 → 目标分区名由镜像文件名推导（"derived"）+ 一条告警；镜像列表为空 → 明确失败。
// 只返回 false 的两种情况：镜像列表为空 / 有镜像读不到。其余一律只告警并返回 true
// （含"没匹配上""放不下"——调用方按 entries.isEmpty() / skippedOversize 自行门控）。
bool buildMtkPlan(const QList<PartitionRef> &partitions, const QStringList &imagePaths,
                  MtkFlashPlan &out, QString *error = nullptr);

// MTK 固件包内的 Android_scatter.txt（只取 partition_name / partition_size）；0 个分区 → 明确失败。
bool parseScatter(const QString &text, QList<PartitionRef> &out, QString *error = nullptr);

// 现代 scatter 是 **XML 方言**（真样本 MT6789_Android_scatter.xml）：以 `<partition_index name="SYSx">` 为块，
// 块内 `<partition_name>` / `<partition_size>` / `<storage>`。⚠️ 真样本 130 块 = EMMC 与 UFS **两份完整副本**
// （每份 65）→ **必须按 storage 过滤**，否则每个分区翻倍且大小对不上。
enum class ScatterStorage { Emmc, Ufs };
bool parseScatterXml(const QString &text, ScatterStorage want, QList<PartitionRef> &out,
                     QString *error = nullptr);

// **方言识别 + 双副本调和**（UI 只调这一个）：文本方言 → parseScatter；XML 方言 → 两份副本都解析，
// 名字→大小**完全一致**才采信；不一致 → 用 EMMC 那份 + 往 log 写告警（**不猜**：预览是咨询性的，
// 写入判据永远以设备实读的分区表为准）；只有一份可用时说明另一份为何失败。
bool parseScatterAnyDialect(const QString &text, QList<PartitionRef> &out,
                            QStringList *log = nullptr, QString *error = nullptr);

// GPT 表 → 参照表（预览用；写入判据仍以设备实读为准）
QList<PartitionRef> toPartitionRefs(const QList<mtkgpt::Partition> &parts, quint32 sectorSize);

QStringList planHeaders();                    // 表头（5 列）
QList<QStringList> planRows(const MtkFlashPlan &plan);
QString planSummaryHtml(const MtkFlashPlan &plan);

} // namespace mtkplan

#endif // MTK_FLASH_PLAN_H
