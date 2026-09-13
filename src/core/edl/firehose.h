// src/core/edl/firehose.h
//
// Firehose（Sahara 之后的第二协议）命令构造 + 响应解析。
//
// 分层：`xml*` 全是**纯函数**（无 QObject、无 IEdlTransport、无 libusb），返回**裸元素**
// （不含 `<?xml …?>` 声明与 `<data>` 根）——这样测试能逐字段、乃至逐字节断言命令文本；
// `<data>` 包裹由唯一的发送出口 `firehoseSendCommand` 补上（线上形态见其注释）。
// 会话编排（发什么、何时发、数据面分块）属 Task 6，不在本文件。
//
// 属性集与判定口径的权威出处：docs/superpowers/specs/oppo-flash-protocol-facts.md §1-§3；
// 每条处标注 reference/qdl/src/firehose.c 或 edl/edlclient/Library/firehose.py 的行号。
// 反面参照（**不得照抄**）：src/core/modes/edl_handler.cpp —— 它的 ACK 判定用 contains、
// 命令帧带 4 字节长度前缀、program 缺 physical_partition_number、read 用错属性名
// （协议速查 §6 列的 5 处既有缺陷）。
#pragma once
#include <QByteArray>
#include <QString>
#include "edl_transport.h"
#include "flash_plan.h"

