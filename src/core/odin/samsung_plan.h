// src/core/odin/samsung_plan.h
//
// 刷写**计划层**：把「选中的 .tar.md5 集合 + PIT」变成可刷写清单（分区 ↔ 镜像 ↔ 偏移）。
// 只读文件（流式索引 + 校验行扫描），**不解包、不落盘**；真正写设备是会话层的职责。
//
// 匹配规则（spec §4；顺序即实现顺序）：
//   1. **以 PIT 条目自带的 flashFilename 为准**（不靠扩展名猜、不靠"分区名 + .img"推断）——
//      真包 J1POP3G 的条目成对给出「分区名 / 文件名」且与 tar 内镜像名逐个吻合。
//   2. hasImageName() == false（空 或 字面 "-"）→ 该条目**不参与匹配、不报"缺镜像"**
//      （它不是"缺"，是没声明）。
//   3. 精确匹配：与 tar 内条目名比较，**大小写不敏感**。
//   4. 精确匹配失败且 flashFilename 以 ".pit" 结尾 → 在所选包内找**唯一**一个 .pit 条目
//      （**已知反例**：PIT 条目声明 J1POP3G_LTN_OPEN.pit，包内是 J1POP3G.pit）→ 命中则记
//      warning 说明用了哪条规则；0 个或多个候选 → 不匹配 + 专门文案。
//   5. 未匹配的**有文件名**条目 → warning「PIT 条目 X 声明的镜像 Y 不在所选包内（跳过）」。
//   6. 包内未被任何 PIT 条目认领的条目 → warning「包内镜像 X 未出现在 PIT 中（跳过）」。
//   7. 大小核对：
//        * 镜像 > 分区 → warning（**严重：写不下**）；
//        * 镜像 < 分区 → 只计数，最后出一条汇总 warning（真数据 10 条匹配里 9 条都是"小于"
//          —— 逐条报会淹没预览）；
//        * 分区 blockCount == 0（未声明，如 J1POP3G 的 USERDATA）→ 若匹配到镜像则 warning
//          「分区大小未声明，无法核对」。
//      ⚠️ 这是对 spec §4「分区大小与镜像大小不符 → warning」的**收窄解释**：按字面执行会在
//      真包上产生 9 条无意义告警（真值已复核：SPRDCP.img 恰好等于分区 8MiB，其余 8 条镜像都
//      小于分区、属正常）。依据见 Task 4 报告与 docs/superpowers/specs/samsung-odin-facts.md §3。
//   8. 排序：按**包内 PIT 的条目顺序**（bootloader 在前，与 Odin 惯例一致；写入顺序不影响
//      正确性 —— 真正决定落盘位置的是设备侧 PIT 的 identifier）。
//   9. 一条都没匹配上 → 失败（fail-closed，绝不放行空计划）。
//
// 与 spec §4 的两处结构差异（有意）：
//   A. verifyOk 放在 SamsungPlanFile（**每包一个结论**）而不是 SamsungPlanEntry —— MD5 校验行的
//      对象是**整个 tar.md5 文件**，逐条目复制同一个 bool 会误导读者以为"每条镜像各自校验过"。
//   B. SamsungPlan 用 files（含路径/大小/校验结论/条目名）而不是 spec 的 tarMd5Files(QStringList)
//      —— 是前者的信息超集，预览对话框要显示"来源包"与校验结论。
#pragma once
#include <QList>
#include <QString>
#include <QStringList>

#include "pit.h"

namespace odin {

// 每个输入包的结论（校验行状态 / 归档内条目名清单）。
struct SamsungPlanFile {
    QString path;              // 输入 .tar.md5 的路径（原样）
    quint64 sizeBytes = 0;     // 文件字节数（含校验行）
    bool md5HasFooter = false; // 是否检测到尾部 MD5 校验行
    bool verifyOk = false;     // 校验行存在**且**校验通过（无校验行 → false，不静默当成功）
    QStringList entryNames;    // 归档内条目名（indexTarStream 规则：去尾 '/'、拼 ustar 前缀）
};

// 一条可刷写项：PIT 条目 → 包内镜像。
struct SamsungPlanEntry {
    QString partition;        // PIT 分区名（= pit.partitionName）
    QString imageFile;        // tar 内镜像条目名
    quint64 sizeBytes = 0;    // 镜像字节数
    quint64 sourceOffset = 0; // 镜像数据在 files[fileIndex].path 内的**绝对偏移**（会话数据面按它流式读）
    int fileIndex = -1;       // 指向 plan.files 的下标
    QString matchRule;        // "文件名精确匹配" / ".pit 唯一性回退"
    PitEntry pit;             // 命中的 PIT 条目（设备侧 identifier / binaryType 等取自它）
};

struct SamsungPlan {
    QString pitSource;                 // PIT 来源描述（预览显示用；空则回退文案显式写 "PIT"）
    QList<SamsungPlanFile> files;      // 按输入顺序，与 tarMd5Files 一一对应
    QList<SamsungPlanEntry> entries;   // 按 PIT 条目顺序（规则 8）
    QStringList warnings;              // 非致命问题（顺序：包级 → 逐条 → 汇总）
    quint64 totalBytes = 0;            // entries 的 sizeBytes 之和（进度基数）
};

// 构建刷写计划。成功 = entries 非空（规则 9 fail-closed）；失败时 *error 非空，plan 可能已含
// 部分 files/warnings（入口先复位，失败路径不回滚）—— 调用方**不得**使用失败后的 plan。
// pitSource 仅用于回显（如 "包内 J1POP3G.pit"），不参与匹配。
bool buildSamsungPlan(const QStringList &tarMd5Files, const PitTable &pit, SamsungPlan &plan,
                      QString *error, const QString &pitSource = QString());

// 从所选包内提取 PIT：**包内 .pit 唯一**才取（多个 → 拒，不同 CSC 的 PIT 可能不同，不擅自选一个）。
// 成功时 out 填充、*pitPathOut 得 "路径（包内 条目名）"（供预览显示来源）。
bool loadPitFromPackage(const QStringList &tarMd5Files, PitTable &out, QString *pitPathOut, QString *error);

} // namespace odin
