#pragma once

// MTK XML（D3）载荷 ①（Phase D2+D3 Task 9）：DA1 上传后的 `CMD:START` 握手 + 环境建立三步
//
// 协议细节对照 mtkclient v2.1.4-20-g71b0175（GPL-3.0，**只读参照，代码文本不进仓库**）：
//   • XL = mtkclient/Library/DA/xmlflash/xml_lib.py
//   • XC = mtkclient/Library/DA/xmlflash/xml_cmd.py（命令信封构造器）
//   事实底稿：.superpowers/sdd/mtk-d2d3-facts-report.md §5.3 / §5.4
//
// 与 XFlash 的**决定性差异**（XL:309-313、XFL:982-985）：DA1 起来后的同步信号是设备发来的
// **CMD:START 文本 XML 消息**，不是 XFlash 的 `0xC0` 单字节；XML 侧没有 sync 命令，也没有
// `0x434E5953` 状态字。随后 XML 发三条文本命令（XL:311-313）：setup_env → setup_hw_init →
// setup_host_info；XFlash 的 set_checksum_level / set_reset_key / get_expire_date /
// get_connection_agent 在 XML 侧**不存在**。
//
// 铁律 18 的姿态（**有意**比上游严格处）：上游 `upload_da1` 对这三步的返回值**一概不检查**
// （XL:311-313 三行裸调用，紧随其后就 `return True`，XL:314），失败也照常回报成功；
// 本层任一失败即中止并给文案。
//
// 诚实边界：
//   • **不发 EMI**：XML 的 DRAM 初始化就是 SET-RUNTIME-PARAMETER 里的
//     `<adv><initialize_dram>YES</initialize_dram></adv>` 一句（XL:184、XC:120-135）
//   • **不做代际路由**：LEGACY 代**没有** setup_env / setup_hw_init（事实报告 §1.1）——
//     调用方必须先判代，不得对本层无脑调用
//   • **取值暂为固定默认档**：上游从 daconfig 取（`uartloglevel` → LogLevel、`logchannel`，
//     XL:167-186），本层尚未接配置层，固定 INFO / UART / LINUX / NONE / AUTO-DETECT
//     —— 与上游默认档一致（`XC:98-100` 的默认参数、`mtk_config.py:81` 的 logchannel="UART"）
//   • 只做握手与环境建立：不发数据命令、不落分区表（T10）
//
// 运行期前提：调用方已完成 DA1 上传与跳转（本层从"等 CMD:START"开始，不自己触发跳转）。
//
// 失败文案约定（写法同 T5 的 SHUTDOWN）：**单前缀 + 点名本步**是硬约定，具体句式两族并存 ——
//   • 握手/setup：`XML：<本步> 失败（<session 原文>）`
//   • 数据通路：`XML：WRITE-FLASH <细节>` / `XML：READ-FLASH <细节>`
// —— `XmlSession` 文案自带的 "XML：" 层名前缀先剥掉再套（不成双前缀）；内层措辞（实收内容 / `ERR!` 码值）逐字保留。

// MTK XML（D3）载荷 ②（Phase D2+D3 Task 10）：WRITE-FLASH / READ-FLASH 的数据通路
//
// 协议细节对照 mtkclient v2.1.4-20-g71b0175（GPL-3.0，**只读参照，代码文本不进仓库**）：
//   • XL = mtkclient/Library/DA/xmlflash/xml_lib.py
//   • XC = mtkclient/Library/DA/xmlflash/xml_cmd.py（命令信封构造器）
//
// **两条数据通路的节拍不同，不要互抄**（计划期预核对 + 本任务与上游逐句核对）：
//   • 写（`writeflash` XL:943-983 + `upload` XL:451-506，后者以 raw=True 调用）：
//     ① WRITE-FLASH（noack）→ ② 设备发 FileSysOp（key 必须 FILE-SIZE，XL:967-968）
//     → ③ ackValue(**length**)（XL:969）→ ④ 设备发 DwnFile（带 packet_length，XL:970-971）
//     → ⑤ **再** ackValue(length) + 读 "OK"（`upload()` 自己又发一次长度 ack，`XL:463-464`；
//       漏掉这一发一读，后面每一帧都错位）→ ⑥ 逐包 {ackValue(0) → 读 "OK" → 发块 → 读 "OK"}
//     （XL:468-486）→ ⑦ ack（`XL:487-488` 的 raw 分支）→ CMD:END(OK) → ack → CMD:START。
//     ⚠️ ⑦ 里 **CMD:END 与 CMD:START 之间没有独立的 "OK" 应答帧** —— `ack()` 是写，它的"回应"
//     就是下一条命令。同形状见 `check_lifecycle`（XL:987-1003：`download()` 的尾 ack `XL:583`
//     之后直接读 CMD:END）。简令的写夹具在两者之间多排了一帧 "OK"，以上游为准（测试文件订正 #3）。
//   • 读（`readflash` XL:918-941 → `download_raw` XL:508-559）：READ-FLASH（noack）→ 设备发
//     UpFile → 裸 `OK@0x<len>` → ack → 读 "OK" → ack → **逐帧** {收一帧 → ack → 读 "OK" → ack}
//     → CMD:START。这个**逐帧 ack** 与 `get_command_result` 的裸 OK@ 分支（`XL:373-388`，单次
//     尾 ack，即 `XmlSession::readCommandResult`）**不是**一回事，故 `xmlReadDataFrames` 独立实现，
//     不得改走 `readCommandResult`（T8 实施期实测 + 控制方核对）。
//
// **与上游的分歧（有意，写在 .cpp 各入口注释里）**：
//   ① 读路径要求"设备宣布的长度 == 请求长度"（上游 `XL:517-518` 直接用宣布值覆盖请求值，
//      调用方拿到短包也发现不了；本层与 T8 的"数据长度不符"检查同姿态，fail-closed）；
//   ② 数据帧途中的 DT_MESSAGE（DA 日志）帧：上游 `xread` 会把内容追加进 UART log，本层**读掉
//      但丢弃** —— `XmlSession` 只提供 `setLogSink` 没有 getter，载荷层拿不到 sink（见 .cpp）。
//
// 诚实边界：
//   • **不发 EMI**、**不判代际**（同 T9）
//   • **不做分区表读取/代际路由**：存储描述符、长度与写地址由调用方给（T12 的链式集成）
//   • 写地址 `<offset>` 由调用方给（XC:452-462 的 `offset=addr`；T12 起是 `xmlWritePartition`
//     的 `addr` 形参，默认 0 = 上游 gpt 伪分区/整盘写法的口径，见函数注释）

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "core/modes/mtk_xml_session.h"

