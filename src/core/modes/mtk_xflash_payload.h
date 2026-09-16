#pragma once

// MTK XFlash 引导层（Phase D2 Task 4）：七步握手 / bring-up 四步 / 只读查询
//                   （Phase D2 Task 5）：INIT_EXT_RAM(EMI) / boot_to / SHUTDOWN
//
// 协议细节对照 mtkclient v2.1.4-20-g71b0175（GPL-3.0，**只读参照，代码文本不进仓库**）：
//   • XFL = mtkclient/Library/DA/xflash/xflash_lib.py
//   • XFP = mtkclient/Library/DA/xflash/xflash_param.py
//
// 时序事实（每条都在上游核过，括号内为 file:line）：
//   • `0xC0` 的单字节读**不在本层**：上游是 JUMP_DA 之后由 upload_da1 先读 1 字节校验 0xC0（XFL:982-984），
//     本仓把这一步放在 BROM 级的 sendDa1 里（计划里的 T7 全链）；本层函数从 SYNC 开始。
//   • 七步握手（XFL:979-995）：0xC0 → sync → SETUP_ENVIRONMENT → SETUP_HW_INIT_PARAMS → 读回 SYNC。
//     **裸 SYNC 命令不读 status**（上游 sync() 只发送、不接收，XFL:903-907）；两个 setup 各经
//     send_param 读**一次** status（XFL:177-188）。故整段**只读 3 帧**：ENV status、HW_INIT status、
//     最后的 SYNC 回包（XFL:986-994）。多读一帧就会把 HW_INIT status 当成 SYNC 回包而判失败。
//   • 上游对两处 setup 的返回值不检查（XFL:989-990；失败也继续）——本层检查并在失败时明确报错（更严，理由同 D1：
//     静默继续会让后续帧全部错位）。
//   • bring-up 四步顺序固定（XFL:1103-1107）：get_expire_date → set_reset_key(0x68) →
//     set_checksum_level(0x0) → get_connection_agent。
//   • **有回包的 devctrl 查询，上游在拿到回包后还会再读一次 status**（XFL:571-578 expire_date /
//     :330-338 connection_agent / :396-418 chip_id / :623-636 packet_length / :421-436 ram_info）；
//     本层照做（尾部 status 只在回包非空时读，同上游的 `回包非空` 前置判断）——不读会让该帧留在
//     设备侧，把**之后**每次读整体错位一帧。**唯一例外**是 GET_PARTITION_TBL_CATA，上游不读
//     （XFL:612-621），本层同样不读（见 xflashGetPartitionCata 注释）。
//   • 子令号原稿有两处会**发错命令**，已按 xflash_param.py 更正：SET_RESET_KEY = 0x020004
//     （0x020005 是 SET_HOST_INFO）、GET_EXPIRE_DATE = 0x040011（0x040002 是 GET_NAND_INFO）。
//
// T5 三函数（每条同样在 XFL 核过，读帧数是测试的主要判据）：
//   • send_emi（XFL:251-270）：INIT_EXT_RAM → status 必须 0 → **sleep 10ms** → 长度 `pack("<I", len(emi))`
//     作为**独立 12B 帧**发出（`xsend` 不带 status）→ `send_param([emi])`（0x200 分块 + 一次 status）。
//     **整段只读 2 帧 status**（INIT_EXT_RAM / send_param）。无地址、无 emiver、无校验和 ——
//     那是 LEGACY 的 ENABLE_DRAM(0xE8) 机制，与 XFlash 的 INIT_EXT_RAM 是两套不同的东西。
//   • boot_to（XFL:288-328）：BOOT_TO → status 0 → `pack("<QQ", addr, len(da))` = **16B 独立帧**
//     → `send_data(da)`（**自带一次 status**，XFL:272-286）→ sleep 500ms → 再读一次 status，
//     **∈ {0x0, 0x434E5953} 才算成功**（XFL:315）。故整段读 **3 帧 status**（BOOT_TO / send_data / 终判）——
//     只给 2 帧会让终判读到空队列而误判失败。
//     `da` 必须**已剥尾部签名**（上游 XFL:1164 传进来的就是剥过的 da2，`XL:304` 处 `da2[:-da2sig_len]`）；
//     本层**不剥**，空切片直接拒绝。
//   • shutdown（XFL:813-833）：SHUTDOWN → status 0 → 32B 参数帧 → status 0。**整段只读 2 帧 status**。
//     `hasflags = (async_mode || dl_bit || bootmode != NORMAL) ? 1 : 0`（XFL:817-825）；
//     上游两条路径都 `port.close(reset=True)`（XFL:828/:832）—— 关端口是**调用方**的事
//     （本层不持有通道所有权，同 XFlashSession 头注）。
//
// T6 三函数（写/读数据路径，每条同样在 XFL 核过）：
//   • 56B 存储参数（XFL:677-680）：`pack("<IIQQ", storage, parttype, addr, len)` = 24B +
//     `NandExtension` 的 **8 个** u32 = 32B（该类有 9 个属性，pack 里**跳过** `operation_type`）。
//     ⚠️ 计划/事实报告原写"48B"是算错 —— 少 8 字节设备会把后 8 字节读成垃圾。
//   • 写（XFL:670-685 + :835-901）：WRITE_DATA → status 0 → 56B 参数（**length 取 512 对齐后的总长**）→
//     每块 `dsize = min(write_packet_length, 剩余)` → 补零到 512 的整数倍 → `checksum = sum(data) & 0xFFFF`
//     → `send_param([<I 0>, <I checksum>, data])`（**一次 status**）→ **循环之后再读一次 status**
//     （XFL:883-893，非 0 即写失败）→ 成功才发 CC_OPTIONAL_DOWNLOAD_ACT
//     （XFL:885；上游不检查其返回值，本层失败只告警 —— 数据此时已写入）。
//   • 读（XFL:687-704 + :706-806）：READ_DATA → status 0 → 56B 参数 → **参数帧之后的第二个 status**
//     （XFL:698-702；漏读会让之后每个数据帧整体错位一帧）→ 帧循环：slength **> 4** 是数据帧
//     （收下 + `ack(rstatus=False)`，XFL:750-757）、**== 4** 是 flag 帧（非 0 即报错；0 只是继续）、
//     其它长度是协议错误 → **收尾帧**（XFL:770-776，slength==4 时判 flag）。
//     **循环以字节数收尾**（上游 `bytestoread`，XFL:730/:757），不是"见到 flag 就停"：
//     中途的 flag==0 帧照上游继续读。字节读满后才读收尾帧。

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "core/modes/mtk_xflash_session.h"

