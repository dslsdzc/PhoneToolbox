// src/core/modes/mtk_chip_table.cpp
//
// ⚠️ **本文件由脚本生成，请勿手改** —— 改表请改生成脚本后重跑：
//     python3 tools/gen_mtk_chip_table.py src/core/modes/mtk_chip_table.cpp
// 生成器写同目录临时文件、成功后原子换入 ⇒ 它**任何**拒绝生成都不会碰本文件。
// ⚠️ **不要**用 `python3 … > src/core/modes/mtk_chip_table.cpp`：bash 下重定向先截断再 exec，
//     生成器一旦拒绝，本文件会被留成 0 字节（实测 17 → 0）—— 该调用式自 W8-T2 起弃用。
//
// 数据来源：bkerler/mtkclient（GPL-3.0）的 mtkclient/config/brom_config.py 的 `hwconfig` 表
//     commit: 71b01752d39461dfbf9c21bc29d7b0e121c40483
//     上游 URL: https://github.com/bkerler/mtkclient
//     上游原文: https://github.com/bkerler/mtkclient/blob/71b01752d39461dfbf9c21bc29d7b0e121c40483/mtkclient/config/brom_config.py
// 本项目为 GPLv3，与本表许可兼容；转写只取静态事实（hw_code / dacode / damode / iot +
// 7 个硬件地址字段），不复制上游代码。
// 本表 89 条 = 上游 hwconfig 的全部条目（其中 iot=True 20 条、dacode 按 hw_code 回填 1 条）；damode 取值同上游 DAmodes。
//
// 本文件含**两张同源表**（同一次生成、同一个 hwconfig）：kChips（代际判定）与
// kHwAddrs（逐芯片硬件地址，W8-T2）。两张表的 hw_code 集合必须完全相同 ——
// 否则 DA 上传路径与漏洞利用路径会对同一颗芯片给出互相矛盾的结论。
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

