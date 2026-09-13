# PhoneToolbox — 三星 Heimdall(Odin) 集成本期设计（Phase C）

日期: 2026-09-14
状态: 已批准（用户逐节确认）
前置: Phase A/B 已交付并合并（`main`），`ctest` 39/39
配套事实：`docs/superpowers/specs/samsung-odin-facts.md`（参照与样本事实，带路径/行号/URL/sha256）

## 1. 背景与目标

功能清单「规划中」的**三星 Heimdall 集成**（Odin 协议开源实现）。目标：PhoneToolbox 能对三星机型**按 PIT 刷写**真实固件包（BL/AP/CP/CSC 的 `.tar.md5`）。

**用户决策记录**
- **范围：全做** —— 离线可验的 PIT/计划层 + 设备侧刷写链（握手/读 PIT/逐分区写入），设备侧写到"**代码就绪 + mock 验证**"为止；**真机验证归持机人**（与 Phase B 同款边界）。
- **路线：自研 + 参照核对**，不整文件复制（与 Phase A/B 一致）。参照许可证见 §9。
- **验证：用真实样本** —— PIT 解析对**真 PIT 文件**硬断言；计划层对**真 tar.md5 包**验证（用户已下载 SM-J110H 全包，见 §2）。
- **两类不匹配（PIT 有而包内无 / 包内有而 PIT 无）：只 warning + 继续**（预览里置顶逐条列出），不拒刷 —— 用户可能只想刷部分分区（如只刷 AP）。
- **设备 PIT 与包内 PIT 不一致时：以设备 PIT 为准**（设备自身布局才是真相；信包内 PIT 可能把镜像写到错偏移），不一致记 warning。
- **repartition 本期不做**（破坏性，需专门设计）。

## 2. 事实基础（要点；逐条出处见配套报告）

**本地既有能力**
- **解包侧 Odin 能力已完整**：`src/image_engine/tar_image.{h,cpp}` 有 `verifyMd5Footer`/`appendMd5Footer` 与流式版，解包自动校验（`tar_image.cpp:438`），真实格式回归在 `tests/test_tar.cpp`。
- **仓库内没有 PIT 解析、没有 Odin 协议代码**；`DeviceDetector` 没有三星入口（`detectProtocolDevices` 的 VID/PID 表只有 MTK/华为/展锐三项）。

**⚠️ 既有缺口（真包下载后实测发现，本期前置修复）**
`tar_image.cpp:125` 的 footer 扫描**硬要求 `[32hex]` 后是两个空格**；而真包实测存在**两种变体**：
- `BL_*.tar.md5` / `CSC_*.tar.md5`：`<hex>␣␣<name>`（md5sum 文本模式）
- **`MODEM_*.tar.md5`：`<hex>␣*<name>`**（二进制模式，带 `*`）
⇒ 后者会走"无校验行 → 跳过校验"分支，**MD5 校验被静默跳过**（损坏包会被接受）。**本期修**：扫描接受两种变体；用例用合成文件（照惯例不依赖 `reference/`），真实 MODEM 包只作注释证据。

**真实样本（已就位，`reference/`，gitignore）**
- **真 PIT 10 个**：`reference/samsung-samples/` 9 个（GT-N7100/SM-M3/SM-J110H/SM-G900/SM-G900FD/KS01LTE/MSM8930/SM-A145P/SM-Q7MQ）+ 从真实 CSC 解出的 `J1POP3G.pit`（5012B，新尺寸档）。尺寸跨 2924–18492，SoC 跨 MSM8974/MSM8930/MTK6765/LSI3475/**SM8750(骁龙 8 Elite)**。
- **真 tar.md5 3 个**（从 `SM-J110H_INS_fac.zip` 解出，保留在 `reference/samsung-samples/sm-j110h/`）：
  - `BL_..._REV02_user_low_ship.tar.md5`（3.8MB）→ `spl.img`/`sboot.bin`/`sboot2.bin`/`param.lfs`
  - `CSC_ODD_..._REV02_user_low_ship.tar.md5`（27MB）→ **`J1POP3G.pit`**/`cache.img`/`hidden.img`
  - `MODEM_..._REV00.tar.md5`（10.7MB）→ `SPRDCP.img`/`SPRDDSP.img`/`nvitem.bin`（**展锐方案**）
  - AP 包 966MB 未解出（体积考虑）

