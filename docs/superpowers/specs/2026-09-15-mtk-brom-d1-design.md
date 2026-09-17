# PhoneToolbox — MTK BROM 刷写 Phase D1（老代 LEGACY 完整化）设计

日期: 2026-09-15
状态: 待用户评审
前置: F1（MTK BROM 直刷骨架）已交付并合并；Phase A/B/C 已交付
配套事实：`.superpowers/sdd/mtk-d2d3-facts-report.md`（736 行，逐条带 file:line/小节）
样本与实测产物：`reference/mtk-samples/`（**gitignored**）
后续：**D2（现代 XFlash）+ D3（最新 XML）另写一份 spec**（本 spec 只覆盖 LEGACY 代闭环）

## 1. 背景与目标

功能清单明写「MTK BROM 分区刷写待接线」：`runBromFlash` 的**逐分区循环早已实现**，缺的是"谁来填 `partitions` 表"，且 `sendPayload` 目前把**整个 DA 文件按 addr=0/sigLen=0** 发出（真机必错）。

**目标**：MTK BROM 通道（老代 LEGACY）从"仅 DA 握手"走到**按计划逐分区刷写**——DA 文件解析与选择、DA1 正确上传、DA1→DA2 两阶段、EMI/DRAM 初始化、计划层与预览、逐分区写入；离线可验（真实 DA/preloader 样本 + mock 逐帧），真机归持机人。

**用户决策记录（2026-09-15）**
- 范围：**一个大计划全做（D1+D2+D3）**，但 **spec 拆两份**：本份 = D1（LEGACY 闭环）；D2+D3 另写。
- **现代芯片也要支持** → 归 D2/D3（XFlash / XML 两套协议族）。
- **EMI/preloader 两条路径**：① 用户提供/自动导入（从固件目录找 `preloader_*.bin`）② 从网络获取 —— **网络路径默认关闭、显式开启**（含来源清单 + sha256 校验 + 风险提示）。
- **代际判定**：内置**较大的**芯片表（hw_code → damode）＋ DA 条目的 `v6` 布尔强制 XML ＋ 用户参数可覆盖 ＋ 判不出就明确报错（不猜）。

## 2. 事实基础（要点；逐条出处见配套报告）

**⚠️ 三处规格文档（`~/lx/02`）纠错 —— 已由实测推翻，实现按代码/实测为准**
| 文档说 | 实测/代码 |
|---|---|
| V5 文件头部标记为 `MTK_DA_V5` | **不存在**该字面量；判别式是**负向**的 `v6 = "MTK_DA_v6" in hdr[:0x68]`（`v6` 在偏移 0x20）；**横幅版本号不能当世代指示**（`iot.bin` 横幅写 v5.1624 却不是 V5） |
| `pagesize` "通常 512" | 实测 `{0, 1, 4096, 12288}`；**当作常量过滤会误杀大量真实条目** |
| §5 主流程把 `SYNC`/`SETUP_ENVIRONMENT`/`SETUP_HW_INIT_PARAMS` 写成通用步骤 | **LEGACY 代根本没有这三条**（全局 grep 只命中 XFlash/XML）→ 照文档写会在老芯片上发 XFlash 命令 |