namespace mtkbrom {

// DA1 后握手（XL:271-321）：等设备发 **CMD:START** 文本消息（不是 XFlash 的 0xC0），
// 随后 setup_env → setup_hw_init → set_host_info。log 非空时追加中文进度行。
// 首条命令**必须**是 CMD:START —— XmlSession 对"具名但未列举"的命令保留名字而不失败
// （`mtk_xml_session.h` 的约定），故这里显式核对 `out.command`。
bool xmlDa1Handshake(XmlSession &x, QStringList *log, QString *error = nullptr);

// SET-RUNTIME-PARAMETER（XL:167-186 + XC:98-136）：version="1.1"、checksum_level=NONE、
// battery_exist=AUTO-DETECT、da_log_level 为**字符串**（TRACE..ERROR，XL 的 LogLevel）、
// log_channel=UART、system_os=LINUX，外加独立的 `<adv>` 块的 initialize_dram=YES。
// 信封的通用形式（XmlSession::envelope）装不下 `<adv>`，故整段 XML 按上游逐字节拼。
bool xmlSetupEnv(XmlSession &x, QString *error = nullptr);

// setup_hw_init（XL:323-327）：HOST-SUPPORTED-COMMANDS（能力串，`XC:139-140` 默认值）
// + NOTIFY-INIT-HW（上游 `cmd_notify_init_hw` 传 content=None → **不写 `<arg>`**，XC:32-42），
// 两条都必须 OK。
bool xmlSetupHwInit(XmlSession &x, QString *error = nullptr);

// SET-HOST-INFO（XL:329-331 + XC:600-612）：`<info>%Y%m%dT%H%M%S</info>`，本地时间戳。
bool xmlSetHostInfo(XmlSession &x, QString *error = nullptr);

// 写一个分区（XL:943-983 + XL:451-506）：数据不足 512 整数倍时**补零**到整数倍（XL:974-976），
// 宣布长度 = 补零后的字节数，descriptor = `MEM://0x8000000:<length>`（XC:452-462 的默认 mem_offset
// 0x8000000）。log 非空时追加一条中文完成行。空数据直接失败。
// `partition` = **存储描述符**（`XC` 的 UFSPartitionType 文本，`ST:216` 起 XML 分支用字符串：
//   eMMC 用户区 = "EMMC-USER"，**不是** GPT 分区名）；`addr` = 写入地址（上游 `writeflash(addr=…)`
//   → `cmd_write_flash(partition, offset=addr, …)` 的 `<offset>hex(addr)</offset>`，XC:452-462）——
//   逐分区写时由调用方给 **GPT 条目地址**（`partition.sector * pagesize`：`v6.py:1095-1097`
//   写 seccfg、`mtk_da_handler.py:544-548` 写任意分区）；0 对应上游对 gpt 伪分区/整盘的写法
//   （`mtk_da_handler.py:537-541`），也是 T10 各调用点的默认（`addr` 由 T12 补出：逐分区写必须
//   带 GPT 条目地址，否则会写到描述符的偏移 0 = 分区表区）。
bool xmlWritePartition(XmlSession &x, const QString &partition, const QByteArray &data,
                       QStringList *log = nullptr, QString *error = nullptr, quint64 addr = 0);

// 收 length 字节数据帧（**上游 `download_raw` 形状**，XL:508-559）：读裸 `OK@0x<len>` → ack →
// 读 "OK" → ack → 循环{ 收一帧 → ack → 读 "OK" → ack }。**不要**复用
// `XmlSession::readCommandResult` 的裸 OK@ 路径（那条是 `get_command_result` 的单次尾 ack，节奏不同）。
bool xmlReadDataFrames(XmlSession &x, quint32 length, QByteArray &out, QString *error = nullptr);

// 读一个分区（XL:918-941）：READ-FLASH（noack）→ 设备发 UpFile → xmlReadDataFrames（逐帧 ack）
// → 收尾 CMD:START。
bool xmlReadPartition(XmlSession &x, const QString &partition, quint64 offset, quint32 length,
                      QByteArray &out, QString *error = nullptr);

// 收尾复位（XL:1038-1046 的 shutdown → XC:429-440 cmd_reboot）：action = DISCONNECT（默认）/
// IMMEDIATE；走 send_command 的**默认**节奏（OK → CMD:END → CMD:START）。
bool xmlReboot(XmlSession &x, bool disconnect = true, QString *error = nullptr);

} // namespace mtkbrom
