# Phase B 协议事实速查（EDL / Sahara / Firehose + 计划来源实证）

核查日期: 2026-09-13。配套 spec: `2026-09-13-oppo-flash-phase-b-design.md`。
参照物：`reference/qdl/`（Linaro qdl，C，libusb+libxml2，**与本项目同构，多数结论以它为主证据**）、`edl/`（bkerler/edl 子模块，Python，3.52.1-428-g51e1102）、`reference/FirmwareKit.Oppo/`（C#）。两实现不一致处已并列。

## 1. `program` 命令

- **出站属性恒发四个**：`SECTOR_SIZE_IN_BYTES`、`num_partition_sectors`、`physical_partition_number`、`start_sector`（`reference/qdl/src/firehose.c:1021-1025`；`edl/edlclient/Library/firehose.py:491-499` 同四个、**不发 filename**）。`filename` 仅非空时发（`firehose.c:1029-1030`）；NAND 加 `PAGES_PER_BLOCK`/`last_sector`（:1032-1035）。
- **入站（rawprogram XML）必需属性**（缺一即丢条目）：`SECTOR_SIZE_IN_BYTES`/`filename`/`label`/`num_partition_sectors`/`physical_partition_number`/`start_sector`/`file_sector_offset`（`reference/qdl/src/program.c:254-275` + `src/util.c:74-88`）；`sparse` 用 `attr_as_bool`，缺失不算错（`src/util.c:108-125`）。
- **数据面**：收到 `<program>` 的 ACK 后，按 `chunk = MIN(max_payload_size/sector_size, left)` **个扇区**为单位**裸写 OUT 端点**（无长度前缀、无帧头）（`firehose.c:1043-1128`，块算法 :1080）；文件尾补零到扇区边界（`firehose.py:512-515`、`firehose.c:1096-1097`）。
- **ZLP**：bkerler 每块后追一次 0 字节写（`firehose.py:517-519` → `Connection/usblib.py:347-359`）；qdl 仅在 `len % out_maxpktsize == 0` 时补（`src/usb.c:548-553`）。SPINOR 写超时放宽到 60s（`src/firehose.c:952,968-972,1108`）。
- **数据发完必须再等一个 ACK**（`firehose.c:1132-1137`；`firehose.py:455-466/522-533`）。
- **sparse 一律主机侧解包**，不存在原样透传：bkerler 用 QCSparse 读头、以**去 sparse 后的 getSize()** 算 `num_partition_sectors`（`Library/sparse.py:53-76` + `firehose.py:475-486`），数据走 `sparse.read()`（:506-507）；qdl 在加载期把 sparse 拆成 per-chunk program op，**只下发 RAW/FILL**，`DONT_CARE` 不下发但 `start_sector` 仍前移（`src/program.c:139-170`）。

## 2. `patch` 命令

- 条目 8 属性（`reference/qdl/tests/data/patch0.xml:6-31`）：`start_sector`/`byte_offset`/`physical_partition_number`/`size_in_bytes`/`value`/`filename`/`SECTOR_SIZE_IN_BYTES`/`what`（qdl 视全部为必需 `src/patch.c:41-48`）。
- **出站不发 `what`**（只进调试日志）（`firehose.c:1408,1410-1424`；`firehose.py:427-443` 同）。
- 语义 = **磁盘偏移改写**：`(ppn, start_sector, byte_offset)` 定位 + `size_in_bytes` 字节 + 小端 `value`。
- **`filename == "DISK"` 才是"下发设备"**；真实文件名（`gpt_main0.bin` 等）是给 QFIL/Trace32 离线改 bin 用的，qdl 与 bkerler **都跳过**（`firehose.c:1405-1406`、`firehose_client.py:977-978`）。
- `value`/`start_sector` 允许表达式（`NUM_DISK_SECTORS-6.`、`CRC32(2,4096)`、`CRC32(NUM_DISK_SECTORS-5.,4096)`）**主机不解释、原样下发**（`firehose.c:1420-1421`；qdl 对 `<program>` 的 start_sector 同款，注释明确"解析会写错地址" `firehose.c:874-879`）。唯一做主机侧替换的是 bkerler 的 **qfil** 分支（`firehose_client.py:951-958`）——非刷机路径。
- `what` 取值：仓库内样本全是英文描述句；grep 不到 `what="GPT"/"ZERO"/"Disk"`；两实现都不按 `what` 分派 → **无法证实存在必须主机侧解释的取值**。

