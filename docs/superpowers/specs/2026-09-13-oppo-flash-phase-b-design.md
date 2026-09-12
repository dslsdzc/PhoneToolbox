# PhoneToolbox — OPPO/一加 EDL 刷写链 (Phase B) 设计

日期: 2026-09-13
状态: 已批准（用户逐节确认）
前置: Phase A（固件解包引擎）已交付并合并到 `main`（`19f617c..b54aecd`）
配套事实速查: `docs/superpowers/specs/oppo-flash-protocol-facts.md`（全部协议结论带参照行号 + 计划来源实证）

## 1. 背景与目标

功能清单"规划中"第 1 项的第二阶段。Phase A 已能把 `.ofp`(QC/MTK) / `.ops` 解包成镜像与清单；Phase B 把**刷写侧**做出来：从解包产物（或整包）构建刷写计划，经 Sahara/Firehose 走完一次真实刷写。

**用户决策记录**
- **交付终点**：做到"代码就绪 + mock 验证"；**真机部分（实机握手/写入/真机验证）明确不做，留给持机人**。UI 必须有"我知晓此路径真机未验证"的显式勾选。
- **架构**：协议与传输**拆两层**（纯协议模块 + 可注入传输接口），`EDLHandler` 退化为薄封装。
- **入口**：解包产物**目录**为主 + **整包**（`.ofp`/`.ops`）便捷入口（内部解包到临时目录后走同一流程）。
- **命令范围**：全覆盖 —— `configure` / `program` / `patch` / `erase` / `reset` / `getstorageinfo` / `setbootablestoragedrive` / `read`（读回/备份）。
- **确认强度**：计划预览 + 显式勾选"真机未验证" + **校验不过则拒刷**。
- **计划来源**：`rawprogram*/patch*.xml` 优先；`.ops` 回退到 `settings.xml` 的 `Program{N}`/`Patch{N}` 块，并用包内 `gpt_main{N}.bin` 的 LBA 表**回填 + 对账**几何。
- **顺带修复**现有 `EDLHandler` 的 5 处既有缺陷（见 §3.4）。

## 2. 事实基础（详细版见协议速查文件）

**关键协议事实**（全部带参照行号，速查文件 §1-§4）：
- `program` 出站恒发 `SECTOR_SIZE_IN_BYTES`/`num_partition_sectors`/`physical_partition_number`/`start_sector` 四属性；数据面**裸写 OUT 端点**（无长度前缀），按 `chunk = MIN(max_payload_size/sector_size, left)` 个扇区分块；**发完还要再等一个 ACK**；ZLP 仅在 `len % out_maxpktsize == 0` 时补；文件尾补零到扇区边界。
- **sparse 一律主机侧解包**（两参照都如此），不存在原样透传。
- `patch`：出站**不发 `what`**；`filename == "DISK"` 才下发设备（真实文件名是给 QFIL 离线改 bin 用的，参照直接跳过）；`value`/`start_sector` 里的表达式（`NUM_DISK_SECTORS-6.`、`CRC32(2,4096)`）**主机不解释、原样下发**。
- `configure` 必须等响应，且常需**两轮**（回读 `MaxPayloadSizeToTargetInBytesSupported` 重发）；`MemoryName` 大小写两侧不一致，存在"报错后换存储类型重试"的现实路径。
- `erase` 不隐含在 `program` 前；`setbootablestoragedrive` 的 `value` = bootloader 分区（`xbl`/`xbl_a`/`sbl1`）所在 LUN。
- `getstorageinfo` 返回 JSON `storage_info`（`total_blocks`/`block_size`）或文本键；LUN 数由它或 `rawprogramN` 序号决定。

