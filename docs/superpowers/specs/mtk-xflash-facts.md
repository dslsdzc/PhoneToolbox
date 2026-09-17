# MTK BROM XFlash / XML 两代 — 协议事实报告（逐条出处）

日期：2026-09-17（Phase D2+D3 实施期：2026-09-16 起）
范围：**只给事实**（路径 / 行号 / 字节与顺序结论），不含实现方案；"项目自有决策"与上游行为**分开列**（§13）。
上游：`bkerler/mtkclient` 子模块，HEAD `71b0175`（"Add more fixes and ssr stuff"）。许可 **GPL-3.0** ——
本文只记录事实与行号，**不转载其代码文本**；引用的常量 / 字节串属协议事实。
本仓落点：`src/core/modes/mtk_xflash_session.{h,cpp}`、`mtk_xflash_payload.{h,cpp}`、`mtk_gpt.{h,cpp}`、
`mtk_xml_session.{h,cpp}`、`mtk_xml_payload.{h,cpp}`、`mtk_preloader_emi.{h,cpp}`、`mtk_payload.{h,cpp}`、
`src/core/mtk_flash_plan.{h,cpp}`（下称"本层"）。LEGACY 代事实见 `mtk-brom-facts.md`（D1 交付）。
配套底稿：`.superpowers/sdd/mtk-d2d3-facts-report.md`（736 行，同一事实体系）。
**本文行号在写本文时逐条 `sed`/`grep` 复核过**（复核命令见 §15）；与底稿不一致处以本文为准（§14 记了两处更正）。

**路径简写**（全部相对 `mtkclient/mtkclient/`；`mtk.py`/`mtk_gui.py` 在 `mtkclient/` 根）：

- **XFL** = `Library/DA/xflash/xflash_lib.py`（1235 行）
- **XFP** = `Library/DA/xflash/xflash_param.py`
- **XFFP** = `Library/DA/xflash/xflash_flash_param.py`
- **DLL** = `Library/DA/legacy/dalegacy_lib.py`
- **XL** = `Library/DA/xmlflash/xml_lib.py`（1079 行）
- **XC** = `Library/DA/xmlflash/xml_cmd.py`（624 行）
- **XP** = `Library/DA/xmlflash/xml_param.py`
- **DC** = `Library/DA/daconfig.py`
- **DL** = `Library/DA/mtk_daloader.py`
- **PL** = `Library/mtk_preloader.py`
- **BC** = `config/brom_config.py`
- **ST** = `Library/DA/storage.py`
- **GPT** = `Library/Partitions/gpt.py`；**PART** = `Library/partition.py`

---

## 1. 三代握手差异（DA1 上传之后）

**共用的前半段**：BROM 级 `SEND_DA`(0xD7) → `JUMP_DA`(0xD5)，DA1 = **`region[1]`**（XFL:963-973、DLL:563-568、
XL:271-278）。DA2 = `region[2]`，其加载地址**逐条目从 DA 文件读**（现代多为 `0x40000000`，老平台 `0x80000000`）。

| 代 | DA1 起来后的同步信号 | 之后的步骤 |
|---|---|---|
| **LEGACY** | 读 **1 字节**必须 == `0xC0`（DLL:596-601） | 无 SYNC / SETUP_ENVIRONMENT / SETUP_HW_INIT_PARAMS，直接进存储信息交换（DLL:607-634） |
| **XFLASH** | 读 1 字节必须 == `0xC0`（XFL:982-985） | **七步**（XFL:979-995）：`sync()` → `setup_env()` → `setup_hw_init()` → 读回 `0x434E5953` |
| **XML** | 等设备发 **`CMD:START` 文本 XML 消息**（XL:309-313） | `setup_env()` → `setup_hw_init()` → `setup_host_info()`；**没有** sync 命令、没有 `0x434E5953` |

**XFlash 七步的帧级细节**（XFL:979-995）：

| 步 | 动作 | 出处 |
|---|---|---|
| 1 | 读 1B 必须 `0xC0`（否则 `"Error on DA sync"`） | XFL:982-985 |
| 2 | `sync()` = `xsend(0x434E5953)`，**只发不读** | XFL:903-907 |
| 3 | `setup_env()` = `xsend(0x010100)` + 20B 参数 | XFL:909-924 |
| 4 | `setup_hw_init()` = `xsend(0x010101)` + 4B `0x0` | XFL:926-932 |
| 5 | `xread()` 必须 == 4 字节 `0x434E5953`（`"Successfully received DA sync"`） | XFL:991-994 |

- 常量：`MAGIC = 0xFEEEEEEF`（XFP:2）、`SYNC_SIGNAL = 0x434E5953`（XFP:3）、`SETUP_ENVIRONMENT = 0x010100` /
  `SETUP_HW_INIT_PARAMS = 0x010101`（XFP:22-23）。
