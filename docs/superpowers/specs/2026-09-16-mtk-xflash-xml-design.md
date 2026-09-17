# PhoneToolbox — MTK BROM 刷写 Phase D2（XFlash）+ D3（XML）设计

日期: 2026-09-16
状态: 待用户评审
前置: D1（LEGACY 完整化）已合并进 `main`（`649873b`，12 任务 / 60 提交）
配套事实：`.superpowers/sdd/mtk-d2d3-facts-report.md`（736 行，逐条带 `file:line`，含 832 preloader 与 2 个 DA 文件的实测）
样本与实测产物：`reference/mtk-samples/`（**gitignored**）
后续：Unisoc 补齐 / 特殊模式（高通 9006、preloader 直连、三星非 Odin 变体）—— 本 spec 不含

## 1. 背景与目标

D1 已让 MTK BROM 通道在**老代 LEGACY** 上闭环（DA 解析与选择、两阶段引导、EMI、按设备分区表逐分区写、FINISH）。本阶段补上**现代两代**：

- **D2（XFlash，`damode=5`）**：12 字节帧协议、DA1 七步握手（`sync`/`setup_env`/`setup_hw_init`）、`INIT_EXT_RAM` EMI、`boot_to`（**剥尾部签名**）、`WRITE_DATA`/`READ_DATA`（48 字节参数）、`SHUTDOWN`、只读查询（chip id / packet length / connection agent / ram info）、**GPT 分区表**读取。
- **D3（XML，`damode=6`）**：文本协议（`<da>`/`CMD:*`/`OK`/`OK@0x<len>`/`OK!EOT`）、`CMD:START` 握手、`setup_env`/`setup_hw_init`/`setup_host_info`、**`CMD:WRITE-FLASH`**（数据走 `CMD:DOWNLOAD-FILE` 的 packet 机制）/ `CMD:READ-FLASH`（数据走 `CMD:UPLOAD-FILE`）、`CMD:END`+`CMD:START` 收尾、GPT 分区表。

**目标**：三条链（LEGACY / XFlash / XML）**共用**同一套上层（计划层、通道、UI、preloader 三路径），按设备代际自动路由；离线可验（真样本 + mock 逐帧 + 对抗变异），真机归持机人。

**用户决策记录（2026-09-16）**
- 拆分：**一份 spec + 一个大计划**（D2+D3 一起交付，与 D1 的"两计划"不同）。
- D2 深度：**完整刷写链**（bring-up + EMI + GPT + 逐分区写 + SHUTDOWN + 只读查询）。
- GPT 样本：**去找真实的**（不满足于"合成夹具 + 边界"）。
- D3 深度：**完整链**（与 D2 对等）。
- 设计期 4 条裁决（用户已认可）：GPT **CRC fail-closed**；**备份 GPT 兜底**要实现正确；XFlash **剥签名**（与 LEGACY 相反）；`DOWNLOAD`/`UPLOAD`/`FORMAT_PARTITION`/SLA/格式化**不做**。

## 2. 事实基础（要点；逐条出处见配套报告）

**跨代三条总纲（facts §0）**
1. **LEGACY 没有 `SYNC`/`SETUP_ENVIRONMENT`/`SETUP_HW_INIT_PARAMS`**（全局 grep 只命中 XFL/XL）——三代握手是**三套**，不是一套。
2. **V6 DA 文件里没有 EMI 数据**（`region[0]` 与 `region[1]` 逐字节相同，实测 4/4）；EMI 只能来自 preloader。
3. **设备不上报 damode**（全仓无此命令）→ 代际判定 = **芯片表 `damode`** + **DA 文件 `v6`**（`DC:173/216`，`MTK_DA_v6` 在偏移 0x20）。**`plcap`/`blver` 那条"设备回报可提升到 XFLASH"是死代码**（`plcap` 唯一写入点 `PL:925` 全仓零调用 → 恒 None；`blver` 默认 -2 → `blver > 1` 亦假，`DL:221-225` 永不触发）——**本阶段不实现该分支**，只注释说明。

