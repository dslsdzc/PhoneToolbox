# MTK BROM（LEGACY）刷写 — 协议事实报告（逐条出处）

日期：2026-09-16
范围：**只给事实**（路径 / 行号 / 字节与顺序结论），不含实现方案；"项目自有决策"与上游行为**分开列**（§9）。
上游：`bkerler/mtkclient` 子模块，HEAD `71b0175`（"Add more fixes and ssr stuff"）。许可 **GPL-3.0** ——
本报告只记录事实与行号，**不转载其代码文本**；引用的常量/字节串属协议事实。
本仓落点：`src/core/modes/mtk_da_file.{h,cpp}`、`mtk_chip_table.{h,cpp}`、`mtk_preloader_emi.{h,cpp}`、
`mtk_preloader_fetch.{h,cpp}`、`mtk_payload.{h,cpp}`、`src/core/mtk_flash_plan.{h,cpp}`（下称"本层"）。
所有行号在写本文时**逐条 grep/sed 复核过**（复核方式见 §11）。

**路径简写**（全部相对 `<repo>/mtkclient/`；本仓的 `mtkclient/` 子模块）：
- **DLL** = `mtkclient/Library/DA/legacy/dalegacy_lib.py`（1262 行，LEGACY 代协议主体）
- **DLP** = `mtkclient/Library/DA/legacy/dalegacy_param.py`（命令/响应常量）
- **DFP** = `mtkclient/Library/DA/legacy/dalegacy_flash_param.py`（存储信息结构体）
- **DC** = `mtkclient/Library/DA/daconfig.py`（DA 文件解析 / 条目选择 / EMI 提取）
- **DL** = `mtkclient/Library/DA/mtk_daloader.py`（装配层）
- **DH** = `mtkclient/Library/DA/mtk_da_handler.py`（交互式 CLI 命令层）
- **MC** = `mtkclient/config/mtk_config.py`（bmtsettings 等）
- **PL** = `mtkclient/Library/mtk_preloader.py`（BROM 级命令）
- **UL** = `mtkclient/Library/Connection/usblib.py`（USB 读原语）

---

## 1. DA1 → DA2 的完整顺序（LEGACY）

入口 `DALegacy.upload_da1()` = **DLL:554-668**。非 IoT 分支（手机）的逐步骤如下：

| # | 动作 | 出处 |
|---|---|---|
| 1 | 取 DA1 = `region[1]`：`m_buf`（文件偏移）/`m_len`/`m_start_addr`/`m_sig_len` —— **下标 1 硬编码** | DLL:563-568 |
| 2 | 取 DA2 = `region[2].m_buf` 起的 `m_len` 字节（`m_sig_len` 另存备用，见 §5） | DLL:570-573 |
| 3 | SBC 修补与 DA2 哈希修补（本层 **D1 不做**） | DLL:574-593 |
| 4 | `send_da`(BROM 级 0xD7) → `jump_da`(0xD5)，均传 `da1address` | DLL:594-595 |
| 5 | 读 **1 字节**，必须 `== b"\xC0"`；否则 `"Error on DA sync"`，成功打印 `"Got loader sync !"` | DLL:596-603 |
| 6 | **存储信息交换**（§2） | DLL:607-634 |
| 7 | `set_stage2_config(hwcode)`（§3）—— 返回 False 则整链失败 | DLL:636 |
| 8 | `brom_send(..., 2)` = 上传 DA2（§5） | DLL:639 |
| 9 | `read_flash_info()` 作 DA2 存活判据（§6）；成功后按 flashtype 定 `flashsize` | DLL:640-649 |
| 10 | `check_usb_cmd()` 读 USB 速率，必要时重连提速 | DLL:651-666 |

**LEGACY 代没有 SYNC / SETUP_ENVIRONMENT / SETUP_HW_INIT_PARAMS**：全局 grep 这三个符号只命中
XFlash（`xflash/xflash_lib.py:903/909/926/989-990`）与 XML（`xmlflash/xml_lib.py:167/311/323/655`），
`DLL` 零命中 —— DA1 起来后**只回一个 `0xC0`**。
**IoT 是另一条路**：`DLL:669-690` 的 `else: # MT6261 / MT2523` 分支改用 `send_env_prepare` +
`region[0]` 作 ENV + `entry_region_index` 作 stage1 下标（本层 D1 **明确不支持**，见 §10）。

