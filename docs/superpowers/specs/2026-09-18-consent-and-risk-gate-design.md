# 用户协议与风险分级门控 设计 spec

> **状态**：待用户评审（重点审 §2 的协议正文与 §3 的分级映射）。
> **用户已定的三条**（2026-09-18）：① 门控 = **启动首屏一次 + 高风险功能二次确认**；② 协议**先落地**，
> exploit 类（kamakiri/carbonara、DA 提取、签名绕过、QFuse）**仍维持"不做"**、日后逐条单独评估；
> ③ 正文**明写**具体敏感行为（自咬式披露）。

## 1. 目标与非目标

**目标**：把"这个工具能做什么、风险是什么、你只能用在什么设备上"变成**产品内的明示条款 + 可验证的同意留痕**，
并按风险分级在**真正的危险动作之前**收一次确认；文本与 `功能清单.txt` / README 的诚实边界**口径一致**。

**非目标（明确不做）**：
- **不做**法律意义上的"许可"：条款不是对违法行为的授权，UI 必须明说这一点（§2 第五条）。
- **不限制 GPLv3 权利**：本仓 GPLv3，条款不得限制再分发/修改（§2 第六条）。
- 不引入在线校验/DRM/遥测；不因为协议而阻断**只读**功能（设备信息查看、Logcat 等一律不门控）。
- 不为"每次操作"弹窗（用户已选"启动 + 高风险二次确认"两档）。

## 2. 协议正文（草稿，`resources/legal/user-agreement.md`，版本常量 `1`）

> 下面是要落进产品的**实际文本**（审这段等于定产品的对外口径）。行文尽量短句、可读；**不写**法律黑话。

```markdown
# PhoneToolbox 使用须知与免责声明（版本 1）

## 一、这是什么工具
PhoneToolbox 是一个**面向设备所有者与维修人员**的 Android 设备工具箱，提供设备信息查看、系统调试、
固件刷写、底层引导模式（EDL / MTK BROM / 展锐 / 华为 / 三星 Odin / Exynos EUB）与漏洞扫描等能力。
它**不包含**任何厂商固件、密钥或签名二进制；所有固件、引导镜像与载荷**由你自行准备**。

## 二、你只能用于自己的设备或已获得明确授权的设备
- 未经设备所有者同意，对**他人设备**进行刷写、解锁、清除数据或漏洞利用，在几乎所有司法辖区都是**违法**的。
- 你确认：你对所连接的每一台设备都拥有所有权，或已取得设备所有者对该次操作的明确授权。

## 三、操作有风险，后果由你承担
本工具的写操作与底层操作**可能**导致：数据丢失、设备变砖、保修失效、不可逆的硬件状态改变
（例如一次性熔丝写入），甚至设备永久无法恢复。**请先备份、先确认机型与固件匹配。**
本工具的部分路径**未经真机验证**（见项目文档中的"诚实边界"），实际行为可能与预期不同。

## 四、以下行为即使你同意本须知也不被许可
本须知**不是**对违法行为的许可。特别是：
- **改写 IMEI、序列号等设备标识**：在多数国家和地区**违法**，无论设备是否属于你。
- **绕过他人设备的安全机制**（引导链校验、签名校验、锁屏、账户锁）：可能违反计算机犯罪相关法律。
- **规避运营商/厂商的合法限制**：可能违反合同与出口管制规定。
本版本**不提供**：引导链漏洞利用（kamakiri / carbonara 一类）、DA 二进制提取/生成、签名绕过、
熔丝（eFuse）写入、以及任何三星签名二进制的内置或分发。若未来版本提供其中某项，会单独说明并再次征求你的同意。

## 五、无担保
本工具按"现状"提供，不附带任何明示或暗示的担保（包括但不限于可商用性、特定用途适用性、不侵权）。
作者与贡献者**不对**使用本工具造成的任何直接或间接损失负责。见 GPLv3 第 15、16 条。

## 六、本须知不影响你的自由软件权利
本工具以 GPLv3 发布。本须知是**使用须知与免责声明**，**不限制** GPLv3 授予你的任何权利
（包括运行、研究、修改与再分发）。你收到的副本仍受 GPLv3 约束。

## 七、同意与版本
勾选同意后，你的选择会记录在本机（不联网、不上传）。本须知更新（版本号变更）后需要你重新确认。
```