- `SETUP_ENVIRONMENT` 载荷 = 5 × u32（20B）：`da_log_level` / `log_channel`（UART=1、USB=2、BOTH=3）/
  `system_os`（`OS_LINUX` = 1）/ `ufs_provision` = 0 / 0（XFL:913-924、XFP:83-85）。
- `SETUP_HW_INIT_PARAMS` 载荷 = 单个 u32 `0x0`（无配置，XFL:929）。
- **整段只读 3 帧**：两次 `send_param` 各读一次 status（XFL:177-187）+ 最后的 SYNC 回包。多读一帧会把
  HW_INIT 的 status 当成 SYNC 回包。
- **上游对两个 setup 的返回值不检查**（XFL:989-990 连续两行裸调用，失败也继续走 5）。

**「设备不回 `0xC0` / XML 不发 `CMD:START`」的区别是两代互斥的信号**：同一台设备只会给其中一种。

## 2. 12B 帧、`status()` 三态与 `ack()` 的 `0x6781` 特例

**帧头**（XFL:112，XML 侧 XL:150 同构）：`pack("<III", magic, datatype, length)` —— **全小端**。
字段：`magic` = `0xFEEEEEEF`（XFP:2）；`datatype`：`1` = `DT_PROTOCOL_FLOW`、`2` = `DT_MESSAGE`（XFP:88-90）；
`length` = 载荷字节数。

- 写入方式：**帧头一次 `usbwrite`，载荷第二次**（XFL:113-114）；int 载荷 `is64bit=False` → `pack("<I")`（4B）、
  `True` → `pack("<Q")`（8B）（XFL:102-110）。
- XML 的文本载荷：`str` 的 `length = len(data) + 1`，实写 `utf-8 + b"\x00"`（**带 NUL 结尾**，XL:146-153）。
  接收侧只接受 `length` 为 **12 或 16** 的头；16 = `DT_MESSAGE`（多一个 `priority:u32`），载荷是 DA 日志行
  （XL:112-135）。主机**从不**发 16B 头。

**`status()` 的三态判据**（XFL:138-158）：

1. 读 12B 头；`magic != 0xFEEEEEEF` → 返回 `-1`（`"Status error: Wrong magic"`）。
2. 读 `length` 字节；**实读 < length** → 返回 `-1`。
3. 按 `length` 解析：
   - `length == 2` → `<H`；**`== 0` 才算成功**（返回 0），否则返回该 u16；
   - `length == 4` → `<I`；**值 == `0xFEEEEEEF` 视为成功**（返回 0），否则返回该 u32；
   - 其它 → 取载荷**第一个** u32 返回。
4. 所有非 0 值都是错误码，经 `Library/error.py` 查表。

**`ack()` 的 `0x6781` 特例**（XFL:85-100）—— 旧规格文档写的"4 字节 magic 短帧"**在代码里不存在**：

- **`dacode == 0x6781`**：**一次**写 16 字节 `pack("<IIII", MAGIC, DT_PROTOCOL_FLOW, 4, 0)`
  （= 12B 头 + 4B 载荷 0 **合并成一次 USB 写**）。
- **其它芯片**：**两次**写（12B 头 + 4B `0`）；代码注释点名 `0x6750, 0x6762, 0x6785, 0x6761, 0x6771`。

**数据读取路径不用 `status()`**：`readflash`（XFL:706-806）自己读 12B 头后按 `slength` 处理 ——
`slength > 4` = 数据块（收下 + `ack(rstatus=False)`，只写 ack 不读 status）；`slength == 4` = flag
（**非 0 即报错**）；其它长度 = 协议错误。循环以**字节数**收尾（不是"见到 flag 就停"），收尾再读一帧。

## 3. `send_param`：0x200 分块与 `0xC0040050`

`XFL:163-188`（XFlash 所有带参命令共用）：

1. 每个参数是**独立的 12B 帧**（帧头一次写，XFL:165-167）；
2. 载荷按 `min(剩余, 0x200)` 循环写（XFL:171-176）；
3. 全部写完后读**一次** `status()`（XFL:177-179）：`0` → 成功。
4. **特例 `status == 0xC0040050`**（= `"EMI setting version error"`，`error.py:700`）：
   **不打印错误、不 `sys.exit`，但仍然 `return False`**（XFL:180-187）。
5. `0xC0020053`（anti-rollback）与 `0xC0020004`（DL forbidden）→ `sys.exit(1)`（XFL:182-186）。

**调用点决定后果（两种路径不同）**：