## 2. 存储信息交换（DLL:607-634）

| 顺序 | 方向 | 字节数 | 出处 |
|---|---|---|---|
| 1 | 读 NAND_INFO | 4B（BE u32，注释 `# 0xBC4`） | DLL:607-609 |
| 2 | 读 NAND id **条数** | 2B（BE u16） | DLL:610 |
| 3 | 读 NAND id 表 | 2B × 条数（循环逐条读） | DLL:611-614 |
| 4 | 读 EMMC_INFO | 4B（BE u32） | DLL:615-617 |
| 5 | 读 EMMC id 表 | **4 × 4B = 16B**（固定 4 条） | DLL:618-621 |
| 6 | **存储类型判定**（**先 nand 后 emmc，都不中才是 nor**） | `nandids[0] != 0` → `"nand"`；否则 `emmcids[0] != 0` → `"emmc"`；否则 `"nor"` | DLL:623-628 |
| 7 | 写 1B `ACK` | — | DLL:630 |
| 8 | 读 **3 × 1B**（累加后仅打印 hex，不判别内容） | 3B | DLL:631-634 |

判定规则只看**每张 id 表的第一个元素是否为 0**（`len(...) > 0` 在 Python 里恒真，实际就是看首元素）。

## 3. stage2 配置写入（DLL:228-296）

写出共 **11 笔 / 18 字节**（字段名 10 个 —— `nor` 拆成"片选模式 + 片选号"两笔）：

| # | 字段 | 写法 | 取值 | 出处 |
|---|---|---|---|---|
| 1 | `bromver` | `B` | 来自 BROM 版本寄存器（§7） | DLL:231 |
| 2 | `blver` | `B` | 同上 | DLL:232 |
| 3 | `m_nor_chip` | `>H` | `0x08` = `CS_WITH_DECODER` | DLL:233-234 |
| 4 | `m_nor_chip_select` | `B` | `0x00` = `CS_0` | DLL:235-236 |
| 5 | `m_nand_acccon` | `>I` | `0x7007FFFF` | DLL:237-238 |
| 6 | `bmtflag` | `B` | `MC.bmtsettings(hwcode)` 算出（下表） | DLL:239-240 |
| 7 | `bmtpartsize` | `>I` | 同上 | DLL:241 |
| 8 | `force_charge` | `B` | `0x01`（注释：工具里 `0x02`=Auto / `0x01`=On） | DLL:242-244 |
| 9 | `resetkeys` | `B` | `0x01`；**`hwcode == 0x6583` → `0`** | DLL:245-248 |
| 10 | `ext_clock` | `B` | `0x02` = `EXT_26M` | DLL:249-251 |
| 11 | `msdc_boot_ch` | `B` | `0` | DLL:252-253 |

**按 hwcode 的追加分支**（写在上表之后、读 errorcode 之前）：

| hwcode | 追加内容 | 出处 |
|---|---|---|
| `0x6592` | `>I is_gpt_solution = 0`（1 笔） | DLL:255-257 |
| `0x6580` / `0x8163` | `>I slc_percent = 0x1` + 19B 常量 `46 46 00×14 FF 00 00 00`（2 笔） | DLL:258-265 |
| `0x8127` | 先 `>I is_gpt_solution = 0`，再走上一行的 `slc_percent` + 19B（同上） | DLL:259-265 |
| `0x6583` | `>I forcedram = 0` | DLL:266-272 |
| `0x6589` | `>I forcedram = 1` | DLL:266-272 |
| `0x6582` | `>I newcombo = 1` | DLL:276-278 |

> ⚠️ 注意 `elif hwcode == 0x8127`（DLL:273-275，`>I skipdl = 0`）**不可达**：`0x8127` 已在
> DLL:258 的 `elif` 里被 `0x6580/0x8163/0x8127` 分支接走。上游死分支，照抄无意义。

**收尾**：`time.sleep(0.350)`（DLL:279）→ 读 `toread`(=4) 字节（DLL:280）→ 长度不足 4 →
`"Didn't receive Stage2 dram info..."` 并返回 False（DLL:281-283）→ `errorcode = >I`（DLL:284）。

**errorcode 的三条去向**：