**计划来源的实证**（本研究新增，速查文件 §5）：
- `.ops` **解包产物不含** `rawprogram*.xml`/`patch*.xml`（RoE Wiki 的 OnePlus 6 EDL 实录、XDA 8T 帖）。
- 但 `<Program{N}>` 块的内容**本身就是** rawprogram 内容：chayleaf 脚本原样导出成 `rawprogram{N}.xml` 后 `edl qfil` 刷机成功（bkerler/edl issue #432）；`edl` 对每条带 `filename` 的 `<program>` **硬读** `start_sector`/`physical_partition_number`（缺失即崩），故这两个属性**强推断存在**；`num_partition_sectors` **未证实**（`edl` 不读它）。
- **`FileOffsetInSrc`/`SizeInByteInSrc`/`SizeInSectorInSrc` 只是包内偏移与长度**，`InSrc` 即 "in source (package)"，**绝不能当设备扇区**（打包器与解包器双向互证）。
- **唯一有开源实证的几何来源 = 包内 `gpt_main{N}.bin` 的 LBA 表**：OplusEdlTool 正是用它回填 `start_sector`/`num_partition_sectors`（`RawProgramXmlProcessor.cs:101-108` + `GptParser.cs:31-115`）；社区清单确认 `.ops` 内含 `gpt_main0-5.bin`/`gpt_backup0-5.bin`。`patch{N}.xml` **不能**由 GPT 推出（是 GPT 头定点修补）。
- **未解矛盾（诚实记录）**：OplusEdlTool v2 硬要求目录里已有 `rawprogram*.xml` 否则中止、且它自己不生成 —— 疑为新老世代差异，未证实。这支持"① XML 优先、② 元数据回退"的顺序。
- **`.ofp`-QC 侧无证据**：其元数据分组只有 Sahara/Config/Provision/ChainedTableOfDigests/DigestsToSign/Firmware，**没有** `Program`/`Patch` 分组；产物里是否有可用 XML **待真包确认**。

**现有 `EDLHandler` 的 5 处既有缺陷**（非本次引入，Phase B 顺带修复）：
1. `sendFirehoseXml`/`recvFirehoseResponse` 带 4 字节长度前缀，两参照都是**裸 XML**（`edl_handler.cpp:456-461,489-491`）。
2. `waitFirehoseDone` 的 ACK 判定**恒假** —— 合法 `<response value="ACK"/>` 命中其 `continue` 分支，只会循环到超时返回 false（`:494-510`）⇒ **现有 EDL 写入路径从未成功过**。
3. `<configure>` 只发不读响应（`:634-648`）。
4. `<program>` 缺 `physical_partition_number`，且 `writeRaw` **完全没有数据面**（无分块/ZLP/补零）（`:791-814`）。
5. `<read>` 用错属性名 `num_sectors`、缺 ppn、多带 `filename`（`:716-725`）。

**待更正的本项目文档**：`docs/superpowers/specs/oppo-format-notes.md:53` 称"rawprogram/patch XML 在 Program/UFS_PROVISION 区域"—— **无证据支持**，Phase B 一并更正。

## 3. 架构

### 3.1 模块划分（新增 `src/core/edl/`，命名空间 `edl`）

```
src/core/edl/
  edl_transport.h           IEdlTransport 纯虚接口 —— 唯一的设备依赖点
  edl_libusb_transport.*    libusb 实现（端点/VID/PID/重枚举策略，从 edl_handler.cpp 搬来）
  sahara.*                  纯协议：HELLO 应答 / 按设备请求分片回吐 programmer / DONE
  firehose.*                纯协议：命令 XML 构造 + 响应解析 + 数据面分块推送
  flash_plan.*              rawprogram/patch XML + OPS 元数据 + GPT → FlashPlan（纯函数）+ 计划校验
  edl_session.*             编排：Sahara → 等重枚举 → configure → getstorageinfo → 校验 → 逐条目 → reset
src/core/modes/edl_handler.*  退化：设备枚举/连接 + 持有 LibusbEdlTransport + 转发 EdlSession；
                              **公开 API 与 outputMessage/progress 两信号保持不变**
src/ui/flash_plan_dialog.*    计划预览对话框（新）
tests/
  mock_edl_transport.h        脚本化读队列 + 写字节记录 + 可编程失败注入
  flash_plan_helpers.h        合成 rawprogram/patch XML、合成 settings.xml、合成 gpt_main{N}.bin（手写字节）
  test_edl_sahara.cpp / test_edl_firehose.cpp / test_flash_plan.cpp / test_edl_session.cpp
```

**依赖方向（单向无环）**：
`edl_handler` → `edl_session` → {`sahara`, `firehose`, `flash_plan`} → `IEdlTransport` ← {`edl_libusb_transport`, `mock_edl_transport`}

**关键性质**：除 `edl_libusb_transport` 与 `edl_handler` 两个边缘模块外，**其余模块不碰 libusb、不含 QObject** —— 这是"无真机可验证"的物理前提（对照现状：`EDLHandler` 是全仓唯一无测试、无注入缝的协议实现）。