- **显式 preloader**：`if not self.send_emi(...): return False`（XFL:1147-1149）→ **EMI 送失败即整链中止**。
- **自动搜索**：命中候选后失败只 `continue` 试下一个（XFL:1131-1138）。

事实报告初稿曾把 `0xC0040050` 写成"可容忍"，**是误读**；T2 实施期已更正（本层：`checkStatus` 对
`0xC0040050` 返回 false）。

## 4. XFlash EMI（`INIT_EXT_RAM`）

`XFL:251-270`：

1. `xsend(INIT_EXT_RAM = 0x01000A)`；2. `status()` **必须 == 0**；3. `sleep(0.01)`；
4. `xsend(pack("<I", len(emi)))` —— **长度单独一帧**；5. `send_param([emi])` —— EMI 本体按 §3 的 0x200 分块；
6. 成功打印 `"DRAM setup passed."`，异常提示加 `--preloader`。

- **EMI 数据没有地址字段**（地址由 DA 内部约定），**`emiver` 在 XFlash 路径完全不用**
  （全局 grep `emiver` 只命中 `DC` 与 `DLL`，`XFL` 零命中）。
- **跳过 EMI 的唯一条件**：`GET_CONNECTION_AGENT`(0x04000A) 返回 `b"preloader"`（XFL:1107-1151）；
  返回 `b"brom"` 且没有 EMI → **只 warning 不中止**（XFL:1143-1144）；其它值 → 返回 False。
- **EMI 切片两代不同（实测）**：XFLASH = **整块 912 B**（`MTK_BLOADER_INFO_v` 头一起发）；
  LEGACY = `MTK_BIN + 0xC` 起 **800 B**（DC:137-144；D1 事实报告 §8 与 `mtk-brom-facts.md`）。

## 5. `boot_to`（DA2）与"剥签名"

- 调用点：`XFL:1164` 传 `region[2].m_start_addr` 与 `self.daconfig.da2`；而 `da2` 在 **XFL:978**
  被设为 `da2[:-da2sig_len]`（`region[2].m_len` 读到后**剥掉尾部 `m_sig_len`**）。
- `boot_to` 本体（XFL:288-328）：`BOOT_TO = 0x010008` → status 0 → `pack("<QQ", addr, len(da))` = **16B 独立帧**
  → `send_data(da)`（帧头 + 按 `wMaxPacketSize` 分块，**自带一次 status**，XFL:272-286）
  → `sleep(timeout)`（默认 **0.5 s**）→ 再读 status，**∈ {`0x434E5953`, `0x0`} 才算成功**（XFL:315）。
- **同一 DA 文件三代送出的字节范围不同**：LEGACY **保留**尾部签名（DLL:591/593 的非 patch 路径，不裁
  `m_sig_len`），XFlash 剥（XFL:978），XML 也剥（XL:304）。
- **成功 ≠ DA2 在跑**：上游随后靠 `reinit` 的查询组合（`GET_RAM_INFO`/`GET_CHIP_ID`/`GET_DA_VERSION` 等，
  XFL:1002-1035 / :1174）能应答才判"DA2 存活"（详见配套报告 §7.4）。
- `region[0]` 在 V6 文件里与 `region[1]` 逐字节相同（实测，配套报告 §7.2）；DA1/DA2 只信任 `region[1]`/`region[2]`。

## 6. 写 / 读数据流（XFlash）

**56B 存储参数**（XFL:677-680）：`pack("<IIQQ", storage, parttype, addr, length)` = 24B +
`NandExtension` 的 **8 个** u32 = 32B（全 0）→ **共 56 字节**。
⚠️ 计划与事实报告初稿写的"48B"是**算错**（少 8 字节；设备会把后 8 字节读成垃圾）。`NandExtension`
类有 9 个属性，打包时**跳过** `operation_type`（字段名见 XFFP 全文）。

**写（`WRITE_DATA = 0x010004`）**：`XFL:670-685`（命令层）+ `XFL:852-899`（数据流）：
命令 → status 0 → 56B 参数 → 循环：`dsize = min(write_packet_length, 剩余)` → 补零到 512 的整数倍 →
`checksum = sum(data) & 0xFFFF` → `send_param([<I 0>, <I checksum>, data])`（**一次 status**）→
**循环之后再读一次 status**（XFL:883-893，非 0 即失败）→ 成功才发 `CC_OPTIONAL_DOWNLOAD_ACT`（XFL:885；
上游不检查其返回值）。

- `write_packet_length` 来自 `GET_PACKET_LENGTH`(0x040007) 回包 `<II` 的第一个字段，**无回退**（XFL:858）；
  `read_packet_length` 有 1 MB 回退（XFL:724）。