| errorcode | 行为 | 出处 |
|---|---|---|
| `0x0` | 成功返回 True；`hwcode == 0x6592` 时**还要多读 5 × 4B**（DLL:286-292） | DLL:285-293 |
| `0xBC3` | 进入 EMI 段（§4） | DLL:294-297 |
| 其它 | `self.eh.status(errorcode)` 报错 → 返回 False | DLL:294-296 |

## 4. EMI / DRAM 初始化

**触发是 request-driven**：只有 `errorcode == 0xBC3`（DA1 明确索取 DRAM 配置）才走 EMI 段
（DLL:294-297）。**此时若没有 EMI 数据，上游直接中止**：
`self.error("Preloader needed due to dram config.")` + `port.close(reset=True)` + `return False`（DLL:399-402）。
即"跳过 EMI 继续"只在 `errorcode == 0`（根本不需要 DRAM 配置）时成立。

### 4.1 `0xBC3` 之后的读取（DLL:297-332）

| 顺序 | 方向 | 字节数 | 出处 |
|---|---|---|---|
| 1 | 读（丢弃） | 4B（`buffer += self.usbread(4)`） | DLL:297-298 |
| 2 | 读 `draminfo` | **16B**；同时生成本地匹配用的两种排列：原序 `pdram[0]=draminfo[:9]`，以及**按 4 字节组各自反转**的 `pdram[1]` | DLL:300-303 |
| 3 | 若本层没有 EMI 数据：在 `<loader_path>/Preloader/` 目录里**按 `pdram[0]`/`pdram[1]` 前缀搜索**任一 preloader 文件并就地提取 EMI | 目录遍历 | DLL:305-318 |
| 4 | 读回执 | 4B，必须 `== 0xBC4`，否则报错返回 False | DLL:319-326 |
| 5 | 读 nand id 条数 | 2B（BE u16） | DLL:328 |
| 6 | 读 nand id 表 | 2B × 条数 | DLL:329-332 |

### 4.2 EMI 发送（DLL:333-398）

前置：`if self.daconfig.emi is not None:` —— **否则**走 DLL:399-402 的中止（见上）。

| 步 | 动作 | 出处 |
|---|---|---|
| 1 | 写 `ENABLE_DRAM 0xE8` | DLL:334 |
| 2 | 写 `>I emiver`；**`emiver == 0` 时写 `0xFFFFFFFF`** | DLL:335-338 |
| 3 | 读 1B：`NACK 0xA5` → `"EMI Config not accepted :("` + `sys.exit()`；`ACK 0x5A` → 继续 | DLL:339-344 |
| 4 | **按 emiver 分档**（见下表） | DLL:345-371 |
| 5 | 写 EMI 本体 | DLL:372 |
| 6 | 读 `>H` checksum（仅打印） | DLL:373-374 |
| 7 | 写 `ACK` → 写 `>I 0x80000001`（Send DRAM config） | DLL:375-376 |
| 8 | 读 `>I M_EXT_RAM_RET`，**必须为 0**；否则报错 + `port.close(reset=False)` + False | DLL:377-382 |
| 9 | 读 1B `M_EXT_RAM_TYPE`（注释 `0x02 HW_RAM_DRAM`） | DLL:383-384 |
| 10 | 读 1B `M_EXT_RAM_CHIP_SELECT`（注释 `0x00 CS_0`） | DLL:385-386 |
| 11 | 读 `>Q M_EXT_RAM_SIZE` | DLL:387-388 |
| 12 | 仅 `emiver == 0x0D`：再读 **5 × 4B**（注释依次为 `00000003` / `1C004004` Raw_0 / `aa080033` Raw_1 / `00000013` CJ_0 / `00000010` CJ_1） | DLL:389-398 |

**分档表**（`emiver` → 发送前的读/写序列）：

| 档位 | 序列 | 出处 |
|---|---|---|
| `{0x0F, 0x10, 0x11, 0x14, 0x15}` | 读 `>I dramlength`（注释 `# 0x000000BC`）→ 写 `ACK` → **`hwcode != 0x8127` 时**写 `>I lendram`（`lendram = len(emi)`） | DLL:345-351 |
| `{0x0A, 0x0B}` | **先读 0x10（16B）info**（注释 `# 0x000000BC`）→ **再**读 `>I dramlength` → 写 `ACK` | DLL:352-356 |
| `{0x0C, 0x0D}` | 读 `>I dramlength` → 写 `ACK` → **改写 EMI 本体**：`emi = >I 0x100 + emi[4:dramlength]` | DLL:357-362 |
| `{0x00}` | 读 `>I dramlength`（注释 `# 0x000000B0`）→ 写 `ACK` → `emi = emi[:dramlength]` → 写 `>I dramlength` | DLL:363-369 |
| 其它 | `"Unknown emi version: %d"` 告警（**不中止**），随后仍写 EMI 本体 | DLL:370-372 |

