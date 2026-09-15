#pragma once

// preloader EMI 提取（LEGACY 切片）—— 纯函数，计划 D1 Task 5
//
// EMI = DRAM 初始化数据。LEGACY 代的刷写流程里，DA1 之前要把从 preloader 里提取出的 EMI
// 发给设备；本模块只做**提取**（发送端属 D1 Task 6，另按 emiver 分档）。
//
// 出处（mtkclient v2.1.4-20-g71b0175，GPL-3.0，**只读引用不复制代码**）：
//   Library/DA/daconfig.py:120-145   m_extract_emi：MMM 分支 → MTK_BLOADER_INFO_v 搜索 → 两代切片
//   Library/DA/daconfig.py:147-164   extract_emi 包装：异常 → emiver=0 / emi=None，**不中止刷写**
//
// 实测依据（reference/mtk-samples/，gitignored；832 个真实 preloader 的统计见
// .superpowers/sdd/mtk-d2d3-facts-report.md §2 —— 统计只作注释证据，不进断言）：
//   · 仅 3/832 个含 MMM 魔术 → 绝大多数走"标记在偏移 0"的路径；
//   · 同一 preloader 两代取**不同切片**：LEGACY = `MTK_BIN+0xC` 起（实测 800B）、
//     XFlash = 整块（实测 912B）。**本模块只实现 LEGACY** —— 上游 `idx == 0 且 damode == XFLASH`
//     的整块分支（daconfig.py:137-139）属 XFlash，D1 不实现（D2 再说）。
//
// 契约：失败 → false + 中文 error（无 MTK_BLOADER_INFO_v / 无 MTK_BIN / 切片为空 /
// MMM 分支字段越界 —— 详见 .cpp 的偏差说明）；**调用方不得因此中止刷写** ——
// mtkclient 同姿态：EMI 缺失只告警（"No preloader given. Operation may fail due to missing
// dram setup."，xflash_lib.py:1143-1144）。
// out 在入口即重置（fail-closed）；`branch` 是**诊断字段**，失败路径也可能已填入命中路径，
// 但 `bytes` 在失败时恒为空。

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace mtkbrom {

// 从 preloader 提取出的 EMI。
struct EmiData {
    QByteArray bytes;    // 待发送的 EMI 本体（**LEGACY 切片**：MTK_BIN+0xC 起）
    quint32 ver = 0;     // 版本号 = "MTK_BLOADER_INFO_v" 之后 2 个 ASCII 字节（如 "38" → 38）；
                         // 非数字 → 0（上游此时整体失败，见 .cpp 的偏差说明）
    QString branch;      // 诊断：命中的路径（"MMM" / "偏移0"）
};

// 纯函数：从 preloader 字节提取 LEGACY 用的 EMI（不读盘、不碰设备、无副作用）。
bool extractEmiLegacy(const QByteArray &preloader, EmiData &out, QString *error);

} // namespace mtkbrom
