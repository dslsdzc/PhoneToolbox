// src/core/edl/flash_plan.h
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace edl {

struct PlanEntry {
    enum class Action { Program, Erase, Patch };
    Action  action = Action::Program;
    QString partitionName;      // rawprogram: label；patch: filename（供日志）
    QString imageFile;          // 绝对路径；Patch 为 "DISK" 时表示下发设备
    quint32 lun = 0;            // physical_partition_number
    quint64 startSector = 0;    // 纯十进制时填此值
    QString startSectorExpr;    // start_sector 非纯十进制时原样保留（firehose 表达式，如 "NUM_DISK_SECTORS-5."）；
                                // 非空时 startSector==0，且发送方必须原样透传该串
                                // （reference/qdl/src/firehose.c:874-879 明确"解析会写错地址"）
    quint64 numSectors = 0;     // Program/Erase：sparse 展开后的 raw 扇区数
    quint32 sectorSize = 4096;  // 逐条目 SECTOR_SIZE_IN_BYTES
    bool    sparse = false;
    // 该条目**将下发的字节量**。同一个字段在两层里由不同步骤回填，读它必须两条口径都吃：
    //   * 计划层（buildPlanFromDir 路径）：只有 sparse 条目由 normalizePlan 回填"去 sparse 后的
    //     字节数"（flash_plan.cpp 的 normalizeProgramImage）；非 sparse 条目**恒为 0**，
    //     由 finalizePlan 按 numSectors × sectorSize 兜底（进度分母同款）。
    //   * EDLHandler 单条目路径（edl_handler.cpp 的 writeImageEntry）：直接填 numSectors × sectorSize
    //     （= 实际推送量，含数据面补零）。
    // 因而**不许**把它当"文件大小"或"非零即可信"用；UI 的"大小"列就是
    // `rawBytes != 0 ? rawBytes : numSectors × sectorSize`（flash_plan_dialog.cpp 的 entryBytes），
    // 与上面两条口径一致。
    quint64 rawBytes = 0;
    QString sha256;             // 可选（rawprogram 无此属性；OPS 元数据有）
    quint64 byteOffset = 0;     // Patch 专用
    quint32 sizeInBytes = 0;    // Patch 专用
    QString value;              // Patch：原样透传（表达式不解释）
    QString what;               // Patch：只进日志，不进 XML
};

struct FlashPlan {
    QString source;             // 计划来源描述
    QString storageType;        // "ufs"/"emmc" → configure 的 MemoryName
    QList<PlanEntry> entries;   // 已排序：先 Erase，再 Program（按 lun、start_sector），最后 Patch
    QStringList warnings;
    quint64 totalBytes = 0;     // Program 条目 rawBytes 之和（进度分母）
};

struct StorageInfo { quint32 lun = 0; quint64 totalBlocks = 0; quint32 blockSize = 4096; };
struct PlanCheck { bool ok = false; QStringList errors; QStringList warnings; };

// 来源①：解析单个 rawprogram{N}.xml / patch{N}.xml（lun 由调用方从文件名序号得出，见 Task 3）
bool parseRawprogramXml(const QString &xmlPath, quint32 lun,
                        QList<PlanEntry> &out, QStringList &warnings, QString *error);
bool parsePatchXml(const QString &xmlPath, quint32 lun,
                   QList<PlanEntry> &out, QStringList &warnings, QString *error);

// ---- 来源探测（Task 3；spec §3.4 三层来源）----
//
// `buildPlanFromDir`：目录 → FlashPlan（**只读目录**，不写盘）。`plan` 是本函数的**输出**：
// 入口先清空 source/storageType/entries/warnings/totalBytes，同一对象可重复调用而不累积。
// 内部完成 `parse* → normalizePlan → finalizePlan` 三步（**不**跑 validatePlan —— 那需要设备
// 几何，由会话层在 getstorageinfo 之后调用，见 §3.5）。来源顺序：
//   ① 目录内有 rawprogram*.xml（**文件名末尾数字 = lun**）→ 连同 patch*.xml 一起解析；
//   ② 否则（或①解析出 0 条目）有 settings.xml → `parseOpsSettingsXml`（OPS 元数据回退）；
//   ③ 都没有 → 失败，*error 列出目录内所有 .xml 文件名（帮助诊断）。
// 所选来源产出 0 条目 → 失败（fail-closed，绝不放行空计划）。
// storageType：目录含 prog_ufs_firehose_* → "ufs"；含 prog_emmc_firehose_* → "emmc"；
// 都无 → "ufs" + warning（真包常见不带 ufs/emmc 标识的 prog_firehose_*.elf）。
// 解析告警一律进 plan.warnings（含 normalizePlan 的）；失败返回 false 并写中文 *error。
bool buildPlanFromDir(const QString &dir, FlashPlan &plan, QString *error);

// 来源②：OPS `settings.xml`（`.ops` 解包产物）→ 条目（**只追加** out，同 parse*Xml 约定：
// 对账只作用于本次新增的条目）。分组语义（协议速查 §5；样本形态见
// reference/FirmwareKit.Oppo/FirmwareKit.Oppo.Tests/Parsers/OpsParserTests.cs:113-124）：
//   <Program{N}> / <Patch{N}> → lun = 标签末尾数字；<UFS_PROVISION> → lun = 0；其余组忽略。
// 条目：子元素带 `filename` 即条目；否则容器（如 `<program label="…">`）的孙元素带 `filename`
// 者逐条产出，属性按"子元素优先、容器兜底"合并（两种真实形态都覆盖）。
// 几何：元数据有 `start_sector`/`num_partition_sectors` 则取用，随后用包内 `gpt_main{N}.bin`
// 的 LBA 表**对账**（一致 → 静默；不一致 → warning 且**以 GPT 为准**；GPT 里查不到 → 保留元数据
// + warning）。`start_sector` 为 firehose 表达式时**不参与对账**（表达式由设备侧求值）。
// **跨单位不比对**：元数据声明了 `SECTOR_SIZE_IN_BYTES` 且与 `gpt_main{N}.bin` 的 LBA 尺寸不同
// → 两边 LBA 编号单位不同、不可比较，整个条目跳过对账（保留元数据值 + warning），不做 ×8/÷8 换算。
// **忽略包内偏移字段** `FileOffsetInSrc`/`SizeInByteInSrc`/`SizeInSectorInSrc` —— 它们只描述文件在
// 包内的位置与长度，与设备扇区无关（协议速查 §5）。
// `packageDir`：包内文件所在目录（imageFile 与 gpt_main{N}.bin 的基准；通常 = settings.xml 所在目录）。
bool parseOpsSettingsXml(const QString &settingsXmlPath, const QString &packageDir,
                         QList<PlanEntry> &out, QStringList &warnings, QString *error);

// 排序 + 统计（spec §3.3/§3.5）：Erase 一律在前，Program 按 (lun, startSector) 升序，Patch 一律最后；
// 同键保持解析顺序（stable_sort）。填 totalBytes = Program 条目 rawBytes（为 0 时退化为
// numSectors × sectorSize）之和 —— 进度分母，Patch/Erase 不计入。
// **调用顺序**：normalizePlan 会按镜像文件事实修正 numSectors/rawBytes，故 totalBytes 应在
// normalizePlan 之后再算（`parse* → normalizePlan → finalizePlan → validatePlan`）。
void finalizePlan(FlashPlan &plan);

// 就地归一化（**会修改 plan**）：把"镜像文件事实"变成最终下发值 ——
//   * Program + sparse：读文件头 → rawBytes = 去 sparse 后字节数；与 XML 声明的扇区数不符 →
//     以文件头为准修正 numSectors + warning
//     （Task 1 模型契约：rawBytes 由本步骤回填，见 flash_plan.cpp `loadProgramTag` 的注释）
//   * 标记 sparse 但文件头不是 sparse：文件大小与声明一致 → 翻转 sparse=false + warning
//     （reference/qdl/src/program.c:79-93）；对不上 → 失败
//   * 镜像缺失/不可打开、sectorSize 为 0 无法换算 → 失败
// warnings 只追加（不动调用方已有内容）；失败返回 false 并把全部失败**逐行合并**进 *error
// （每条带条目名与路径，便于一次性修包）。非 Program 条目不改不查。
// 调用链：`parse* → normalizePlan → validatePlan`（Task 3 的 buildPlanFromDir 内部完成前两步）；
// 必须先归一化再校验 —— 否则 XML 少报的扇区数会绕过越界判定（见 validateSparseCorrectionFeedsBoundsCheck）。
bool normalizePlan(FlashPlan &plan, QStringList &warnings, QString *error);

// 刷前校验（spec §3.5 规则 1/2/3/6/7/8；在 getstorageinfo 之后、进入写入之前调用）。
// **纯函数：不改入参**（唯一入参是 const 引用 —— 归一化已在 normalizePlan 里完成）。
// 规则 4/5（文件与 sparse）由 normalizePlan 负责，见上面的调用链。
// errors 非空 → ok=false（拒刷，绝不放行）；warnings 只进预览与日志。
PlanCheck validatePlan(const FlashPlan &plan, const QList<StorageInfo> &device);

} // namespace edl
