#pragma once

// 芯片码文本（hw_code）的解析与格式化 —— W8-T6 的两个 UI 入口共用（BROM 漏洞利用区 / DA 切分另存）。
//
// 为什么单独一层：两处输入框必须**同一口径**（同一个文本有两种解释就是"两处各解一次、口径分叉"，
// 本仓反复吃亏的形态），而这两处对更重的东西（判据 / 载荷 / DA 解析）的需求并不相同 ——
// 抽出来之后 DA 对话框不必为了解析一个芯片码去链整个漏洞利用层。
//
// 依赖：**只有 Qt Core**（无 Widgets、无芯片表、无设备、无网络）。
// 形态约定见 parseHwCodeText：不带前缀的文本**一律按十六进制**解释（`6735` = 0x6735）。

#include <QString>
#include <QtGlobal>

namespace bromui {

// 芯片码文本 → hw_code。接受 `0x6735` / `0X6735` / `6735`（大小写不敏感、允许前后空白）。
// 失败（空 / 仅前缀 / 非十六进制字符 / 超出 quint16）→ false + *error（中文、可行动），且**不写** *out。
bool parseHwCodeText(const QString &text, quint16 *out, QString *error = nullptr);

// var1（BROM 触发控制传输的 wIndex；0x00..0xFF）的文本解析 —— **与 `parseHwCodeText` 同一形态规则**
// （前缀可有可无、一律按十六进制解释、字符集显式判），只是上限不同。两者共用同一份实现
// （.cpp 的 `parseHexCore`），故"同一份文本在两条入口里被解释成两个值"这种事不会发生。
// 为什么要有它：芯片表里 10 条芯片上游未给 var1，而编排层对缺 var1 是**响亮失败** ——
// 面板必须能让用户显式填（否则那 10 条"能力不可达"，本仓不接受）。
bool parseVar1Text(const QString &text, quint8 *out, QString *error = nullptr);

// hw_code → 来源清单 `chipHint` 的**唯一**形态（`0x%04x`，小写）。
// `resolveExploitPayload` 的 `hwHint` 必须由本函数格式化：形态对不上会**静默**少试逐芯片来源
// （清单里是 `0x6735` 形态，写成 `6735` 就匹配不上 —— 见 mtk_brom_exploit.h 的说明）。
QString formatHwCodeHint(quint16 hwCode);

} // namespace bromui