- **checksum 档位**（`PLAIN=0`/`CRC32=1`/`MD5=2`，XFP:77-80）上游**恒设 0x0**（XFL:1106）→ 实际用的是
  16 位加法和；`ChecksumAlgorithm` 类全仓只被实例化一次、字段无引用。

**读（`READ_DATA = 0x010005`）**：`XFL:687-704`（命令层）+ `XFL:706-806`（数据流）：
命令 → status 0 → 56B 参数 → **参数帧之后再读一个 status**（XFL:698-702）→ 逐帧收数（§2 的
`readflash` 规则：数据帧 / flag 帧 / 收尾帧）。

## 7. XML 协议：信封 / 应答 / 数据流 / 无 EMI

- **传输**：同样的 12B 帧头（XL:150），`magic` 同为 `0xFEEEEEEF`（XC:15）；载荷是 UTF-8 文本 + NUL（§2）。
- **信封**（XC:18-27）：`<?xml version="1.0" encoding="utf-8"?><da><version>{v}</version>` +
  `<command>CMD:{NAME}</command>` + 各参数字段 + `</da>`。版本默认 `"1.0"`，**只有 `SET-RUNTIME-PARAMETER`
  用 `"1.1"`**（XL:184）；**版本协商不存在**（主机从不解析回包的 version）。根元素：主机 → `<da>`，DA → `<host>`。
- **应答**：`OK`（XL:158-159）；错误文本含 `ERR!`（XL:212-219，`ERR!UNSUPPORTED` 单独特判）；
  带长度的 `OK@0x<hexlen>\0`（XL:161-162）；`CMD:PROGRESS-REPORT` 是保活循环，直到哨兵 **`OK!EOT`**（XL:390-404）。
- **完成永远是两步**：`CMD:END`(OK) → `CMD:START`（XL:195-209）。
- **批量数据不在 XML 里内联**：命令只带 `MEM://0x<addr>:0x<len>` 描述符，字节流走独立帧块。
  写 = `CMD:DOWNLOAD-FILE` 回包给 `packet_length` → 逐包 ack（XL:419-424 / :451-506）；
  读 = `CMD:UPLOAD-FILE` → `download_raw` **逐帧 ack**（XL:425-431 / :508-559）。
- **地址文本 = Python `hex()`**：小写、`0x` 前缀、**无零填充**（XC:457 的 `<offset>{hex(offset)}</offset>`、
  XC:479 的 `<length>{hex(length)}</length>`），解析一律 `int(x, 16)`。
- **`SET-RUNTIME-PARAMETER`**（XL:167-186 发送 + XC:100-135 构造）：`checksum_level="NONE"`、
  `battery_exist="AUTO-DETECT"`、`da_log_level` 是**字符串**（TRACE/DEBUG/INFO/WARN/ERROR）、
  `log_channel="UART"`、`system_os="LINUX"`，外加**独立 `<adv>` 块的 `<initialize_dram>YES|NO</initialize_dram>`**。
  → **这就是 XML 的 DRAM 初始化**：XML 侧**没有 EMI blob、没有 `INIT_EXT_RAM`、没有地址/长度帧**。
  （XC:101-115 有一段未被使用的死 f-string，真正的构造器在 XC:116-135。）
- **`setup_hw_init`**（XL:323-327）= `HOST-SUPPORTED-COMMANDS`（`<host_capability>` 能力串，
  默认 `CMD:DOWNLOAD-FILE^1@CMD:FILE-SYS-OPERATION^1@CMD:PROGRESS-REPORT^1@CMD:UPLOAD-FILE^1@`，XC:139-140）
  + `NOTIFY-INIT-HW`（**上游传 `content=None` → 不写 `<arg>`**，XC:32-42）。另有 DA1 阶段一次的
  `SET-HOST-INFO`（`<info>%Y%m%dT%H%M%S</info>`，XL:329-331、XC:600-612）。
- **上游对三步的返回值一概不检查**（XL:311-313 三行裸调用，紧接着 `return True`，XL:314）。
- **checksum 字段被解析但从不校验**（XL:452-453/510-512/562-565），策略由 `checksum_level` 一次性选定。
- XML 侧**不存在**的命令（全仓 grep）：`GET-PACKET-LENGTH`、`WRITE-DATA`、`READ-DATA`、`SETUP-ENVIRONMENT`、
  `SETUP-HW-INIT-PARAMS`、`INIT-EXT-RAM`；等价物是 `READ-PARTITION-TABLE`（分区表）、`GET-HW-INFO`、
  `COMMAND:HOST-SUPPORTED-COMMANDS`+`NOTIFY-INIT-HW`；`SHUTDOWN` 由 `CMD:REBOOT`（XC:439）取代。

## 8. GPT 路径：布局 / 双 CRC / 备份窗口 / 真样本实测