### 3.2 传输接口

```cpp
class IEdlTransport {
public:
    virtual ~IEdlTransport() = default;
    virtual bool        open(QString *error) = 0;          // 打开 9008 设备
    virtual void        close() = 0;
    virtual bool        write(const QByteArray &data, QString *error) = 0;   // 裸写 OUT
    virtual QByteArray  read(int maxBytes, int timeoutMs, QString *error) = 0; // 裸读 IN
    virtual bool        resetDevice(QString *error) = 0;   // 退出 EDL / 复位
    virtual bool        waitReenumerate(int timeoutMs, QString *error) = 0;  // Sahara→Firehose 转折
    virtual int         maxPacketSize() const = 0;         // ZLP 判定
};
```

### 3.3 数据模型（`flash_plan.h`）

```cpp
struct PlanEntry {
    enum class Action { Program, Erase, Patch };
    Action  action;
    QString partitionName;      // label / patch 条目标识
    QString imageFile;          // 包内绝对路径；Patch 时可能是 "DISK"
    quint32 lun = 0;            // physical_partition_number
    quint64 startSector = 0;
    quint64 numSectors = 0;     // Program/Erase：**sparse 展开后的 raw 扇区数**
    quint32 sectorSize = 4096;  // 逐条目 SECTOR_SIZE_IN_BYTES
    bool    sparse = false;
    quint64 rawBytes = 0;
    QString sha256;             // 可选
    quint64 byteOffset = 0;     // Patch
    quint32 sizeInBytes = 0;    // Patch
    QString value;              // Patch：**原样透传**（表达式不解释）
    QString what;               // Patch：只进日志，不进 XML
};

struct FlashPlan {
    QString source;             // 实际来源（"rawprogram0..5.xml" / "OPS 元数据 + GPT 回填"）
    QString storageType;        // "ufs"/"emmc" → configure 的 MemoryName
    QList<PlanEntry> entries;   // 已排序（按 lun、start_sector；patch 在 program 之后）
    QStringList warnings;
    quint64 totalBytes = 0;     // 进度分母
};

struct StorageInfo { quint32 lun; quint64 totalBlocks; quint32 blockSize; };

bool buildPlanFromDir(const QString &dir, FlashPlan &plan, QString *error);
struct PlanCheck { bool ok; QStringList errors; QStringList warnings; };
PlanCheck validatePlan(const FlashPlan &plan, const QList<StorageInfo> &device);
```

### 3.4 计划来源三层（同一 `FlashPlan`，预览标注实际来源）

1. **`rawprogram*.xml` / `patch*.xml` 存在** → 直接解析（`.ofp` 产物若含 XML 自然走这条；用户自备 XML 也走这条）。
2. **`.ops` 的 `settings.xml`** → `<Program{N}>`/`<Patch{N}>` 块 → 条目（**属性原样保留**）；几何用包内 `gpt_main{N}.bin` 的 LBA 表**回填 + 对账**（复用 `image_engine` 既有 GPT 解析）：元数据有值则两者对账、不一致 → warning 并**以 GPT 为准**；元数据缺 `start_sector`/`num_partition_sectors` 则用 GPT 回填。
3. **都没有** → 明确报错，并**列出目录内所有 `.xml` 文件名**帮助诊断（`.ofp`-QC 的 Config/Provision 组待真包确认）。

**不做**：不自己推几何、不查设备 GPT 反推 start_sector（几何来自元数据或包内 GPT 文件）。`patch` 条目若元数据里没有 → 预览里明确警告"缺少 patch 条目（GPT 头定点修补），刷后可能无法引导"，由用户决定（不静默）。

**sparse 的展开位置（明确归属，避免两处实现）**：`flash_plan` 只负责**算出** `numSectors`/`rawBytes`（读 sparse 头，复用 `image_engine` 的 sparse 解析）；**展开发生在发送时** —— `edl_session` 的数据面按 sparse 结构逐块产出 `RAW`/`FILL` 数据（`DONT_CARE` **不下发但 start_sector 仍前移**），**不生成临时 raw 文件**（与 qdl 的 per-chunk op 同构、与 bkerler 的 `sparse.read()` 同构）。