**DA 文件（`MTK_AllInOne_DA_*.bin`）—— 六文件 / 161 条目实测**
- `[0x00]` 头部 `0x68` 字节（前 16 字节 `MTK_DOWNLOAD_AGENT`，横幅从 0x20 起）；`[0x68]` = `count_da`(u32 LE)；条目自 `0x6C` 起，步长 `0xDC`（老格式 `0xD8`，按 `0x6C+0xD8` 处是否 `\xDA\xDA` 探测）。
- 条目字段序（各 2B LE）：`magic(==0xDADA, 161/161 无例外)`、`hw_code`、`hw_sub_code`、`hw_version`、`sw_version`、`reserved1`、`pagesize`、`reserved3`、`entry_region_index`、`entry_region_count`；随后 N×20B EntryRegion：`m_buf`(文件偏移) / `m_len` / `m_start_addr`(加载地址) / `m_start_offset` / `m_sig_len`（全 u32 LE）。
- **`region[1]` = DA1、`region[2]` = DA2 是三代共同硬编码**（`dalegacy_lib.py:563-571`）；`entry_region_index` **仅** IoT(MT6261/MT2523 等) 特例使用。
- **条目选择键必须是 5 元组** `(hw_code, hw_sub_code, hw_version, sw_version, pagesize)` —— 4 元组在 2625/7687/iot 三个真实文件上会碰撞（iot 的 `0x6226` 两条前四字段完全相同）→ 会**静默丢条目**。
- 选择规则（`daconfig.py:208-218`）：`dacode` → 过滤 `hw_version <= 设备hw_version` 且 `sw_version <= 设备sw_version`（**==0 时旁路**）；**不看 plcap/blver**。
- 真实文件里存在的"必须容忍"形态：`m_len == 0`（2625:19/7687:22/iot:6）、`hw_code == 0x0000` 占位条目（iot:2）、`pagesize ∈ {0,1}`。
- `m_start_offset` **必须原样读取、不得由 `m_len - m_sig_len` 推导**（27 个真实反例，全在 iot 的 `region[0]`）。
- `m_buf + m_len <= filesize` 检查必须保留（本次 0 越界是官方文件的性质，不是输入合法性保证）；`count_da` 必须与 EOF 交叉校验（P9）。
- **V6 文件里 `region[0]` 与 `region[1]` 逐字节相同**（9/9）且全文**无 EMI 数据**；V5 文件带 50 个 `MMM\x01\x38` 块（`region[0]`，len 恒 `0x270`）。

**LEGACY 执行流的确切帧（`dalegacy_lib.py`）**
- DA1 上传走 **BROM 级** `SEND_DA 0xD7`（地址/长度/签名长度，**BE**）→ `JUMP_DA 0xD5`(addr)。
- DA1 起来后**只回一个单字节 `0xC0`**（`DLL:546`）——**没有 SYNC / SETUP_ENVIRONMENT / SETUP_HW_INIT_PARAMS**。
- **EMI**（`DLL:333-398`，由 preloader/DA1 索取 DRAM 信息触发）：`ENABLE_DRAM 0xE8` → 写 `>I emiver`（**emiver==0 时写 `0xFFFFFFFF`**）→ 读 1B（`NACK 0xA5` → 拒绝；`ACK 0x5A` → 继续）→ 按 emiver **分档**（`{0xF,0x10,0x11,0x14,0x15}` / `{0x0A,0x0B}` / `{0x0C,0x0D}` / `{0x00}` 各不同；**0x0C/0x0D 会改写 EMI 本体** `emi = >I 0x100 + emi[4:dramlength]`）→ 发 EMI → 读 `>H` checksum → 写 ACK → 写 `>I 0x80000001` → 读 `>I` M_EXT_RAM_RET（须 0）→ 1B TYPE → 1B CHIP_SELECT → `>Q` SIZE（emiver `0x0D` 另读 5 个 u32）。
- **`boot_to(DA2)`**（`DLL:907-940`）：写 `>I addr`（`region[2].m_start_addr`）→ `>I size`（`region[2].m_len`）→ `>I packetsize`（默认 `0x1000`）→ 读 1B 须 `ACK` → 按 packetsize 分块写数据（**每块后读 1B 须 ACK**）→ `sleep(0.5)` → 写 ACK → 读 1B 须 ACK。
  ⚠️ **LEGACY 的 DA2 保留尾部签名**（`DLL:537` 不剥；XFlash 才剥 `m_sig_len`）—— 同一文件两代送出的字节范围不同。