**PIT 格式（三方独立核对后的要点）**
- magic `0x12349876`；**28 字节头** + **132 字节/条目**。
- 头 `8..15` = `com_tar2`、`16..23` = `cpu_bl_id`、`24..25` = **`lu_count`**、`26..27` = `reserved`（Heimdall 全命名 `unknownN` 且读错形状；Thor/odin4/samloader-rs 三方一致给出上述语义；`lu_count` 实测 SM8750 = 4，**不是 padding**）。
- **文件长度 ≠ 28 + count×132**：10/10 样本尾部都有**签名块**，长度不定（256/272/512/652B…；A14M 的是 ASCII `SignerVer02`）。**要求等长或尾部全零的解析器会在全部真样本上失败。**
- `deviceType = 8`(UFS) 出现在 SM8750 全部条目（Heimdall 枚举只到 3）。
- `attributes` 字段**三种第三方解读互不一致**（Heimdall 位域 / Thor 枚举下标 / libmagic `attr & 2`），真数据上无法裁定。
- 字符串字段真数据含**字面 CR/LF**（如 `fotaFilename` = `b'remained\r\n'`）。

**设备与协议**
- **PID 覆盖**：Heimdall 白名单只有 3 个老 PID（0x6601/0x685D/0x68C3）；odin4 有 6 个；**Thor 与 odin4 都已改用 CDC_DATA 接口类匹配**（覆盖现代机型）。
- 协议命令号/包结构以 **Heimdall + odin4-llucs + Thor 三方对照**落实，**逐条带出处**；三方不一致处并列标注、不擅自裁定（依据：配套报告 §B）。

## 3. 架构与模块

```
src/core/odin/                     新增，命名空间 odin（与 src/core/edl/ 平级）
  odin_transport.h                 IOdinTransport（可注入；真机=libusb，测试=mock）
  odin_libusb_transport.*          libusb 实现（CDC_DATA 类匹配为主 + PID 兜底表）
  pit.{h,cpp}                      PIT 解析（纯函数）：28B 头 + 132B 条目 + 尾部签名容忍
  odin_protocol.{h,cpp}            协议帧构造/解析（纯函数）
  odin_session.*                   编排：握手 →（可选）读设备 PIT → 校验计划 → 逐条目写入 → 收尾
  samsung_plan.{h,cpp}             计划层：tar.md5 条目 + PIT → SamsungPlan
src/core/flash_tool.*              新增第 5 通道 samsung-odin（+ params 注释同步）
src/ui/flash_plan_dialog.*         复用"计划预览 + 未验证勾选"骨架（同族对话框/同一门控语义）
src/image_engine/tar_image.*       前置修复：footer 两变体（§2）
```

**三条设计取向（与 Phase B 对齐，避免重蹈覆辙）**
1. **传输层是纯字节管道**：不补 ZLP、不做协议判断 —— 责任分工必须落在代码里，不写在注释里就算了（Phase B 教训：命令帧 ZLP 曾"两处注释互相推诿、实际都没做"）。
2. **PIT / 协议 / 计划全是纯函数**：真机之外全部可单测；传输是唯一设备依赖点。
3. **设备检测用 CDC_DATA 类匹配**（Thor/odin4 现行做法）+ PID 表兜底，**不照抄 Heimdall 的 3 个老 PID**。

## 4. PIT 数据模型与计划层

```cpp
namespace odin {
struct PitEntry {
    QString name;        // 分区名（清洗：截 NUL + 去 CR/LF + 去首尾空白）
    quint32 id;
    quint32 deviceType;  // 0..3 老枚举；8 = UFS（按数值透传，不做枚举裁剪）
    quint32 attributes;  // **不做语义解释**（三方解读不一致）→ 只透传原值
    quint64 offset;      // 起始扇区
    quint64 size;        // 扇区数
    // …其余字段按三方一致部分落实，逐字段带出处；三方不一致的字段如实标注"解读存疑"
};
struct PitTable {
    QByteArray comTar2;   // 头 8..15
    QByteArray cpuBlId;   // 头 16..23
    quint16 luCount;      // 头 24..25（不当 padding）
    QByteArray trailing;  // 28+count*132 之后的尾部签名块（原样收，不解释，只记长度）
    QList<PitEntry> entries;
};
bool parsePit(const QString &path, PitTable &out, QString *error);   // 纯函数
bool serializePit(const PitTable &t, QByteArray &out, QString *error); // 仅在需要时（本期可能不做）
}
```

**四条陷阱 → 设计与测试**