**头部布局**（GPT:42-57，小端）：`signature`(8B) / `revision`(u32) / `header_size`(u32) / **`crc32`(u32)** /
`reserved` / `current_lba`(u64) / `backup_lba`(u64) / `first_usable_lba`(u64) / `last_usable_lba`(u64) /
`disk_guid`(16B) / `part_entry_start_lba`(u64) / `num_part_entries`(u32) / `part_entry_size`(u32)。

**条目布局**（GPT:59-67）：`type GUID`(16B) → `unique GUID`(16B) → `first_lba`(u64) → `last_lba`(u64) →
`flags`(u64) → 名字（`ustring`，缓冲上限 **72 B = 36 个 UTF-16 码元**）。

- **扇区探测**：**512 → 4096** 的循环在 `guid_gpt.parse()` 内部（GPT:218-224：`for sectorsize in [0x200, 0x1000]`，
  签名 `== b"EFI PART"` 即进 `parse_gpt`）；而**首次读头**用的是 `config.pagesize`（PART:41 强制赋值），
  eMMC 恒 512（缺陷③，见 §9）。
- **版本闸门**：`revision != 0x10000` 直接拒（GPT:165-167）。
- **条目区起点**：`part_entry_start_lba != 0` 时按**字节偏移**用（不乘扇区），否则用头部字段 × 扇区
  （GPT:168-171）—— 两个分支量纲不同，是缺陷①的一部分。
- **空条目判据**：上游用 **unique GUID** 全 0 且**遇空即 `break`**（GPT:192-193）→ 表中间有空槽时会漏掉
  其后的分区；本层按 UEFI 规范用 **type GUID** 且逐条 `continue`。
- **备份 GPT**：备份头在**磁盘最后一扇区**，条目区在窗口开头（头里的 `part_entry_start_lba` 指向窗口起始
  LBA）；本层的备份窗口 = 从末扇区取 `part_entry_start_lba` 反推的若干扇区。上游的备份兜底**实际不生效**
  （缺陷②，见 §9）。

**真样本实测**（`reference/mtk-samples/`，**4096 字节扇区**，独立 Python `struct`/`zlib` 复核）：

| 事实 | `PGPT.img`（主） | `SGPT.img`（备） |
|---|---|---|
| 大小 / 布局 | 32768 B = 8 × 4096 | 32768 B = 8 × 4096 |
| `EFI PART` 位置 | **仅 @4096**（512 处全零，无第二处） | **仅 @28672**（窗口**最后**一扇区） |
| 头部关键字段 | `current_lba=1`、`backup_lba=124960767`、`part_entry_start_lba=2` | `current_lba=124960767`、`backup_lba=1`、`part_entry_start_lba=124960760`（= 窗口起始 LBA） |
| 条目表 | 条目 @8192，`n=61`、`entry_size=128`，61 条**全非空** | 按窗口起点换算 `entriesBase=0` |
| CRC | 头部 CRC（**crc 字段清零后**）= `0x25C45852`、条目表 CRC = `0xAC89A396`，**均通过** | 头部 / 条目表 CRC 均通过 |
| 分区名 | 前三条 `misc`(LBA 8..135) / `para`(136..263) / `expdb`；末条 `flashinfo` | 同主表 |
| 备注 | 备份表与主表**逐字节相同** | — |

**独立对拍基准的诚实说明**：同目录 `sgdisk_print.txt` **不是本 `PGPT.img` 的分区表** ——
它是同一机型**另一版布局**（65 条，含 `shared`/`arch`/`void`/`backup`，且 `gz_a`/`super`/`userdata`
的 LBA 与样本不符）→ 只能当"分区名集合"的旁证，**不能**当逐条对拍基准。
`preloader` **不在 GPT 里**（GPT 之外是裸区），只出现在 `MT6789_Android_scatter.xml`。

## 9. 上游 GPT 的 4 处已知缺陷（本层不复刻）

| # | 缺陷 | 出处 |
|---|---|---|
| ① | `--gpt-num-part-entries` / `--gpt-part-entry-size` 解析后**对解析完全无效**；`--gpt-part-entry-start-lba` 有效但被当**字节偏移**而非 LBA；`--sectorsize` 定义后**无任何读取点** | GPT:168-171、`mtk.py:98` |
| ② | **备份 GPT 兜底实际不生效**：PART:44-45 的 `seek(flashsize - 0x4000)` 被随后 `parseheader()` 内部的**绝对** `seek(sectorsize)` 覆盖（GPT:160-162）→ GPT 不在 LBA1 的设备拿不到分区表 | PART:44-45、GPT:160-162 |
| ③ | **eMMC 恒用 512B 扇区**：`emmc.block_size` 在 XFL:448 被读出但**从未赋值给 `config.pagesize`**（PART:41 用的是 `config.pagesize`） | XFL:448、PART:41 |
| ④ | **CRC32 被解析但从不校验**：`self.crc32 = sh.dword()`（GPT:48）是全包唯一出现处，无 `zlib.crc32`、无比较 | GPT:48 |