- DA2 存活判据（`DLL:629`）：最终 ACK + `read_flash_info()`（NOR/NAND/EMMC/SDC/Config/PassInfo，`pi.ack == 0x5A` 或 `m_download_status & 0xFF == 0x5A`）。
- 分区表：**PMT**（`A5 SDMMC_READ_PMT`）；读写：`READ 0xD6`/`WRITE 0xD5`（`>Q addr/len`）；关闭：`FINISH 0xD9`。
- 地址字节序：**LEGACY 全大端**（XFlash 才是小端）。

**EMI 数据来源（`daconfig.py:120-164`）**
- `extract_emi(preloader)`：搜 `MMM\x01\x38\0\0\0` → 取 `+0x20`/`+0x2C` 的 `mlen`/`siglen` → `data[:mlen-siglen]` → 读末尾 `dramsize`（为 0 则先 `data[:-0x800]`）→ `data[-dramsize-4:-4]` → 搜 `MTK_BLOADER_INFO_v` → **LEGACY 分支**：`data[find("MTK_BIN")+0xC:]`（XFlash 分支取整块）。
- **832 个真实 preloader 实测**：只有 **3 个**含 MMM 魔术；**829 个**的 `MTK_BLOADER_INFO_v` 在**偏移 0**；832/832 含 `MTK_BIN` → 现实几乎全走"偏移 0"路径。
- 样例实测：同一 preloader，**XFlash 切片 912B / LEGACY 切片 800B**（起点 `MTK_BIN+0xC`）。
- 自动匹配（LEGACY）：按 preloader 回报的 **DRAM info 16 字节**双字节序匹配（`DLL:295-318`）。
- **可跳过 EMI 的条件**：`preloader` 连接来源（该设备已初始化过 DRAM）—— 但 `GET_CONNECTION_AGENT(0x04000A)` 是 **XFlash 专有**；LEGACY 侧**已核实**为 request-driven：仅 `errorcode == 0xBC3` 才索取 DRAM 配置，此时缺 EMI 上游**中止**（`dalegacy_lib.py:294-297`、`:399-402`；见 §5 更正与 §8 诚实边界）。

## 3. 架构与模块

```
新增 src/core/modes/mtk_da_file.{h,cpp}        纯函数：DA 文件字节 → DaSelection（5 元组选择/region[1][2]/P1-P12 全落地）
新增 src/core/modes/mtk_chip_table.{h,cpp}     hw_code → damode（**由 tools/gen_mtk_chip_table.py 从 mtkclient 转写**；GPL-3.0→GPL-3.0，注明出处）
新增 tools/gen_mtk_chip_table.py               生成脚本（可复现；含来源 URL + commit + 许可声明）
新增 src/core/modes/mtk_preloader_emi.{h,cpp}  纯函数：preloader 字节 → EMI（两条分支；LEGACY 切片）
新增 src/core/mtk_flash_plan.{h,cpp}           计划层：镜像文件 + 设备分区表 → MtkFlashPlan（匹配/告警/大小）
新增 src/core/modes/mtk_preloader_fetch.{h,cpp} 网络获取 preloader（**默认关闭**；来源清单 + sha256 + 显式开启）
改  src/core/modes/mtk_payload.{h,cpp}         sendPayload 改用 DaSelection 发 DA1；新增 da1Sync//bootToDa2；EMI 发送（LEGACY 帧）
改  src/core/modes/mtk_emmc.{h,cpp}            按需补 LEGACY 帧（EMI/read_flash_info 的复用点）
改  src/core/flash_tool.{h,cpp}                mtk-brom 通道：构建计划 → 逐分区（复用既有 runBromFlash 循环）
改  src/ui/flash_panel.cpp + 复用 src/ui/plan_preview_widget.{h,cpp}   ← Phase C 抽出的通用预览控件
测试：test_mtk_da_file / test_mtk_preloader_emi / test_mtk_flash_plan（新）+ test_mtk_payload / test_pipeline（扩）
```

## 4. DA 文件解析与选择（`mtk_da_file`）

