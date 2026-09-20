#pragma once

// 芯片表：hw_code → (dacode, DA 协议代, IoT 位)（计划 D1 Task 3）
//
// 表体在 mtk_chip_table.cpp，**由脚本生成，请勿手改**（改了会被下次重跑覆盖）：
//   python3 tools/gen_mtk_chip_table.py src/core/modes/mtk_chip_table.cpp
// （✓ 这条形态：生成器写同目录临时文件后原子换入；**别用 `> 目标`** —— bash 会先截断再 exec，
//   生成器一旦拒绝就把入库文件留成 0 字节，见生成器 docstring）
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

// —— 逐芯片字段（W8-T2 的 7 个硬件地址 + W8-T3 补的 var1）——
//
// 漏洞利用（kamakiri 系）要往载荷尾部写几个 **SoC 寄存器/内存地址**，它们逐芯片不同，
// 而上游 brom_config.py 的 `hwconfig` **给得极不齐**（89 条，上游 71b0175 实测显式给出数）：
//   watchdog 71 / uart 66 / bromPayloadAddr 68 / daPayloadAddr 74 / plPayloadAddr 66 /
//   cqdmaBase 38 / sejBase 68；**var1 56（W8-T3 补的第 8 列）**。
// ⇒ **存在位是这张表的一等公民**：`hasXxx` 为假时 `xxx` 槽里的 0 不是"地址 0"，是"上游没给"。
//   要往硬件写地址的人必须先查存在位，缺了 → **响亮报错（拒绝，不猜）**：往 watchdog 写 0 是砖机。
//   （生成器按对称口径落地：**缺 → 值槽 0 + 存在位假**；**存在 → 值必非 0**，present 且值为 0
//     在生成期就被拒绝。tests/test_mtk_chip_hwaddrs.cpp 把这个当不变量全局扫。）
//
// 上游 71b0175 的实测边界（**上游数据事实，不是本仓缺陷，不许含糊成"未收录"**）：
//   - 89 条里 **23 条缺 watchdog 或 uart**（其中 10 条八个字段全缺，上游原文整段注释掉）
//     ⇒ 漏洞利用对它们**不可用**；它们**仍在表内**（lookup 返回非空），只是凑不齐三要素。
//   - 反过来：**只缺 cqdmaBase** 的有 17 条，而 kamakiri **不用** cqdma
//     ⇒ 若按"八字段全齐才收录"实现，可用芯片会从 **66 条掉到 35 条**（白扔 31 条）。**不许**加这种过滤。
//   - 三要素（watchdog + uart + 任一 payload 地址）齐全的 66 条里，**56 条 var1 也有值**
//     （开箱即用）；剩下 10 条要调用方显式覆盖 var1（上游对它们也没给）。
//   这些数字由 tests/test_mtk_chip_hwaddrs.cpp 钉住（上游换版本重跑生成器后，以用例的复核为准）。
//
// 字段语义（供消费者按需取用，**不是所有消费者都要全部**）：
//   watchdog  → kamakiri 触发用（往载荷尾 4 字节写）        —— 缺 = 不可用
//   uart      → 同上（尾 8..4 字节），决定 UART 基址           —— 缺 = 不可用
//   bromPayloadAddr / daPayloadAddr / plPayloadAddr → 载荷注入目标（三者取一即可）
//   cqdmaBase → CQ-DMA 系利用路径用（kamakiri 不用）
//   sejBase   → 安全引擎基址
//   var1      → 漏洞利用触发控制传输的 wIndex（kamakiri.py:44）—— 缺 = 需调用方覆盖，否则不可用
struct ChipHwAddrs {
    quint16 hwCode = 0;
    // 值槽：**仅当对应 hasXxx 为真时**才是上游给出的地址（为假时是 0，无意义 —— 见上）
    quint32 watchdog = 0;
    quint32 uart = 0;
    quint32 bromPayloadAddr = 0;
    quint32 daPayloadAddr = 0;
    quint32 plPayloadAddr = 0;
    quint32 cqdmaBase = 0;
    quint32 sejBase = 0;
    // var1（W8-T3，控制方 2026-09-20 裁决补的第 8 个字段）：BROM 利用触发控制传输的 wIndex
    // （`kamakiri.py:44` 的 `ctrl_transfer(0xA1, 0, 0, self.var1, 0)`）。**不是地址**，
    // 但与 watchdog/uart 同类：逐芯片配置常量，且**多数芯片都有** —— 上游 89 条实测
    // **56 条有值 / 33 条未写 / 0 条显式 None**，取值 {10(众数,41 条), 37, 40, 115, 127, 172,
    // 177, 180, 204}。上游正是因为它逐设备/逐芯片不同才写了 bruteforce 扫 0..0xFF
    // （`kamakiri.py:50-101`）。缺的 33 条是"上游也没给"（不是 0），消费方必须响亮失败或显式覆盖。
    quint32 var1 = 0;
    // 存在位（上游"这一条给了这个 kwarg 且不是 None"；显式 None 与缺失**同义**）
    bool hasWatchdog = false;
    bool hasUart = false;
    bool hasBromPayloadAddr = false;
    bool hasDaPayloadAddr = false;
    bool hasPlPayloadAddr = false;
    bool hasCqdmaBase = false;
    bool hasSejBase = false;
    bool hasVar1 = false;

