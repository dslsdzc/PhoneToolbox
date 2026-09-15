#!/usr/bin/env python3
"""从 mtkclient 的 brom_config.py 转写芯片表（hw_code -> dacode/damode/iot）到 C++。

用法:  python3 tools/gen_mtk_chip_table.py > src/core/modes/mtk_chip_table.cpp
依据:  mtkclient/mtkclient/config/brom_config.py 的 `hwconfig = { ... }`（第 ~479 行起），
       条目形如 `0x6226: Chipconfig(damode=DAmodes.LEGACY, dacode=0x6226, iot=True, name="MT6226")`。

为什么用 ast 而不是 import：只取静态事实、避免拉起 mtkclient 的 usb/加密依赖；
也避免把它的运行期副作用带进生成流程。

许可: mtkclient 为 GPL-3.0，本项目为 GPLv3 → 兼容；本脚本入库以保证可复现。
"""
import ast
import collections
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "mtkclient" / "mtkclient" / "config" / "brom_config.py"

DAMODE = {"LEGACY": 3, "XFLASH": 5, "XML": 6}   # 与 brom_config.DAmodes 一致

# 规模下限：**只**用来抓"表被换成 stub / 解析读到别的 dict"这类整体性事故。
# **上限不是我拍的、也不是 100 这种圆整数**：上游 v2.1.4.1-20-g71b0175 的 hwconfig 实测
# **89** 条（交叉核对上游 main 分支同为 ~89 条），100 是不可达的。下限 80 = 留约 10% 余量。
# 真正的"解析回归"由 extract() 里的结构不变量兜底（解析条数 == dict 的 int key 数 + 逐 key
# 校验，见下），不靠这个数字。
MIN_ENTRIES = 80


def source_commit() -> str:
    """上游 commit —— 产物头的 permalink 与"可复现锚点"全靠它。

    取不到就**拒绝生成**（而不是写 "unknown"）：出处写不出的产物等于不可复现，
    而本表的全部价值就在于"能追回上游那 89 条到底是哪个版本"。
    """
    try:
        commit = subprocess.check_output(["git", "-C", str(REPO / "mtkclient"),
                                          "rev-parse", "HEAD"], text=True).strip()
    except Exception as exc:
        sys.exit(f"取不到 mtkclient 的上游 commit（{exc}）—— 出处不可复现，拒绝生成")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        sys.exit(f"上游 commit 不是 40 位十六进制（得到 {commit!r}）—— 拒绝生成")
    return commit