| 事实 | 设计 | 测试 |
|---|---|---|
| 尾部签名、长度不定 | **不要求** `28+count×132 == fileSize`；剩余字节收进 `trailing`，只记长度 | **10 个真 PIT 必须全部解析成功**（硬断言） |
| `lu_count` ≠ 0 | 不当 padding；与条目数的关系**只记不拒**（三方说法不一） | 真样本上断言读到非 0 |
| `attributes` 三解读 | **不做位域解释**；只透传原值，UI 显十六进制 | 断言原值透传 |
| 字符串含 CR/LF | 统一清洗（截 NUL + 去 CR/LF + 去首尾空白） | 合成用例覆盖含 `\r\n` 的名字 |

**计划层**
```cpp
struct SamsungPlanEntry { QString partition;   // PIT 分区名
                          QString imageFile;   // tar.md5 内的条目名
                          quint64 sizeBytes;   // 镜像大小
                          bool    verifyOk; }; // 构建时校验结果（MD5/sha256/大小）
struct SamsungPlan { QString pitSource;            // "包内 *.pit" / "设备 dump" / "用户指定"
                     QStringList tarMd5Files;      // 参与的 BL/AP/CP/CSC
                     QList<SamsungPlanEntry> entries;
                     QStringList warnings;         // 两类不匹配 + 大小不符 + 未校验 …
                     quint64 totalBytes; };
bool buildSamsungPlan(const QStringList &tarMd5Files, const PitTable &pit, SamsungPlan &out, QString *error);
```
- 拆包**复用 `tar_image`**（不重写）。
- **镜像 ↔ 分区匹配规则（真数据已给定，见下）**：**以 PIT 条目自带的文件名字段为准**（不是靠扩展名猜、也不靠"分区名 + .img"推断）——
  真包实证：`J1POP3G.pit` 的条目成对给出「分区名 / 文件名」，且与 tar 内镜像名逐个吻合：
  `BOOT/spl.img`、`BOOT2/spl2.img`、`SBOOT/sboot.bin`、`SBOOT2/sboot2.bin`、`WDSP/SPRDDSP.img`、`MODEM/SPRDCP.img`、`wfixnv1/nvitem1.bin` …；
  对照 BL/MODEM 包内容：`spl.img`/`sboot.bin`/`sboot2.bin`、`SPRDCP.img`/`SPRDDSP.img`/`nvitem.bin` ✓ 全对上。
  **已知反例（必须在实现里处理）**：tar 内 PIT 文件名是 `J1POP3G.pit`，而 PIT 内部该条目声明 `J1POP3G_LTN_OPEN.pit` —— **不一致**，故 `PIT` 条目不能按精确名匹配（按 `.pit` 扩展名 + 唯一性处理，并记一条日志说明用了哪条规则）。
  匹配细节（大小写、路径前缀、未匹配条目）一律**记进 `warnings`**，并对真包断言其行为。
- **两类不匹配只 warning + 继续**（用户决策）；warnings 在预览里置顶。
- 分区大小与镜像大小不符 → warning（不自动裁切/补齐）。

## 5. 协议与会话

**协议层**（`odin_protocol`，纯函数）
- 握手（`Odin` 命令）→ 设备信息（版本/机型/deviceType）→ 版本协商 → （可选）PIT dump → 逐分区：**发文件段（分区 ID + 序号 + 数据块）→ 结束帧（含整包 MD5）→ 等 ACK**。
- 所有命令号/字段/长度语义**逐条带出处**（三方对照）；三方不一致处**并列标注**。

**会话层**（`odin_session`）
```
1. 打开传输（CDC_DATA 类匹配 → 兜底 PID 表）
2. 握手 + 版本协商（失败 → 中文错误带阶段名，**不复位**）
3. 计划校验 + 对账：设备 PIT（若读到）vs 包内 PIT → 不一致 **warning**，**以设备 PIT 为准**
4. 逐条目写入（分块 + 进度 + 每步严格等 ACK）
5. 收尾：结束会话；复位 best-effort（**不因超时/NAK 判失败，但必须落日志**）
```
- **中止语义**照 Phase B：任何 ACK 异常 → 立即停、**不复位**、错误含条目名/已写字节/偏移。
- **repartition 本期不做**；PIT 的 offset/size 越界或与镜像矛盾 → 按 §4 只 warning（**但预览必须显眼**）。

## 6. 验证策略与诚实边界

