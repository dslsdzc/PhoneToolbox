# 展锐 `.pac` 内 FDL 自动提取 设计 spec（队列项 ①）

> **状态**：待用户评审。**队列来源**：`功能清单.txt` 排队中（特殊模式）第 1 条
> —— 现状 "FDL1/FDL2 需用户手选"。

## 1. 目标与非目标

**目标**：用户只选一个 `.pac`，工具**自动提取**其中的 FDL1/FDL2 并用于刷写；
**手选保留为回退**（包内没有 FDL、或用户想用别的 FDL 时）。

**非目标**：
- **不做**"强制手选/覆盖自动提取"的显式开关（YAGNI：提取失败时自然走手选）。
- **不做**从 `.pac` 内 XML 读加载地址 —— 现有实现用**常量**（FDL1 `0x40004000` / FDL2 `0x14000000`，
  `spd_storage.cpp:249,258`），本项不改变这一点。
- **不解压/不转换** FDL 二进制（原样上传；FDL 是否签名由用户自备的包决定）。
- 不改动 SPD 协议层/刷写链本身。

## 2. 事实（全部来自本仓现有实现，无新协议事实）

| 事实 | 出处 |
|---|---|
| `.pac` 解析器已存在；条目含 `name`（分区 ID，注释举例 `FDL1 / preloader / boot`）与 `fileName`（容器内文件名） | `src/image_engine/pac_image.h:15-17` |
| FDL 分区名判定已是**纯函数**（单测覆盖） | `src/core/modes/spd_storage.h:78` |
| 现有集成路由：`runSpdFlash(pacPath, fdl1Path, fdl2Path, …)` 已解析 pac 并切片分区数据（`pacData.mid(offset,size)`） | `src/core/modes/spd_storage.cpp:210-289` |
| UI 现状：SPD 分支先选 pac、再弹两个手选（FDL1/FDL2） | `src/ui/flash_panel.cpp:727-735` |
| 测试侧已有 pac 构造器（新/老两代布局） | `tests/test_pac.cpp:95,111` |

## 3. 设计

### 3.1 核心：按名提取（`src/core/modes/spd_storage.{h,cpp}` 或新建 `spd_pac.{h,cpp}`）

```cpp
namespace spd {
struct ExtractedFdl { QString name; QString fileName; QByteArray data; };
// 从 pac 内提取 FDL1 与 FDL2（按 isFdlPartition 判名，大小写不敏感——沿用既有实现）。
// 两个都找到 → true；任一缺失 → false + 文案列出**缺哪个**与**包内有哪些条目**（可行动）。
bool extractFdlsFromPac(const QString &pacPath, ExtractedFdl &fdl1, ExtractedFdl &fdl2, QString *error);
}
```
- 实现：`QFile` 读入 → `imgpac::parsePac` → 遍历找 FDL1/FDL2（复用 `isFdlPartition`，
  若它不区分 1/2 则按名字含 `fdl1`/`fdl2` 细分，**以其实现为准，不另立判据**）→ `pacData.mid(offset, size)`。
- **失败语义**：pac 打不开 / 解析失败 / 缺任一 FDL → false + 原因（缺项 + 包内条目摘要）。

### 3.2 接线（`src/ui/flash_panel.cpp` 的 SPD 分支）

```
选 pac → 尝试 extractFdlsFromPac
  ├─ 成功 → 跳过两个手选对话框；日志：`已从 pac 自动提取 FDL1（<条目名>，N 字节）与 FDL2（…）`
  │         并把两个临时文件（或字节）交给通道（沿用现有 params：fdl1Path/fdl2Path）
  └─ 失败 → 日志给出原因（缺哪个 + 包内条目），**照旧弹两个手选**（行为不变，用户无感降级）
```
- 提取出的 FDL 需要**落盘**才能走现有 `fdl1Path/fdl2Path` 参数（通道按路径读）——
  写到 `QStandardPaths::TempLocation` 下的临时文件（文件名带条目名），并在刷写结束后删除（best-effort）。
  ⚠️ 本机 `/tmp` 满 → **用例不依赖真实临时目录**（提取函数返回字节；落盘部分用 `QTemporaryDir` 测）。

### 3.3 真样本（可选但推荐，用户已授权下载）

下载一个**展锐 `.pac`**（公开固件包，如 SC9863A/A55 等机型的官方卡刷包）到 `reference/spd-samples/`
（gitignored），核对：真包内 FDL 条目的**实际名字**是否被 `isFdlPartition` 命中、提取出的字节是否为可识别的
FDL 载荷（大小/头部自洽）。**拿不到就如实写"未取到真包"**，不阻塞。

## 4. 测试策略

| 目标 | 覆盖 |
|---|---|
| `test_pac.cpp`（扩展）或新 `test_spd_pac.cpp` | 用 `buildPacNew`/`buildPacLegacy` 造含 FDL1+FDL2 的包 → 提取字节与包内切片逐字节相等；缺 FDL2 → false + 文案含 `FDL2` 与包内条目；非 pac 文件 → false；**多条目重名/顺序无关**（提取不依赖条目顺序） |
| `test_flash_panel` 侧（若不可离线驱动则记录缺口） | SPD 分支自动提取成功 → **不再弹手选**（可用注入口/或记录为"UI 层无自动化覆盖"） |

## 5. 交付清单

1. `spd::extractFdlsFromPac` + 用例（合成 pac）
2. FlashPanel SPD 分支接线（自动优先 + 手选回退 + 日志）
3. （可选）真 `.pac` 样本核对，结论写进本 spec 或 facts
4. 文档：`功能清单.txt` 该队列项移入已交付并写明边界；README 展锐段落补一句

## 6. 诚实边界

- 提取本身是**纯解析**（可离线完整验证）；**FDL 的真机可用性**仍属既有 SPD 刷写链的未验证范围（真机待持机人）。
- 常数加载地址（`0x40004000` / `0x14000000`）是既有实现的口径：**若真机需要按芯片/包内 XML 取值**，
  那是另一项工作（需真实抓包或设备证据），本项**不声称**地址一定正确。
