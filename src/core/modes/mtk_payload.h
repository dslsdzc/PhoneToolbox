#pragma once

// MTK payload 上传与刷写集成（计划 F1-3；LEGACY 两阶段帧 = D1 Task 6）
//
// 协议细节对照（上游 mtkclient v2.1.4-20-g71b0175，GPL-3.0，**只读引用不复制代码**）：
//   • SEND_DA/JUMP_DA 帧      → 规格 §2.4 完整时序（帧在 Task 4 的 BromSession 侧）
//   • DA1 就绪 0xC0           → Library/DA/legacy/dalegacy_lib.py:596-601
//     （LEGACY **没有** SYNC/SETUP_ENVIRONMENT/SETUP_HW_INIT_PARAMS —— 那是 XFlash 的）
//   • 存储信息交换 + 存储类型 → dalegacy_lib.py:607-633（类型判定在 :623-628）
//   • stage2 配置 + errorcode → dalegacy_lib.py:228-403
//   • EMI/DRAM 分档           → dalegacy_lib.py:333-398（逐档差异见 sendEmiLegacy 注释）
//   • boot_to(DA2)            → dalegacy_lib.py:907-940（brom_send）
//   • read_flash_info         → dalegacy_lib.py:526-552（PassInfo 定义在 :28-39）
//   • SBC 修补字节模式        → GPLv3 子模块来源标注（5 个核心模式，见 mtk_payload.cpp）
//   • EMMC 写入               → 规格 §3.5 Legacy 命令集 + §3.6 分区表（F1-2 DaStorage）
//
// 诚实边界：
//   • DA 二进制由调用方提供（官方固件/设备提取的自研工具为后续任务）
//   • SLA（0x1D0D）检测到返回明确错误；RSA 响应生成（规格 §2.5）为后续任务
//   • V6 新平台 BROM 已修补（规格 §1.2），无专用检测 —— 以 SEND_DA/JUMP_DA 失败
//     形式传播，V6 检测标后续
//   • 本文件只做 **LEGACY** 通道（XFlash/XML 的 DA 帧属 D2）；DA2 起来后的
//     存储详情（NOR/NAND/EMMC 各表字段）**只读走不解析** —— 那是 D2/D3 的范围
//   • D1 的存储路径只有 eMMC（PMT/分区表）→ NAND/NOR 的 BMT 分支不实现，
//     sendStage2Config 对非 eMMC 明确拒绝（不瞎写上游 nand 分支的值）
//   • nandcount 两级都为 0 时上游走 usbread(-4)（读到没有为止），本实现有界化 —— 真机未验证

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include "core/modes/mtk_brom.h"
#include "core/modes/mtk_da_file.h"
#include "core/modes/mtk_emmc.h"
#include "core/modes/mtk_preloader_emi.h"

namespace mtkbrom {

// ---- DA1 ----

// DA1：SEND_DA(region[1].startAddr, m_len, m_sig_len, region[1] 切片) → JUMP_DA(同址)。
// 地址/长度/签名长度**全部来自 region[1]**（三代共同硬编码，spec §2 P5）——
// 取代旧的 sendPayload（旧版把**整个文件**按 addr=0/sigLen=0 发出，真机必错）。
bool sendDa1(BromSession &s, const DaSelection &sel, QString *error = nullptr);

// DA1 起来后**先回单字节 0xC0**（dalegacy_lib.py:596-601）——LEGACY 没有 SYNC/SETUP_ENVIRONMENT。
// 收到其它字节一律失败（不得把任意字节当就绪）。
bool waitDa1Ready(BromSession &s, int timeoutMs, QString *error = nullptr);

// 存储信息交换（dalegacy_lib.py:607-633 逐句）：读 4B NAND_INFO → 2B id 数 → id数×2B →
// 4B EMMC_INFO → 4×4B → 写 1B ACK → 读 3×1B。顺带按上游规则（:623-628）定出存储类型：
//   nandids[0] != 0 → "nand"；否则 emmcids[0] != 0 → "emmc"；否则 "nor"。
// 不做这一步，DA1 不会进入 stage2 配置（后面所有步骤都会错位）。
bool exchangeDa1StorageInfo(BromSession &s, QString *flashtype, QStringList *log,
                            QString *error = nullptr);

// stage2 配置写入 + errorcode 握手（dalegacy_lib.py:228-294）。
// *emiNeeded = (errorcode == 0xBC3)；**errorcode == 0 时本步正常结束且不需要 EMI**。
// 非 eMMC 存储直接拒绝（D1 只支持 eMMC，见文件头诚实边界）。
bool sendStage2Config(BromSession &s, quint16 hwCode, quint8 bromVer, quint8 blVer,
                      const QString &flashtype, bool *emiNeeded, QStringList *log,
                      QString *error = nullptr);

// errorcode == 0xBC3 之后的读取（dalegacy_lib.py:295-327）：
//   读 4B（丢弃）+ 16B draminfo → 读 4B **必须 == 0xBC4** → 2B nand_id_count → count×2B。
// draminfo 交回调用方 —— D1 **仅用于日志**（不做上游那种"按 draminfo 在固件目录里
// 自动找 preloader"的补救匹配；EMI 必须由调用方显式给出）。
bool beginEmiDramInfo(BromSession &s, QByteArray *dramInfo, QStringList *log,
                      QString *error = nullptr);

// EMI/DRAM 初始化（dalegacy_lib.py:333-398 逐句对应；地址/长度全**大端**）。
// ⚠️ 各档位的读/写顺序**不同**（档 A/B/C/D/其它，见 .cpp 的逐档注释）—— 顺序错位即真机全盘错位。
bool sendEmiLegacy(BromSession &s, const EmiData &emi, quint16 hwCode, QString *error = nullptr);

// DA2：>I addr → >I size → >I packetsize(0x1000) → 读 1B ACK → 分块写（每块读 1B ACK）
//   → sleep(0.5) → 写 ACK → 读 1B ACK（dalegacy_lib.py:907-940）。
// ⚠️ **LEGACY 保留尾部签名**：发送的是 region[2] 的完整 m_len 字节（不裁 sigLen）。
bool bootToDa2Legacy(BromSession &s, const DaSelection &sel, QString *error = nullptr);

// DA2 存活判据（dalegacy_lib.py:526-552）：逐段读掉上游会读的字节（长度全部来自上游），
// 只解析 PassInfo 的 ack/下载状态 —— **不解析 NOR/NAND/EMMC 详情**（只记字节数）。
// hwCode 由调用方从 getHwCode(0xFD) 得来（上游在 :543-545 用它决定是否多读 4B）。
bool readFlashInfoDa2(BromSession &s, quint16 hwCode, QStringList *log, QString *error = nullptr);

// ---- 刷写集成（F1-3）----

// SBC 修补：preloader 字节模式替换（GPLv3 子模块来源标注，5 个核心模式）。
// 返回是否发生任何替换；不匹配时原样返回。
bool patchPreloaderSecurity(QByteArray &preloader);

// 刷写分区：listPartitions 查名 → emmcWrite(分区偏移, 镜像)。未找到返回明确错误。
bool flashPartition(BromSession &s, DaStorage &st, const QString &name,
                    const QByteArray &image, QString *error = nullptr);

} // namespace mtkbrom