## 5. DA2 上传（`brom_send`，DLL:907-940）与 FINISH

| 步 | 动作 | 出处 |
|---|---|---|
| 1 | `size = region[2].m_len`，`address = region[2].m_start_addr` —— **不剥 `m_sig_len`（保留尾部签名）** | DLL:908-910；对照 XFlash：`xflash_lib.py:974-978` 在非修补/非 SBC 分支裁掉 `da2[:-da2sig_len]` |
| 2 | 写 `>I address` → `>I size` → `>I packetsize`（默认 `0x1000`） | DLL:912-914 |
| 3 | 读 1B 须 == `ACK`；否则报错返回 False | DLL:915-916、DLL:937-939 |
| 4 | 按 `packetsize` 分块写，**每块后读 1B 须 == ACK**（失败即 break + 报错） | DLL:917-927 |
| 5 | `time.sleep(0.5)` → 写 `ACK` → 读 1B 须 == `ACK` → `"Successfully uploaded stage {stage}"` | DLL:929-936 |

**FINISH（0xD9）**：命令常量 `FINISH_CMD = b"\xD9"`（DLP:91）；帧体 = 写 `0xD9` → 读 1B 须 ACK →
写 `>I value` → 读 1B 须 ACK（DLL:972-980）。
上游的调用点只有 `shutdown()`（DLL:903-905，`FINISH` 之后立即 `port.close(reset=True)`）与交互式
`reset` 命令（DH:1379-1383）；`ShutDownModes.NORMAL = 0`（DL:307-310，`bootmode` 直接当 `value` 写下去）。
**分区分级关闭**（`num_partitions` 逐分区 FINISH）在上游存在实现（`finish` 的 `value` 参数），
本层 D1 只在刷写收尾调一次 `FINISH(0)`。

## 6. `read_flash_info`（DLL:526-552）与存储结构体

按读取顺序（DA2 存活判据）：

| # | 段 | 字节数 | 出处 |
|---|---|---|---|
| 1 | NOR info | `0x1C`（28B） | DLL:527 |
| 2 | NAND info 原始块 | `0x11`（17B，先按 **NandInfo64** 解） | DLL:528-529 |
| 3 | NAND id 表 | `nandcount * 2`；**`nandcount == 0` 时按 NandInfo32 重解**，再用 `usbread(nandcount * 2 - 4)` = **`usbread(-4)`** | DLL:530-537 |
| 4 | NAND info2 | 9B | DLL:539 |
| 5 | EMMC info | `0x5C`（92B） | DLL:540 |
| 6 | SDC info | `0x1C`（28B） | DLL:541 |
| 7 | flash config info | `0x26`（38B） | DLL:542 |
| 8 | **仅 `hwcode ∈ {0x8127, 0x8163}`**：多读 4B（丢弃） | 4B | DLL:543-545 |
| 9 | PassInfo | `0xA`（10B） | DLL:546 |

**`usbread(-4)` 读 0 字节（不是"读到超时"）**：`UL:462-468` 里 `if resplen <= 0: self.info("Warning !")`
（只印一句），循环守卫 `while bytestoread > 0`（UL:483-489）在 `resplen = -4` 时**根本不进循环**，
`res` 保持空，末尾 `return res[:resplen]`（UL:535）返回 `b""`。调用点 DLL:534。
**eMMC 手机的常态路径**：NandInfo64 的 `m_nand_flash_id_count` 为 0 → 走 NandInfo32 再看，
仍为 0 → `usbread(-4)` 静默返回空；`data[-4:]` 那 4 字节兜住了 `nc`（`unpack(">" + "0H")` 得空元组）。

**PassInfo 判活**（DLL:28-39，`structhelper_io` 顺序读）：
`ack = bytes()`（**1B**）、`m_download_status = dword(big)`（4B）、`m_boot_style = dword(big)`（4B）、
`soc_ok = bytes()`（**1B**）—— 合计 10B。判活：`pi.ack == 0x5A` → True（DLL:547-548）；
否则 `pi.m_download_status & 0xFF == 0x5A` → **再多读 1B** 后 True（DLL:549-551）；都不中 → False。

