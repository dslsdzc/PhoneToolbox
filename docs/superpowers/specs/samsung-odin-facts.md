# Heimdall 集成 — 参照源码 + 真实 PIT 样本盘点报告

日期：2026-09-14
范围：只给事实（路径/行号/URL/校验和），不含实现方案。
所有克隆均在 `reference/`（已在 `.gitignore`），只读盘点，未修改任何上游文件。

---

## 1. Heimdall（主参照）

| 项 | 值 |
|---|---|
| 仓库 | `https://github.com/Benjamin-Dobell/Heimdall.git` |
| 本地路径 | `/home/DslsDZC/PhoneToolbox/reference/heimdall` |
| 克隆方式 | `git clone --depth 1`（单分支） |
| HEAD commit | `3997d5cc607e6c603c6e7c0d07e42e9868c62af2`（2021-03-14，"Setup GitHub actions"） |
| 上游分支 | **只有 `master` 一个分支**（`git ls-remote --heads` 确认）；tag 最高到 `v1.4.2` |
| 版本字符串 | `v1.4.2` — `heimdall/source/Interface.cpp:45` |
| 体积 | 小（含 `Win32/Drivers/zadig.exe` 与 macOS kext 二进制） |

### 1.1 许可证结论

`LICENSE` 全文为标准 **MIT**：

```
Copyright (c) 2010-2017 Benjamin Dobell, Glass Echidna
```

`README.md` 亦自述 "It is licensed under the MIT license (see LICENSE)"；维护方为 Glass Echidna。
`libpit/source/libpit.h:1-19`、`heimdall/source/*.h` 每个文件头都重复了同一段 MIT 声明。

**结论**：MIT 是宽松许可，允许复制/修改/再分发，只需保留版权与许可声明。即"引用格式事实"之外的复用（例如直接移植 `libpit` 的解析逻辑）在许可上是被允许的，唯一义务是保留版权声明。这比 `mtkclient`/`edl` 两个 GPLv3 子模块宽松得多。

### 1.2 协议实现关键文件清单

`heimdall/source/` 下（每个文件职责一句话）：

**核心/传输层**
- `BridgeManager.h` / `BridgeManager.cpp` — 全协议核心：libusb 设备枚举与接口抢占、VID/PID 表、`ODIN`/`LOKE` 握手、会话开始/结束、PIT 收发、文件传输时序与超时参数。
- `Interface.cpp` / `Interface.h` — CLI action 注册表、打印辅助、`Interface::PrintPit()`（PIT 人类可读打印器）、版本字符串。
- `Arguments.cpp` / `Arguments.h` — 命令行参数模型（`FlagArgument` / `StringArgument` / `UnsignedIntegerArgument`）。
- `Utility.cpp` / `Utility.h` — 杂项辅助。

**PIT 动作**
- `DownloadPitAction.cpp/h` — 把设备上的 PIT dump 成**本地文件**（`FileOpen(outputFilename,"wb")` + `fwrite`）。
- `PrintPitAction.cpp/h` — 打印 PIT：既可读本地 `.pit`，也可先 dump 设备再打印（两条路径都走 `PitData::Unpack`）。
- `DetectAction.cpp/h`、`InfoAction.cpp/h` — 设备探测与信息输出。

**会话/控制包**
- `Packet.h`（基类）、`OutboundPacket.h` / `InboundPacket.h`（打包/解包基类，含字节序处理）。
- `ControlPacket.h` — 控制包类型枚举（见 1.4）。
- `ResponsePacket.h` — 响应包类型枚举。
- `SessionSetupPacket.h` — **实际生效的**会话设置请求枚举。
- `SessionSetupResponse.h` — 会话设置响应（`GetResult()`）。
- `SetupSessionPacket.h` / `SetupSessionResponse.h` — ⚠️ **死代码**，见 1.4 警告。
- `BeginSessionPacket.h`、`DeviceTypePacket.h`、`TotalBytesPacket.h`、`FilePartSizePacket.h`、`EnableTFlashPacket.h` — 各请求的具体包。

**文件传输包**
- `FileTransferPacket.h` — 请求枚举。
- `BeginDumpPacket.h`、`DumpPartFileTransferPacket.h`、`FlashPartFileTransferPacket.h`、`FlashPartPitFilePacket.h`、`EndFileTransferPacket.h`、`EndPhoneFileTransferPacket.h`、`EndModemFileTransferPacket.h`、`EndPitFileTransferPacket.h`。
- `ReceiveFilePartPacket.h`（入站 PIT 分片）、`SendFilePartPacket.h`（出站数据片）、`SendFilePartResponse.h`、`DumpResponse.h`。
- `PitFilePacket.h`、`PitFileResponse.h`（响应携带 PIT 文件长度）。
- `EndSessionPacket.h`。

**PIT 解析（独立库）**
- `libpit/source/libpit.h`、`libpit/source/libpit.cpp` — PIT 结构体 + `Unpack()` / `Pack()`，与 Heimdall 主程序解耦。

> 注意：`heimdall-frontend/` 是 Qt GUI，`libpit/` 可独立编译；二者与协议实现无关。

### 1.3 PIT 结构体定义（`libpit/source/libpit.h` + `libpit.cpp`）

**文件头**（`PitData`，`libpit.h:252-279`；解析在 `libpit.cpp:88-115`）：

| 偏移 | 类型 | Heimdall 字段名 | 常量/说明 |
|---|---|---|---|
| `0x00` | u32 | magic | `PitData::kFileIdentifier = 0x12349876`（`libpit.h:258`） |
| `0x04` | u32 | `entryCount` | 条目数 |
| `0x08` | u32 | `unknown1` | 见 1.3.1 |
| `0x0C` | u32 | `unknown2` | 见 1.3.1 |
| `0x10` | u16 | `unknown3` | 见 1.3.1 |
| `0x12` | u16 | `unknown4` | |
| `0x14` | u16 | `unknown5` | |
| `0x16` | u16 | `unknown6` | |
| `0x18` | u16 | `unknown7` | |
| `0x1A` | u16 | `unknown8` | |
| `0x1C` | — | 条目表开始 | `kHeaderDataSize = 28`，注释 `// Entries start at 0x1C` |

- **头大小 28 字节**：`PitData::kHeaderDataSize = 28`（`libpit.h:259`）。
- **条目大小 132 字节**：`PitEntry::kDataSize = 132`（`libpit.h:49`）。
- **字节序**：**小端**。`UnpackInteger` / `UnpackShort`（`libpit.h:281-303`）在 `WORDS_BIGENDIAN` 下做翻转，其余直接 `data[o] | data[o+1]<<8 | ...` → 明确 LE。
- **填充常量**：`kPaddedSizeMultiplicand = 4096`（`libpit.h:260`）；`GetPaddedSize()`（`libpit.h:364-373`）把 `28 + count*132` 上取整到 4096 的倍数——**这是传输用的 padded size，不是文件实际大小**（见 3.3）。

