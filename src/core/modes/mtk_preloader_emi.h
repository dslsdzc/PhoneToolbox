#pragma once

// preloader EMI 提取（两代切片：LEGACY / XFlash）—— 纯函数，D1 Task 5 + D2 Task 3
//
// EMI = DRAM 初始化数据。刷写流程里 DA1 起来后、DA2 之前要把从 preloader 里提取出的 EMI 发给
// 设备；本模块只做**提取**（发送端：LEGACY 见 mtk_payload 的 sendEmiLegacy，按 emiver 分档；
// XFlash 走 INIT_EXT_RAM + 4B 长度帧，见 mtk_xflash_payload —— 该模块由本分支 **Task 4/5 交付**，
// 写此指针时尚未落地，别去找不存在的文件）。
//
// 出处（mtkclient v2.1.4-20-g71b0175，GPL-3.0，**只读引用不复制代码**）：
//   Library/DA/daconfig.py:120-145   m_extract_emi：MMM 分支 → MTK_BLOADER_INFO_v 搜索 → 两代切片
//   Library/DA/daconfig.py:147-164   extract_emi 包装：异常 → emiver=0 / emi=None，**不中止刷写**
//
// 实测依据（reference/mtk-samples/，gitignored；832 个真实 preloader 的统计见
// .superpowers/sdd/mtk-d2d3-facts-report.md §2 —— 统计只作注释证据，不进断言）：
//   · 仅 3/832 个含 MMM 魔术 → 绝大多数走"标记在偏移 0"的路径；
//   · 同一 preloader 两代取**不同切片**：LEGACY = `MTK_BIN+0xC` 起（实测 800B）、
//     XFlash = 整块（实测 912B = LEGACY + 前 112B）。两代**各自独立取法，不得互相复用**：
//     调用方按代选函数（上游按 `damode` 在同一个 m_extract_emi 里分叉，daconfig.py:137-144）。
//     XFlash 的整块返回上游另有 `idx == 0` 前提（标记正在窗口起点）——本实现同样要求，
//     见 extractEmiXflash 的注释与 .cpp 偏差 6。
//
// 契约：失败 → false + 中文 error（无 MTK_BLOADER_INFO_v / 无 MTK_BIN / 切片为空 /
// **版本字节非数字** / MMM 分支字段越界 / **XFlash 要求标记在窗口偏移 0** —— 详见 .cpp 的偏差说明）；
// **调用方不得因此中止刷写** ——
// mtkclient 同姿态：EMI 缺失只告警（"No preloader given. Operation may fail due to missing
// dram setup."，xflash_lib.py:1143-1144）。
// out 在入口即重置（fail-closed）；`branch` 是**诊断字段**，失败路径也可能已填入命中路径，
// 但 `bytes` 在失败时恒为空。**失败后 `ver` 不可读**：契约不保证其取值（当前实现按路径而异 ——
// LEGACY 的"无 MTK_BIN"失败留已解析值，XFlash 的"标记不在窗口偏移 0"失败留 0），false 返回后
// 调用方只能看 `bytes` 是否为空，不得读 `ver`。

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace mtkbrom {

// 从 preloader 提取出的 EMI。
struct EmiData {
    QByteArray bytes;    // 待发送的 EMI 本体 —— 取法**由调用的是哪个函数决定**：
                         // extractEmiLegacy → MTK_BIN+0xC 起；
                         // extractEmiXflash → **整个窗口**（MMM 分支裁到 dramsize 的那个窗口；
                         // 未命中 MMM 时窗口就是整块输入，**不读** dramsize —— 别把切片长度当
                         // dramsize 校验值，见 .cpp 的窗口定义）
    quint32 ver = 0;     // 版本号 = "MTK_BLOADER_INFO_v" 之后 2 个 ASCII 字节（如 "38" → 38；
                         // 真样本是 "35" → 35）。**读不懂即整体失败**，绝不返回"ver=0 的成功"：
                         // 0 在上游是**合法档位**（tier-0），不得与"非数字"混用（见 .cpp）。两代同规则。
    QString branch;      // 诊断：命中的路径。extractEmiLegacy 填 ("MMM" = 命中 MMM 魔术分支；
                         // "未命中MMM" = 未命中该魔术)，只表达"魔术在不在"，**不代表**标记位于偏移 0；
                         // extractEmiXflash 恒填 "XFLASH"（走的是另一条取法，"魔术在不在"不再是判据）
};

// 纯函数：从 preloader 字节提取 LEGACY 用的 EMI（不读盘、不碰设备、无副作用）。
bool extractEmiLegacy(const QByteArray &preloader, EmiData &out, QString *error);

// 纯函数：从 preloader 字节提取 XFlash 用的 EMI —— **整个窗口**（上游 daconfig.py:137-139 的
// `idx == 0 且 damode == XFLASH` 分支整块返回；本实现把 `damode` 的选择留给调用方，见 .cpp）。
// "窗口"= MMM 分支裁到 dramsize 的那段；**未命中 MMM 时窗口就是整块输入（不读 dramsize）** ——
// 切片长度在非 MMM 路径上与 dramsize 无关，别当校验值用（.cpp 的窗口定义是唯一权威）。
// 与 LEGACY（`MTK_BIN+0xC` 起）在**同一份 preloader 上取不同切片**：真样本 preloader.bin 实测
// XFlash 912 B / LEGACY 800 B（差 112 B = MTK_BIN 在窗口里的偏移 100 + 0xC；832 样本同此）。
// 失败条件 = extractEmiLegacy 的公共前缀各条 + **标记不在窗口偏移 0**（上游此情形改取 MTK_BIN
// 切片，本实现不静默换切片，见 .cpp 偏差 6）。
bool extractEmiXflash(const QByteArray &preloader, EmiData &out, QString *error = nullptr);

} // namespace mtkbrom
