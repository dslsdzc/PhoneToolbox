# MTK preloader 直连：检测接线 设计 spec（W6-T1）

> **状态**：待用户评审。**来源**：`.superpowers/sdd/w6-d-class-recon.md` §2（只读侦察，结论：**做**——前一半）。
> **用户指令**（2026-09-18）：「W6：每项推进到'能做则做、不能则给诚实结论与前置'」。

## 1. 目标与非目标

**目标**：让本仓**看得见**以 preloader 身份插入的 MTK 设备（当前完全看不见它），并把它交给**已交付的 BROM 链**。
**非目标**：
- **不做** `0x70/0x71`（`SEND_PARTITION_DATA` / `JUMP_TO_PARTITION`，"把 preloader 当 DA 用"）—— W6 侦察的建议是
  "先侦察再定"；本项**零动作**。
- **不做** `plstage` / patch（那一步接近漏洞利用链，另有条目）。
- **不改** 协议层行为（握手、`0xA0` 预发、`GET_BL_VER` 判据都已实现且不动）。

## 2. 事实（W6 侦察已核，控制方二次核对）

| 事实 | 出处 |
|---|---|
| 握手字节**完全相同**；唯一差别是"PID 不在 BROM 表里就先单发一个 `0xA0`" | 上游 `Port.py:136-141` |
| BROM / preloader 的身份是**握手之后**由 `GET_BL_VER` 回值判定（`== 0xFE` → BROM） | 上游 `mtk_preloader.py:664-673`；本仓 `mtk_brom.cpp:638-649` 已实现 |
| 协议层的 PID 表**已含** preloader 一族 | `src/core/modes/mtk_brom.cpp:19-26`（`0x0E8D:0x6000/0x2000/0x2001/0x20FF/0x3000`、`0x1004:0x6000`、`0x22D9:0x0006`、`0x0FCE:*`） |
| **检测层只认 BROM** | `src/core/device_detector.cpp:555` = `{ 0x0E8D, 0x0003, MODE_MTK_BROM }` —— preloader PID 全部不在表内 |
| 上游在非 BROM 时**主动关掉** patch（`daloader.patch = False`） | `mtk_da_handler.py:199-201`（即 preloader 分支**比 BROM 更保守**） |

⇒ **这不是能力缺失，是能力不可达**：协议层写好了，设备却进不了门。

## 3. 设计

### 3.1 单一来源（本项的关键取舍）
检测层与协议层**各自维护一份 VID/PID 表**会漂移（本仓已多次吃亏）。因此：
- 在 MTK 层暴露一个**判据函数**（与 `kUsbIds` 同文件、同源），例如
  `namespace mtkbrom { bool isKnownMtkUsbId(quint16 vid, quint16 pid); }`；
- 检测层**调用它**，不再自己列第二份表。
- 若实现者发现"`device_detector.cpp` 引入 `mtk_brom.h` 的代价过大"（该头拖 libusb —— 而检测层本就用 libusb 做 EDL 判定，
  故预期不构成问题），**退路**是把 VID/PID 表抽到一个薄头（如 `mtk_usb_ids.h`）供两边共享；**无论走哪条，表只此一份**。

### 3.2 检测层接线
- 命中 → `MODE_MTK_BROM`（**不新增模式**：协议层对两者是同一条链；新增模式会波及 `flash_button_labels`/`risk_ui`/
  `flash_panel`/`main_window` 等 12 处 `switch`）。
- 模式文案/UI 提示：**如实**说明"可能是 BROM 或 preloader（握手后判定）"，不许把 preloader 说成 BROM。
- 检测顺序：仍排在 ADB / Fastboot 之后、与其他 USB 类判据并列（**不抢**已有厂商判据：三星 EUB 的教训）。

## 4. 测试策略

| 目标 | 覆盖 |
|---|---|
| `test_mtk_usb_ids`（新，纯函数） | `isKnownMtkUsbId` 对 BROM PID / preloader PID / 未知 PID 的判定；**与 `kUsbIds` 同源**（改表即两侧同时变） |
| `test_preloader_detection`（新或扩既有） | 检测层在给定 VID/PID 下判为 `MODE_MTK_BROM`；**给定 0x0E8D:0x6000 → 命中**（变异：删该行 → 红）；给定无关 VID/PID → 不命中 |
| 回归 | 既有检测用例全绿（尤其**不得**抢走 Odin / 华为 / 展锐的判据） |

**真机不可验证**（本机无 MTK 设备）："preloader 身份插入后握手能否走通"留给持机人；文档与 UI 必须如实标注。

## 5. 交付清单
1. MTK 层判据函数（单一来源）+ 用例
2. `device_detector.cpp` 接线 + 用例（含"不抢既有判据"的负控制）
3. `功能清单.txt` 队列项「MTK preloader 直连」更新为"检测已交付 + `0x70/0x71` 仍待侦察 + 真机待持机人"
4. README 一句

## 6. 诚实边界
- **无真机**：本项交付到"检测 + 交链"，**不声称** preloader 上的握手/刷写可用。
- preloader 的**等待窗口比 BROM 短**（上游 `Port.py:188-192` 有提示）—— 实际抢不抢得到窗口是设备侧事实，未验。
- 本项**不改变**任何既有判据的优先级；若真机上出现"同一设备被两个判据同时命中"，属既有行为，不在本项范围。
