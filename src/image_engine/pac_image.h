#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

namespace imgpac {

// .pac 固件容器分区描述。注意: .pac 实为 Spreadtrum/Unisoc（SPD ResearchDownload）
// 固件容器（社区所有 pac 解包工具均针对 SPD; MTK SP Flash Tool 使用 scatter 文件）。
// 精确布局以社区参考实现为准（两源独立确认）:
//   divinebird/pacextractor (C)                     —— 旧格式, 头 1220B 无魔数
//   HemanthJabalapuri/pacextractor (Java/Python)    —— 新格式 BP_R1.0.0 / BP_R2.0.1
// parse 支持两种布局自动识别（新格式按版本串识别, 旧格式按结构校验）,
// 字段级偏移与参考源码对照标注, 见 pac_image.cpp。
struct PacPartition {
    QString name;        // 分区 ID（如 FDL1 / preloader / boot / system）
    QString fileName;    // 分区数据在容器内的文件名（如 boot.img）
    quint64 offset = 0;  // 分区数据在 pac 文件中的绝对偏移
    quint64 size = 0;    // 分区数据长度（字节；0 = 操作型条目无数据，如 "FLASH"）
};

bool isPac(const QByteArray &pac);                              // 能完整解析即为 pac
bool parsePac(const QByteArray &pac, QList<PacPartition> &out, QString *error);

} // namespace imgpac
