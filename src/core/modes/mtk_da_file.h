#pragma once

// AllInOne DA 文件解析（纯函数，计划 D1 Task 1）
//
// 布局与判据全部来自**实测**，不是推测：
//   6 个真实文件 / 161 条目 = reference/mtk-samples/（gitignored，不进仓库），
//   独立解析器基准 = reference/mtk-samples/parse_da.py，
//   逐字段实测表   = reference/mtk-samples/da_parse_report.json。
// 出处（mtkclient v2.1.4-20-g71b0175，GPL-3.0，**只读引用不复制代码**）：
//   Library/DA/daconfig.py:120-205       解析（DAconfig.parse_da_loader / DA.__init__ 字段序）
//   Library/DA/legacy/dalegacy_lib.py:563-571  region[1] = DA1 / region[2] = DA2 硬编码
//
// P1–P12 实测铁律（全部来自真实文件，违反即静默丢数据或砖机；spec §2 有完整出处）：
//   1. P2 条目选择键是 5 元组 (hw_code, hw_sub_code, hw_version, sw_version, pagesize)
//      —— 用 4 元组建 map 会在 2625/7687/iot 上**静默丢条目**（iot 的 0x6226 两条前四字段完全相同）。
//   2. P5 region[1] = DA1、region[2] = DA2 是硬编码（三代共同）；entry_region_index **仅** IoT 特例
//      使用 —— 按它取 stage1 会把 region[0]（EMI/env）当 DA1 上传。
//   3. P6 不得按内容去重 region：V6 的 region[0] 与 region[1] **逐字节相同**（9/9）；
//      按 md5 去重会把 EMI 槽与 DA1 合并。也不得断言两者必不同。
//   4. P8 0xD8 老格式无真实样本 → 只能合成夹具，**不得**在文档/提交信息里写"已验证"。
//   5. P9 count_da 不可信：必须与 EOF 交叉校验并在越界时报错（不得按它循环 seek）。
//   6. P10 世代判别式是负向的：isV6 = data.left(0x68).contains("MTK_DA_v6")；
//      **横幅版本号不得用于逻辑分支**。
//   7. P11 m_buf（文件偏移，用于 seek）≠ m_start_addr（加载地址，发给设备）；
//      m_buf + m_len <= filesize 检查必须保留。
//   8. P12 端序：DA 文件内字段全 **LE**；LEGACY 协议参数全 **BE** → 解析层与协议层分别封装，
//      **禁止裸 memcpy**。
//   9. P1 pagesize 不得当常量过滤（实测 {0,1,4096,12288}）。
//  10. P7 容忍形态：m_len == 0（真实存在）与 hw_code == 0 占位条目必须跳过而**不 abort**；
//      **不得**"上传 0 字节后静默成功"。
//  11. P3 m_start_offset 原样读取（27 个真实反例不符 m_len - m_sig_len）。
//  12. P4 0xD8/0xDC 探测必须与 0x6C + count*size <= filesize 交叉校验，不自洽就报错
//      （探测点落在 entry[0] 自己的槽内，nregions >= 10 时会变成真实数据）。
//
// 本文件**只做解析**：P2/P5/P6 属选择层（后续任务），此处仅保证字段被完整读出；
// P12 的协议端序属协议层，本文件只按 LE 读文件字段。

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace mtkbrom {

// DA 条目内的一个载荷区间（对照 daconfig.py EntryRegion）。
struct DaRegion {
    quint32 fileOffset = 0;    // m_buf：文件内偏移（seek 用），不是加载地址（P11）
    quint32 len = 0;           // m_len：载荷字节数（0 合法，真实文件里存在 —— P7）
    quint32 startAddr = 0;     // m_start_addr：加载地址（发给设备）
    quint32 startOffset = 0;   // m_start_offset：原样读取，不得推导（P3）
    quint32 sigLen = 0;        // m_sig_len：尾部签名长度
};

// 一条 DA 条目（一代芯片的一套 DA1/DA2 集合）。
struct DaEntry {
    quint16 magic = 0;             // 固定 0xDADA（条目边界唯一锚点）
    quint16 hwCode = 0;            // 0 = 占位条目（真实文件里存在，P7）
    quint16 hwSubCode = 0;
    quint16 hwVersion = 0;
    quint16 swVersion = 0;
    quint16 pagesize = 0;          // 实测 {0,1,4096,12288} —— 不做常量校验（P1）
    quint16 entryRegionIndex = 0;  // 仅 IoT 特例使用（P5）
    quint16 regionCount = 0;       // = regions.size()（解析后）
    QList<DaRegion> regions;
};

// 整个 AllInOne DA 文件。
struct DaFile {
    bool isV6 = false;             // 头部含 "MTK_DA_v6"（P10 负向判别式）
    bool oldFormat = false;        // 0xD8 条目步长（P8：无真实样本，仅合成夹具覆盖）
    quint32 count = 0;             // count_da（已与 EOF 交叉校验，P9）
    QByteArray banner;             // 0x20 起 0x48 字节横幅，去尾部 NUL；**不得**用于世代判别（P10）
    QByteArray raw;                // 原始字节（选择层据此切 region 载荷，免二次读盘）
    QList<DaEntry> entries;
};

// 解析 AllInOne DA。成功返回 true 并填充 out；失败返回 false 且 error（可空）写中文诊断。
// fail-closed：头部标记/条目 magic/count 越界/region 越界任一不成立即整体拒绝（不返回半份数据）。
bool parseDaFile(const QByteArray &data, DaFile &out, QString *error);

} // namespace mtkbrom