```cpp
namespace mtkbrom {
struct DaRegion { quint32 fileOffset = 0; quint32 len = 0; quint32 startAddr = 0;
                  quint32 startOffset = 0; quint32 sigLen = 0; };
struct DaEntry   { quint16 magic = 0; quint16 hwCode = 0; quint16 hwSubCode = 0;
                   quint16 hwVersion = 0; quint16 swVersion = 0; quint16 pagesize = 0;
                   quint16 entryRegionIndex = 0; quint16 regionCount = 0; QList<DaRegion> regions; };
struct DaFile    { bool isV6 = false; quint32 count = 0; QList<DaEntry> entries;
                   QString banner; bool oldFormat = false; };
struct DaSelection { DaEntry entry; DaRegion da1; DaRegion da2;   // da1=region[1], da2=region[2]
                     QByteArray da1Bytes; QByteArray da2Bytes; bool isXmlForced = false; };
bool parseDaFile(const QByteArray &data, DaFile &out, QString *error);            // 纯函数
bool selectDaEntry(const DaFile &f, quint16 hwCode, quint16 hwVersion, quint16 swVersion,
                   DaSelection &out, QString *error);                             // 5 元组 + 版本过滤
}
```
**硬约束（全部来自实测）**：`magic==0xDADA`；`count_da` 与 EOF 交叉校验；0xD8/0xDC 探测与 `0x6C+count*size<=filesize` **交叉校验**（P4）；`m_buf+m_len<=filesize`；`m_start_offset` 原样；容忍 `m_len==0`/`hw_code==0`/`pagesize ∈{0,1}`；**不得**按内容去重 region（P6）；**必须**显式跳过零长度 region（不得"上传 0 字节后静默成功"）；`region[1]/[2]` 硬编码（`entry_region_index` 仅 IoT 分支）。

## 5. DA 两阶段执行流（LEGACY）

```
1. 枚举 → 打开 → connect（既有）→ getTargetConfig（既有）→ SLA/DAA 检查（既有，明确报错）
2. parseDaFile + selectDaEntry（hwCode 来自 TargetConfig；判不出 → 明确报错）
3. SEND_DA(da1.startAddr, da1.len, da1.sigLen, da1 切片) → JUMP_DA(da1.startAddr)
4. DA1 起来：读单字节 == 0xC0（否则报错）
4a. **存储信息交换**（dalegacy_lib.py:607-634）：读 NAND_INFO(4B) + id 数(2B) + id 表 + EMMC_INFO(4B)
    + 4×4B → 写 1B ACK → 读 3×1B；顺带定出存储类型（nand/emmc/nor，判定 :623-628）。**spec 初版漏写**
4b. **stage2 配置**（dalegacy_lib.py:228-296）：写 11 笔配置（bromver/blver/nor 片选模式+片选号/nand_acccon/
    bmtflag/bmtpartsize/force_charge/resetkeys/ext_clock/msdc_boot_ch）+ 按 hwcode 追加 → sleep(0.35)
    → 读 4B errorcode
5. EMI（**仅当 errorcode == 0xBC3**；0x0 = 不需要 DRAM 配置）：先读 4B + 16B draminfo + 回执 0xBC4
   + nand id 表，再 ENABLE_DRAM 0xE8 + emiver 分档 + 大端 + checksum + M_EXT_RAM_* 回包
   EMI 数据 = 用户提供的 preloader / 固件目录自动导入 / （默认关闭的）网络获取
6. boot_to(DA2)：>I addr → >I size → >I 0x1000 → ACK → 分块写（每块 ACK）→ sleep → ACK → ACK
   注意：**LEGACY 保留 DA2 尾部签名**
7. read_flash_info（DA2 存活判据）→ DaStorage::listPartitions（既有）
8. 计划校验（分区存在 + 大小）→ 逐分区 emmcWrite（既有）
9. FINISH 0xD9 收尾；失败语义照 Phase B/C（即停/不复位/错误含分区名与已写字节/进度回调）
```

