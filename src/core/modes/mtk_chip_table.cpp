// src/core/modes/mtk_chip_table.cpp
//
// ⚠️ **本文件由脚本生成，请勿手改** —— 改表请改生成脚本后重跑：
//     python3 tools/gen_mtk_chip_table.py > src/core/modes/mtk_chip_table.cpp
//
// 数据来源：bkerler/mtkclient（GPL-3.0）的 mtkclient/config/brom_config.py 的 `hwconfig` 表
//     commit: 71b01752d39461dfbf9c21bc29d7b0e121c40483
//     上游 URL: https://github.com/bkerler/mtkclient
//     上游原文: https://github.com/bkerler/mtkclient/blob/71b01752d39461dfbf9c21bc29d7b0e121c40483/mtkclient/config/brom_config.py
// 本项目为 GPLv3，与本表许可兼容；转写只取静态事实（hw_code / dacode / damode / iot）。
// 本表 89 条 = 上游 hwconfig 的全部条目（其中 iot=True 20 条、dacode 按 hw_code 回填 1 条）；damode 取值同上游 DAmodes。
#include "mtk_chip_table.h"

namespace mtkbrom {
namespace {
const ChipInfo kChips[] = {
    {0x0279, 0x6797, DaMode::XFlash, false},
    {0x0321, 0x6735, DaMode::Legacy, false},
    {0x0326, 0x6755, DaMode::XFlash, false},
    {0x0335, 0x6735, DaMode::Legacy, false},
    {0x0337, 0x6735, DaMode::Legacy, false},
    {0x0507, 0x6758, DaMode::Legacy, false},
    {0x0551, 0x6757, DaMode::XFlash, false},
    {0x0562, 0x6799, DaMode::XFlash, false},
    {0x0571, 0x0571, DaMode::Legacy, false},
    {0x0598, 0x0598, DaMode::Legacy, false},
    {0x0601, 0x6755, DaMode::XFlash, false},
    {0x0633, 0x6570, DaMode::XFlash, false},
    {0x0688, 0x6758, DaMode::XFlash, false},
    {0x0690, 0x6763, DaMode::XFlash, false},
    {0x0699, 0x6739, DaMode::XFlash, false},
    {0x0707, 0x6768, DaMode::XFlash, false},
    {0x0717, 0x6761, DaMode::XFlash, false},
    {0x0725, 0x6779, DaMode::XFlash, false},
    {0x0766, 0x6765, DaMode::XFlash, false},
    {0x0788, 0x6771, DaMode::XFlash, false},
    {0x0813, 0x6785, DaMode::XFlash, false},
    {0x0816, 0x6885, DaMode::XFlash, false},
    {0x0886, 0x6873, DaMode::XFlash, false},
    {0x0907, 0x0907, DaMode::Xml, false},
    {0x0908, 0x8696, DaMode::XFlash, false},
    {0x0930, 0x8195, DaMode::XFlash, false},
    {0x0950, 0x6893, DaMode::XFlash, false},
    {0x0959, 0x6877, DaMode::XFlash, false},
    {0x0989, 0x6833, DaMode::XFlash, false},
    {0x0992, 0x0992, DaMode::XFlash, false},
    {0x0996, 0x6853, DaMode::XFlash, false},
    {0x1066, 0x6781, DaMode::XFlash, false},
    {0x1129, 0x1129, DaMode::Xml, false},
    {0x1172, 0x1172, DaMode::Xml, false},
    {0x1203, 0x1203, DaMode::Xml, false},
    {0x1208, 0x1208, DaMode::Xml, false},
    {0x1209, 0x1209, DaMode::Xml, false},
    {0x1229, 0x1229, DaMode::Xml, false},
    {0x1236, 0x1236, DaMode::Xml, false},
    {0x1296, 0x1296, DaMode::Xml, false},
    {0x1357, 0x1357, DaMode::Xml, false},
    {0x1375, 0x1375, DaMode::Xml, false},
    {0x1471, 0x1471, DaMode::Xml, false},
    {0x2523, 0x2523, DaMode::Legacy, true},
    {0x2601, 0x2601, DaMode::Legacy, true},
    {0x2625, 0x2625, DaMode::Legacy, true},
    {0x3967, 0x3967, DaMode::Legacy, false},
    {0x5932, 0x5932, DaMode::Legacy, true},
    {0x6225, 0x6225, DaMode::Legacy, true},
    {0x6226, 0x6226, DaMode::Legacy, true},
    {0x6236, 0x6236, DaMode::Legacy, true},
    {0x6238, 0x6238, DaMode::Legacy, true},
    {0x6253, 0x6253, DaMode::Legacy, true},
    {0x6255, 0x6255, DaMode::Legacy, true},
    {0x6256, 0x6256, DaMode::Legacy, true},
    {0x625A, 0x625A, DaMode::Legacy, true},
    {0x6261, 0x6261, DaMode::Legacy, true},
    {0x6268, 0x6268, DaMode::Legacy, true},
    {0x6270, 0x6270, DaMode::Legacy, true},
    {0x6276, 0x6276, DaMode::Legacy, true},
    {0x6280, 0x6280, DaMode::Legacy, true},
    {0x6291, 0x6291, DaMode::Legacy, true},
    {0x6516, 0x6516, DaMode::Legacy, false},
    {0x6571, 0x6571, DaMode::Legacy, false},
    {0x6572, 0x6572, DaMode::Legacy, false},
    {0x6573, 0x6573, DaMode::Legacy, false},
    {0x6575, 0x6575, DaMode::Legacy, false},
    {0x6577, 0x6577, DaMode::Legacy, false},
    {0x6580, 0x6580, DaMode::Legacy, false},
    {0x6582, 0x6582, DaMode::Legacy, false},
    {0x6583, 0x6583, DaMode::Legacy, false},
    {0x6592, 0x6592, DaMode::Legacy, false},
    {0x6595, 0x6595, DaMode::Legacy, false},
    {0x6752, 0x6752, DaMode::Legacy, false},
    {0x6795, 0x6795, DaMode::Legacy, false},
    {0x6899, 0x1357, DaMode::Xml, false},
    {0x7682, 0x7682, DaMode::Legacy, true},
    {0x7686, 0x7686, DaMode::Legacy, true},
    {0x8127, 0x8127, DaMode::Legacy, false},
    {0x8135, 0x8135, DaMode::Legacy, false},
    {0x8163, 0x8163, DaMode::Legacy, false},
    {0x8167, 0x8167, DaMode::XFlash, false},
    {0x8168, 0x8168, DaMode::XFlash, false},
    {0x8172, 0x8173, DaMode::Legacy, false},
    {0x8176, 0x8173, DaMode::Legacy, false},
    {0x8512, 0x8512, DaMode::XFlash, false},
    {0x8518, 0x8518, DaMode::XFlash, false},
    {0x8590, 0x8590, DaMode::Legacy, false},
    {0x8695, 0x8695, DaMode::XFlash, false},
};
constexpr int kChipCount = int(sizeof(kChips) / sizeof(kChips[0]));
} // namespace

const ChipInfo *lookupChip(quint16 hwCode)
{
    // 线性查找：表按 hw_code 升序，二分更快，但表只有 89 项且调用点极少（每次会话一次）
    for (int i = 0; i < kChipCount; ++i)
        if (kChips[i].hwCode == hwCode)
            return &kChips[i];
    return nullptr;
}

int chipTableSize() { return kChipCount; }

QString damodeName(DaMode m)
{
    switch (m) {
    case DaMode::Legacy: return QStringLiteral("LEGACY");
    case DaMode::XFlash: return QStringLiteral("XFLASH");
    case DaMode::Xml:    return QStringLiteral("XML");
    }
    return QStringLiteral("未知");
}

} // namespace mtkbrom