**XFlash 帧与握手（facts §1.1/§3）**
- DA1 上传与 LEGACY 共用 BROM 级 `SEND_DA(0xD7)`/`JUMP_DA(0xD5)`（`XFL:979-981`），DA1 = **`region[1]`**、DA2 = `region[2]`（`XFL:963-973`；实测 V6 的 DA2 地址恒 `0x40000000`、V5 现代条目亦然）。
- `0xC0` 之后是**七步**（`XFL:979-995`）：读 1B 必须 `0xC0`（`XFL:982-985`）→ `sync()`=`xsend(0x434E5953)`（`XFL:903-907`，`XFP:3`）→ `setup_env()`=`0x010100` + 20B（`pack("<IIIII", uartloglevel, log_channel, OS_LINUX=1, 0, 0)`，`XFL:909-924`）→ `setup_hw_init()`=`0x010101` + 4B 0（`XFL:926-932`）→ `xread()` 必须 == `pack("<I",0x434E5953)`（`XFL:991-994`）。**两处 setup 的返回值上游不检查**（失败也继续）。
- **12B 帧头**（`§3.1`）：`magic(0xFEEEEEEF) | datatype | length`，全**小端**（与 LEGACY 的"全大端"相反）。
- `send_param` 通用语义：**按 0x200 分块**（facts §1.2）。
- **0x6781 特例**（`XFL:85-100`）：该芯片用**一次 16 字节写**（头+载荷合并），其余芯片两次写。文档"4 字节短帧"**不存在**。

**XFlash EMI（facts §2.2/§2.4）**
- `INIT_EXT_RAM = 0x01000A` → `status()` 必须 0 → `sleep(0.01)` → `xsend(pack("<I", len(emi)))`（**长度单独一帧**）→ `send_param(emi)`（0x200 分块）。**无地址字段、`emiver` 完全不用**（全局 grep `emiver` 零命中 XFL）。
- **跳过 EMI 的唯一条件**：`GET_CONNECTION_AGENT(0x04000A)` 返回 `b"preloader"`（`XFL:1107-1151`）；返回 `b"brom"` 且无 EMI → **只 warning 不中止**（`XFL:1143-1144`）；其它值 → 返回 False。
- **EMI 切片取法两代不同**（实测）：XFlash = **整块 912 B**；LEGACY = `MTK_BIN+0xC` 起 **800 B**。同一 preloader 两个函数。

**XFlash 命令集（facts §4.1/§4.2）**
- 有完整实现且本阶段要用：`WRITE_DATA 0x010004` / `READ_DATA 0x010005`（`xsend` → status 0 → **48B 参数** `pack("<IIQQ", storage, parttype, addr, length)` + 8×u32 NandExtension → `send_param`）、`SHUTDOWN 0x010007`（32B 参数）、`BOOT_TO 0x010008`、`DEVICE_CTRL 0x010009`（子命令机制）、`INIT_EXT_RAM`、`SETUP_*`；只读查询 `GET_CHIP_ID 0x04000D`（5×u16）/`GET_PACKET_LENGTH 0x040007`（`<II`）/`GET_CONNECTION_AGENT`/`GET_RAM_INFO 0x04000C`/`GET_EMMC_INFO 0x040001`/`GET_PARTITION_TBL_CATA 0x040009`（**`0x64`=GPT、`0x65`=PMT**，`XFL:613-621`）。
- `write_packet_length` **无回退**（`XFL:858`）→ 拿不到即明确报错；`read_packet_length` 有 1 MB 回退（`XFL:724`）。
- `storage`/`parttype` 取值见 `ST:16-49`（EMMC=0x1、UFS=0x30、USER=8…）。
- **零调用命令不做**：`DOWNLOAD 0x010001`、`UPLOAD 0x010002`、`FORMAT_PARTITION 0x010006`、`NAND_BMT_REMARK`、多数 `SET_*`/`CTRL_*`（facts §4.2）。