**实现约束**：正文以**单一副本**存于 `resources/legal/user-agreement.md` 并编入 `resources.qrc`
（`QFile(":/legal/user-agreement.md")` 读取）；协议版本常量与正文里的"版本 N"必须一致（用例钉住）。

## 3. 风险分级与映射（三个 tier）

| Tier | 名称 | 覆盖的功能（**本版本实际存在的**） | 二次确认时机 |
|---|---|---|---|
| 1 | 使用须知 | 全部（首屏） | 启动时（未接受过该版本 → 弹窗阻断，未接受则退出程序） |
| 2 | 底层引导与写操作 | EDL（oppo-edl）/ MTK BROM / 展锐 / 华为 USB Update / 三星 Odin / **EUB 救援** / 死砖修复 / FRP 清除 / Fastboot 分区刷写·擦除·格式化 / fastboot update / Sideload / ADB Root 刷入 / Bootloader 解锁·回锁 / MTK 解锁·回锁 / 内存转储 / 运行刷机脚本 | 该版本首次进入任一项时 |
| 3 | 诊断与安全研究 | 漏洞扫描与利用面板（**exploit 脚本可执行**）/ 工程模式入口 / SafetyNet 绕过检测 / 签名伪造支持检测 / 设备信息伪造指南 / IMEI 相关入口 | 该版本首次进入任一项时 |

**纯函数映射**（可单测，不碰 Widgets/libusb —— 与 `flashui::flashButtonLabelsFor` 同款分层）：
```cpp
namespace riskui {
enum class RiskTier { None = 0, Tier2Boot, Tier3Diagnostic };
RiskTier riskTierForMode(DeviceDetector::DeviceMode mode);   // 按设备模式（协议通道/EDL/…）
enum class RiskFeature { DieRepair, FrpClear, PartitionFlash, Unlock, Sideload, AdbRoot,
                         VulnExploit, EngineerMode, SafetyNet, SignatureSpoof, ImeiTool, MtkUnlock };
RiskTier riskTierForFeature(RiskFeature f);                  // 按功能入口
QString tierTitle(RiskTier t);                               // "底层引导与写操作" / "诊断与安全研究"
QStringList tierWarnings(RiskTier t);                        // 该 tier 的具体风险要点（进二次确认对话框）
}
```
- **默认保守**：未列出的模式/功能一律 `RiskTier::None`（不弹），但**新增危险功能时必须显式登记**
  （用例里加一条"全枚举覆盖"断言，漏登记即红）。
- **Tier 3 覆盖漏洞面板是本次的重点**：exploit 脚本目前**可以直接执行**，而它属"绕过安全机制"一族 ——
  协议正文第四条已明写其法律边界，门控在这一版把它纳入。

## 4. 同意留痕（`src/legal/consent_store.{h,cpp}`）

```cpp
namespace legal {
constexpr int kAgreementVersion = 1;                 // 与 resources/legal/user-agreement.md 的"版本 N"一致（用例钉住）

class ConsentStore {                                 // 可注入 QSettings（用例指向临时 ini）
public:
    explicit ConsentStore(QSettings *settings = nullptr);   // 默认 UserScope 的 IniFormat
    bool hasAcceptedAgreement() const;               // legal/agreementVersion >= kAgreementVersion
    void acceptAgreement();                          // 写入当前版本
    bool hasAcceptedTier(riskui::RiskTier t) const;  // legal/tier2Version >= kAgreementVersion（Tier3 同理）
    void acceptTier(riskui::RiskTier t);
};
}
```
- 存储：`QSettings`（本仓首次引入；UserScope + IniFormat，跨平台路径由 Qt 决定）。
- **不联网、不上传**（正文第七条已向用户承诺）。
- 版本升级（`kAgreementVersion` 增大）→ 首屏与两个 tier 都要求重新确认。