**条目**（`PitEntry`，`libpit.h:43-250`；解析在 `libpit.cpp:116-153`）：

| 条目内偏移 | 类型 | 字段名 | 语义枚举（`libpit.h:55-80`） |
|---|---|---|---|
| `+0` | u32 | `binaryType` | `kBinaryTypeApplicationProcessor=0`（AP）/ `kBinaryTypeCommunicationProcessor=1`（CP） |
| `+4` | u32 | `deviceType` | `kDeviceTypeOneNand=0` / `kDeviceTypeFile=1`（FAT）/ `kDeviceTypeMMC=2` / `kDeviceTypeAll=3` |
| `+8` | u32 | `identifier` | 分区 ID |
| `+12` | u32 | `attributes` | `kAttributeWrite=1`、`kAttributeSTL=2`（`BML` 被注释掉，标注 `???`） |
| `+16` | u32 | `updateAttributes` | `kUpdateAttributeFota=1`、`kUpdateAttributeSecure=2` |
| `+20` | u32 | `blockSizeOrOffset` | 注释原文：*"Different versions of Loke (secondary bootloaders) on different devices interpret this differently"*（`libpit.h:162`） |
| `+24` | u32 | `blockCount` | 块数 |
| `+28` | u32 | `fileOffset` | 注释 `// Obsolete`（`libpit.h:93`） |
| `+32` | u32 | `fileSize` | 注释 `// Obsolete`（`libpit.h:94`） |
| `+36` | char[32] | `partitionName` | `kPartitionNameMaxLength = 32` |
| `+68` | char[32] | `flashFilename` | `kFlashFilenameMaxLength = 32`，注释 "USB flash filename" |
| `+100` | char[32] | `fotaFilename` | `kFotaFilenameMaxLength = 32`，注释 "Firmware over the air" |

`IsFlashable()`（`libpit.h:107-110`）= `strlen(partitionName) != 0`。
`PitData::GetDataSize()` = `kHeaderDataSize + entryCount * PitEntry::kDataSize`（`libpit.h:359-362`）。

#### 1.3.1 `unknown1..unknown8` 已被第二参照解析（重要）

`libpit` 把头部 8..27 字节读作 2×u32 + 4×u16 并全命名为 `unknownN`，`Interface::PrintPit()`（`Interface.cpp:212-219`）原样打印成 `Unknown 1: ...`。
**Thor 的 `PIT/PitData.cs:24-26` 给出了这些字节的真实含义**（实测 7 个真样本全部吻合，见 3.2）：
- 字节 `8..15` = ASCII 字符串 `"COM_TAR2"`（固定标签，对应 Heimdall 的 `unknown1`+`unknown2` 两个 u32）
- 字节 `16..23` = ASCII 平台名，NUL 填充（如 `"MSM8974"` / `"MTK6765"` / `"LSI3475"` / `"Mx-MDM"`，对应 Heimdall 的 `unknown3..unknown6` 四个 u16）
- 字节 `24..27` = `Reserved`，实测恒为 `0`（Heimdall 的 `unknown7`+`unknown8`）

> 即 Heimdall 把这 20 字节"读错了形状"（它能无损往返 Pack/Unpack，但语义被丢掉）。平台名对识别机型/SoC 有直接价值。

### 1.4 Odin 协议命令号与握手

**握手**（`BridgeManager.cpp:296-342`，`InitialiseProtocol()`）：
1. bulk OUT 发送 ASCII `"ODIN"`（4 字节，timeout 1000ms）；
2. bulk IN 期望收 ASCII `"LOKE"`（4 字节）；
3. 收到 4 字节且等于 `"LOKE"` → 协议初始化成功。

**控制包类型**（`ControlPacket.h:33-39`）：
`kControlTypeSession = 0x64`、`kControlTypePitFile = 0x65`、`kControlTypeFileTransfer = 0x66`、`kControlTypeEndSession = 0x67`。

**会话设置请求**（`SessionSetupPacket.h:33-41`，**生效的枚举**）：
`kBeginSession = 0`、`kDeviceType = 1`、`kTotalBytes = 2`、`kFilePartSize = 5`、`kEnableTFlash = 8`。

> ⚠️ **陷阱**：同目录另有 `SetupSessionPacket.h:36-38`（`kBeginSession=0`/`kDeviceInfo=1`/`kTotalBytes=2`），但它是**死代码**——它引用未定义的 `ControlPacket::kControlTypeSetupSession`，且 `grep` 全仓无任何 `.cpp` include 它。不要把它当成生效枚举。

**文件传输请求**（`FileTransferPacket.h:35-40`）：`kRequestFlash = 0x00`、`kRequestDump = 0x01`、`kRequestPart = 0x02`、`kRequestEnd = 0x03`。
`EndFileTransferPacket.h:38-41`：`kDestinationPhone = 0x00`、`kDestinationModem = 0x01`。

**PIT 包请求**（`PitFilePacket.h`，与文件传输同号）：`kRequestFlash=0x00` / `kRequestDump=0x01` / `kRequestPart=0x02` / `kRequestEndTransfer=0x03`。

**结束会话**（`EndSessionPacket.h:32-36`）：`kRequestEndSession = 0`、`kRequestRebootDevice = 1`。

**响应类型**（`ResponsePacket.h:31-38`）：`kResponseTypeSendFilePart = 0x00`，其余 0x64/0x65/0x66/0x67 与控制类型镜像。

**关键传输常量**：
- `ReceiveFilePartPacket.h:33`：`kDataSize = 500` —— PIT 分片大小（Thor 一致，见 2.2）。
- `BridgeManager.h:82-86`：`kDefaultTimeoutSend = 3000`、`kDefaultTimeoutReceive = 3000`、`kDefaultTimeoutEmptyTransfer = 100`。
- `BridgeManager.cpp:71-77`：`kFileTransferSequenceMaxLengthDefault = 800`、`kFileTransferPacketSizeDefault = 131072`、`kFileTransferSequenceTimeoutDefault = 30000`。
- `BridgeManager.cpp:530-532`（`BeginSession` 内，当设备返回非 0 默认包大小时改写）：`fileTransferSequenceTimeout = 120000`、`fileTransferPacketSize = 1048576`、`fileTransferSequenceMaxLength = 30`（注释：30 MiB/序列）。

**PIT dump 流程**（`BridgeManager.cpp:891-994` `ReceivePitFile()`）：
`0x65 req 0x01`（请求 dump）→ 读 `PitFileResponse` 拿 `fileSize` → 循环 `0x65 req 0x02 + partIndex(i)` 收 `ReceiveFilePartPacket`（每片 500）→ `0x65 req 0x03`（结束）→ 收校验响应。
`DownloadPitFile()`（`BridgeManager.cpp:986-1000`）只是它的日志包装。