namespace edl {

// ---- 命令构造（纯函数；属性集与参照出处见协议速查 §1-§3）----

// `<configure MemoryName=… [MaxPayloadSizeToTargetInBytes=…] Verbose="0" ZlpAwareHost="1"
//  SkipStorageInit="0"/>`（reference/qdl/src/firehose.c:510-515）。maxPayloadBytes==0 →
// 省略该属性（首轮不知道设备上限时即如此，让它回 Supported 值）。
QByteArray xmlConfigure(const QString &memoryName, quint32 maxPayloadBytes = 0);

// `<program SECTOR_SIZE_IN_BYTES num_partition_sectors physical_partition_number start_sector
//  [filename]/>`（reference/qdl/src/firehose.c:1021-1035）。filename 取 imageFile 的**文件名**
// 且非空才发。start_sector 见下方 startSectorAttr 说明。
QByteArray xmlProgram(const PlanEntry &e);

// `<patch SECTOR_SIZE_IN_BYTES byte_offset filename physical_partition_number size_in_bytes
//  start_sector value/>`（reference/qdl/src/firehose.c:1414-1424）。**不发 `what`**
// （只进日志，firehose.c:1408；bkerler 同：edl/edlclient/Library/firehose.py:427-443）。
// filename 原样透传（"DISK" = 下发设备；真实文件名由计划层过滤，见 firehose.c:1405-1406）。
QByteArray xmlPatch(const PlanEntry &e);

// numSectors==0 **且** startSectorExpr 为空 → 省略 start_sector/num_partition_sectors
// = 整 LUN 擦（reference/qdl/src/firehose.c:611-621 的 `program->num_sectors > 0`）。
// 表达式非空时省略不成立（numSectors==0 只表示"未知/不适用"），照发两个属性。
QByteArray xmlErase(const PlanEntry &e);

// `<read SECTOR_SIZE_IN_BYTES num_partition_sectors physical_partition_number start_sector/>`
// （reference/qdl/src/firehose.c:1197-1207；注意属性名是 num_partition_sectors，
// 既有 edl_handler.cpp:716-725 的 num_sectors 是错的）。
QByteArray xmlRead(quint32 lun, quint64 startSector, quint64 numSectors, quint32 sectorSize);

// `<getstorageinfo physical_partition_number="N"/>`（reference/qdl/src/firehose.c:1907-1908；
// bkerler 固定 "0"：firehose.py:1293-1294 —— 本项目逐 LUN 传，故带 LUN）。
QByteArray xmlGetStorageInfo(quint32 lun);

// `<setbootablestoragedrive value="N"/>`（value = bootloader 分区所在 LUN：
// reference/qdl/src/firehose.c:1561-1569，LUN 取值规则见 src/program.c:423-451）。
QByteArray xmlSetBootableStorageDrive(quint32 lun);

// `<power value="reset" DelayInSeconds="10"/>`（reference/qdl/src/firehose.c:1590-1592；
// DelayInSeconds 是 qdl 为"防重启失败"补的，bkerler 不带：firehose.py:331-334）。
QByteArray xmlReset();

// ---- 响应解析（纯函数）----

struct FirehoseResponse {
    bool    ack = false;         // <response value="ACK"/>（**严格相等**判定）
    bool    nak = false;         // <response value="NAK"/>
    QString raw;                 // 原始 XML（错误文案里回显）
    QString errorText;           // <log value="..."/> 里的文本（XML 反转义后；多条以 \n 连接）
    QString storageInfoJson;     // getstorageinfo 时 <log value="{...}"/> 的 JSON 文本
};
FirehoseResponse parseFirehoseResponse(const QByteArray &xml);

// getstorageinfo 响应 → 设备几何。JSON 口径见 reference/qdl/src/firehose.c:1874-1884
// （storage_info.total_blocks / block_size）；回退认 bkerler 的文本键
// （edl/edlclient/Library/firehose.py:1272-1275 的 SECTOR_SIZE_IN_BYTES /
// num_physical_partitions，键值分隔见 :1303-1317）—— 文本键只补扇区大小。
// **成功判据 = totalBlocks > 0**（唯一来源 storage_info.total_blocks）：只拿到扇区大小返回 false
// + 中文 *error，绝不返回带默认值的假几何 —— totalBlocks==0 会让 validatePlan 的越界判定
// 把每条 Program 报成"越界"而非"LUN 无设备几何"（见实现处注释）。
bool parseStorageInfo(const FirehoseResponse &r, quint32 lun, StorageInfo &out, QString *error);

// getstorageinfo 响应里"设备报的 LUN 数"（Task 7 集成新增：`EDLHandler::listPartitions` 据此决定
// 逐个查几个 LUN）。键值与解析口径同 bkerler 的存储信息文本
// （edl/edlclient/Library/firehose.py:1266-1275 的 parse_storage）：
//   * `num_physical_partitions` —— 十进制（bkerler:1274-1275）
//   * `bNumberLu`              —— 十进制（qdl/设备侧对同一概念的别名）
//   * `UFS Total Active LU`    —— **十六进制**（bkerler:1271-1272 用 int(x, 16) 读，UFS 侧）
// 0 = 响应**没报** → 调用方按"只查 LUN 0"处理（绝不猜 LUN 数）。
// 解析结果带上限 8（防呆，见实现处注释）：设备报怪值时不得让调用方发成千上万条查询。
quint32 parseLunCount(const FirehoseResponse &r);

// ---- 会话级小工具（需要传输）----

// 发一条命令并等**一个** `<response>`（设备可能先发 <log> 再发响应，也可能反过来 ——
// reference/qdl/src/firehose.c:250-270 的注释；bkerler 循环等 `<response value`：
// edl/edlclient/Library/firehose.py:269-281）。线上形态 = `<?xml …?><data>命令</data>`，
// **无长度前缀**（firehose.c:399-405；firehose.py:904-922）。
// **命令帧的 ZLP 由本函数补**（Task 7 集成修正）：包裹后载荷长度恰为 `maxPacketSize()` 整数倍时
// 追加一次 0 字节写（qdl 的规则对**所有**写生效：reference/qdl/src/usb.c:548-553）；不补则设备侧
// bulk 读看不到短包 → 真机卡住/超时。数据块的 ZLP 在 edl_session 的数据面，传输层一概不补。
// 返回值：拿到响应元素（ACK 或 NAK 都算）→ true；写失败/超时/没有任何 <response> → false + 中文 *error。
// ACK/NAK 的**语义**由调用方判定（本函数不替调用方决定成败）。
// ⚠️ 见到 <response> 即停读：响应之后才到的 <log> 留给下一个命令的读循环去 drain
// （qdl 是"见到响应后再读到超时"：firehose.c:229-250；会话层的 drain 策略属 Task 6）。
bool firehoseSendCommand(IEdlTransport &t, const QByteArray &xml, FirehoseResponse &resp,
                         int timeoutMs, QString *error);

// configure 协商（reference/qdl/src/firehose.c:534-548 两轮；bkerler 同：
// edl/edlclient/Library/firehose.py:917-940）——**最多两次发送**（重发后再报的新值只采纳不再发）：
//   1. 发 xmlConfigure → 等 ACK；响应带 MaxPayloadSizeToTargetInBytesSupported → 用该值**重发一次**；
//   2. NAK 且文案含 "Not support configure MemoryName" → 换另一存储类型**重试一次**（入参 memoryName 同步改写）；
//   3. NAK 且文案含 "Only nop and sig tag can be" → 设备要求 EDL 鉴权：**直接失败、不重试**
//      （firehose.py:941-956 走小米鉴权分支，本项目明确不做 —— spec §8）；
//   4. 其它失败 → false + 中文 *error（带设备返回原文）。
// 成功时 memoryName 为实际生效的类型、maxPayloadBytes 为协商后的载荷上限。
bool firehoseConfigure(IEdlTransport &t, QString &memoryName, quint32 &maxPayloadBytes, QString *error);

// 发命令前的 drain：把 IN 端点里"响应之后还跟着的字节"读干净（0 超时轮询读，直到端点静默或达到
// 次数上限 —— 给设备"话痨日志"留余量，同时保证不会挂死）。依据：qdl 的注释明确"不消费完，后续写
// 会超时"（reference/qdl/src/firehose.c:249-252），而 firehoseSendCommand 见到 <response> 即停读
// （响应之后才到的 <log> 就留在端点里）。会话层（edl_session.cpp）与 EDLHandler 共用本实现。
void drainResidual(IEdlTransport &t);

} // namespace edl
