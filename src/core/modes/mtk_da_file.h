#pragma once

// AllInOne DA 文件解析 + 条目选择（纯函数，计划 D1 Task 1/2）
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
//   4. P8 0xD8 老格式无真实样本 → 只能合成夹具，**不得**在文档/提交信息里写"已验证"；
//      **探测到老格式一律明确拒绝解析**（老格式字段偏移不同 —— upstream daconfig 在 0xD8 下
//      pagesize 在 0x08、region 表少 4 字节 —— 按新格式偏移硬解会静默读出垃圾；
//      DaFile.oldFormat/count 仍照填，供调用方观测探测结果）。
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
// 本文件做**解析**与**条目选择**两层：解析层只把字段完整读出（不做选择、不按内容去重 —— P6），
// 选择层（D1 Task 2，`selectDaEntry`）落实 P2（5 元组才是唯一键）与 P5（region[1]/[2] 硬编码）；
// P12 的协议端序属协议层，本文件只按 LE 读文件字段。
//
// 选择层规则出处（mtkclient v2.1.4-20-g71b0175，GPL-3.0，**只读引用不复制代码**）：
//   Library/DA/daconfig.py:200            装载阶段剔除 hw_code==0 占位条目（本层对 dacode==0 拒绝）
//   Library/DA/daconfig.py:207-218        按 hw_code 找候选 + hw/sw 版本过滤（`or … == 0` 即设备值 0 旁路），
//                                         取**文件顺序首个满足者**（first-match，非"取最大版本"）
//   Library/DA/legacy/dalegacy_lib.py:563-571  region[1]=DA1 / region[2]=DA2 硬编码
// "空 region 跳过"与"跳过的原因要记明"来自本仓真样本实测（见 `selectDaEntry` 注释）。

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
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
    bool oldFormat = false;        // 0xD8 条目步长（P8：探测结果照填；命中即整体拒绝，仅合成夹具覆盖）
    quint32 count = 0;             // count_da（已与 EOF 交叉校验，P9）
    QByteArray banner;             // 0x20 起 0x48 字节横幅，去尾部 NUL；**不得**用于世代判别（P10）
    QByteArray raw;                // 原始字节（选择层据此切 region 载荷，免二次读盘）
    QList<DaEntry> entries;
};

// 解析 AllInOne DA。成功返回 true 并填充 out；失败返回 false 且 error（可空）写中文诊断。
// fail-closed：头部标记/条目 magic/count 越界/region 越界任一不成立即整体拒绝（不返回半份数据）。
bool parseDaFile(const QByteArray &data, DaFile &out, QString *error);

// 选中一条 DA 条目 + 切出两阶段载荷（D1 Task 2）。仅查已解析的 DaFile（不再读文件，raw 即来源）。
struct DaSelection {
    int entryIndex = -1;      // 命中的条目下标（诊断用）
    DaEntry entry;
    DaRegion da1;             // **恒 region[1]**（三代共同硬编码；不得改用 entryRegionIndex ——
    DaRegion da2;             //   P5：按它取 stage1 会把 region[0]（EMI/env）当 DA1 上传）
    QByteArray da1Bytes;      // 文件切片 [da1.fileOffset, +da1.len)
    QByteArray da2Bytes;      // 文件切片 —— **LEGACY 保留尾部签名**，调用方不得裁剪
    bool isXmlForced = false; // 所在文件 isV6（只能推向 XML，不能推向 LEGACY）
};

// 选择规则（daconfig.py:207-218 + 实测）：
//   0. dacode == 0 → 直接拒绝（判不出芯片；0 会与 hw_code==0 的占位条目相撞，上游装载阶段已剔除它们）。
//   1. 候选 = hwCode == dacode 的条目（dacode 由芯片表给出，默认 = 设备 hw_code）。
//   2. 版本过滤：hwVersion <= deviceHwVer 且 swVersion <= deviceSwVer；**设备值为 0 时该维旁路**
//      （上游 `or hw_ver == 0`；IoT/取不到版本的真实情形）。
//   3. 跳过 regionCount < 3 或 region[1]/region[2] 长度为 0 的候选（真实文件里大量 region[1].len == 0；
//      DA1/DA2 缺一不可）—— 跳过的条目写进 warnings，**不得**"上传 0 字节后静默成功"（P7）。
//   4. 取**文件顺序上首个满足者**（上游 first-match，`if self.da_loader is None` 即不再覆盖）；
//      多条满足 → warnings 记明（P2 的 5 元组才是唯一键，本层不做 pagesize 匹配）。
//   5. 无候选 → false + 中文 error，并列出该文件里出现过的 hw_code（诊断）。
// 成功返回 true 并填充 out；warnings/error 可空。out 在入口处被重置，失败时不返回半份选择。
bool selectDaEntry(const DaFile &f, quint16 dacode, quint16 deviceHwVer, quint16 deviceSwVer,
                   QStringList *warnings, DaSelection &out, QString *error);

} // namespace mtkbrom
