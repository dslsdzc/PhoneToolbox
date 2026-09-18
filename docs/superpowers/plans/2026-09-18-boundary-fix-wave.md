# 边界修复波（W4）Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development 逐任务执行。

**Goal:** 把 2026-09-18 全仓边界盘点里**仍成立**的条目落地（代码 A 类 + 文档 B 类），每条都带用例或如实记录缺口。

**条目来源（唯一权威）：** `.superpowers/sdd/w4-fix-candidates.md` —— 58 条逐条核实过（今天的位置与逐字引用），
每条含：现状位置 / 逐字代码 / 是否仍成立 / 最小修法 / 影响面 / 是否触碰 flash_panel.cpp / 是否需决策。
**任务书正文 = 该报告的区域切片**（见 `.superpowers/sdd/w4-task-*-brief.md`），不要另起一份需求。

**已失效、不要动**：A-07 / A-42 / A-44（已被既有交付覆盖）、B-04 / B-10（已写入文档）。
**控制方已裁决（2026-09-18，无需再议）**：
- **A-14**（libusb 骨架抽取）：`mtk_brom.cpp` / `spd_flash.cpp` 是协议文件里直接调 libusb，**不是** DeviceOpener
  骨架 → 不构成"第四份骨架"，**维持 backlog**，本波不动。
- **A-28**（UFS 未拒）：取**方案 1 fail-closed** —— 引导链拿到的存储类型不是 eMMC 就明确拒绝并点名原因。
- **A-32**（`samplesAvailable()` 无调用方）：**保留函数 + 注释改成实况**（不删，后续任务可能要用）。
- **A-36**（scatter 解析失败中止）：**维持中止**，不加"继续"降级（降级会让选错文件被放过；现有预览已可见）。
  在 `flash_panel.cpp` 该分支补一行注释说明这是有意的。
- **A-40**（altsetting）：**补实现**（`DeviceInspection` 带 altSetting；claim 后 `!= 0` 才
  `libusb_set_interface_alt_setting`，失败进 notes）—— odin 与 edl 两处**同款**，不许只改一边。
- **A-41**（`IOdinTransport::write` 无超时形参）：**维持不改**（既有裁决），不动。
- **A-45**（元数据未声明 SECTOR_SIZE）：**维持 warn-only**，不动判据（改判据会真回归两条既有用例）。
- **A-17**（全库 `-Wall -Wextra` + 113 条既有警告）：**不属于本波**，在 W5 单独做。

## Global Constraints

沿用本仓相位约束：无真机不得写"已验证"；`/tmp` 满 → 全新构建目录放 `/home`；
**"0 警告"须显式 `-Wall -Wextra`**；只 `git add` 明确路径（提交前 `git status --porcelain` 对表）；
**恒真断言要加固并给逐条可复核的变异证据**（日志进 `.superpowers/sdd/w4-*-logs/`，要能复核命令与输出）；
文档风格不写"我们决定不做 X"这类留痕式表述。

## 任务划分（每任务 = 一个实现者 + 一个独立审查者）

| 任务 | 范围 | 报告切片 | 备注 |
|---|---|---|---|
| W4-T1 | EUB A 类（A-01～A-16） | `w4-task-1-brief.md` | 跳过 A-07/A-14；A-17 归 W5；A-16 是"注入口"（与 W3 的 EubOptions 同项，一并做） |
| W4-T2 | MTK A 类（上：A-18～A-31） | `w4-task-2-brief.md` | 含 A-28 fail-closed |
| W4-T3 | MTK A 类（下：A-32～A-39） | `w4-task-3-brief.md` | A-36 = 只补注释；A-32 = 改注释 |
| W4-T4 | Odin + EDL A 类（A-40/A-41/A-43/A-45） | `w4-task-4-brief.md` | A-40 两处同款；A-41/A-45 不动 |
| W4-T5 | B 类文档/文案 13 条 + inventory 失真项更正 | `w4-task-5-brief.md` | 含 W6 侦察对 `功能清单.txt` 第 370/373/375/377 行的更正建议 |

**顺序**：T1 → T2 → T3 → T4 → T5（共用一棵工作树，逐个来）。触碰 `src/ui/flash_panel.cpp` 的只有
A-36（只加注释）与 A-17（W5）；与 W1-T2 / W2-T5 的 flash_panel 改动**排在不同批次**，不并行。

## 计划自查

- 覆盖：报告里"仍成立"的 36(A) + 11(B) 条全部落进某个任务；已失效 5 条明确不动；7 条决策项逐条给了裁决。
- 已知缺口：报告里标注"需真机/无自动化覆盖"的条目，本波只能补到用例或如实记录，不许造假绿。