**PIT 回写流程**（`BridgeManager.cpp:791-...` `SendPitData()`）：按 `GetPaddedSize()` 分配缓冲区，`PitFilePacket::kRequestFlash` 起头。

### 1.5 支持的设备 VID/PID

**只有 3 个 PID**，全是 2011 年前后的老机型：

- `BridgeManager.h:69-79`：`kVidSamsung = 0x04E8`；`kPidGalaxyS = 0x6601`、`kPidGalaxyS2 = 0x685D`、`kPidDroidCharge = 0x68C3`。
- `BridgeManager.h:59`：`kSupportedDeviceCount = 3`。
- `BridgeManager.cpp:65-69`：`supportedDevices[]` 数组（仅这 3 项）。
- `heimdall/60-heimdall.rules:1-3`：udev 规则，同样 3 个 PID（`6601` / `685d` / `68c3`）。
- `OSX/heimdall.kext/Contents/Info.plist`：macOS kext 有 3 个 PID 字典（`idVendor = 1256` 即 `0x04E8`；`idProduct` = `26113`/`26717`/`26819` 即 `0x6601`/`0x685D`/`0x68C3`），与上表一致。
  ⚠️ 上游笔误：第一个字典的键写作 `<key>0x6001</key>`，而内部 `idProduct` 是 `26113`(`0x6601`) —— 键与值不一致（不影响功能，仅记录）。

**覆盖范围（Heimdall 全仓）**：`heimdall-frontend/source/Packaging.cpp` 有 ustar TAR 解包（`ExtractTar()`，`:45`，链接 `zlib`），但**全仓无任何 MD5 校验代码**（`grep -i md5` 零命中），也**无 `.lz4` 支持**；不含 FUS/固件下载（无 samloader 类功能）。协议侧只做「连设备 → 收发 PIT → 刷写」，固件解密/下载不在范围内。

> **结论：Heimdall `master`(v1.4.2) 的 PID 白名单对现代三星机型完全不适用。** 上游唯一的官方续作是 **Thor**（见 2.2），它已放弃 PID 白名单，改为「VID 0x04E8 + USB 接口类 0x0A(CDC_DATA) + 有 bulk in/out 端点」的类匹配。`BridgeManager.cpp:60` 里也有同样的 `#define USB_CLASS_CDC_DATA 0x0A`，说明 Heimdall 自己的接口筛选逻辑本就基于 class。

### 1.6 测试数据：**没有**

对整仓 `find`（排除 `.git`）结果：
- 无任何 `*.pit` 文件；
- 无 `test/`、`tests/`、`sample/`、`fixtures/`、`data/` 目录；
- 无 `*.tar`、`*.tar.md5`、`*.lz4`、`*.img` 固件样本。

**Heimdall 仓库不含任何测试数据或 PIT 样本。** 因此 PIT 样本必须外部获取（见第 3 节）。

---

## 2. 第二参照

### 2.1 `Llucs/odin4` — 现代 C++ 重写（与 PhoneToolbox 同语言，**建议纳入参照**）

| 项 | 值 |
|---|---|
| 仓库 | `https://github.com/Llucs/odin4.git` |
| 本地路径 | `/home/DslsDZC/PhoneToolbox/reference/odin4-llucs` |
| 许可证 | **Apache-2.0**（`LICENSE` + `NOTICE`） |
| 体积 | 2.9M |
| HEAD | `5138665`（2026-09-09，dependabot 合并）——**仍在活跃维护** |
| 语言 | C++（CMake） |

**覆盖的管线步骤**（齐备度最高，含 FUS 下载之外的几乎全部）：
- **PIT 解析**：`src/core/odin_types.h:34-65`（见 2.3）；`tests/fuzz_pit.cpp` 有 PIT 模糊测试。
- **Odin 协议**：`src/usb/odin_protocol.cpp`、`src/protocol/thor_protocol.h`（后者实现了扩展命令集，文件头含完整的 LE/BE 转换宏）。
- **固件包处理**：`src/firmware/firmware_package.cpp`（`.tar` / `.tar.md5`，CLI `-b` 引导包 / `-a` AP 包）。
- **LZ4 解压**：自带 `lib/lz4/`（完整 lz4 + lz4frame + xxhash 源码）→ **支持直接处理 `.lz4`**。
- **USB 设备匹配**：`src/usb/usb_device.cpp`（libusb，带接口评分 `find_best_interface()`）。
- **`--check-only` 干跑**：可只校验不刷写（CLI）。
- **不含** FUS 固件下载：**全仓无任何网络代码**（对 `curl`/`http`/`socket` 的 grep 只命中许可证 URL）；`grep -i fus` 无命中。

**设备 PID 列表**（`src/usb/usb_device.h:18`，共 **6 个**，是 Heimdall 3 个的超集）：
```cpp
static constexpr std::array<uint16_t, 6> SAMSUNG_DOWNLOAD_PIDS{{0x6601, 0x685D, 0x68C3, 0x68EF, 0x4EEE, 0x4EEF}};
```
`SAMSUNG_VID 0x04E8`（`src/usb/usb_device.h:13`）。匹配策略是「PID 命中 +20 分」与「接口 class 为 CDC_DATA」并用打分取最优（`usb_device.cpp:239-265`，`is_known_download_pid()` 在 `:131`）。

### 2.2 `topjohnwu/samloader-rs` — 唯一带**许可证干净的真 PIT 测试数据**的参照（**建议纳入参照**）

| 项 | 值 |
|---|---|
| 仓库 | `https://github.com/topjohnwu/samloader-rs.git` |
| 本地路径 | `/home/DslsDZC/PhoneToolbox/reference/samloader-rs` |
| 许可证 | **Apache-2.0 + MIT 双许可**（`LICENSE`、`LICENSE-MIT`） |
| 体积 | 792K |
| HEAD | `4f47733`（2026-09-13，非常新） |
| 作者/渊源 | `pit/src/lib.rs:1-3` 版权行：`Copyright 2026 John "topjohnwu" Wu` / `Copyright 2021-2024 Henrik Grimler` / **`Copyright 2010-2017 Benjamin Dobell, Glass Echidna`** → **本模块是 Heimdall `libpit` 的 Rust 直系移植** |