// —— 逐芯片硬件地址（W8-T2）：同一张上游表的另 7 个字段 ——
// 列序同 mtk_chip_table.h 的 ChipHwAddrs（生成期已核对该 struct 的成员顺序，不一致即拒绝生成）：
//   hwCode, watchdog, uart, bromPayloadAddr, daPayloadAddr, plPayloadAddr, cqdmaBase, sejBase,
//   hasWatchdog, hasUart, hasBromPayloadAddr, hasDaPayloadAddr, hasPlPayloadAddr, hasCqdmaBase, hasSejBase
// **缺的字段写「值槽 0 + 存在位 false」** —— 值槽里的 0 不是「地址 0」，是「上游没给」；
// 反过来 present ⇒ 值必非 0（生成器拒绝 0 值）。谁要往硬件写地址，先查存在位。
// 覆盖（生成器实测，改上游后以 tests/test_mtk_chip_hwaddrs.cpp 的复核为准）：
//   watchdog 71/89  uart 66/89  brom_payload_addr 68/89  da_payload_addr 74/89  pl_payload_addr 66/89  cqdma_base 38/89  sej_base 68/89
//   七字段全齐 37；kamakiri 三要素（watchdog+uart+任一 payload）齐全 66（其中 29 条字段不全但可用）；
//   三要素缺 23（其中七字段全缺 10 条：0x6236 0x6238 0x6253 0x6256 0x625a 0x6268 0x6270 0x6276 0x6291 0x8518） ⇒ 漏洞利用对它们**不可用**。
const ChipHwAddrs kHwAddrs[] = {
    {0x0279, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212C00, 0x1000A000, true, true, true, true, true, true, true},
    {0x0321, 0x10212000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10217C00, 0x10008000, true, true, true, true, true, true, true},
    {0x0326, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212C00, 0x1000A000, true, true, true, true, true, true, true},
    {0x0335, 0x10212000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10217C00, 0x10008000, true, true, true, true, true, true, true},
    {0x0337, 0x10212000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10217C00, 0x10008000, true, true, true, true, true, true, true},
    {0x0507, 0x10210000, 0x11020000, 0x00100A00, 0x00201000, 0x00000000, 0x00000000, 0x00000000, true, true, true, true, false, false, false},
    {0x0551, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212C00, 0x1000A000, true, true, true, true, true, true, true},
    {0x0562, 0x10211000, 0x11020000, 0x00100A00, 0x00201000, 0x40200000, 0x11B30000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0571, 0x10007000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, true, false, false, false, false, false, false},
    {0x0598, 0x10211000, 0x11020000, 0x00100A00, 0x00201000, 0x00000000, 0x10212C00, 0x1000A000, true, true, true, true, false, true, true},
    {0x0601, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212C00, 0x1000A000, true, true, true, true, true, true, true},
    {0x0633, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x80001000, 0x1020AC00, 0x1000A000, true, true, true, true, true, true, true},
    {0x0688, 0x10211000, 0x11020000, 0x00100A00, 0x00201000, 0x40200000, 0x10200000, 0x10080000, true, true, true, true, true, true, true},
    {0x0690, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0699, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0707, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0717, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0725, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0766, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0788, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0813, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0816, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0886, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0907, 0x1C007000, 0x11001000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0908, 0x10007000, 0x00000000, 0x00100A00, 0x00201000, 0x00000000, 0x00000000, 0x00000000, true, false, true, true, false, false, false},
    {0x0930, 0x10007000, 0x11001200, 0x00100A00, 0x00201000, 0x40200000, 0x00000000, 0x00000000, true, true, true, true, true, false, false},
    {0x0950, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0959, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0989, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x0992, 0x10007000, 0x11003000, 0x00100A00, 0x00201000, 0x00000000, 0x00000000, 0x1000A000, true, true, true, true, false, false, true},
    {0x0996, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x1066, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x00000000, 0x1000A000, true, true, true, true, true, false, true},
    {0x1129, 0x1C007000, 0x11001000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x1172, 0x1C007000, 0x11001000, 0x00100A00, 0x00201000, 0x40200000, 0x10212000, 0x1C009000, true, true, true, true, true, true, true},
    {0x1203, 0x1C007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x00000000, 0x1040E000, true, true, true, true, true, false, true},
    {0x1208, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40200000, 0x00000000, 0x1000A000, true, true, true, true, true, false, true},
    {0x1209, 0x1C007000, 0x11002000, 0x00000000, 0x02001000, 0x40200000, 0x00000000, 0x1000A000, true, true, false, true, true, false, true},
    {0x1229, 0x1C007000, 0x11002000, 0x00100A00, 0x02001000, 0x40200000, 0x00000000, 0x1C009000, true, true, true, true, true, false, true},
    {0x1236, 0x1C00B000, 0x00000000, 0x00100A00, 0x02001000, 0x40200000, 0x00000000, 0x1040E000, true, false, true, true, true, false, true},
    {0x1296, 0x1C007000, 0x1C011000, 0x00100A00, 0x00201000, 0x40200000, 0x00000000, 0x1C009000, true, true, true, true, true, false, true},
    {0x1357, 0x1C010000, 0x16000000, 0x00100A00, 0x00201000, 0x40200000, 0x00000000, 0x1800E000, true, true, true, true, true, false, true},
    {0x1375, 0x1C00A000, 0x11001000, 0x00100A00, 0x02010000, 0x40200000, 0x00000000, 0x1040E000, true, true, true, true, true, false, true},
    {0x1471, 0x1C010000, 0x16010000, 0x00100A00, 0x00201000, 0x40200000, 0x00000000, 0x1800E000, true, true, true, true, true, false, true},
    {0x2523, 0x10007000, 0x11005000, 0x00100A00, 0x02008000, 0x81E00000, 0x00000000, 0x1000A000, true, true, true, true, true, false, true},
    {0x2601, 0x10007000, 0x11005000, 0x00100A00, 0x02008000, 0x81E00000, 0x00000000, 0x1000A000, true, true, true, true, true, false, true},
    {0x2625, 0x10007000, 0x11005000, 0x00100A00, 0x04001000, 0x00000000, 0x00000000, 0x1000A000, true, true, true, true, false, false, true},
    {0x3967, 0x00000000, 0x00000000, 0x00100A00, 0x00201000, 0x40020000, 0x00000000, 0x00000000, false, false, true, true, true, false, false},
    {0x5932, 0x00000000, 0x00000000, 0x00100A00, 0x00201000, 0x40020000, 0x00000000, 0x00000000, false, false, true, true, true, false, false},
    {0x6225, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80140000, false, false, false, false, false, false, true},
    {0x6226, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80140000, false, false, false, false, false, false, true},
    {0x6236, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, false, false, false, false, false, false, false},
    {0x6238, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, false, false, false, false, false, false, false},
    {0x6253, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, false, false, false, false, false, false, false},
    {0x6255, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80140000, false, false, false, false, false, false, true},
    {0x6256, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, false, false, false, false, false, false, false},
    {0x625A, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, false, false, false, false, false, false, false},
    {0x6261, 0xA0030000, 0xA0080000, 0x00100A00, 0x00201000, 0x00000000, 0x00000000, 0xA0110000, true, true, true, true, false, false, true},
    {0x6268, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, false, false, false, false, false, false, false},
    {0x6270, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, false, false, false, false, false, false, false},
    {0x6276, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, false, false, false, false, false, false, false},
    {0x6280, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x80080000, false, false, false, false, false, false, true},
    {0x6291, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, false, false, false, false, false, false, false},
    {0x6516, 0x10003000, 0x10023000, 0x00000000, 0x00201000, 0x00000000, 0x00000000, 0x1002D000, true, true, false, true, false, false, true},
    {0x6571, 0x10007400, 0x00000000, 0x00000000, 0x02009000, 0x80001000, 0x00000000, 0x00000000, true, false, false, true, true, false, false},
    {0x6572, 0x10007000, 0x11005000, 0x010036A0, 0x02008000, 0x81E00000, 0x00000000, 0x00000000, true, true, true, true, true, false, false},
    {0x6573, 0x70025000, 0x00000000, 0x00000000, 0x90006000, 0xF1020000, 0x00000000, 0x7002A000, true, false, false, true, true, false, true},
    {0x6575, 0xC0000000, 0xC1009000, 0xF0000A00, 0xC2001000, 0xC2058000, 0x00000000, 0xC101A000, true, true, true, true, true, false, true},
    {0x6577, 0xC0000000, 0xC1009000, 0x00000000, 0xC2001000, 0xC2058000, 0x00000000, 0xC101A000, true, true, false, true, true, false, true},
    {0x6580, 0x10007000, 0x11005000, 0x00100A00, 0x00201000, 0x80001000, 0x1020AC00, 0x1000A000, true, true, true, true, true, true, true},
    {0x6582, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x80001000, 0x00000000, 0x1000A000, true, true, true, true, true, false, true},
    {0x6583, 0x10000000, 0x11006000, 0x00100A00, 0x12001000, 0x80001000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x6592, 0x10007000, 0x11002000, 0x00100A00, 0x00111000, 0x80001000, 0x10212000, 0x1000A000, true, true, true, true, true, true, true},
    {0x6595, 0x10007000, 0x11002000, 0x00100A00, 0x00111000, 0x00000000, 0x00000000, 0x1000A000, true, true, true, true, false, false, true},
    {0x6752, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40001000, 0x10212C00, 0x1000A000, true, true, true, true, true, true, true},
    {0x6795, 0x10007000, 0x11002000, 0x00100A00, 0x00110000, 0x40200000, 0x10212C00, 0x1000A000, true, true, true, true, true, true, true},
    {0x6899, 0x1C010000, 0x16010000, 0x00100A00, 0x00201000, 0x40200000, 0x00000000, 0x1040E000, true, true, true, true, true, false, true},
    {0x7682, 0x00000000, 0x00000000, 0x00100A00, 0x00201000, 0x40020000, 0x00000000, 0x00000000, false, false, true, true, true, false, false},
    {0x7686, 0x00000000, 0x00000000, 0x00100A00, 0x00201000, 0x40020000, 0x00000000, 0x00000000, false, false, true, true, true, false, false},
    {0x8127, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x80001000, 0x00000000, 0x1000A000, true, true, true, true, true, false, true},
    {0x8135, 0x10000000, 0x11002000, 0x00000000, 0x12001000, 0x80001000, 0x00000000, 0x00000000, true, true, false, true, true, false, false},
    {0x8163, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40001000, 0x10212C00, 0x1000A000, true, true, true, true, true, true, true},
    {0x8167, 0x10007000, 0x11005000, 0x00100A00, 0x00201000, 0x40001000, 0x10212C00, 0x1000A000, true, true, true, true, true, true, true},
    {0x8168, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40001000, 0x00000000, 0x1000A000, true, true, true, true, true, false, true},
    {0x8172, 0x10007000, 0x11002000, 0x00120A00, 0x000C0000, 0x40001000, 0x10212C00, 0x1000A000, true, true, true, true, true, true, true},
    {0x8176, 0x10007000, 0x11002000, 0x00120A00, 0x000C0000, 0x40200000, 0x10212C00, 0x1000A000, true, true, true, true, true, true, true},
    {0x8512, 0x10007000, 0x11002000, 0x00100A00, 0x00111000, 0x40200000, 0x10214000, 0x1000A000, true, true, true, true, true, true, true},
    {0x8518, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, false, false, false, false, false, false, false},
    {0x8590, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x80001000, 0x00000000, 0x1000A000, true, true, true, true, true, false, true},
    {0x8695, 0x10007000, 0x11002000, 0x00100A00, 0x00201000, 0x40001000, 0x00000000, 0x1000A000, true, true, true, true, true, false, true},
};
constexpr int kHwAddrsCount = int(sizeof(kHwAddrs) / sizeof(kHwAddrs[0]));

const ChipHwAddrs *lookupChipHwAddrs(quint16 hwCode)
{
    // 线性查找，理由同 lookupChip（表小、调用点极少）。只按 hw_code 命中：
    // 字段不全的芯片**照样返回非空**（缺哪个由 hasXxx 表达），只有未收录才是 nullptr。
    for (int i = 0; i < kHwAddrsCount; ++i)
        if (kHwAddrs[i].hwCode == hwCode)
            return &kHwAddrs[i];
    return nullptr;
}

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