### 3.5 计划校验（`validatePlan`）

- 逐条 `start_sector + num_sectors ≤ 该 LUN 的 total_blocks`
- **重叠检查只针对 Program 类条目之间**（同 LUN 区间相交 = 拒绝）；`Erase` 与 `Patch` 不参与重叠判定 —— 整 LUN 擦本来就会覆盖后续 program 的范围，而 patch 打的是 GPT 头等任意磁盘偏移。排序保证 `Erase` 先于其后的 `Program`
- 条目 `sectorSize` 与设备 `block_size` 不一致 → **warning 而非 error**（参照允许逐条目 `SECTOR_SIZE_IN_BYTES` 覆盖全局值，以条目值为准；qdl 即如此）
- 镜像文件存在且可读；`sparse` 条目与文件头一致性
- 有 `sha256` 时：默认**边写边算、写完即校**；可选"刷前完整校验"
- **任何 `errors` 非空 → 拒刷**；`warnings` 只进预览与日志

## 4. 执行流与错误处理（`edl_session.cpp`）

```
1. Sahara：等 HELLO_REQ → HELLO_RSP → 循环读 READ_DATA(offset,len) 从 programmer 切片回吐
          → END_OF_IMAGE → DONE_REQ/DONE_RSP → 关设备
2. 等重枚举：**可注入策略**（真机=轮询；测试=立即返回，不必真等 3 秒）
3. Firehose：configure（回读 MaxPayloadSizeToTargetInBytesSupported → 必要时重发一次；
                       失败文本 "Not support configure MemoryName …" → 换存储类型重试一次）
          → getstorageinfo（逐 LUN）→ validatePlan（不过则拒刷，绝不进入写入）
          → 逐条目 Program/Erase/Patch（数据面分块 + ZLP + 扇区补零 + 每步严格等 ACK）
          → setbootablestoragedrive（计划含 xbl/sbl1 时取其 LUN）
          → reset（**仅成功路径发**；失败不发，保持设备在 EDL 便于重试）
4. 进度：条目名 + 已处理字节/总字节，经回调上报
```

| 阶段 | 失败形态 | 行为 |
|---|---|---|
| Sahara | 设备未请求 / programmer 读失败 / 超时 | 中止 + 中文错误（带阶段名）+ **不发 reset** |
| configure | NAK / MemoryName 不支持 | 换类型重试一次；仍失败中止（错误带设备返回原文） |
| 计划校验 | 越界 / 重叠 / 扇区大小不符 / 文件缺失 | **不进入写入**，拒绝 + 逐条列出 |
| 数据面 | 写失败 / 超时 / ACK 非 ACK | 中止当前条目并停止后续；错误里给"已写入 N 扇区、失败于偏移 M" |
| 传输 | 设备掉线 | 中止 + 提示重新进 EDL 重试（不复位、不假装成功） |

**明确不做**：整包自动重试；隐式 erase（只执行计划里显式的 erase 条目）；**小米 EDL 鉴权分支**（参照里有，本项目不覆盖 —— 遇到报"设备要求鉴权，暂不支持"）。

**重跑语义（写进日志）**：program = 覆盖写（同扇区重写安全）；erase = 破坏性不可逆；patch = 定点改写（幂等）。

## 5. UI 接线（`src/ui/flash_plan_dialog.*`）

- **入口 1**：选解包产物目录 → 后台线程解析 + 校验 → 计划预览
- **入口 2**：选 `.ofp`/`.ops` 整包 → 后台解包到 `QTemporaryDir`（进程内，刷完/取消清理，日志说明）→ 同一预览
- **预览内容**：来源、存储类型与 LUN、条目数、总字节、warnings 明细、逐条 `分区/LUN/起始扇区/扇区数/大小/文件/校验`（`QTableView`）
- **未连接设备也能预览**；点"开始刷写"先连设备 + `getstorageinfo` → `validatePlan` 不过则拒刷
- **执行中**：进度条（`totalBytes` + 当前条目名）、逐条目日志、**"中止"只在条目边界生效**
- **勾选**：`[ ] 我知晓此路径真机未验证` —— 不勾选则"开始刷写"禁用
- **与"死砖修复"的关系**：那是"设备已坏、按名字从目录猜映射"的另一场景，**保持不动**
- 项目约定：不加菜单栏；不动现有三条协议通道的行为