## 3. configure / getstorageinfo / erase / setbootablestoragedrive

- **configure 属性**：qdl 发 `MemoryName`/`MaxPayloadSizeToTargetInBytes`/`Verbose=0`/`ZlpAwareHost=1`/`SkipStorageInit`（`firehose.c:510-515`）；bkerler 另加 `AlwaysValidate`/`MaxDigestTableSizeInBytes=2048`/`SkipWrite`（`firehose.py:897-909`）。
- **MemoryName 大小写不一致**：qdl 全小写 `emmc|ufs|nand|spinor|nvme`（`src/util.c:127-133`）；bkerler 默认 `"eMMC"`/`"UFS"`（`firehose.py:175`）。
- **必须等响应，且常需两轮**：响应带 `MaxPayloadSizeToTargetInBytesSupported` 则用它重发一次（`firehose.c:462-484,520,534-548`；`firehose.py:917-935` 同款递归重试）；失败文本 `Not support configure MemoryName eMMC` → 换 UFS 重试（`firehose.py:936-940`）；`Only nop and sig tag can be` → 小米 EDL 鉴权（:941-956，本项目不覆盖）。
- **getstorageinfo**：`<getstorageinfo physical_partition_number="N"/>`（qdl 传 LUN `firehose.c:1907-1908`；bkerler 固定 "0" `firehose.py:1294`）。返回走 `<log>`：qdl 只认 JSON `storage_info` 的 `total_blocks`/`block_size`（`firehose.c:1874-1884`）；bkerler 另认 `num_physical`/`page_size`/`mem_type`/`prod_name` 与文本键 `num_physical_partitions`/`SECTOR_SIZE_IN_BYTES`（`firehose.py:1260-1275,1317-1331`）、属性 `bNumberLu`（:1297-1300）。"Failed to open the SDCC Device" 时改判 UFS（:1340-1345）。
- **erase**：`<erase SECTOR_SIZE_IN_BYTES physical_partition_number [num_partition_sectors start_sector] [slot]/>`，省略 start/count = 整 LUN 擦，发完等 ACK（`firehose.c:611-628,636`）。**program 前不隐含 erase**：qdl 的 erase 只来自 rawprogram XML 的 `<erase/>` 标签（`src/program.c:343-344`，独立 op `firehose.c:1783-1787`）。bkerler 先查 `supported_functions` 是否含 erase，不含则退化为"发 program + 全 0 数据流"（`firehose.py:600-657`）。
- **setbootablestoragedrive**：`<setbootablestoragedrive value="N"/>` + 等 ACK（`firehose.c:1561-1569`；`firehose.py:403-405`）。`value` = **bootloader 分区所在 LUN**：从 label ∈ {xbl, xbl_a, sbl1} 的 program 条目取 `physical_partition_number`（`src/program.c:423-451`，调用点 `src/qdl.c:969-975`；bkerler 同标签集合、多候选即报错 `firehose_client.py:141-153`）。

## 4. UFS / 多 LUN

- **`rawprogramN.xml` 的序号 == 该文件内条目的 `physical_partition_number`**（`reference/qdl/tests/data/rawprogram1.xml:5-8` 四条全 ppn=1；bkerler 生成器同规则 `edl/edlclient/Library/gpt.py:426-427,441,455,468`）。**刷写实现只读属性、不读文件名**（`edl/edlclient/Library/firehose_client.py:950-962`、`reference/qdl/src/program.c:40-42`）。
- **LUN 数来源**：bkerler 靠 getstorageinfo（`bNumberLu` 或 `num_physical` → maxlun；默认 99）或 `--lun`；eMMC 只取 [0]，UFS 取 `range(0,maxlun)`（`firehose.py:878-888,1297-1323`）。qdl 不枚举 LUN，逐条目取 `physical_partition_number`（`src/program.c:259`）。
- **sector size 是逐条目量**：qdl 每个 read/program op 可带自己的 `SECTOR_SIZE_IN_BYTES`（`firehose.c:1022,1198`，来源 `src/program.c:254`），缺省才用全局值（:605,984）；全局值靠**对 LUN0 先试 4096 再试 512** 的读探测（:526-529,566-588）。
- bkerler 侧是单一全局 `cfg.SECTOR_SIZE_IN_BYTES`：eMMC=512、其他=4096（`firehose.py:891-895`），可被 `--sectorsize`/getstorageinfo 的 `page_size` 覆盖（:1272-1273,1326-1327）。
- 仓库内测试样本 `SECTOR_SIZE_IN_BYTES` 一律 4096（单样本，不能证明"各 LUN 相同"）。