另记两条同类问题（本层同样不复刻）：**PMT 路径不可达** —— `DL.partition_table_category()` **无条件返回
`"GPT"`**（DL:390-393），且 PART:80-81 把 `int.from_bytes(...)` 与 **bytes** 字面量比较 → `get_pmt` 永远返回空。
**`XFL:612-621` 的 `GET_PARTITION_TBL_CATA`** 回包 `0x64` = GPT、`0x65` = PMT，其它值返回 0（上游）。

## 10. 代际判定：三方投票与 `plcap` 死代码

唯一裁决点 `DL:229-241`（`set_da()`），优先级自上而下：

1. 默认 `LEGACY`（`DAmodes.LEGACY = 3` / `XFLASH = 5` / `XML = 6`，BC:1-4）。
2. `config.plcap` 非 None 且 **bit0（XFLASH 支持位）置位**且 `blver > 1` → `XFLASH`（DL:231-235）。
3. `chipconfig.damode == XFLASH` → `XFLASH`；`damode == XML` **或** `da_loader.v6` → `XML`（DL:236-239）。
4. 缓存覆盖：`reinit()` 读 `<hwparam_path>/.state` 的 `flashmode` 字符串（`"LEGACY"`/`"XFLASH"`/`"XML"`）
   直接设定 `damode` 与 `flashmode`（DL:158-176）。

**设备不上报 `damode`**：全仓 grep 无任何协议命令返回它；最接近的两个回报是 `GET_PL_CAP`（能力位图）
与 `GET_CONNECTION_AGENT`（**连接来源**，非协议代）。

**`plcap`/`blver` 是死代码**（本阶段不实现该分支）：

- `config.plcap` 唯一写入点 `get_plcap()`（PL:923-926）**全仓零调用** → 初值 `None`（`mtk_config.py:53`）
  → 第 2 条**永不触发**。
- `config.blver` 初值 **-2**（`mtk_config.py:54`），由 `get_blver()`（PL:664-672，BROM 阶段调用）写入；
  但第 2 条还要求 `plcap` 非 None，故整体不可达。

**v6 只推向 XML**：`v6 = b"MTK_DA_v6" in hdr`（`hdr` = DA 文件前 `0x68` 字节，DC:173，实测命中偏移 0x20）；
`DC:216` 据此把 `chipconfig.damode` **改写为 XML**。

**XML 名单**（`BC` 内 `damode=XML` 的静态条目）= 13 条 4nm/3nm 世代（MT6983/MT6855/MT6895/MT6897/
**MT6789**/MT6835/MT6886/MT6989W/MT6985/MT6878/MT6899/MT6991/MT6993）。真样本 `MT6789`（`BC:1970` 起，
`dacode=0x1208`）即其中之一。

## 11. 真样本清单（本阶段新增；全部 gitignored、只读离线）

来源：<https://github.com/ReCoreShift/mtk-gpt-tool>（**MPL-2.0**）的 `tests/fixtures/`，落
`reference/mtk-samples/`（**不进提交、不进构建**）。sha256 为本地实测。

| 文件 | 大小 | sha256 | 实测值 |
|---|---|---|---|
| `PGPT.img` | 32768 B | `1124d035ba406c906af60152e6809001c5fc5236d7279bf00668242b06d0302b` | 4096B 扇区主 GPT：EFI PART@4096、n=61、双 CRC 通过（§8） |
| `SGPT.img` | 32768 B | `45495fd5a8f39d725bfb56f6430bc1eb2200190d79afefee890a9d3037e8355f` | 备份 GPT：EFI PART@28672、窗口布局（§8） |
| `sgdisk_print.txt` | 4739 B | `16f25ac757ec632e05a24ba8e2c758a210d0d76a95a3ebb446dce0cd9b532073` | **另一版布局**的 sgdisk 输出（§8 已说明不可逐条对拍） |
| `MT6789_Android_scatter.xml` | 99188 B | `63487fed875c24d82772a5f47bf84d7d7371e0aaf893be9146dd0c96ea4d8a41` | scatter **XML 方言**：130 块 = EMMC/UFS 两份 × 65（§12） |

D1 已有样本（DA 文件 / preloader / 文本 scatter）见 `mtk-brom-facts.md` §8；本阶段新增的 XFlash 用法：
832 个 preloader 的 **912 B 切片**对拍（`mtk_preloader_emi` 的两代切片）。

## 12. scatter XML 的 EMMC / UFS 双副本