## 6. 测试策略（无真机如何证明"写对了"）

**`MockEdlTransport`**（`tests/mock_edl_transport.h`）：脚本化读队列（预置设备响应序列）+ **写字节全记录** + 可编程失败注入（第 N 次写超时 / 指定命令回 NAK）。照 `MockUsbChannel` 先例（`tests/test_mtk_brom.cpp:8-44`、`tests/test_spd_flash.cpp:8-47`）。

| 测试 | 断言 |
|---|---|
| `test_flash_plan.cpp` | 合成 XML（**手写字节，不调被测代码**）逐字段断言；表达式原样保留；非 DISK patch 跳过 + warning；sparse 展开后的 `numSectors`；边界（缺必需属性/越界/区间重叠 → 拒绝）；OPS 分支的 GPT 回填值与"与元数据不符即告警" |
| `test_edl_firehose.cpp` | 命令 XML 逐字段（configure 两轮协商、program 四属性、patch 不带 `what`、erase 形态、ACK/NAK 严格判定、getstorageinfo 解析），注释标 qdl/bkerler 对应行号 |
| `test_edl_sahara.cpp` | HELLO 应答字节；按请求分片回吐 programmer（跨请求边界、尾部短包）；DONE 序列 |
| `test_edl_session.cpp` | 合成 programmer + 小镜像计划跑完整个会话 → 断言**写出的字节流**（XML 序列 + 数据面分块 + ZLP + 补零 + reset 发送位置）、进度序列、失败注入下的中止语义（NAK → 停在该条目且**不发 reset**） |
| 数据面等价性 | mock 记录的镜像数据块 + 补零 == 期望镜像**逐字节相等**（"无真机也能证明写对"的核心断言） |

**离线证明不了的部分（必须由持机人验证）**：真机 USB 时序（重枚举窗口、ZLP 行为）、programmer 与具体机型的兼容性、真实 NAK 的触发条件、**刷完能否开机**。

## 7. 交付物清单

- [ ] `src/core/edl/` 6 个模块（§3.1）
- [ ] `EDLHandler` 退化 + 5 处既有缺陷修复（§2）；**既有"备份/读回"路径改走新模块的 `read` 实现**（顺带获得正确的属性名 `num_partition_sectors` 与 `physical_partition_number` 支持），对外 API 不变
- [ ] `read`（读回/备份）覆盖：`firehose` 实现 `read` 命令 + 数据面接收；会话层支持"只读回不写入"的一次性路径
- [ ] `src/ui/flash_plan_dialog.*` + FlashPanel 入口 + `FlashTool` 新增 `oppo-edl` 通道
- [ ] 4 个测试文件 + 2 个测试头（mock / 合成夹具）
- [ ] 文档：`oppo-format-notes.md:53` 更正；`功能清单.txt` 规划中段更新；协议速查文件随 spec 提交
- [ ] 诚实边界：真机未验证（UI 有勾选 + 交付说明保留）

## 8. 明确不做

- **真机联调与真机验证**（用户明确划出，留给持机人）
- 小米 EDL 鉴权分支；MTK BROM 路径（`.ofp`-MTK 是另一条链，本阶段不动）
- 整包自动重试、隐式 erase、加密写路径/repack
- `EDLHandler` 现有"死砖修复/备份"流程的重写（保持可用）

## 9. 参照物与许可

- 主参照：`reference/qdl/`（Linaro qdl，C，与本项目同构）、`edl/`（bkerler/edl 子模块，Python）、`reference/FirmwareKit.Oppo/`（C#）
- 计划来源实证：bkerler/edl issue #432（chayleaf 脚本）、RoE Wiki OnePlus 6 EDL 实录、`https://github.com/salokrwhite/OplusEdlTool`（rawprogram/GPT 回填做法）
- 本项目 GPLv3；自研代码按"独立实现，格式事实与公开常量可引用"标注来源；不整文件复制

## 10. 验证清单（全部完成后）

- [ ] `ctest --test-dir build --output-on-failure` 全绿（含新 4 个目标）
- [ ] 合成包 → 计划预览 → mock 会话全流程测试通过
- [ ] **真机验证**：待持机人执行（本阶段不做）；届时优先验：真机重枚举时序 → 单条目 program → 整包 → 刷完能否开机
- [ ] `git status` 无意外产物