**NAND 结构体字段布局**：

| 结构体 | 字段（顺序） | `m_nand_flash_id_count` 偏移 | 出处 |
|---|---|---|---|
| `Legacy_NandInfo64` | `m_nand_info(>I)` / `m_nand_chip_select(1B)` / `m_nand_flash_id(>H)` / `m_nand_flash_size(>Q)` / `m_nand_flash_id_count(>H)` | **15**（4+1+2+8） | DFP:61-78 |
| `Legacy_NandInfo32` | 同上前四字段，但 `m_nand_flash_size` 为 `>I` | **11**（4+1+2+4） | DFP:170-179 |

即 17B 的原始块正好够 NandInfo64；`count == 0` 时用同一块按 32 位布局重解（`m_nand_flash_size` 只取低 4B）。

## 7. 版本读法、代际判定与芯片表

**BROM 版本读法**（PL:657-673）：`get_bromver()` 写命令值（`0xFF`）→ 读 1B → `bromver`（PL:657-662）；
`get_blver()` 写命令值（`0xFE`）→ 读 1B → `blver`，**且若读回值 == 命令值（`0xFE`）则判定"当前在 BROM"**
并置 `is_brom = True`（PL:664-673）。

**hw_code 的读法**（PL:174-193）：先 `echo(GET_HW_CODE 0xFD)`；非 IoT 时 `val = self.rdword()`
（PL:179-181），取 `hwcode = (val >> 16) & 0xFFFF`、`hwver = val & 0xFFFF`（PL:190-191）。
**IoT 回退**：`val is None or config.iot` 时改走 `read_a2(0x80000000/0x80000008/0x8000000C)`
+ `read32(0xA01C0108)` 回填 hwver/hwcode/hw_sub_code/swver 并置 `iot = True`（PL:182-188）。

**DA 条目查找键 = 芯片表的 `dacode`，不是设备回报的 hw_code**：
`dacode = config.chipconfig.dacode` 后 `if dacode in self.dasetup`（DC:208-209）；
`dasetup` 的键是**条目自己的 `hw_code`**（DC:188-201）。
过滤条件：`loader.hw_version <= config.hwver or config.hwver == 0`（DC:212）、
`loader.sw_version <= config.swver or config.swver == 0`（DC:213）—— **不看 plcap/blver**；
首个满足者即选中（`if self.da_loader is None:`，DC:214-218）。
`0x6261` 特例：`da_loader is None and dacode != 0x6261` 才报 `"No da_loader config set up"`（DC:219-220）。

**芯片表**：`mtkclient/config/brom_config.py` 的 `hwconfig` dict（`:479` 起）是**唯一生效**的 `damode` 表
（全文件另外两处 `damode=` 分别在 `chipconfig.__init__` 的签名默认值 `:383` 与一段三引号死文本 `:448-477` 内）；
本仓转写 **89 条**（= 上游该表全部条目，其中 `iot=True` 20 条），生成脚本与出处见 §11。

**DA 文件选择键 = 5 元组**（DA 文件侧，与上表查找键相区分）：条目字段序 `magic(==0xDADA)` /
`hw_code` / `hw_sub_code` / `hw_version` / `sw_version` / `reserved1` / `pagesize` / `reserved3` /
`entry_region_index` / `entry_region_count`，随后 N×20B region（`m_buf`/`m_len`/`m_start_addr`/
`m_start_offset`/`m_sig_len`，全 u32 LE）—— 4 元组会在真实文件上碰撞，故选择键必须含 `pagesize`
（见 §8 与配套侦察报告）。

## 8. 真实样本与实测值

样本目录 `reference/mtk-samples/`（**gitignored**，不进提交/不进构建）：