## 6. 计划层（`mtk_flash_plan`）

- **来源**：用户选一组镜像文件；**匹配对象是设备分区表**（`listPartitions`，来自设备）。
- **匹配**：镜像文件名去扩展名 vs 分区名，大小写不敏感；MTK 特有的**带后缀 preloader**（`preloader_k65v1_64_bsp.bin` → 前缀匹配 `preloader`）→ 命中即记 warning 说明用了哪条规则。
- **校验**：`镜像 > 分区` → 逐条 warning（放不下）；`<` → 一条汇总；分区表查不到 → 逐条 warning（跳过）；**两类不匹配只 warning + 继续**（沿用 Phase C 口径）。
- **排序**：按设备分区表顺序；`totalBytes` = 匹配到的镜像字节和（进度分母）。
- **预览**：复用 `src/ui/plan_preview_widget.{h,cpp}`（表格 + 告警 + "未验证"勾选门控）。

## 7. EMI / preloader 两路径

| 路径 | 行为 |
|---|---|
| ① 用户提供/自动导入 | 显式 `preloaderPath` 优先；否则在所选固件目录内找 `preloader*.bin`（大小写不敏感）——**唯一命中才用**；多个候选 → **不猜**，列候选并告警后跳过（D1 不做交互选择） |
| ② 网络获取 | **默认关闭**；显式开启（UI 勾选或参数）→ 按**用户配置**的来源清单（`mtk_preloader_sources.json`，**默认不内置**；含 URL + 期望 sha256）下载 → 校验 → 缓存到本地；**日志写清来源与风险**（错误的 preloader 有砖机风险） |
| 都不可用 | **分两分支**（见下）：`errorcode == 0` → 确实不中止（info 级日志，因为 DA1 根本没要 DRAM 配置）；`errorcode == 0xBC3`（DA1 明确索取 DRAM 配置）→ **没有 EMI 必须中止** |

> **更正（初版错）**：初版把"都不可用"写成一律不中止。上游是 request-driven：
> 只有 `errorcode == 0xBC3` 才进 EMI 段（`dalegacy_lib.py:294-297`），**此时若 `daconfig.emi is None`
> 上游直接中止**——`self.error("Preloader needed due to dram config.")` + `port.close(reset=True)`
> + `return False`（`dalegacy_lib.py:399-402`）。继续只会让 DA2 起不来（无 DRAM 配置），
> 所以这里**必须中止**，不能"如实 warning 后继续"。

## 8. 验证策略与诚实边界