**覆盖的管线步骤**（**唯一一站式覆盖下载 + 解密 + PIT + 刷写**）：
- **工作区版本** `2.1.0`（`Cargo.toml:11`），4 个 crate：`samloader-fus 2.0.0` / `samloader-pit 2.0.0` / `samloader-odin 2.0.0` + CLI `samloader`。
- **FUS 认证**：`fus/src/auth.rs`（`Aes128` + 内置 `AUTH_AES_KEY`，`decrypt_nonce()` 于 `:33`，`generate_client_nonce()` 于 `:48`，`auth:<nonce>:00000001` 签名格式于 `:74`）。
- **固件解密**：`fus/src/xml.rs:231` 按文件名分派 —— `.enc2` 走 logic-check 派生 key（`:242-243`），否则走 v4 key 路径；类型注释为 "128-bit key used for AES-128 decryption"（`xml.rs:212`）。
- **下载**：`fus/src/download.rs`（`reqwest` + cookies，多并行连接）。
- **PIT**：独立 crate `pit/`（完整解析 + 重打包）；CLI 有 `dump-pit`、`print-pit`。
- **刷写**：`odin/src/`（`odin.rs`、`flash.rs`、`packets.rs`、`firmware.rs`）；支持 **LZ4 帧头解析与流式解压**（`flash.rs:61,84-111`）、**`.tar.md5` footer 校验**（`verify_md5_footer`，`lib.rs:31`）、**repartition**（`flash.rs:143`，错误提示见 `error.rs:253`）；USB 后端 `nusb` 与 `vcom` 双选（`odin/src/usb/`，另有 `mock.rs` 与 `serial.rs`）。
- **`test-data/`（关键）**：含 2 个**真 PIT 样本** + `history.xml` + `version.xml`（后两者是 FUS 响应夹具）。

**PIT 结构定义**（`pit/src/lib.rs:27-33`）：`FILE_IDENTIFIER = 0x12349876`、`HEADER_DATA_SIZE = 28`、`DATA_SIZE = 132`、三个名字段各 `32` 字节 —— 与 Heimdall `libpit` 常量逐一对齐。

**`DeviceType` 枚举扩展了 Heimdall**（`pit/src/lib.rs:79-93`）：`OneNand=0` / `File=1` / `MMC=2` / `All=3` / **`UFS=8`**（注释：UFS 扇区 4096 字节，MMC 512 字节）。`partition_size()`（`:155-160`）按 `MMC→512B`、`UFS→4096B` 换算分区字节数。
`Attribute` 位域（`:102-113`）：`write`=bit0、`stl`=bit1、其余 30 位 `#[skip]`。
`UpdateAttribute` 位域（`:116-128`）：`fota`=bit0、`secure`=bit1、其余 30 位 skip。

### 2.3 头部字段语义：**三份现代实现独立收敛**（取代 Heimdall 的 `unknown1..unknown8`）

Heimdall 把头部 8..27 字节读成 2×u32 + 4×u16 并全部命名为 `unknownN`。以下**三个互相独立的实现**（Thor / samloader-rs / odin4）给出**完全一致**的真实拆分：

| 偏移 | Heimdall | Thor (`PIT/PitData.cs:24-26`) | samloader-rs (`pit/src/lib.rs:181-186`) | odin4 (`odin_types.h:55-61`) |
|---|---|---|---|---|
| `0x08`, len 8 | `unknown1`+`unknown2`（2×u32） | `Unknown`（`ReadString(8)`） | **`com_tar2`** | **`com_tar2[8]`** |
| `0x10`, len 8 | `unknown3..unknown6`（4×u16） | `Project`（`ReadString(8)`） | **`cpu_bl_id`**（"CPU or bootloader target hardware tag"） | **`cpu_bl_id[8]`** |
| `0x18`, 2B | `unknown7`（u16） | —（并入 `Reserved` int32） | **`lu_count`**（"Logical partition units count"） | **`lu_count`**（u16） |
| `0x1A`, 2B | `unknown8`（u16） | `Reserved`（u32） | （`pad_before = 2`） | **`reserved`**（u16） |

> 即 Heimdall 的 `unknown7` 并非 padding，而是 `lu_count`；`unknown8`/`reserved` 才是那 2 字节填充。实测印证见 3.3。
> odin4 的 `PitTable` 另有 `header_size` 成员，但它是内存簿记值（`entry_count` 在文件偏移 4、`com_tar2` 在 8），**不是文件中的字段**。

### 2.4 Thor — 社区维护的 Heimdall 替代品（**建议纳入参照**）

| 项 | 值 |
|---|---|
| 仓库 | `https://github.com/Samsung-Loki/Thor.git` |
| 本地路径 | `/home/DslsDZC/PhoneToolbox/reference/thor` |
| 克隆方式 | `git clone --depth 1`，体积 **432K** |
| HEAD commit | `2993002c9223bd092c3fc9bd1c407c8ec70d947b`（2025-06-07） |
| 上游分支 | `main` |
| 许可证 | **MPL-2.0**（`LICENSE` = Mozilla Public License Version 2.0） |
| 语言/运行时 | C# / .NET 7 |
| 活跃度 | 411 stars，2025-06 仍在更新；README 自述 "An alternative to Heimdall" |

**为什么相关**：README「How's it different from Heimdall?」明确列出对我们的场景直接有用的能力——
NAND Erase All / 任意分区擦除、`ufs`/`lun` 相关、直接刷 Odin `.tar`/`.tar.md5`、直接处理 `.lz4`、
扩展过的 PIT 解析器、EFS Clear 与 Bootloader Update 选项、改 sales code。

**PIT 相关文件**：
- `TheAirBlow.Thor.Library/PIT/PitData.cs` — PIT 解析器（`Parse()`，`:19-54`）。头部读取顺序：`ReadInt32()` 校验 `== 0x12349876`（不等则 `InvalidDataException("Magic number mismatch!")`）→ `ReadInt32()` 条目数 → `ReadString(8)` = `Unknown` → `ReadString(8)` = `Project` → `ReadInt32()` = `Reserved`；随后每条目 9×`ReadInt32()` + 3×`ReadString(32)`。
  `IsNewVersion` 判定（`:43-44`）：若存在 `i>0` 且 `BlockSize != 前一条目 BlockSize`，判为「新版 PIT」。
- `TheAirBlow.Thor.Library/PIT/PitEntry.cs` — 12 个字段，命名与 Heimdall 一一对应（`BlockSize` 即 Heimdall 的 `blockSizeOrOffset`，`FileName` = `flashFilename`，`DeltaName` = `fotaFilename`）。
- `TheAirBlow.Thor.Library/PIT/FieldMapper.cs` — **语义映射表**，填补 Heimdall 的枚举空缺：
  - `NewPitMapper`（`:23-47`）：`DeviceType` = OneNAND / NAND / EMMC / SPI / IDE / NAND X16；`Attributes` = None / BCT / Bootloader / Partition Table / NV-Data / Data / MBR / EBR / GP1 / GP1；`UpdateAttributes` = None / Basic / Enhanced / EXT2 / YAFFS2 / EXT4；`blockSize` 字段标题 = **"Start Block"**；`blockCount` = "Block Count"。
  - `OldPitMapper`（`:49-69`）：`blockSize` 标题 = **"Block Size"**；`Attributes` = Read-only / Read-write / STL；`UpdateAttributes` = None / FOTA / Secure / Secure FOTA。