def extract():
    tree = ast.parse(SRC.read_text(encoding="utf-8", errors="replace"))
    dict_node = None
    for node in ast.walk(tree):
        # 找 `hwconfig = {...}` 这个赋值
        if not isinstance(node, ast.Assign):
            continue
        if not any(isinstance(t, ast.Name) and t.id == "hwconfig" for t in node.targets):
            continue
        if dict_node is not None:
            sys.exit("brom_config.py 出现多处 hwconfig 赋值 —— 不知道以哪处为准，拒绝生成")
        if not isinstance(node.value, ast.Dict):
            sys.exit("hwconfig 不是 dict 字面量（上游改成 update()/函数返回？）—— 拒绝生成")
        dict_node = node
    if dict_node is None:
        sys.exit("brom_config.py 里找不到 `hwconfig = {...}` —— 上游表结构变了？")

    entries = []
    dacode_fallbacks = []
    for key, value in zip(dict_node.value.keys, dict_node.value.values):
        # 结构不变量（比事后数字下限更早更准地抓住"静默丢条目"）：
        # key 必须是整数字面量 —— 命名常量/表达式 key 会被下面的取值逻辑静默跳过。
        if not isinstance(key, ast.Constant) or not isinstance(key.value, int):
            sys.exit(f"hwconfig 出现非整数字面量 key: {ast.unparse(key)} —— 会静默丢条目，拒绝生成")
        if key.value < 0 or key.value > 0xFFFF:
            sys.exit(f"hwconfig[{hex(key.value)}] 超出 quint16 —— 拒绝生成")
        # value 必须是 Chipconfig(...)：否则下面取字段全落空、默认值会被当事实写进表。
        if not (isinstance(value, ast.Call) and isinstance(value.func, ast.Name)
                and value.func.id == "Chipconfig"):
            sys.exit(f"hwconfig[{hex(key.value)}] 不是 Chipconfig(...) 调用 —— 拒绝生成")

        hw = key.value
        damode, dacode, iot = None, None, False
        for kw in value.keywords:
            if kw.arg == "damode":
                # 只认 DAmodes.<ATTR> 这一种形态：其它写法（字符串/表达式）取不到 → 由下面的
                # 硬失败拦住，而不是静默落到 LEGACY。
                if isinstance(kw.value, ast.Attribute):
                    damode = DAMODE.get(kw.value.attr)
            elif kw.arg == "dacode":
                # **kw 存在**却读不出整数 → 拒绝：静默回填 hw_code 会把"读不懂"伪装成"事实"。
                # （kw 完全缺失是另一回事 —— 上游 0x6280 确实没给，回填 hw_code，见下。）
                if not (isinstance(kw.value, ast.Constant)
                        and isinstance(kw.value.value, int)
                        and not isinstance(kw.value.value, bool)):
                    sys.exit(f"0x{hw:04X} 的 dacode 不是整数字面量（{ast.unparse(kw.value)}）—— 拒绝生成")
                dacode = kw.value.value
                # 范围校验而不是 `& 0xFFFF`：截断会把 0x10907 静默变成 0x0907（写错一条芯片）
                if not 0 < dacode <= 0xFFFF:
                    sys.exit(f"0x{hw:04X} 的 dacode 0x{dacode:X} 超出 (0, 0xFFFF] —— 拒绝生成（拒绝静默截断）")
            elif kw.arg == "iot":
                # 只认 bool 字面量：`bool("False")` 是 True —— 字符串形态会静默把非 IoT 芯片标成 IoT
                if not (isinstance(kw.value, ast.Constant)
                        and isinstance(kw.value.value, bool)):
                    sys.exit(f"0x{hw:04X} 的 iot 不是 bool 字面量（{ast.unparse(kw.value)}）—— 拒绝生成")
                iot = kw.value.value            # 上游 hwconfig 的 iot=True（如 0x6226）
        # damode 缺失/不可解析 → **拒绝**而不是默认 LEGACY：猜代际就是砖机（本表存在的唯一理由）。
        if damode is None:
            sys.exit(f"0x{hw:04X} 的 damode 缺失或不可解析（期望 DAmodes.LEGACY/XFLASH/XML）—— 拒绝生成")
        if dacode is None:
            # 走到这里只可能是 **kw 完全缺失**（读不懂的 dacode 在上面已 exit）：
            # 上游确有 1 条（0x6280）未给 dacode → 按 hw_code 回填（v2.1.4.1-20 实测先例）。
            dacode_fallbacks.append(hw)
            dacode = hw
        # 表内**不得**出现 dacode == 0：它是 DA 条目的匹配键，0 会让匹配必然落空。
        # 显式 dacode=0 已被上面的范围校验拦掉，这里兜的是"回填路径"（即 hw_code == 0 的占位 key）。
        if dacode == 0:
            sys.exit(f"hwconfig[{hex(hw)}] 的 dacode 为 0（hw_code 为 0 的占位 key？）—— 拒绝生成"
                     "（0 会让 DA 条目匹配必然落空）")
        entries.append((hw, dacode, damode, iot))

    # 结构不变量（与下限双保险）：解析条数必须等于表字面量里的 int key 数 ——
    # 少一条就说明有 key 被吞（上面的逐 key 校验与之独立，两者都过才算没丢条目）。
    keys = sum(1 for k in dict_node.value.keys
               if isinstance(k, ast.Constant) and isinstance(k.value, int))
    if len(entries) != keys:
        sys.exit(f"解析条数 {len(entries)} != 表里 int key 数 {keys} —— 有 key 被吞，拒绝生成")
    # 重复 hw_code：dict 字面量里重复键在 python 里是"后者覆盖前者"，逐条收会产出重复条目
    dups = [hex(h) for h, c in collections.Counter(e[0] for e in entries).items() if c > 1]
    if dups:
        sys.exit(f"hwconfig 出现重复 hw_code {dups} —— dict 字面量语义有歧义，拒绝生成")
    entries.sort()
    return entries, dacode_fallbacks