| 样本 | 实测 |
|---|---|
| `MTK_AllInOne_DA_{2625,7687,iot,mt6590}.bin` + `MTK_DA_V5.bin` + `MTK_DA_V6.bin` | 6 文件 / **161 条目**；`magic == 0xDADA` 161/161；`MT6765_Android_scatter.txt` = **50 分区**（SYS0..SYS49） |
| `preloader.bin` | MMM 魔术 @**0**；`MTK_BLOADER_INFO_v` 标记 @**254392**、版本字节 `"35"`；`MTK_BIN` @**254492**；MMM 头字段 `mlen=0x3EBB8` / `siglen=0x66C` / `dramsize=912`；**三个字段都是小端 u32**（`unpack("<I", …)`，DC:124/125/127 —— 按大端读会得 2416115712，只有小端才是 912，与本文自洽）；dramsize 窗口 = [254392, 255304) = 912B；**LEGACY 切片 800B**（窗口内 `MTK_BIN+0xC` 起，块内相对偏移 112）**vs XFlash 整块 912B** |
| `MTK_DA_V5.bin` / `MTK_DA_V6.bin` | V6 的 `region[0]` 与 `region[1]` **逐字节相同**且全文无 EMI 数据；V5 带 50 个 `MMM\x01\x38` 块（`region[0]`，`len` 恒 `0x270`） |
| 全库 preloader 统计（832 个） | 含 MMM 魔术 **仅 3 个**；`MTK_BLOADER_INFO_v` 在**偏移 0** 的 **829 个**；含 `MTK_BIN` **832/832** |

硬断言落点：`tests/test_mtk_da_file.cpp`（6 文件/161 条目 + 独立 Python 解析器 `reference/mtk-samples/parse_da.py` 逐字段对拍）、
`tests/test_mtk_preloader_emi.cpp`（800 vs 912 切片、版本字节 35）、`tests/test_mtk_flash_plan.cpp`（50 分区 scatter）。

## 9. 项目自有决策（**非上游行为**，不得当成上游出处引用）

| 决策 | 说明 / 落点 |
|---|---|
| preloader 自动导入规则 | 在用户所选固件目录内找 `preloader*.bin`（大小写不敏感），**唯一命中才用**；多候选**不猜**、列候选告警 |
| 不实现"按 draminfo 在目录里猜 preloader" | 上游 DLL:305-318 的补救路径**不做**：那要求本层先拿到 16B draminfo 才能找文件，而"没有 EMI"正是要在那之前决策 |
| 网络获取 | **默认关闭**；来源清单由**用户提供**（`mtk_preloader_sources.json`），**不内置任何第三方 URL/sha256**；fail-closed（不可判定的期望哈希在发请求前即拒） |
| 逐分区写入 | 按设备实读分区表（`listPartitions`）排序；`镜像 > 分区` 逐条告警并**跳过不写**；末尾单次 `FINISH(0)`，失败**只告警**（数据已落盘） |
| **超分区镜像无强写出口**（如实限制） | 计划期裁决 2 曾写"用户要强写走既有单分区入口"——**该出口在 BROM 模式不存在**：BROM 下 `onFlashClicked` 先在协议通道分支处理并走 `flashFullPackage`（`flash_panel.cpp:675-853`），分区刷写段（`mtkWritePartition` 的调用点 `:907`）**在其后**，且 `:359-368` 已把分区表清空 ⇒ BROM 下 `mtkWritePartition` 不可达 ⇒ **超分区镜像只能跳过**（`mtk_flash_plan` 也不提供覆盖开关） |
| 死砖恢复不适用于 BROM（如实限制） | 「死砖修复」按钮只在 `MODE_MTK_DA` 且已连接时 enable（`flash_panel.cpp:341`，BROM 在 `:359-368` 已 return），且 handler `:1898-1906` 只认 EDL/MTK DA → BROM 用户点它只会得到"当前模式不支持" ⇒ BROM 侧**不含**备份关键分区与失败重试 |
| 零长度 region | **显式跳过**（不得"上传 0 字节后静默成功"）；`region[1]`/`region[2]` 任一为空 → 该条目不可用（`mtk_da_file.cpp:39-53`） |

## 10. 诚实边界（**不得写成"已验证"**）

- **真机全链未验证**：枚举 / 打开 / 握手 / DA1 / `0xC0` / 存储信息交换 / `0xFC` 降级 / stage2 /
  EMI / DA2 / `read_flash_info` / `read_pmt` / 写入 / FINISH —— **全段**都只有离线验证
  （真 DA/preloader 样本 + mock 逐帧）。
- **只有代码级保证、无用例的路径**：枚举 / 打开 / 握手（`runBromFlash` 从 `enumerate` 起不可离线测）。
  代际拒绝 / 空计划早拒 / `0xFC` 降级 / FINISH 只告警这四类在 T9 抽出注入缝 `bromFlashOnSession`
  之后**已补决策用例**（`tests/test_mtk_payload.cpp:1141` 起），但**真机段仍无用例**。