## 5. 门控接线（钩子点，逐处）

| 位置 | 行为 |
|---|---|
| `MainWindow` 构造/显示前 | `hasAcceptedAgreement()` 否 → `AgreementDialog`（模态）；用户拒绝 → `QApplication::quit()`（不进入主界面） |
| `FlashPanel::onFlashClicked`（分派前） | `riskTierForMode(mode)` ≠ None 且该 tier 未接受 → `RiskConfirmDialog`；拒绝则**不发起任何操作**（静默返回，与"用户取消"同口径） |
| `EubRecoveryDialog::onStart`（发送前） | 同上（EUB 的 tier 由 `riskTierForMode(MODE_SAMSUNG_EUB)` 给） |
| `SystemToolPanel` 的 5 个入口（死砖修复 / FRP / 解锁 / Root / Sideload）与诊断类 4 个 | `riskTierForFeature(...)` 同上 |
| `VulnPanel` 的"批量利用/单条利用/批量扫描" | `riskTierForFeature(RiskFeature::VulnExploit)` 同上（**扫描**也纳入：它出自 exploit DB） |

**共同形态**（沿用仓内既有"未验证勾选门控"）：对话框展示 `tierWarnings()` + 完整协议入口（可展开/滚动）
+ 一个必须勾选的确认框 → 勾选后"继续"可用 → 接受则写入 `ConsentStore` 并继续原操作。

## 6. 测试策略

| 目标 | 覆盖 |
|---|---|
| `test_risk_tiers.cpp`（新，纯函数） | `riskTierForMode` 逐枚举（**含"全枚举覆盖"断言**：`DeviceMode` 每个值都有明确判定，新增模式必须显式登记）；`riskTierForFeature` 逐项；`tierWarnings` 非空且含关键风险词 |
| `test_consent_store.cpp`（新） | 未接受 → false；接受 → true；**版本升级语义**（把 store 里写入 `kAgreementVersion+1` 的旧值 → `hasAcceptedAgreement()` 应为 false 的反向断言）；tier 独立记录（接受 Tier2 不影响 Tier3）；用临时 ini（`QTemporaryDir`） |
| `test_agreement_dialog.cpp`（新，Widgets + offscreen） | 勾选两个框前"接受"禁用；勾选后可用；拒绝关闭 → 返回 Rejected；**协议资源存在且含"版本 1"**（与常量一致） |
| `test_risk_confirm_dialog.cpp`（新，同上） | 未勾选 → 继续禁用；勾选 → Accept；展示的要点来自 `tierWarnings()` |
| `test_flash_button_labels.cpp`（backlog 任务 3，已有计划） | 不受影响 |

## 7. 交付清单

1. `resources/legal/user-agreement.md` + `resources.qrc` 登记 + 版本一致性用例
2. `src/legal/consent_store.{h,cpp}` + 用例
3. `src/ui/risk_ui.{h,cpp}`（纯函数映射 + 文案）+ 用例
4. `src/ui/agreement_dialog.{h,cpp}`（首屏）+ 用例
5. `src/ui/risk_confirm_dialog.{h,cpp}`（二次确认）+ 用例
6. 接线：MainWindow 首屏门控、FlashPanel、EubRecoveryDialog、SystemToolPanel、VulnPanel
7. 文档：`功能清单.txt` / `README.md` 增加"使用须知与风险分级"一节（口径与正文一致）+ `docs/superpowers/specs/` 本 spec 定稿

## 8. 诚实边界（本特性自身必须写清）

- 这是**意图留痕与风险告知**，不是法律许可、也不能把违法变合规（正文第四条已明写）。
- 门控是**客户端**的：删除配置或改源码即可绕过 —— 它的价值在"提醒与留痕"，不在"强制"。
- 真机行为：本特性全部可离线验证（纯函数 + settings + offscreen 对话框），**无真机依赖**（这是本特性与刷写链的区别）。