`MT6789_Android_scatter.xml`（真样本，实测）：

- `<partition_index>` 块 **130 个 = 两份完整副本 × 65**：`<storage>HW_STORAGE_EMMC</storage>` **65 个**、
  `HW_STORAGE_UFS` **65 个**。
- 块内 `<region>` 取值：EMMC 侧 `EMMC_BOOT1` ×1 / `EMMC_BOOT2` ×1 / `EMMC_USER` ×63；
  UFS 侧 `UFS_LU0` ×1 / `UFS_LU1` ×1 / `UFS_LU2` ×63。
- → **解析必须按 storage 过滤**：不过滤会让每个分区**翻倍**且大小对不上（计划层预览会立刻失真）。

## 13. 项目自有决策（**非上游行为**，不得当成上游出处引用）

| 决策 | 说明 / 落点 |
|---|---|
| GPT **双 CRC fail-closed** | 头部（crc 字段清零后）与条目表 CRC 都校验，不符即明确报错；不复刻缺陷④ |
| 备份 GPT **兜底真生效** | 主 GPT 签名/revision/CRC/尺寸任一不符 → 按末扇区取窗口读备份（`mtk_gpt::readTable`） |
| 512/4096 **探测** | 不依赖上游的 `config.pagesize`（缺陷③）；调用方给默认值时仍两档探测 |
| 空条目按 **type GUID** 且 `continue` | 复刻 UEFI 语义而非上游的 unique GUID + break |
| `diskSectors = 0` → **只走主 GPT** | XFlash/XML 都没有"磁盘总扇区数"的可靠来源 → 备份兜底不可用，且**如实说明**（`readTable` 报主 GPT 的错）；D4 真机口径 |
| 存储暂只 **eMMC user** | XFlash `storage=0x1` / `parttype=8`（ST:16-49）；XML 描述符字符串 `"EMMC-USER"`（ST:239、XC:452-461）。**UFS 机型未拒绝**（XML 侧 eMMC 描述符也照发）→ 记入 D4 改 fail-closed |
| XFlash 写**非 512 对齐 → fail-closed** | 上游对每个包自动补零（XFL:872-874）；本层拒绝该取值（补零会插进数据中间、实发字节与参数承诺不符） |
| **PMT 分支明确拒绝** | 上游 XFlash 的 PMT 分支不可达 + 有 bug（§9）；本层不猜读法 |
| **上游不检查的步骤本层检查** | 两个 setup（XFL:989-990）、XML 三步（XL:311-313）、`boot_to` 之后的存活判据 |
| XML 写**必须带写地址** | 逐分区写传 GPT 条目地址（`first_lba × 扇区`）；不传会写到描述符偏移 0 = **分区表区**（上游调用点：`v6.py:1095-1097`、`mtk_da_handler.py:544-548`） |
| XML 读路径要求"宣布长度 == 请求长度" | 上游直接用设备宣布值覆盖请求值；本层 fail-closed |
| XML 收尾 `REBOOT` 用 **IMMEDIATE** | 上游 `cmd_reboot` 默认 `disconnect=False`（XC:429-433，False → `IMMEDIATE`），唯一调用点 XL:1045 显式传 False |
| `X_CTRL_CC_OPTIONAL_DOWNLOAD_ACT = 0x800005` **照发** | 上游就是这个值（同段邻居多为 `0x0800xx`）；改一个 0 就是另一条命令 |

## 14. 诚实边界（**不得写成"已验证"**）

- **真机全链未验证（三代都没有）**：枚举 / 打开 / 握手 / DA1 / DA2 / EMI / 分区表 / 写入 / 收尾全段
  只有**离线**保证（真 DA/preloader/GPT 样本 + mock 逐帧 + 对抗变异）。
- **XML 代无真实设备样本**：帧格式与命令序列**全部来自上游代码**；`CMD:START` 的实际形态、
  `0x6781` 之外的芯片差异都无真机复核。
- **GPT 样本来自第三方仓库**（`ReCoreShift/mtk-gpt-tool`，MPL-2.0 的 `tests/fixtures/`，**不是我们自己的设备**）；
  `reference/` gitignored，不随仓库分发、不进构建。