namespace mtkbrom {

// ---- devctrl 子命令（XFP:5-74 整表核过）----
enum XDevCtrl : quint32 {
    X_CTRL_GET_CHIP_ID          = 0x04000D,   // XFP:53；回包 5×u16：(hw_code, hw_sub_code, hw_version, sw_version, chip_evolution)
    X_CTRL_GET_PACKET_LENGTH    = 0x040007,   // XFP:47；回包 <II = (write_packet_length, read_packet_length)
    X_CTRL_GET_CONNECTION_AGENT = 0x04000A,   // XFP:50；回包字符串（b"brom" / b"preloader"）
    X_CTRL_GET_PARTITION_CATA   = 0x040009,   // XFP:49（上游名 GET_PARTITION_TBL_CATA）；回包 <I：0x64=GPT / 0x65=PMT
    X_CTRL_GET_RAM_INFO         = 0x04000C,   // XFP:52；回包 24B(32 位) / 48B(64 位)：3 元组 ×2 = (sram, dram)
    X_CTRL_SET_CHECKSUM_LEVEL   = 0x020003,   // XFP:27；<I（PLAIN=0/CRC32=1/MD5=2）；上游恒设 0（XFL:1106）
    X_CTRL_SET_RESET_KEY        = 0x020004,   // XFP:28；<I（上游传 0x68，XFL:1104 / :206-209）
    X_CTRL_GET_EXPIRE_DATE      = 0x040011,   // XFP:57；无参数，回包文本（XFL:571-578）
    // ⚠️ 这个值上游就是 **0x800005**（同段邻居都是 0x0800xx，是上游自己的写法，XFP:64-69）——
    // **照发**，不要"修正"成 0x080005：设备按字面收，改一个 0 就是另一条命令。
    X_CTRL_CC_OPTIONAL_DOWNLOAD_ACT = 0x800005,   // 写完一个分区后的可选下载动作（XFL:885，无参 devctrl）
};

struct XPacketLength {
    quint32 writeLength = 0;
    quint32 readLength = 0;
};

struct XChipId {
    quint16 hwCode = 0;
    quint16 hwSubCode = 0;
    quint16 hwVersion = 0;
    quint16 swVersion = 0;
    quint16 chipEvolution = 0;
};

enum class PartitionCata {
    Gpt,
    Pmt,
    Unknown,   // 回包其它值：调用方按"两者都试"处理（上游 XFL:616-621 同样返回 0）
};

// ① 七步握手（XFL:979-995，入口在 `0xC0` 已校验之后）：sync(0x434E5953) → SETUP_ENV(20B)
//    → SETUP_HW_INIT(4B) → 读回必须 == "SYNC"。两处 setup 的 status 非 0 即失败（比上游更严，见文件头）。
bool xflashDa1Handshake(XFlashSession &x, QStringList *log, QString *error = nullptr);

// ② bring-up 四步（顺序固定，XFL:1103-1107）：get_expire_date → set_reset_key(0x68) →
//    set_checksum_level(0x0) → get_connection_agent。后者回包原样经 connectionAgent 传出。
bool xflashBringUpSteps(XFlashSession &x, QByteArray *connectionAgent, QStringList *log, QString *error = nullptr);

// ③ 只读查询（均含上游的尾部 status 读；GET_PARTITION_TBL_CATA 例外见实现注释）
// GET_CHIP_ID 回包**长于** 10 字节时只取前 5×u16（照上游截断，不判失败 —— 未知硬件可能带填充），
// 但截断会写进 log（可空），不再静默。log 追加在 error 之后，既有 3 参调用点不受影响。
bool xflashGetChipId(XFlashSession &x, XChipId &out, QString *error = nullptr, QStringList *log = nullptr);
bool xflashGetPacketLength(XFlashSession &x, XPacketLength &out, QString *error = nullptr);
bool xflashGetPartitionCata(XFlashSession &x, PartitionCata &out, QString *error = nullptr);
bool xflashGetRamInfo(XFlashSession &x, QByteArray *raw, QString *error = nullptr);   // 原始 24/48B（解析留给 D4/诊断）

// ④ 载荷发送（T5）
// EMI（DRAM 初始化数据，XFL:251-270）：读 2 帧 status（见文件头）。空 EMI 直接拒绝。
bool xflashSendEmi(XFlashSession &x, const QByteArray &emi, QString *error = nullptr);
// boot_to（跳 DA2，XFL:288-328）：DA2 必须**已剥尾部签名**（调用方给；见 D1 的 DaSelection.da2Bytes
// 与 m_sig_len）。读 3 帧 status，终判接受 0x0 / 0x434E5953 两个值。
// ⚠️ **返回 true 只表示终判状态字匹配，不等于 DA2 已在跑**：上游靠随后的 reinit 查询组合
// （GET_RAM_INFO / GET_CHIP_ID / GET_DA_VERSION … 能应答）才判"DA2 存活"（facts report §7.4），
// 本层**不跑**那组查询。故调用方日志只能写"已上传"，别写"已就绪 / 已验证"。
bool xflashBootTo(XFlashSession &x, quint64 addr, const QByteArray &da2, QString *error = nullptr);
// SHUTDOWN（XFL:813-833）：32B 参数 pack("<IIIIIIII", hasflags, enablewdt, async_mode, bootmode,
// dl_bit, dont_resetrtc, leaveusb, 0)；hasflags 由 async_mode/dl_bit/bootmode 推导（XFL:817-825）。
// 只发命令、不关端口（上游 XFL:828/:832 的 port.close 由调用方负责）。
bool xflashShutdown(XFlashSession &x, quint32 bootmode = 0, QString *error = nullptr);

// ⑤ 写/读数据路径（T6）
// 56B 存储参数（铁律 15，见文件头）：24B 包头 + 32B NandExtension（全 0）。恒 56 字节。
QByteArray xflashStorageParam(quint32 storage, quint32 partType, quint64 addr, quint64 length);
// 写一个区间（XFL:670-685 + :835-901）。`writePacketLength` 取 GET_PACKET_LENGTH 的
// write_packet_length，**为 0 即拒绝（无回退，铁律 14）**；**非 512 的整数倍也拒绝**（fail-closed：
// 那种取值下补零会插进数据中间、实发字节与参数承诺不符 —— 静默写坏镜像，真机不会报这种值）。
// 读帧数：2 + 块数 + 1（收尾）+ 2（CC devctrl）+ 1（CC 回包）。
// CC_OPTIONAL_DOWNLOAD_ACT 失败**只告警不判失败**（数据已写入，上游也不检查）。
bool xflashWriteData(XFlashSession &x, quint64 addr, const QByteArray &data,
                     quint32 storage, quint32 partType, quint32 writePacketLength,
                     QStringList *log = nullptr, QString *error = nullptr);
// 读一个区间到内存（XFL:687-704 + :706-806）。`out` 只收**数据帧**（flag 帧不进）；
// 读满 `length` 字节后再读一帧收尾。flag 非 0 即失败。
bool xflashReadData(XFlashSession &x, quint64 addr, quint32 length,
                    quint32 storage, quint32 partType, QByteArray &out, QString *error = nullptr);

} // namespace mtkbrom