| 层 | 内容 |
|---|---|
| **真 DA 样本** | 6 个真实 AllInOne（161 条目）硬断言：头部标记/`count_da`/`magic==0xDADA`/逐条目 5 元组字段/**region 5 字段**/选择结果与版本过滤；**与 `reference/mtk-samples/parse_da.py` 独立解析器逐字段对拍** |
| **真 preloader** | 真实 `preloader.bin` 的 EMI 提取（LEGACY 切片 + 边界）；EMI 算法在 832 个真实 preloader 上的统计特征作注释证据 |
| **mock 逐帧** | 扩展既有 `MockUsbChannel`：断言 SEND_DA/JUMP_DA/`0xC0`/EMI 各档/`boot_to` 的分块与 ACK 时序/**LEGACY 保留签名**这一字节范围差异 |
| **计划层** | 合成设备分区表 + 合成镜像 → 匹配/告警/大小；真实 `MT6765_Android_scatter.txt` 作分区名参照（50 分区） |
| **合成 DA** | 0xD8 老格式、`nregions>=10`（P4 探测风险）、`count_da` 损坏、`m_buf+m_len` 越界 |

**诚实边界（写进交付物）**
- **真机全链未验证**（USB 时序、真机 ACK、DA2 是否接受、EMI 分档的真机行为）。
- **0xD8 老格式无任何真实样本**（6 文件全 0xDC）→ 只合成夹具，**不得标"已验证"**。
- **PMTv1/PMTv3 解析无真实样本**（公开仓库不存；分区名靠设备实读的 `listPartitions`，不走本地 PMT 解析）。
- **EMI 的 MMM 分支只有 3/832 真实命中**；**LEGACY 的 EMI 触发条件已核实 = request-driven**：
  仅 `errorcode == 0xBC3` 时进 EMI 段（`dalegacy_lib.py:294-297`），本层按**同一顺序**发
  （0xBC3 后的 4B/16B draminfo/0xBC4 回执/nand id 表 → `ENABLE_DRAM 0xE8` → emiver 分档 → EMI →
  checksum → `>I 0x80000001` → `M_EXT_RAM_RET/TYPE/CHIP_SELECT/SIZE`）。
- **存储信息交换 / stage2 配置写入 / `read_flash_info` 全段读 真机未验证**（mock 逐帧已验证）。
- **NAND/NOR 存储类型明确拒绝**（D1 只支持 eMMC；BMT 的 nand 分支未实现）。
- **SLA/DAA 签名不做**（仍明确报错）；**BROM 漏洞利用（kamakiri/carbonara 等）不做**；DA 二进制**提取/生成**不做。
- 内置芯片表的覆盖度 = 转写时 mtkclient 的版本；表外芯片 → **明确报错**（不猜代际）。

## 9. 明确不做（本期）

- ~~XFlash / XML 两代协议（D2/D3）~~ —— **XFlash / XML 两代已于 2026-09-16 交付**（见 `2026-09-16-mtk-xflash-xml-design.md`）；
  `DOWNLOAD`/`UPLOAD`/`FORMAT_PARTITION`（实测仅定义零调用）**仍不做**。
- repartition / 格式化 / UFS 命令族 / seccfg 解锁 / NV 项 / RPMB。
- DA 二进制提取/生成、签名绕过。

## 10. 许可

- 本项目 **GPLv3**；`mtkclient` 为 **GPL-3.0** → **兼容**，芯片表可整表转写，但**必须**：`tools/gen_mtk_chip_table.py` 保留生成逻辑与**来源 URL + commit**，生成的头文件顶部注明"转写自 bkerler/mtkclient（GPL-3.0）"。
- 真样本（DA/preloader/scatter/vbmeta）在 `reference/mtk-samples/`（**gitignored**，不进提交/不进构建）——与 `reference/samsung-samples/` 同款纪律。

## 11. 交付清单

- [x] `mtk_da_file.{h,cpp}` + `test_mtk_da_file`（真样本 + 对拍 + 合成边界）
- [x] `mtk_chip_table.{h,cpp}` + `tools/gen_mtk_chip_table.py`（含出处声明）
- [x] `mtk_preloader_emi.{h,cpp}` + `test_mtk_preloader_emi`（真 preloader）
- [x] `mtk_preloader_fetch.{h,cpp}`（默认关闭 + 校验 + 日志）
- [x] `mtk_flash_plan.{h,cpp}` + `test_mtk_flash_plan`
- [x] `mtk_payload` 改造（DA1 正确上传 + `0xC0` + EMI + `boot_to`）+ `test_mtk_payload` 扩
- [x] `flash_tool` mtk-brom 通道 + `flash_panel` 入口（DA/镜像/preloader 选择）+ 预览复用
- [x] 文档：功能清单（"待接线"→已交付 + 诚实边界）、README、配套事实报告归档

**无遗留 `[ ]` 项**：XFlash / XML 两代**已于 2026-09-16 交付**（见 `2026-09-16-mtk-xflash-xml-design.md`），不在本清单内。
交付时的离线证据与未验证边界见 `docs/superpowers/specs/mtk-brom-facts.md` §10。
"真机全链未验证"不是遗留项，是**本期的验收口径**（真机归持机人）。