- **`0x6781` 的 16B 合并 ack 与其它芯片的两次写**：只按上游实现，**无该芯片真机复核**。
- **mock 看不见"设备实际读了多少字节"**：读侧判据靠"逐条对上游读长度"+ probe。
- **"0 warning" 只是编译器默认档**：`CMakeLists.txt` 未开 `-Wall -Wextra`。
- **spec §9 的三条"实现时核实"到此结案**：
  1. **XML 分区表读取路径**：`CMD:READ-FLASH`（`XC:474-484` 区间读）+ **同一个 GPT 解析器**
     （上游同口径 `mtk_daloader.py:296-302`）—— **设计期已核实**；
  2. **`UFSPartitionType` 文本表示**：XML 用**字符串** `"EMMC-USER"`（`ST:239`、`XC:452-461`）；
     整数枚举（BOOT1=1/BOOT2=2/USER=3/RPMB=4，`ST:31-48`）只在 **XFlash** 侧用 —— 已核实；
  3. **`max_address_length = 9`**：全仓**只此一处定义、零 import、零消费点** → 与 `plcap`/`blver`
     同类的**死常量**，本实现**不实现、不猜语义**。
     ⚠️ **更正**（本次复核发现）：配套报告 §5.6 与 spec 原写"只被 import 无消费点（`USBLIB:21`、
     `seriallib.py:9`）"—— 那两处 import 的是**同文件的另一个常量 `max_xml_data_length`**，与
     `max_address_length` 无关；结论（死常量）不变，**论据已按本条更正**。
- ⚠️ **另一处行号更正**：配套报告 §1.1 把 LEGACY 的 `0xC0` 读写成 `DLL:545-553`、存储信息交换写成
  `DLL:558-566`、DA2 保留签名写成 `DLL:537` —— 经本文复核，正确位置依次是 **DLL:596-601**、
  **DLL:607-634**、**DLL:591/593**（`545-553` 是 `read_flash_info` 的 `PassInfo` 尾部、`558-566` 是
  `upload_da1` 内的 region[1] 字段、`537` 在别的函数里）。**结论不变**（LEGACY 无 SYNC/SETUP_*、
  保留签名），仅行号按本文更正；D1 事实报告 `mtk-brom-facts.md` 的引用本来就是正确的那组。
- **不做项**：XML/XFlash 的 SLA（明确报错）；`DOWNLOAD`/`UPLOAD`/`FORMAT_PARTITION`（上游零调用，
  布局无从得知）；UFS 专属命令族 / RPMB / 格式化入口；DA 扩展注入（`0x4FFF0000` + `CUSTOM_ACK`）。

## 15. 复核方式（可复现）

```bash
# 行号复核（本文所有 XFL/XFP/XL/XC/XP/DC/DL/PL/BC/ST/GPT/PART 引用；以仓库根为 cwd）
git -C mtkclient log --oneline -1          # 应为 71b0175
MC=mtkclient/mtkclient
grep -n "def ack\|def xsend\|def status\|def send_param\|def send_emi\|def boot_to\|def writeflash\|def upload_da1" \
     $MC/Library/DA/xflash/xflash_lib.py
grep -n "def xread\|def xsend\|def ack_value\|def setup_env\|def setup_hw_init\|def get_command_result\|OK!EOT\|def download_raw" \
     $MC/Library/DA/xmlflash/xml_lib.py
grep -n "def create_cmd\|def cmd_write_flash\|def cmd_set_runtime_parameter" \
     $MC/Library/DA/xmlflash/xml_cmd.py
grep -n "v6 = b\|damode = DAmodes.XML" $MC/Library/DA/daconfig.py          # 173 / 216
grep -n "def get_plcap\|def get_blver" $MC/Library/mtk_preloader.py        # 923 / 664（get_plcap 零调用）
grep -rn "get_plcap" $MC/                                                  # 只应命中定义处
grep -rn "max_address_length" $MC/                                         # 只应命中 xml_param.py:2
grep -n "EMMC-USER" $MC/Library/DA/storage.py                              # 239
grep -n "crc32\|EFI PART\|for sectorsize" $MC/Library/Partitions/gpt.py    # 48 / 160-162 / 218-224

# 真样本（gitignored，需自备）
ls -l reference/mtk-samples/ && sha256sum reference/mtk-samples/{PGPT.img,SGPT.img,sgdisk_print.txt,MT6789_Android_scatter.xml}
grep -c "<partition_index" reference/mtk-samples/MT6789_Android_scatter.xml   # 130（开+闭标签则 260）
grep -o "HW_STORAGE_[A-Z]*" reference/mtk-samples/MT6789_Android_scatter.xml | sort | uniq -c   # 各 65
```

**配套底稿**：`.superpowers/sdd/mtk-d2d3-facts-report.md`（736 行：XFlash/XML 命令表、EMI 两套机制、
GPT 上游缺陷、代际投票、832 preloader 与 2 个 DA 文件的实测脚本说明）。
**LEGACY 侧**：`docs/superpowers/specs/mtk-brom-facts.md`（D1 交付，含 §8 真样本与 §10 边界）。
**设计**：`docs/superpowers/specs/2026-09-16-mtk-xflash-xml-design.md`（D2+D3 spec，§9 边界 / §11 交付清单）。