**XML 协议（facts §5）**
- 传输**同样 12B 帧头**（`XL:150`，magic 同 `0xFEEEEEEF`），载荷 UTF-8 文本：`str` 载荷 `length = len+1` 且**带 NUL 结尾**（`XL:146-153`）；`DT_MESSAGE`（DA→主机日志）用 16B 头（多 `priority:u32`）。
- 根元素：主机 → `<da>`、DA → `<host>`（`XC:19`）；命令名统一加 `CMD:` 前缀。
- 应答：`OK`；错误含 `ERR!`；带长度 `OK@0x<hexlen>\0`；**完成永远两步**：`CMD:END`(OK) → `CMD:START`（`XL:195-209`）。
- 批量数据**不在 XML 里内联**：命令带 `MEM://0x<addr>:0x<len>` 描述符，字节流走独立帧块；写入 `CMD:DOWNLOAD-FILE` 回包里给 `packet_length`（`XL:419-424`），读 `CMD:UPLOAD-FILE`（`XL:425-431`）。
- 地址文本 = Python `hex()`：**小写、`0x`、无零填充**（`XC:457/479`）；解析一律 `int(x,16)`。
- **checksum 被解析但从不校验**（`XL:452-453/510-512/562-565`），策略由 `SET-RUNTIME-PARAMETER` 的 `checksum_level` 一次性选定，默认 `NONE`（`XC:131`）。
- **DA1 后握手是 `CMD:START` 文本消息**（不是 XFlash 的 `0xC0` 单字节，`XL:271-321`）→ 之后 `setup_env`/`setup_hw_init`/`setup_host_info`；**不发** XFlash 那几个 `SET_*`/`GET_*`（`§5.4`）。
- XML 专有：`PL:151-152` 强制读 SoC ID；`PL:336-337` XML + SLA → 触发 **BROM 级** SLA（**本阶段不做，明确报错**）。

**GPT 路径（facts §6 + 本 spec 的真样本实测）**
- 设备侧源数据：**LBA0..N 的原始扇区**（XFlash 经 `READ_DATA` 读）；扇区大小：eMMC 512、UFS 4096（`XFL:448` 读出 `emmc.block_size` 但**从未赋给 `config.pagesize`** —— 上游缺陷③）、XML+UFS 硬编码 4096（`XL:805`）。
- 解析：LBA1 头 `EFI PART`（8 字节整体比较，`gpt.py:45/223`）× 512/4096 两档探测（`gpt.py:219-224`）；`revision != 0x10000` 上游直接拒（`gpt.py:165-167`）；CRC32 **被解析但从不校验**（缺陷④）。
- **上游 4 处已知缺陷，我们不复刻**：① `--gpt-num-part-entries`/`--gpt-part-entry-size` 无效、`--gpt-part-entry-start-lba` 被当**字节偏移**（`gpt.py:168-171`）；② **备份 GPT 兜底实际不生效**（`partition.py:45` 的 seek 被 `gpt.py:161` 覆盖）；③ eMMC 恒 512；④ CRC 不校验。
- **真样本**（`reference/mtk-samples/PGPT.img`，来源与实测见 §9）：**4096 字节扇区**、LBA1@4096 才是 `EFI PART`、61 条目、条目 CRC 校验通过、头部 CRC 需"字段清零后再算"才通过；`sgdisk_print.txt` 为独立对拍基准。

**DA 文件（沿用 D1 解析器，本阶段新增的消费点）**
- `region[2].m_start_addr` **逐条目从文件读**（现代几乎都是 `0x40000000`），**不是常量**；DA2 二进制 = `region[2].m_buf` 起 `m_len` **再剥 `m_sig_len`**（`XFL:970-978`）。
- V6 文件 `region[0] == region[1]` 逐字节相同（实测）；条目步长 `0xDC` 只解析前 `0x50` 字节（facts §7.2）。