- 命令实现：`TheAirBlow.Thor.Shell/Commands/ProtoOdin/DumpPIT.cs`、`FlashPIT.cs`、`PrintPIT.cs`。

**Odin 命令字节**（`TheAirBlow.Thor.Library/Protocols/Odin.cs`，比 Heimdall 更全）：
- 会话开始区 `0x64`：req `0x00`（begin session，`:41`）、`0x05`（packet size，`:91`）、`0x02`（total bytes，`:104`）、`0x07`（`:116`）、`0x08`（`:129`、`:144`）。
- 结束会话区 `0x67`：req `0x00`/`0x01`/`0x02`/`0x03`（`:157,168,179,190`）。
- PIT 区 `0x65`：req `0x01`（DumpPIT 请求，`:203`）、`0x02`（取块 / begin flash，`:220,262`）、`0x03`（结束，`:239,280`）、`0x00`（FlashPIT 请求，`:252`）。**PIT 分块大小 = 500 字节**（`:13`，`size / 500`），与 Heimdall `ReceiveFilePartPacket::kDataSize` 一致。
- 刷写区 `0x66`：req `0x00`/`0x02`/`0x03`（`:317,342,376,387`）。
- **失败标记**（`Extensions.cs:14-29` `OdinFailCheck`）：响应首字节 `0xFF` 即失败；错误码 `-2` = WP、`-3` = Erase、`-4` = Write、`-5` = Auth、`-6` = Size、`-7` = Ext4。
- `Extensions.cs:9-12` `OdinAlign()`：Odin 出站包补齐到 **1024 字节**（real Odin 行为，Heimdall 的 `ControlPacket` 构造也是 `OutboundPacket(1024)`，见 `ControlPacket.h:54`）。

**设备匹配方式**（`TheAirBlow.Thor.Library/Communication/USB.cs:6` + `Platform/Linux.cs`）：
`USB.Vendor = 0x04E8`，**没有 PID 白名单**。`Linux.cs:89-146` 遍历接口描述符，取满足 `clss == 0x0A`（CDC_DATA）且同时具备 bulk-in / bulk-out 端点、且 `bInterfaceSubClass`/协议位校验通过的接口。传输走 `/dev/bus/usb` + `USBDEVFS_BULK` ioctl（`Linux.cs:282-354` 的 `Interop`），不依赖 libusb。

---

## 3. 真实 `.pit` 样本（关键交付）

### 3.1 清单（9 个，全部已落盘）

存放目录：`/home/DslsDZC/PhoneToolbox/reference/samsung-samples/`

| # | 文件（绝对路径） | 大小 | sha256 | 平台 / 条目数 |
|---|---|---|---|---|
| 1 | `…/samsung-sm-a145p-a14m-awg1-A14M_MEA_OPEN.pit` | 8064 | `6cfa5f1fae8427178c6cd295007f46f798bc731bea1568a9963e963c5c16fb42` | MTK6765 / 57 |
| 2 | `…/samsung-sm-g900-klte-chn-cucmcc-MSM8974.pit` | 4244 | `6e3ba573a3bd7b56d28f33c404c2dc2d10eaa1ff95fd8c03e50c4bcbd6bf920d` | MSM8974 / 30 |
| 3 | `…/samsung-ks01lte-eur-16g-hidden200m-MSM8974.pit` | 4244 | `362957206f169efb4bafa725e14ad356f40dd28b390de86bf8c32074dde629a1` | MSM8974 / 30 |
| 4 | `…/samsung-sm-g900fd-klte-duos-cucmcc-MSM8974.pit` | 4640 | `623ca7e97729561eb98e5e9dbe6345ff5fed49ecee9d21f25369d403d37d56e7` | MSM8974 / 30 |
| 5 | `…/samsung-sm-j110h-j1xlte-LSI3475.pit` | 3732 | `400ea89af4bbb58731919bc27f8672c5caf02b34cc5cfce78fb2bec177693a06` | LSI3475 / 26 |
| 6 | `…/samsung-msm8930-generic-pitfile.pit` | 3980 | `156eb3234607f675e3367f2963b461f52695f5783d877b8e96280718a679dced` | MSM8930 / 28 |
| 7 | `…/samsung-gt-n7100-t03g-0907-Mx-MDM.pit` | 2924 | `12d793afae729c16bd587fb5a9faf094b969ef0b35ed824991e220bad1cc3fc3` | Mx-MDM / 20 |
| **8** | `…/samsung-sm-m3-eur-open-4g-Mx-MDM.pit` | 2924 | `f9be04a921a696fb3452c93708733e0fda0bb7adea1b988e0533224fd5f7f92e` | Mx-MDM / 20 |
| **9** | `…/samsung-sm-q7mq-eur-openx-SM8750.pit` | 18492 | `2fb71cdd4da340668c1dca0ba87797a3922ed9484da952ce48f6639eb5d77dcd` | **SM8750 / 136** |

样本覆盖了 4 个不同 SoC 家族（高通 MSM8974/MSM8930、MTK6765、LSI3475、现代 SM8750）与 4 个不同 PIT 尾部形态，其中 **#9 是 2024+ 现代机型（骁龙 8 Elite）样本，`deviceType=8`(UFS) 全覆盖**，对验证现代路径价值最高。

### 3.2 来源 URL 与可用性

**#1–#7：已逐个重新下载比对，全部 HTTP 200 且 sha256 一致**

| # | 源仓库 | 精确 URL | 仓库许可证 |
|---|---|---|---|
| 1 | `forforksake/a14m_dump`（A145P Android 13 AWG1 完整 dump，repo size ~459MB） | `https://raw.githubusercontent.com/forforksake/a14m_dump/AWG1/pit/A14M_MEA_OPEN.pit` | **GPL-3.0** |
| 2 | `MHermit/samsung_pit_files`（描述 "pit files for s4&s5"） | `https://raw.githubusercontent.com/MHermit/samsung_pit_files/master/KLTE_CHN_CUCMCC.pit` | **无 LICENSE 文件**（GitHub API `license: null`） |
| 3 | 同上 | `https://raw.githubusercontent.com/MHermit/samsung_pit_files/master/KS01LTE_EUR_16G_0719_hidden200M.pit` | 同上 |
| 4 | 同上 | `https://raw.githubusercontent.com/MHermit/samsung_pit_files/master/SM-G900FD_KLTE_DUOS_CUCMCC.pit` | 同上 |
| 5 | `Yayo4242/Samsung-PIT-Archive`（自称 "Official archive of Samsung PIT files"） | `https://raw.githubusercontent.com/Yayo4242/Samsung-PIT-Archive/main/SM-J110H_J1XLTE.pit` | **无 LICENSE 文件**（`license: null`） |
| 6 | `oznotes/Pit`（"Samsung Pit Parser"；上游把 `.pit` 改名 `._pit`） | `https://raw.githubusercontent.com/oznotes/Pit/master/pitfile._pit` | **Unlicense**（public domain） |
| 7 | 同上 | `https://raw.githubusercontent.com/oznotes/Pit/master/t03g_0907._pit` | 同上 |