- **IoT 芯片**：若 `0xFD` 不答 4 字节，上游走 A2 寄存器回退（`mtk_preloader.py:182-188`），本层**未实现**
  → 真机表现为"读版本失败"，**不是**"IoT 明确拒绝"。IoT（`iot=True`）在本层是**明确拒绝**。
- **mock 只看写、看不见"读了多少字节"**：`MockUsbChannel::read` 是 `takeFirst()` + `left(maxLen)`
  （`tests/test_mtk_brom.cpp:38-45`）——**欠读会无声通过**；读侧判别力只能靠"逐条对上游读长度"
  （`mtk_payload.cpp` 的 `readExactBytes` 走 `IBromUsb::readExact`）+ probe。
- **"0 warning" 只是编译器默认档**：`CMakeLists.txt` **未开** `-Wall/-Wextra`（想要更强的静态保证需另行开启，
  那会引入一批既有告警，属独立工程）。
- **`0xD8` 老格式**：探测到即**明确拒绝**（无真实样本、字段偏移不同）；**PMTv1/v3** 无真实样本；
  **MMM-EMI 分支** 3/832；**NAND/NOR 设备**在 stage2 处明确拒绝（`mtk_payload.cpp:255-260`；D1 只支持 eMMC）；
  **IoT 与"选中条目 DA 空 region"机型**（2625 的 `0x2523`、iot 的 `0x6261`）报错不静默
  —— 这类 hw_code 需要上游那套 0 字节/特殊映射语义（如 DLL:669-690 的 ENV+`entry_region_index`），
  **D1 明确不做**（报错优于静默刷坏）。
- **内置芯片表 89 条** = 上游 `hwconfig` 全表（**不是**"条目数更多"）；覆盖度 = 转写时的 mtkclient 版本；
  **表外芯片明确报错**。
- **网络获取默认关闭且不内置任何第三方来源**（清单由用户提供）。
- **UI 真机交互未验证**（模态 `exec()` + 进度泵重绘）；`ExcludeUserInputEvents` 仍放行排队 QTimer 的重入面
  —— 与 EDL 路径同级（既有模式）。
- **`ver == 0x0D` 的 EMI 切片起点不一致**：独立工具 `Tools/preloader_to_dram.py:26-27` 用 `MTK_BIN+0x16`，
  协议层（DC:141-144 / DLL 调用方）用 `MTK_BIN+0xC` → **D1 按协议层**（`mtk_preloader_emi.cpp:39`），真机未验证。
- **未验证的读分支**：`read_flash_info` 的 NAND id 表分支只测了 `count == 0`（N64≠0 与 N32≥3 无覆盖）
  —— 对 D1 接受的设备**不可达**（stage2 已按 `flashtype != emmc` 拒绝 NAND 设备）。

## 11. 复核方式（可复现）

```bash
# 行号复核（本文所有 DLL/DLP/DFP/DC/DL/MC/PL/UL/DH 引用；以下均以**仓库根**为 cwd）
git -C mtkclient log --oneline -1                 # 应为 71b0175
grep -n "def upload_da1\|def set_stage2_config\|def read_flash_info\|def brom_send\|def finish" \
     mtkclient/mtkclient/Library/DA/legacy/dalegacy_lib.py
grep -n "def bmtsettings" mtkclient/mtkclient/config/mtk_config.py
grep -n "def m_extract_emi\|dacode in self.dasetup" mtkclient/mtkclient/Library/DA/daconfig.py
grep -n "def usbread" mtkclient/mtkclient/Library/Connection/usblib.py
grep -n "GET_VERSION = \|GET_BL_VER = \|GET_HW_CODE = " mtkclient/mtkclient/Library/mtk_preloader.py

# 芯片表出处（生成脚本头部 + 生成物注释）
head -12 src/core/modes/mtk_chip_table.cpp
grep -c "^    {0x" src/core/modes/mtk_chip_table.cpp    # 89

# 真样本（gitignored，需持机人/复现者自备）
ls reference/mtk-samples/
```

**配套侦察报告**：`.superpowers/sdd/mtk-d2d3-facts-report.md`（736 行，XFlash/XML 代 + DA 两阶段 + EMI 的
更细出处；含 832 个 preloader 与 6 个 DA 文件的实测脚本说明）。