## 3. 架构与模块

```
新增 src/core/modes/mtk_xflash_session.{h,cpp}   12B 帧 + xsend/xread/status/send_param/send_data/DEVICE_CTRL 子命令
新增 src/core/modes/mtk_xflash_payload.{h,cpp}   七步握手 + EMI(INIT_EXT_RAM) + boot_to + WRITE/READ_DATA + SHUTDOWN + 只读查询
新增 src/core/modes/mtk_gpt.{h,cpp}              纯函数：扇区探测/头部与条目解析/CRC 校验/备份 GPT/名字→区间
新增 src/core/modes/mtk_xml_session.{h,cpp}      文本帧 + OK / OK@0x<len> / OK!EOT / get_command_result
新增 src/core/modes/mtk_xml_payload.{h,cpp}      CMD:START 握手 + setup_* + WRITE-FLASH/READ-FLASH（数据流 = DOWNLOAD-FILE/UPLOAD-FILE）+ CMD:END/START
改  src/core/modes/mtk_preloader_emi.{h,cpp}     +extractEmiXflash（整块切片；与 LEGACY 切片并存）
改  src/core/mtk_flash_plan.{h,cpp}              参照表来源 +scatter XML 方言（<partition_name>/<partition_size>）
改  src/core/modes/mtk_payload.{h,cpp}           bromFlashOnSession 按代际路由（Legacy 已实现 / +XFlash / +XML 三条）
改  src/core/flash_tool.{h,cpp} / src/ui/flash_panel.cpp   日志与文案说明走了哪条链（**不新增入口**）
测试：test_mtk_xflash_session / test_mtk_xflash_payload / test_mtk_gpt / test_mtk_xml_session / test_mtk_xml_payload（新）
      + test_mtk_preloader_emi（扩 XFlash 切片）/ test_mtk_flash_plan（扩 XML 方言）/ test_mtk_payload（扩路由）
```

**共用与边界**
- 三条链**共用**：`BromSession`（BROM 级：握手/`getTargetConfig`/`getHwCode`/`sendDa`/`jumpDa`/`readExact`）、DA 解析与选择（D1）、`mtkplan`（计划层）、preloader 三路径（D1）、通道与 UI（D1）。
- 三条链**各自的**：握手序列、EMI 帧、DA2 提交方式（LEGACY `boot_to` 保留签名 / XFlash `BOOT_TO` 剥签名 / XML `CMD:DOWNLOAD-FILE`）、分区表来源（LEGACY=PMT；XFlash=GPT(0x64) 或 PMT(0x65)；XML=GPT）。

## 4. 代际路由（唯一裁决点）

`damode`（芯片表）→ 三条链；`v6`（DA 文件）→ 覆盖为 XML（`DC:216`）。表外芯片/`damode` 缺失 → 明确报错（D1 已有）。
**路由实现位置**：`bromFlashOnSession` 内、DA 选择之后（与 D1 的 `iot`/`damode` 拒绝同一处），三条链各自一个 `xxxBringUp(...)` + 各自的分区表读取 + 各自的写入函数；计划层与 UI 完全复用。
**`iot` 位**：D1 已拒绝（IoT 在 LEGACY 走另一套 region 映射）；XFlash/XML 代不存在 IoT 条目（芯片表 `iot` 全在 LEGACY 侧，实测 89 条中 `iot=true` 20 条均为 LEGACY）。

## 5. XFlash 执行流