**#8–#9：`samloader-rs` 仓库自带测试数据 —— 许可证最干净的一对**

| # | 源仓库 | 仓库内路径 | 仓库许可证 |
|---|---|---|---|
| 8 | `topjohnwu/samloader-rs` | `test-data/M3_EUR_OPEN_4G.pit`（clone 于 `reference/samloader-rs/`） | **Apache-2.0 + MIT 双许可** |
| 9 | 同上 | `test-data/Q7MQ_EUR_OPENX.pit` | 同上 |

#8/#9 是从 `reference/samloader-rs/test-data/` 直接 `cp` 过来的（仓库已在本地，无需网络），已用 `sha256sum` 记录原值；上游 `pit/src/lib.rs:334-349` 的 `test_pit_repack` 测试正是用 #9 做「解析 → 重打包 → 逐字节比对」的回归夹具，说明这两个文件被上游当作正式测试数据维护。

**可用性说明**：
- 全部为**公开仓库中的独立小文件直链**，无需登录、无需固件镜像站账号，均未下载任何 GB 级固件（#1 只取了 8KB 的 PIT，未取该 repo 的 459MB dump）。
- PIT 数据本体是**三星固件/设备的原始数据**，不是上述仓库作者的著作权作品。仓库许可证只约束仓库作者可版权的内容：`samloader-rs` 为 Apache-2.0/MIT 双许可、`oznotes/Pit` 为 Unlicense（这两组最宽松，适合随仓库分发）；`forforksake/a14m_dump` 为 GPL-3.0；`MHermit`、`Yayo4242` 两个仓库**未声明任何许可**（默认保留所有权利）。
- 这些文件只落在 `reference/`（**已 gitignore、不进构建、不进提交**），当前仅作本机测试夹具，不存在再分发。若将来要随仓库分发样本，**首选 #8/#9（Apache-2.0/MIT）或 #6/#7（Unlicense）**。
- 本报告与本地样本均**未合成、未伪造**任何 PIT —— 9 个文件全部逐字节来自上述上游。

### 3.3 真样本验证结果（**由本报告独立复算，非转述**）

对 9 个样本逐一按 1.3 的布局解析，**全部通过**：

1. **魔数**：9/9 首 4 字节 = `76 98 34 12`（LE `0x12349876`）✅
2. **头部 28 字节 + 132 字节条目**：9/9 的 `entryCount` 与实际可解析条目数一致，且条目名（`+36`）、flash 名（`+68`）、fota 名（`+100`）全部解出可读 ASCII ✅
3. **头部语义**（破解 Heimdall 的 `unknownN`）：9/9 的字节 `8..15` = `b'COM_TAR2'`；`16..23` = CPU/BL 标签（`MSM8974\0` / `MTK6765\0` / `LSI3475\0` / `MSM8930\0` / `Mx-MDM\0\0` / `SM8750\0\0`）✅
4. **`lu_count` 是真字段（修正了初版结论）**：9 个样本中 8 个的字节 `24..25` = `0`，但 **#9（SM8750）的 `lu_count = 4`**（`24..27` = `04 00 00 00`）→ 推翻「这 2 字节恒为 padding」的假设，与 samloader-rs `lu_count`（"Logical partition units count"）/ odin4 `lu_count` 的命名吻合；字节 `26..27`（`reserved`）在 **9/9 样本中均为 `0`**。
5. **字段语义实证**（样本 #2 `SM-G900` 全 30 条 dump）：`blockSizeOrOffset` 严格递增且满足 `blk[i] + blkCount[i] == blk[i+1]`（例：APNHLOS 8192+30720=38912=MODEM 起点），末条 SGPT `blkSize=30777311`（≈整盘块数−33）→ **证实该字段在这些样本中是「起始块号 / Start Block」而非「块大小」**，与 Thor `NewPitMapper` 的 "Start Block" 标题、`lu_count` 的存在一致。
6. **两个 obsolete 字段确为 0**：样本 #2 全 30 条的 `fileOffset` / `fileSize` 均为 `0`，与 `libpit.h:93-94` 的 `// Obsolete` 注释吻合 ✅
7. **`deviceType` 出现 Heimdall 枚举之外的值**：全部 377 条条目中 `deviceType` 分布为 `{2(MMC): 241, 8: 136}` —— **样本 #9（SM8750）的 136 条全部是 `8`**，而 Heimdall `libpit.h:61-67` 的枚举只到 `kDeviceTypeAll = 3`。`samloader-rs` 把 `8` 命名为 **`UFS`（扇区 4096B）**，`MMC` 为 512B（`pit/src/lib.rs:87-92`、`155-160`）。→ Heimdall 枚举对现代 UFS 机型不完整。
8. **`attributes` 的两种读法在真数据上并存**：377 条中 373 条为 `5`、**只有 4 条为 `2`，且这 4 条恰好都是各样本的第 0 条、分区名都叫 `BOOTLOADER`/`bootloader`**。
   - Thor 按「枚举下标」读（`FieldMapper.NewPitMapper`）：`2` = *Bootloader*、`5` = *Data* → 与「名叫 BOOTLOADER 的分区取值为 2」高度自洽；
   - Heimdall / samloader-rs 按「位域」读：`2` = bit1 = STL（且 bit0 write 未置位）、`5` = bit0(write) + bit2（**bit2 两边都未定义**）→ 自洽性较弱。
   两种读法在真样本上给出不同结论，**事实如此，不作取舍**。
   （另：`binaryType` 全部 377 条均为 `0` = AP，未见 CP 取值。）
9. **`fotaFilename` 语义**：**9/9 样本都恰好只有 1 条非空**，值是字面量 **`remained`**（意为"保持原样"），出现在 `USERDATA` 条目上（#6 MSM8930 除外，它 0 条非空）。
   ⚠️ **真数据里有脏值**：样本 #5（LSI3475 J110H）的该字段实测为 **`b'remained\r\n'`** —— 32 字节字符串字段内**含字面 CR/LF**，并非干净的 NUL 结尾 ASCII。解析时必须容忍/裁剪空白字符，不能假设字段内容是规整标识符。