| 层 | 内容 | 强度 |
|---|---|---|
| **真样本** | PIT 解析对 **10 个真 PIT 硬断言**（分区数/名字/offset/size 自洽、`trailing` 非空、`lu_count` 非 0） | 本计划最强的离线证据 |
| **真包** | `buildSamsungPlan` 对 **3 个真 tar.md5**（BL/CSC/MODEM）验证：拆包 + 镜像↔分区匹配 + MD5 校验（含 `␣*` 变体） | 真实包实例 |
| **对拍** | 协议帧常量与 Heimdall/odin4/Thor 逐字段对拍（带出处）；PIT 字段语义与 samloader-rs 对照 | 三方一致才敢用 |
| **mock** | `IOdinTransport` 脚本化响应 + 写字节全记录 → 断言完整会话的字节序列（分块/结束帧/MD5） | 与 Phase B 同款 |
| **前置修复** | `tar_image` footer 两变体：合成文件用例 + 判别力自证 | 修既有静默缺口 |

**诚实边界（写进交付物与功能清单）**
- **真机全链未验证**：USB 时序、CDC_DATA 类匹配的实际枚举、真实 ACK、**bootloader 是否接受未签名镜像**。
- **设备侧读 PIT（dump）无真机可验** —— 只到"代码就绪 + mock"。
- **repartition 不做**；**`attributes` 不做语义解释**（三方解读不一致）；`deviceType` 只透传不裁枚举。
- 真包验证覆盖**一台老机型**（SM-J110H，展锐方案）的 BL/CSC/MODEM；AP 包（966MB）未纳入；其它机型/现代 UFS 机型待验。

## 7. 交付物清单

- [ ] `src/core/odin/` 6 个模块（§3）
- [ ] `src/image_engine/tar_image.*` footer 两变体前置修复 + 用例
- [ ] `FlashTool` 第 5 通道 `samsung-odin` + params 注释同步
- [ ] `src/ui/flash_plan_dialog.*`（或同族对话框）接入 SamsungPlan 预览 + 未验证勾选
- [ ] 测试：`test_pit`（10 真样本）/ `test_odin_protocol` / `test_odin_session`（mock）/ `test_samsung_plan`（3 真包）/ `test_tar` 补两变体
- [ ] 文档：`功能清单.txt`（规划中那行 + 刷写段）、`README.md`（源码树 + 通道）、本 spec 配套事实报告归档
- [ ] 诚实边界：真机未验 / 设备 dump PIT 未验 / repartition 不做 / attributes 不解释

## 8. 明确不做

- **真机联调与真机验证**（用户明确划出，留给持机人）
- **repartition**（破坏性，需专门设计）
- **FUS 下载/解密**（`.enc2`/`.enc4`：`samloader` 那族覆盖，本项目不做；用户自行下载固件）
- `attributes` 位域语义解释
- Heimdall 的 `--print-pit` 之类纯工具命令（除非顺带需要）

## 9. 参照物与样本许可

| 参照 | 许可 | 用途 |
|---|---|---|
| `reference/heimdall`（Benjamin-Dobell，v1.4.2） | **MIT** | 主参照（含 `libpit`） |
| `reference/odin4-llucs` | Apache-2.0 | C/C++ 参照（PIT/校验/LZ4/刷写/`--check-only`） |
| `reference/samloader-rs` | Apache-2.0 + MIT | PIT 语义直系对照（`libpit` 的 Rust 移植） |
| `reference/thor`（TheAirBlow.Thor） | MPL-2.0 | 第三方对拍 |
| `reference/samloader` | GPL-3.0（已归档） | **不使用**（无 PIT/无刷写） |
| `brokkr-flash` | GPL-3.0（仅登记，未克隆） | 同许可且 C++/Qt6，若需更深引用再评估 |
| `SamloaderKotlin` | MIT（仅登记） | 仅下载/解密，与本计划无关 |

- 真 PIT/tar.md5 样本来源、sha256 见配套报告；**样本与参照均在 `reference/`（gitignored），不进构建、不进提交**。
- 本项目 GPLv3；自研代码按"独立实现，格式事实与公开常量可引用"标注来源；不整文件复制。

## 10. 验证清单（全部完成后）

- [ ] `ctest --test-dir build --output-on-failure` 全绿（含新目标）
- [ ] 10 个真 PIT 解析硬断言通过；3 个真 tar.md5 的计划构建 + 校验通过（含 `␣*` footer 变体）
- [ ] mock 会话断言完整字节序列（分块/结束帧/MD5/严格 ACK）
- [ ] **真机验证：本阶段不做**（交付说明保留"设备侧未验证"清单）
- [ ] GUI 拖放/渲染待用户手动验证一次