    // —— 带检查的取用口（W8-T3；控制方裁决：消费者**不得直接读值槽**）——
    //
    // 为什么非有不可：存在位与值槽是**两个字段**，C++ 挡不住消费者写成 `a.watchdog` 却漏查
    // `hasWatchdog` —— 那会读到 0，而**把 0 写进 watchdog 是砖机路径**。取用口把"查存在位"
    // 与"取值"合成一个动作，漏查在语法上不可能。这与本批 I-1 是同一类事故
    // （约定只靠注释维持 → 静默分叉），所以不靠注释、靠接口。
    //
    // 语义：存在位为假、或 `out` 为 nullptr → **返 false 且不写 `*out`**（调用方拿不到那个 0）；
    // 存在位为真 → 写 `*out` 并返 true。**没有 0 兜底、也没有默认值**：取不到就由调用方
    // **响亮失败**（进日志 + 阶段 + error）。
    //
    // 命名：用 `getXxx` 而不是 `xxxAddr` —— 三个 payload 字段的**成员名本身**就叫
    // bromPayloadAddr/daPayloadAddr/plPayloadAddr，同名成员函数会与之撞名编译不过；
    // 八个取用口统一同一形态，免得"哪几个能这么叫"要靠记。
    // 位置：定义在 header 内联 —— 表体所在的 mtk_chip_table.cpp 是**生成文件**，往里加函数会被下次重跑抹掉。
    bool getWatchdog(quint32 *out) const { return takeChecked(hasWatchdog, watchdog, out); }
    bool getUart(quint32 *out) const { return takeChecked(hasUart, uart, out); }
    bool getBromPayloadAddr(quint32 *out) const { return takeChecked(hasBromPayloadAddr, bromPayloadAddr, out); }
    bool getDaPayloadAddr(quint32 *out) const { return takeChecked(hasDaPayloadAddr, daPayloadAddr, out); }
    bool getPlPayloadAddr(quint32 *out) const { return takeChecked(hasPlPayloadAddr, plPayloadAddr, out); }
    bool getCqdmaBase(quint32 *out) const { return takeChecked(hasCqdmaBase, cqdmaBase, out); }
    bool getSejBase(quint32 *out) const { return takeChecked(hasSejBase, sejBase, out); }
    bool getVar1(quint32 *out) const { return takeChecked(hasVar1, var1, out); }

private:
    // 唯一的读值槽之处（八个取用口都经它）。私有成员**函数**不影响本 struct 的聚合性
    // （C++17 聚合只排除 private/protected 的**非静态数据成员**）—— kHwAddrs 的按位置初始化照旧。
    static bool takeChecked(bool present, quint32 value, quint32 *out)
    {
        if (!present || out == nullptr)
            return false;
        *out = value;
        return true;
    }
};

// 按 hw_code 查硬件地址。**仅当 hwCode 不在表内**才返 nullptr（维持 89 条口径：字段不全的芯片
// 照样返回非空指针，缺哪个由 hasXxx 表达）。上层拿 nullptr 必须明确报错，不得猜代际/地址。
const ChipHwAddrs *lookupChipHwAddrs(quint16 hwCode);

} // namespace mtkbrom