10. **`flashFilename` 非空计数**（9 个样本）：20/20、23/30、21/28、35/57、23/30、23/30、25/26、20/20、115/136。为空是正常情况（如 `DDR`、`FSG`、`PAD`、`FOTA`、`BACKUP` 等非文件分区）。

**必须注意的解析健壮性事实（真样本推翻了「干净公式」假设）**：

- **文件长度 ≠ `28 + entryCount*132`**。9/9 样本在条目表之后都有额外尾部数据：

  | 样本 | 计算表长 | 实际文件长 | 尾部字节 |
  |---|---|---|---|
  | #1 MTK6765 A14M | 7552 | 8064 | **512** |
  | #2 MSM8974 G900 | 3988 | 4244 | **256** |
  | #3 MSM8974 KS01LTE | 3988 | 4244 | **256** |
  | #4 MSM8974 G900FD | 3988 | 4640 | **652** |
  | #5 LSI3475 J110H | 3460 | 3732 | **272** |
  | #6 MSM8930 | 3724 | 3980 | **256** |
  | #7 Mx-MDM N7100 | 2668 | 2924 | **256** |
  | #8 Mx-MDM M3 4G | 2668 | 2924 | **256** |
  | #9 SM8750 Q7MQ | 17980 | 18492 | **512** |

- **尾部长度不固定**：样本 #2 与 #4 的条目表长度完全相同（都是 3988），但尾部一个是 256、一个是 652 —— 排除「固定结构」的可能。
- **尾部内容两种形态**（非全零、不可当 padding 忽略）：
  - MTK（#1）与现代高通（#9）：ASCII 可读，以 **`SignerVer02`** 开头，后接 `64343703R`、`A145PXXU1AWD1` 等机型/版本串；
  - 老高通/LSI（#2–#8）：高熵字节（疑似签名/HMAC），#5 的尾部以 `01 00 00 00` + 一段 0 开头再接高熵数据。
- **上游对其性质的权威说明**：`samloader-rs` `pit/src/lib.rs:345-349` 的测试注释原文指出，Q7MQ 文件尾部是 *"an appended 512-byte Samsung cryptographic signature (`SignerVer02...`) at the end, which is ignored by `PitData` during parsing and not serialized by `PitData::pack()`"* —— 并断言 `pack()` 输出长度恰为 `28 + count*132`。
  同一实现的 `PitData::new()`（`:195-201`）只在 `data.len() < expected_size` 时报错（"Buffer size is smaller than declared PIT size"），**对尾部多余字节是容忍的**。这是「必须容错尾部」的第三方权威依据。
- **文件长度不是 4096 的倍数**：`kPaddedSizeMultiplicand = 4096` 只用于传输时的 padded size（`libpit.h:364-373`），**不能**用来校验文件本身。
- 结论性事实：解析器若要求 `filesize == 28 + count*132`、或要求尾部全零、或要求文件长度对齐 4096，都会在**这 9 个真样本上全部失败**。

**分区命名的大小写差异**（影响按名匹配）：高通/LSI 样本分区名为全大写（`APNHLOS`、`MODEM`、`SBL1`、`SYSTEM`、`BOOTLOADER`…），MTK 样本为全小写（`bootloader`、`pgpt`、`pit`…）。

**`file(1)` / libmagic 自带 PIT 签名（一个独立的第三方交叉验证，且它也有同样的现代机型盲区）**：

```
$ file samsung-sm-m3-eur-open-4g-Mx-MDM.pit
… Partition Information Table for Samsung smartphone, 20 entries;
  #1 BOOTLOADER+RW (0x50) "sboot.bin"; #2 TZSW (0x51) "tz.img"; #3 PIT (0x46) "m3.pit";
  #4 MD5HDR (0x47) "md5.img"; #5 BOTA0 (0x1) "-"; #6 BOTA1 (0x2) "-"; #7 EFS (0x3) "efs.img"; …
$ file samsung-sm-q7mq-eur-openx-SM8750.pit
… data
```

- 对 #8（`device_type=2`，MMC）libmagic 能完整展开条目名与标识符（`#1 … (0x50)` 的 `0x50` 与实测 `identifier=0x50` 一致）；
- 对 #9（`device_type=8`，UFS）libmagic **直接退化成 `data`** —— 连系统 magic 库都还没覆盖 UFS 变体，佐证「UFS 是较新、易被漏掉的分支」。
- libmagic 打印的 flag 列：`attr=2` 显示 `+RW`、`attr=5` 显示 `-`（即它的规则是在 `attr & 2` 上判 RW）——**与 Heimdall 的 bit0=Write / Thor 的「2=Bootloader」读法又都不同**。同一字节存在三种互不一致的第三方解读，事实如此。

### 3.4 顺带取回的其他三星小样本

**无**。#1–#7 均为独立 `.pit` 直链；#8–#9 直接取自本地 `reference/samloader-rs/test-data/`。未遇到需要从 `.tar.md5` 中抽取的路径（`samloader-rs/test-data/` 里的另两个文件 `history.xml`、`version.xml` 是 FUS 响应夹具，与 PIT 无关，未取回）。

---

## 4. 参照仓库总表（许可证一览）

| 仓库 | 本地路径 | 许可证 | 体积 | 最后活动 | 覆盖：FUS下载 / 解密解包 / PIT / 刷写 |
|---|---|---|---|---|---|
| `Benjamin-Dobell/Heimdall` | `reference/heimdall` | **MIT**（2010-2017 Benjamin Dobell, Glass Echidna） | 13M | 2021-03 | ✗ / ✗ / ✅（解析+收发） / ✅ |
| `Llucs/odin4` | `reference/odin4-llucs` | **Apache-2.0** | 2.9M | 2026-09 | ✗ / ◐（lz4 解压 + `tar.md5` 的 MD5 校验，**无** `.enc2` 解密） / ✅ / ✅ |
| `topjohnwu/samloader-rs` | `reference/samloader-rs` | **Apache-2.0 + MIT** | 792K | 2026-09 | ✅ / ✅ / ✅ / ✅ |
| `samloader/samloader` | `reference/samloader` | **GPL-3.0**（`COPYING`）+ 维护者 `STATEMENT.pdf` 附加声明；**项目已归档** | 280K | 2023-06 | ✅ / ✅ / ✗ / ✗ |
| `Samsung-Loki/Thor` | `reference/thor` | **MPL-2.0** | 432K | 2025-06 | ✗ / ◐（lz4/`tar.md5`） / ✅ / ✅ |

**许可证风险提示**：上表中只有 `samloader/samloader` 是 **copyleft（GPL-3.0）**，且它已被作者归档并附 `STATEMENT.pdf` 附加许可声明 —— 若考虑复用其代码需先读该声明。其余四个（MIT / Apache-2.0 / MIT+Apache-2.0 / MPL-2.0）都允许在保留声明的前提下复用。**Heimdall 的 MIT 最宽松且是本任务的主参照。**