```
1. 枚举 → 打开 → connect → getTargetConfig（SLA/DAA 明确报错，沿用 D1）
2. getHwCode/getHwSwVer → 芯片表 → damode == XFlash（否则另两条链）
3. parseDaFile + selectDaEntry（dacode 键，沿用 D1）→ 拒绝 v6 文件（那是 XML 代）
4. SEND_DA(region[1]) → JUMP_DA → 读 1B == 0xC0
5. sync() → setup_env() → setup_hw_init() → 读回 == "SYNC"(0x434E5953)
6. set_checksum_level / set_reset_key（次序按 XFL:1103-1107）
7. GET_CONNECTION_AGENT：== "preloader" → 跳过 EMI；== "brom" → 需要 EMI（缺 EMI 只 warning 不中止）；其它 → 失败
8. GET_CHIP_ID / GET_PACKET_LENGTH（write 无回退，拿不到即报错）
9. boot_to(region[2].m_start_addr, region[2] 去 m_sig_len) → sleep(0.5) → status ∈ {0, "SYNC"}
10. GET_PARTITION_TBL_CATA：0x64 → READ_DATA 读 LBA0..N → mtk_gpt 解析；0x65 → 沿用 LEGACY 的 PMT 读取
11. 计划（mtkplan，参照表 = GPT/PMT 实体表）→ 逐分区 WRITE_DATA（48B 参数 + send_param 分块，按 write_packet_length）
12. SHUTDOWN(32B 参数) 收尾；失败只告警（数据已落盘）
```
`0x6781` 特例（16B 合并写）在 `xsend` 层实现（按 hwcode 分支）。

## 6. XML 执行流（D3）

```
1..3 同上（damode == Xml 或 DA 文件 v6）
4. SEND_DA(region[1]) → JUMP_DA → get_command_result() 期待 CMD:START
5. setup_env() → setup_hw_init() → setup_host_info()
6. SET-RUNTIME-PARAMETER（checksum_level = NONE，与上游默认一致）
7. 分区表（**已核实，不再是待定项**）：`CMD:READ-FLASH`（`<partition>EMMC-USER</partition>` + `<offset>0x0` + `<length>0x100000`，`xml_cmd.py:474-483`）→ 回包 `UpFile` → 数据由 `download_raw` 按 packet 收（`XL:918-941`）→ **复用同一个 `mtk_gpt`**；上游同款口径见 `mtk_daloader.py:296-302`（`partition_table_category()=="GPT"` → 共用 `partition.get_gpt`）
8. 逐分区写：`CMD:WRITE-FLASH`（`xml_cmd.py:452-461`，`parttype` 用**字符串** `"EMMC-USER"`）→ 回包 `DwnFile` 给 `packet_length` → `OK@0x<len>` → 每包 `ack(0)`+原始帧块+`OK` → `CMD:END` → `CMD:START`（与读取路径同一收尾握手）
9. 收尾：CMD:END + CMD:START（与写入路径同一握手）
```
**XML 的分区表读取路径**、`UFSPartitionType` 的文本表示（`"EMMC-USER"` 一类字符串）、`max_address_length=9` 的真实语义 —— 三条为**实现时须对照上游核实的点**（facts §5.3/§5.6），核实结论写进代码注释与交付文档。

## 7. GPT 解析（`mtk_gpt`，纯函数）

- 输入：LBA0 起的原始字节 + 扇区大小（调用方按 storage 类型给默认，512/4096 两档探测兜底）。
- 步骤：LBA0 嗅探（`55AA`@0x1FE）→ LBA1 头（`EFI PART`）→ **CRC 校验（头部：crc 字段清零后算；条目表：整表算）** → 条目解析（名字 UTF-16LE、`first_lba`/`last_lba`）→ **主 GPT 不可用（签名/CRC/revision 不符）→ 读磁盘末端的备份 GPT**（`alternate_lba`）。
- 产出：`GptPartition{name, firstLba, lastLba, sizeBytes, offsetBytes, guid}` + `GptTable{sectorSize, entries, usedBackup}`。
- **fail-closed**：头部/条目 CRC 不符 → 明确报错"分区表不可信"（不复刻上游的"解析但不校验"）；`revision != 0x10000` → 明确报错。
- 字节偏移换算：`offsetBytes = firstLba * sectorSize`；写盘用 GPT 的**字节区间**（与 D1 的 PMT `EmPartition` 对齐成同一上层类型）。

