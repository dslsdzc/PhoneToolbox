#pragma once

// 芯片表：hw_code → (dacode, DA 协议代, IoT 位)（计划 D1 Task 3）
//
// 表体在 mtk_chip_table.cpp，**由脚本生成，请勿手改**（改了会被下次重跑覆盖）：
//   python3 tools/gen_mtk_chip_table.py > src/core/modes/mtk_chip_table.cpp
//
// 出处（mtkclient v2.1.4.1-20-g71b0175 / commit 71b01752d39461dfbf9c21bc29d7b0e121c40483，
// GPL-3.0，与本品 GPLv3 兼容；**只读转写静态事实，不 import 不复制其代码**）：
//   mtkclient/config/brom_config.py:1-4     class DAmodes（LEGACY=3 / XFLASH=5 / XML=6 ——
//                                           本枚举取值与之一致，改写即与上游 DA 装载类错配）
//   mtkclient/config/brom_config.py:343     class Chipconfig（条目字段名 damode/dacode/iot/name）
//   mtkclient/config/brom_config.py:479     hwconfig = { ... }（该 commit 下 89 条，唯一来源表）
// 上游 URL: https://github.com/bkerler/mtkclient
//
// 为什么一张表就能判代际：hw_code 与 DA 协议代强绑定（LEGACY/XFLASH/XML 是三代不同的
// DA 上传+跳转流程），表外芯片**判不出来**。
//
// 三条铁律（违反即砖机或静默走错协议）：
//   1. **未收录 → nullptr，上层必须明确报错，不得猜测代际**（spec §8）。
//      尤其**不得**回退成"默认 LEGACY"——把 XFLASH/XML 设备按 LEGACY 流程写就是砖。
//   2. `iot` 位是**另一套 region 映射**的开关（IoT 芯片的 entryRegionIndex 特例，
//      见 mtk_da_file.h 的 P5）：IoT 芯片在 LEGACY 下 region[0]/region[1] 的含义与常规不同，
//      D1 **不实现** → 上层拿到 iot == true 必须显式拒绝（Task 2 审查 ⚠️ 项）。
//   3. 表内**无 dacode == 0 的条目**（缺省回填 hw_code）：dacode 是 DA 条目匹配键，
//      0 会让匹配必然落空，且与 hw_code == 0 的占位条目相撞
//      （selectDaEntry 规则 0 亦拒绝 dacode == 0，见 mtk_da_file.h）。
//
// 注意 dacode **不恒等于** hw_code：上游 89 条里有 31 条两者不同（如 0x1066 → 0x6781、
// 0x633 → 0x6570），dacode 才是查 DA 条目的键，hw_code 只是设备报的 ID。

#include <QString>
#include <QtGlobal>

namespace mtkbrom {

// DA 协议代（取值同 mtkclient DAmodes：LEGACY=3 / XFLASH=5 / XML=6）
enum class DaMode { Legacy = 3, XFlash = 5, Xml = 6 };

struct ChipInfo {
    quint16 hwCode = 0;
    quint16 dacode = 0;                    // DA 条目匹配用的 dacode（通常 == hwCode，见上）
    DaMode  damode = DaMode::Legacy;
    bool    iot = false;                   // 上游 hwconfig 的 iot=True（如 0x6226）—— 上层据此拒绝
};

// 芯片表（**由 tools/gen_mtk_chip_table.py 从 mtkclient 的 hwconfig 转写**，见 mtk_chip_table.cpp 顶部）。
// 未收录 → nullptr —— **上层必须明确报错（"未收录的芯片，无法判定协议代"），不得猜测代际**。
const ChipInfo *lookupChip(quint16 hwCode);
int chipTableSize();
QString damodeName(DaMode m);

} // namespace mtkbrom