> ⚠️ **`samloader/samloader` 的附加声明比"转为公有领域"要窄**（我校核过 `README.md` 内嵌声明全文）：声明由维护者 nlscc 于 2023-06-19 作出，**只把"由 nlscc 本人 authored 的 commit 文本（diff 集合）"释放为公有领域**，并明确 "*excluding any text not authored by me but required to implement the instructions*"，且 "*this statement does not apply to commits authored by people who are not nlscc*"。仓库根目录仍是 **GPL-3.0**（`COPYING`）。**因此该仓整体仍应按 GPL-3.0 对待**，不可当作公有领域使用。
> `samloader/samloader`（Python 原版，前身地址 `nlscc/samloader`）：只做 FUS 侧（`check-update` / `download` / 解密 `.enc2`），**无 PIT 解析、无刷写**；仓库内不含 PIT 测试数据。
> `odin4-llucs` 的 Crypto++ 依赖经核查**只用于 MD5**（`src/firmware/firmware_package.cpp:38-39,244,264-265`），不是用于固件解密。

### 4.1 同生态的其余实现（**未克隆**，仅登记仓库与许可证，供按需取用）

| 仓库 | 许可证 | 体积 / 活跃度 | 覆盖 | 备注 |
|---|---|---|---|---|
| `Gabriel2392/brokkr-flash` | **GPL-3.0** | 1.8M / 2026-09-08 / 341★ | PIT 读+改、LZ4、刷写 | **C++/Qt6**，与 PhoneToolbox 技术栈最接近；**但 GPL-3.0 copyleft，只能在许可上隔离地参考** |
| `zacharee/SamloaderKotlin` | **MIT** | 27M / 2026-09-05 / 1598★ | 版本查询、下载、解密 | Kotlin MPP（桌面/Android/iOS GUI）；无 PIT/刷写 |
| `jesec/samfirm.js` | **GPL-3.0** | 128K / 2023-12 / 279★ | 版本查询、流式下载、解密（`.enc4` 已确认）、zip 解包 | TypeScript，8 个 commit，低活跃；无 PIT/刷写 |
| `MatrixEditor/samloader3` | **GPL-3.0** | 214K / 2023-12 / 12★ | 下载器（覆盖范围未核实） | 未验证 |
| `lineageos-infra/libmjolnir`（原 `r3pwn/libmjolnir`） | 未核实 | — | TypeScript/WebUSB，Odin 协议 + PIT 解析 + 刷写 | 已易主、停止维护；许可证未核实 |

**已确认不存在 / 已死**（避免后续重复排查）：
- `Codium-io/odin4-rs` —— **404，仓库不存在**（GitHub API 直接返回 `Not Found`）。
- `Vurpas/odin4`（原始泄露版 odin4 的常见归因）—— **GitHub 404**；GitLab 上确有 `vurpas` 用户，但**未公开任何 odin4 项目**，Codeberg/GitLab 搜索亦无镜像。**未找到任何可获取的原始 odin4 镜像。**

---

## 5. 与「没找到什么」相关的事实

- **Heimdall 仓库内无任何测试数据/PIT 样本**（第 1.6 节）。
- **Heimdall 上游只有一个 `master` 分支**（`git ls-remote --heads` 确认），无 `2.0`/`develop` 分支可供获取更多 PID 或新协议实现；最高 tag `v1.4.2`（第 1.5 节）。
- **Heimdall 全仓不含 MD5 校验与 `.lz4` 支持**：`grep -i md5` 零命中；`Packaging.cpp` 只有 ustar TAR 解包。
- **GitHub 代码搜索对 PIT 无效**：`filename:.pit` 有 21056 条命中但全是误报（PHP `class.pitemp.inc.php`、SHARPpy 天气 `.PIT` 数据库、Blender `.pit` 资源、polyglot 文件等）；`extension:pit` + 内容关键词（`COM_TAR2` / `MTK6765` / `GT-I9300` / `odin` / `heimdall`）返回 0 或误报，**因为二进制内容不被索引**。有效的查询是反直觉的 `12349876 filename:pit`。
- 以下仓库经核查**不含** PIT 数据文件：`Kulim13/PIT-Magic-Source`（仅 C#/VB 源码）、`salvogiangri/SamsungPITViewer`、`MhAhmadAli/odin4`、`Samsung-Loki/samsung-docs`、`Grimler91/samsung-docs`、`h4rithd/OdinMac`、`CruelKernel/samsung_pit|webpit|pitdump`、`FergusInLondon/PitParser`、`Durban42092/nFlasher`（测试引用的夹具不存在）、`Benjamin-Dobell/Heimdall` 本身、`samloader/samloader`。
- 空仓库 / 无载荷：`ne0z/Z200F_FULL_STOCK_FIRMWARE`（空）、`ohjhas/openpit`（空）、`thiago-202611/SFS-Archive`（README 声称有 PIT，实际树内只有 README/LICENSE/.gitignore）。
- **原始 odin4 找不到**：`Codium-io/odin4-rs` 与 `Vurpas/odin4` 均 404；GitLab/Codeberg 无镜像（第 4.1 节）。
- **未获取到的内容**：无。所有要求的交付项（Heimdall 参照 + PIT 结构/协议/PID/测试数据盘点 + 第二参照 + 真 PIT 样本）均已拿到；真样本超出预期（9 个而非 1 个）。
- **未克隆（有意）**：`brokkr-flash`（GPL-3.0）、`SamloaderKotlin`（MIT，27M）、`samfirm.js`、`samloader3`、`libmjolnir` —— 均为「覆盖范围已被现有 5 个克隆覆盖」或「许可证需先评估」，仅登记在 4.1 表。

---

## 6. 复核方式（可复现）

本报告所有事实均可在本地复算：

```bash
# 1. 许可证与文件清单
cat reference/heimdall/LICENSE
grep -n 'kFileIdentifier\|kHeaderDataSize\|kDataSize' reference/heimdall/libpit/source/libpit.h
grep -n 'kVidSamsung\|kPidGalaxy\|kSupportedDeviceCount' reference/heimdall/heimdall/source/BridgeManager.h

# 2. 真样本魔数与校验和（9 个文件）
cd reference/samsung-samples && for f in *.pit; do xxd -l 4 -p "$f"; sha256sum "$f"; done

# 3. 协议命令号
grep -n 'kControlType' reference/heimdall/heimdall/source/ControlPacket.h
grep -n 'kBeginSession\|kFilePartSize' reference/heimdall/heimdall/source/SessionSetupPacket.h

# 4. 三份现代实现的头部字段命名对照
sed -n '181,188p' reference/samloader-rs/pit/src/lib.rs
sed -n '55,62p'  reference/odin4-llucs/src/core/odin_types.h
```