def main():
    if not SRC.exists():
        sys.exit(f"缺少 {SRC}（mtkclient 子模块未初始化？git submodule update --init）")
    entries, dacode_fallbacks = extract()
    if len(entries) < MIN_ENTRIES:
        sys.exit(f"只解析到 {len(entries)} 个条目（上游 89）—— 疑似表被换成 stub，拒绝生成")
    commit = source_commit()
    iot_count = sum(1 for e in entries if e[3])
    # 诊断一律走 stderr：stdout 是产物本身。
    print(f"转写 {len(entries)} 条（iot=True {iot_count} 条；dacode 回填 {len(dacode_fallbacks)} 条"
          f"{'：' + ' '.join(hex(h) for h in dacode_fallbacks) if dacode_fallbacks else ''}）",
          file=sys.stderr)
    out = []
    out.append("// src/core/modes/mtk_chip_table.cpp")
    out.append("//")
    out.append("// ⚠️ **本文件由脚本生成，请勿手改** —— 改表请改生成脚本后重跑：")
    out.append("//     python3 tools/gen_mtk_chip_table.py > src/core/modes/mtk_chip_table.cpp")
    out.append("//")
    out.append("// 数据来源：bkerler/mtkclient（GPL-3.0）的 mtkclient/config/brom_config.py 的 `hwconfig` 表")
    out.append(f"//     commit: {commit}")
    out.append("//     上游 URL: https://github.com/bkerler/mtkclient")
    out.append(f"//     上游原文: https://github.com/bkerler/mtkclient/blob/{commit}/mtkclient/config/brom_config.py")
    out.append("// 本项目为 GPLv3，与本表许可兼容；转写只取静态事实（hw_code / dacode / damode / iot）。")
    out.append(f"// 本表 {len(entries)} 条 = 上游 hwconfig 的全部条目（其中 iot=True {iot_count} 条、"
               f"dacode 按 hw_code 回填 {len(dacode_fallbacks)} 条）；damode 取值同上游 DAmodes。")
    out.append('#include "mtk_chip_table.h"')
    out.append("")
    out.append("namespace mtkbrom {")
    out.append("namespace {")
    out.append("const ChipInfo kChips[] = {")
    for hw, dacode, damode, iot in entries:
        mode = {3: "DaMode::Legacy", 5: "DaMode::XFlash", 6: "DaMode::Xml"}[damode]
        iotText = "true" if iot else "false"
        out.append(f"    {{0x{hw:04X}, 0x{dacode:04X}, {mode}, {iotText}}},")
    out.append("};")
    out.append("constexpr int kChipCount = int(sizeof(kChips) / sizeof(kChips[0]));")
    out.append("} // namespace")
    out.append("")
    out.append("const ChipInfo *lookupChip(quint16 hwCode)")
    out.append("{")
    out.append(f"    // 线性查找：表按 hw_code 升序，二分更快，但表只有 {len(entries)} 项"
               f"且调用点极少（每次会话一次）")
    out.append("    for (int i = 0; i < kChipCount; ++i)")
    out.append("        if (kChips[i].hwCode == hwCode)")
    out.append("            return &kChips[i];")
    out.append("    return nullptr;")
    out.append("}")
    out.append("")
    out.append("int chipTableSize() { return kChipCount; }")
    out.append("")
    out.append('QString damodeName(DaMode m)')
    out.append("{")
    out.append('    switch (m) {')
    out.append('    case DaMode::Legacy: return QStringLiteral("LEGACY");')
    out.append('    case DaMode::XFlash: return QStringLiteral("XFLASH");')
    out.append('    case DaMode::Xml:    return QStringLiteral("XML");')
    out.append("    }")
    out.append('    return QStringLiteral("未知");')
    out.append("}")
    out.append("")
    out.append("} // namespace mtkbrom")
    print("\n".join(out))


if __name__ == "__main__":
    main()