## 8. EMI 两代（`mtk_preloader_emi` 扩展）

| 代 | 取法 | 发送 |
|---|---|---|
| LEGACY（D1 已实现） | `MTK_BIN+0xC` 起（实测 800 B） | `0xE8` + emiver 分档 + `M_EXT_RAM_*` |
| XFlash（本阶段） | **整块**（实测 912 B） | `INIT_EXT_RAM` + 4B 长度 + blob（0x200 分块；无 emiver/无 checksum/无回包链） |

两条共用"从 preloader 定位标记"的前半段（`MMM` 分支与版本解析，D1 已实现）；**切片尾部不同**。真样本回归：832 个 preloader **两代各跑一遍**，与上游两个函数的逐字 Python 移植对拍。

## 9. 验证策略与诚实边界

| 层 | 内容 |
|---|---|
| **真实 GPT 样本** | `reference/mtk-samples/PGPT.img`/`SGPT.img`（**4096 字节扇区**，来源 `https://github.com/ReCoreShift/mtk-gpt-tool` `tests/fixtures/`，MPL-2.0 仓库，**非我们自己的设备**；sha256 `1124d035…`/`45495fd5…`）：硬断言 61 条目、`EFI PART`@4096、**头部 CRC（字段清零后）= 0x25c45852**、**条目 CRC = 0xac89a396**、前几条分区名（`misc`/`para`/`expdb`/`frp`/`nvcfg`/`nvdata`）；与 `sgdisk_print.txt` 独立对拍 |
| **真实 scatter（新方言）** | `MT6789_Android_scatter.xml`（同源，真样本）：**130 个 `<partition_index>` 块 = EMMC 与 UFS 两份完整副本**（同一个 `SYS0` 标签，一份 `<region>EMMC_BOOT1</region>/<storage>HW_STORAGE_EMMC</storage>`、一份 `<region>UFS_LU0</region>/<storage>HW_STORAGE_UFS</storage>`）→ **解析必须按 storage 过滤**（每份 65 个分区：`preloader`/`preloader_backup`/`pgpt`/`misc`/`para`/`expdb`…），不过滤会让每个分区翻倍且大小对不上；解析结果与 GPT 条目交叉对照 |
| **真 DA 样本** | V5/V6：`region[1]`=DA1、`region[2]` 地址逐条目（`0x40000000`）、**剥签名**（XFlash）与**保留签名**（LEGACY）双向断言 |
| **832 preloader** | 两代 EMI 切片与上游逐字移植对拍（沿用 D1 的方法） |
| **mock 逐帧** | 12B 帧与 `datatype`、0x200 分块边界、**0x6781 的 16B 合并写**、`WRITE_DATA` 48B 参数、`boot_to` 16B+数据+sleep+status 判据、`SHUTDOWN` 32B、XML 文本握手序列（`OK`/`OK@0x…`/`OK!EOT`/`CMD:END`+`CMD:START`） |
| **合成边界** | GPT：坏 CRC、垃圾签名、`revision` 不符、备份 GPT 兜底、条目数 0/65、512↔4096；XML：`ERR!` 分支、`packet_length` 分块、hex() 地址形态 |
| **对抗变异** | 每个关键判据至少一组"改坏实现 → 必须有用例红"（沿用 D1 的做法） |

