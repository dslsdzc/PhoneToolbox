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
// 失败文案约定（写法同 T5 的 SHUTDOWN）：每个入口的失败文案都是**单前缀 + 点名本步**
//     `XML：<本步> 失败（<session 原文>）`
// —— `XmlSession` 文案自带的 "XML：" 层名前缀先剥掉再套（不成双前缀）；点名到具体命令
// （`setup_hw_init` 的两条命令可分），内层措辞（实收内容 / `ERR!` 码值）逐字保留。

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

} // namespace mtkbrom