## 5. 计划来源实证（`.ops` / `.ofp` 解包产物里有什么）

- **`.ops` 解包产物不含 `rawprogram*.xml`/`patch*.xml`** —— RoE Wiki 的 OnePlus 6（enchilada）EDL 实录（2024-06，含 MD5，已删除页面的历史版本 `?rev=1721601791`）明确分四步：Decrypt → **Generate/Create XMLs** → Flash (`edl qfil rawprogram#.xml patch#.xml`，注明 "for each xml (there's 6)"）；XDA OnePlus 8T 帖原话 "lack of rawprogram0.xml * patch0.xml"。**XML 是生成的，不是解出来的**。
- **但 `<Program{N}>` 块的内容本身就是 rawprogram 内容**：chayleaf 脚本（bkerler/edl issue #432 评论）把 `<Program>`/`<Patch>` 分组切块导出为根标签 `<data>`/`<patches>` 的 XML，`edl qfil` 直接刷机成功（OP6/6T 实测）。→ `settings.xml` 的 Program 块与 rawprogram 条目同构。
- **`start_sector` / `physical_partition_number` 在元数据里的存在性是"强推断"**：`edl` 对每条带 `filename` 的 `<program>` **硬读**这两个属性（`firehose_client.py:950-962`，缺失即 `TypeError`），而"原样导出后刷机成功"意味着它们存在。`num_partition_sectors` **未证实**（`edl` 不读它，按文件大小自算 `firehose.py:473-500`；qdl 读且缺失即报错 `program.c:40-42`）。
- **`FileOffsetInSrc`/`SizeInByteInSrc`/`SizeInSectorInSrc` 只是包内偏移与长度**（`InSrc` = "in source (package)"）：打包器按包内累计位置**重算**它们（`reference/oppo_decrypt/opscrypto.py:503-513,533-544`），解包器按 `×0x200` seek 切片（`:594-597,616-618,631-633`）；另一实现注释写 "offset **in the archive**"（`reference/FirmwareKit.Oppo/FirmwareKit.Oppo.Core/Models/OppEntry.cs:15-19`）。**绝不能当设备侧扇区**。
- **唯一有开源实证的几何来源 = 包内 `gpt_main{N}.bin` 的 LBA 表**：OplusEdlTool（`https://github.com/salokrwhite/OplusEdlTool`）用 `Services/RawProgramXmlProcessor.cs:101-108` + `GptParser.cs:31-115` 回填 `start_sector`/`num_partition_sectors`/`start_byte_hex`/`size_in_KB`；社区一手清单确认 `.ops` 内含 `gpt_main0-5.bin`/`gpt_backup0-5.bin`。**`patch{N}.xml` 不能由 GPT 推出**（是 GPT 头定点修补 `NUM_DISK_SECTORS-*`，见 `reference/qdl/tests/data/patch1.xml`）。
- **未解矛盾**：OplusEdlTool v2 解出 `.ops` 后**硬要求**目录里已有 `rawprogram*.xml` 否则报错退出，且它自己不生成（`Services/OpsDecryptor.cs:21-100` 只解载荷 + 写 `settings.xml`；`MainWindow.axaml.cs:715-740` 找不到即 `NoRawprogramFound`）→ 疑为新老世代差异，**未证实**。这支持"XML 优先、元数据回退"的顺序。
- **`.ofp`-QC 侧无证据**：其元数据分组穷举只有 `Sahara`/`Config`/`Provision`/`ChainedTableOfDigests`/`DigestsToSign`/`Firmware`（`reference/oppo_decrypt/ofp_qc_decrypt.py:340-347`；`FirmwareKit.OfpReader/Parsers/OfpQcParser.cs:43-71` 结构一致），**没有 `Program`/`Patch` 分组**；产物里是否有可用 XML **待真包确认**。
- ⚠️ **本项目文档需更正**：`docs/superpowers/specs/oppo-format-notes.md:53` 称"rawprogram/patch XML 在 Program/UFS_PROVISION 区域" —— 无证据支持。（Phase B 收尾已在该文件就地更正，见其"更正（Phase B 核查）"条；原行号即本清单触发更正的那一行。）

## 6. 本项目 `EDLHandler` 现状（5 处既有缺陷，全部非本次引入）

1. `sendFirehoseXml`/`recvFirehoseResponse` 在 XML 前加/响应前丢 **4 字节长度前缀**，两参照都是**裸 XML**（`src/core/modes/edl_handler.cpp:456-461,489-491`）。
2. `waitFirehoseDone` 的 ACK 判定**恒假**：合法 `<response value="ACK"/>` 命中它的 `continue` 分支 → 只会循环到超时返回 false（`edl_handler.cpp:494-510`）⇒ **现有 EDL 写入路径从未成功过**。
3. `<configure>` 只发**不读响应**（两参照都等：`firehose.py:917`、`firehose.c:520`）（`edl_handler.cpp:634-648`）。
4. `<program>` 缺 `physical_partition_number`（`edl_handler.cpp:791-800`）；`writeRaw` 发完 XML **完全没有镜像数据推送**（无分块/ZLP/扇区对齐补零，:802-814）。
5. `<read>` 用错属性名 `num_sectors`（参照 `num_partition_sectors`）、缺 ppn、多带 filename（`edl_handler.cpp:716-725`）。

## 7. 构建接入

- 主程序源 `file(GLOB_RECURSE "src/*.cpp" "src/core/*.cpp" "src/ui/*.cpp")`（`CMakeLists.txt:107-111`）→ `src/core/edl/*.cpp` **自动进主程序**；无 `CONFIGURE_DEPENDS` ⇒ 新增文件后需重跑一次 cmake 配置。
- 测试源显式列举（`CMakeLists.txt:266-295` 等）；多源编入同一测试目标的范例：:318-321（mtk_brom+mtk_emmc）、:340-353（test_pipeline 九个源含 edl_handler.cpp）；libusb 按名补链（:367-371）。

## 8. Phase B 实现期口径（落地时确认/裁定的口径，冲突时以本节为准）

1. **ZLP 一条规则、一处责任**：传输层 `write()` **一律不补** ZLP（它无法区分命令帧与数据块，补了会让数据块被补两次）；**数据块**的 ZLP 由会话数据面负责（`edl_session.cpp` 的 `writeRaw`，规则 `len % maxPacketSize() == 0` → 追加一次 0 字节写，`reference/qdl/src/usb.c:548-553`）；**命令帧**的 ZLP 由 `firehoseSendCommand` 负责。三处注释必须一致（Phase B 落地前曾出现"两处注释互相推诿、实际都没做"的悬空责任）。
2. **`read(timeoutMs == 0)` = 非阻塞轮询**（drain 语义）：libusb 的 `timeout = 0` 是**无限等待**（`libusb sync.c`："For an unlimited timeout, use value 0"），故 libusb 传输层必须把它换算成 ≥1ms（bkerler 同款：`edl/edlclient/Library/Connection/usblib.py:380-381` 的 `if timeout == 0: timeout = 1`）。
3. **DONT_CARE 处**：qdl 跳过不发（per-chunk op 模型，`reference/qdl/src/program.c:139-170`），bkerler 补零（`sparse.py:148-151` 的 `0xCAC3 → 零填充`）。本项目选**补零**（一条目一 `<program>`；`startSectorExpr` 条目主机侧无法算子区间；落盘内容 == 该 sparse 镜像对应的 raw 内容），代价是 DONT_CARE 区也会写入（带宽换确定性）。
4. **`listPartitions` 给不出分区名**：Firehose 无枚举分区名的命令，逐 LUN 的 `getstorageinfo` 只能给 `lun<N>` 与几何 —— 分区名必须以刷写计划/GPT 为准；UI 侧依赖"镜像 basename ↔ 分区名"匹配的旧路径（EDL 目录批量刷写）因此失效，已改为明确报错并指路刷写计划。
5. **重枚举契约**：`waitReenumerate` 调用前须已 `close()`（不替调用方 close，fail-closed）、返回 true 时设备已重新 `open()`、首试前先等一个轮询间隔（3s，既有实现同 `msleep(3000)`）、总预算 45s（= 3s×15）。
6. **sha256 口径待真包确认**：`.ops` 元数据的 `Sha256` 若实指**包内文件**（含 sparse 头）而非展开后镜像，sparse 条目会一致地报不符 —— 失败文案已按"计算值/期望值"如实打印，不称"文件损坏"；真包验证时优先确认此口径。