**诚实边界（写进交付物）**
- **真机全链未验证**（三代皆是）；枚举/打开/握手/EMI/DA2/分区表/写入/收尾全段只有代码级保证。
- **XML 代无真实设备样本**：帧格式与命令序列全部来自上游代码；`UFSPartitionType` 文本表示、`max_address_length` 语义两点**已于计划期结案**（**XML 的分区表读取路径已在设计期核实**：`CMD:READ-FLASH` + 同一个 GPT 解析器）：
  - `UFSPartitionType` 在 XML 下是**字符串** `"EMMC-USER"` 一类（`ST:216` 起、`XC:452-461`）；整数枚举（BOOT1=1/BOOT2=2/USER=3/RPMB=4）只用于 XFlash。**已核实**（事实报告 §5.3/§6）。
  - `max_address_length = 9`（`XP:2`）：**全仓库只此一处定义、零 import、零消费点**（`grep -rn max_address_length` 只命中定义行）→ 与 plcap/blver 同类的**死常量**，本实现**不实现、不猜语义**。
    （交付期更正：原写"只被 import 无消费点（`USBLIB:21`、`seriallib.py:9`）"——那两处 import 的是**另一个常量** `max_xml_data_length`；结论不变，论据见 `mtk-xflash-facts.md` §14。）
  - 非真机可证的部分（`0x6781` 的 16 字节 ack、DRAM 时序）只按上游实现，边界如实披露。
- **GPT 样本来自第三方仓库**（非我们自己的设备，MPL-2.0 仓库的测试夹具）；`reference/` gitignored，不随仓库分发。
- **mock 看不见"读了多少字节"**（D1 已记录）：读侧判据靠"逐条对上游读长度"+ probe。
- **"0 warning" 只是编译器默认档**（`CMakeLists.txt` 未开 `-Wall/-Wextra`）。
- `0x6781` 的 16B 合并写**仅按上游代码实现**（无该芯片样本）。

## 10. 明确不做（本阶段）

- `DOWNLOAD`/`UPLOAD`/`FORMAT_PARTITION`（上游零调用，布局无从得知）；`FORMAT`/格式化入口；`SET_*`/`CTRL_*` 的大部分；`NAND_BMT_REMARK`。
- SLA/DAA（BROM 级与 DA 级，含 XML 的 `CMD:SECURITY-SET-FLASH-POLICY`）→ **明确报错**。
- UFS 专属命令族、RPMB、seccfg 解锁、NV 项。
- `plcap`/`blver` 的"设备回报提升到 XFLASH"分支（上游死代码，只注释）。
- DA 二进制提取/生成、签名绕过、BROM 漏洞利用。

## 11. 交付清单

- [x] `mtk_xflash_session.{h,cpp}` + `test_mtk_xflash_session`
- [x] `mtk_xflash_payload.{h,cpp}` + `test_mtk_xflash_payload`（含 0x6781 特例、EMI、boot_to、WRITE/READ_DATA、SHUTDOWN、只读查询）
- [x] `mtk_gpt.{h,cpp}` + `test_mtk_gpt`（真样本 + 合成边界 + CRC fail-closed + 备份兜底）
- [x] `mtk_xml_session.{h,cpp}` + `mtk_xml_payload.{h,cpp}` + 两个测试目标
- [x] `mtk_preloader_emi` 扩 `extractEmiXflash` + 832 样本两代对拍
- [x] `mtk_flash_plan` 扩 scatter XML 方言 + `test_mtk_flash_plan` 扩
- [x] `bromFlashOnSession` 三代路由 + `test_mtk_payload` 扩（XFlash/XML 两条链的 bring-up 与失败边界）
- [x] 通道/UI 文案（说明走了哪条链）+ 无需新入口
- [x] 文档：功能清单、README、事实报告归档（`docs/superpowers/specs/mtk-xflash-facts.md`）、D1 spec 的"不做"项回收

## 12. 许可

- `mtkclient`（GPL-3.0）**只读参照**：只取事实与 `file:line`，代码文本不进仓库；沿用 D1 的芯片表转写纪律（生成脚本 + 来源 URL + commit）。
- **新样本**（PGPT/SGPT/sgdisk/scatter.xml）来自 `ReCoreShift/mtk-gpt-tool`（**MPL-2.0**）：只作 `reference/` 下的**只读离线样本**（gitignored），不随仓库分发、不进构建；来源 URL、sha256、对拍基准写入事实报告。
