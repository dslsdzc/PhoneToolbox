#!/usr/bin/env python3
"""从 mtkclient 的 brom_config.py 转写芯片表（hw_code -> dacode/damode/iot）到 C++。

用法:  python3 tools/gen_mtk_chip_table.py [输出路径|-]
       给了输出路径 → 写**同目录临时文件**、成功后 `os.replace()` 原子换入
                     （拒绝路径全部是具名 `sys.exit`，当前 30 处，`grep -c sys.exit` 可复核；
                      原子换入保证**任何拒绝都不碰目标文件**）
       不给路径 / 给 `-` → 产物写 stdout（向后兼容，便于管道）
       例: python3 tools/gen_mtk_chip_table.py src/core/modes/mtk_chip_table.cpp

       ⚠️ **不要**写 `python3 tools/gen_mtk_chip_table.py > src/core/modes/mtk_chip_table.cpp`：
       bash（noclobber 关）会在 exec 之前先截断目标，生成器一旦拒绝生成，入库文件就被留成
       **0 字节**（实测 17 → 0；zsh/fish 的 noclobber 会把那句 `>` 直接拒掉、文件反而没事 ——
       即这个坑**换 shell 才发作**）。"拒绝生成"与"工作树被破坏"是两件事，别混在一起。
依据:  mtkclient/mtkclient/config/brom_config.py 的 `hwconfig = { ... }`（第 ~479 行起），
       条目形如 `0x6226: Chipconfig(damode=DAmodes.LEGACY, dacode=0x6226, iot=True, name="MT6226")`。

为什么用 ast 而不是 import：只取静态事实、避免拉起 mtkclient 的 usb/加密依赖；
也避免把它的运行期副作用带进生成流程。

**已知盲区（W8-T2 审查 Minor-1，如实记录、有意不修）**：上游若把某个字段**改名**
（如 `watchdog` → `watchdog2`），本生成器会把它当作"该字段**缺失**"处理（present=False）——
不报错、不中断。这不是静默砖机路径：受影响芯片的存在位为假 ⇒ 消费方（带检查的取用口）
会**响亮拒绝**该芯片；且用例钉死的覆盖直方图（watchdog 71/89 等）在重跑后**立刻变红**、
失败信息会点名字段。要机器化拦住它，就得枚举上游 `Chipconfig.__init__` 的全部形参
（其中大量与本表无关，不能一律拒绝），收益不抵复杂度 —— 故按"记录 + 用例兜底"处理。

许可: mtkclient 为 GPL-3.0，本项目为 GPLv3 → 兼容；本脚本入库以保证可复现。
"""
import ast
import collections
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "mtkclient" / "mtkclient" / "config" / "brom_config.py"

DAMODE = {"LEGACY": 3, "XFLASH": 5, "XML": 6}   # 与 brom_config.DAmodes 一致

# —— 逐芯片硬件地址（W8-T2）——
# (上游 kwarg 名, C++ 值成员, C++ 存在位成员)。**这个列表的顺序就是 emit 顺序**，
# 且必须与 src/core/modes/mtk_chip_table.h 的 struct ChipHwAddrs 成员声明顺序逐字一致
# —— 表体是按**位置**初始化的聚合（C++17 没有指定初始化器），顺序不一致 = 89 行整表错位，
# 而 watchdog 与 uart 互换就是砖机级别的静默错误。header_struct_members() 在生成期核对。
HW_FIELDS = [
    ("watchdog",          "watchdog",        "hasWatchdog"),
    ("uart",              "uart",            "hasUart"),
    ("brom_payload_addr", "bromPayloadAddr", "hasBromPayloadAddr"),
    ("da_payload_addr",   "daPayloadAddr",   "hasDaPayloadAddr"),
    ("pl_payload_addr",   "plPayloadAddr",   "hasPlPayloadAddr"),
    ("cqdma_base",        "cqdmaBase",       "hasCqdmaBase"),
    ("sej_base",          "sejBase",         "hasSejBase"),
]
HW_HEADER = REPO / "src" / "core" / "modes" / "mtk_chip_table.h"

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


def header_struct_members():
    """从 mtk_chip_table.h 的 `struct ChipHwAddrs { ... };` 里取成员名（声明顺序）。

    为什么要读 header：表体按**位置**初始化（C++17 无指定初始化器），header 里挪一行成员
    声明，89 行就整体错位，而 compiler 一声不吭。生成期核对一次，把"改 header 忘了改生成器"
    从静默错位变成拒绝生成。
    """
    if not HW_HEADER.exists():
        sys.exit(f"缺少 {HW_HEADER} —— 无法核对 ChipHwAddrs 的成员顺序，拒绝生成")
    m = re.search(r"struct\s+ChipHwAddrs\s*\{(.*?)\n\};",
                  HW_HEADER.read_text(encoding="utf-8"), re.S)
    if not m:
        sys.exit("mtk_chip_table.h 里找不到 `struct ChipHwAddrs { ... };`（结构改了？）—— 拒绝生成")
    members = re.findall(r"^\s*(?:quint16|quint32|bool)\s+(\w+)\s*=", m.group(1), re.M)
    expected = (["hwCode"] + [cpp for _, cpp, _ in HW_FIELDS]
                + [has for _, _, has in HW_FIELDS])
    if members != expected:
        sys.exit("mtk_chip_table.h 的 ChipHwAddrs 成员与生成器不一致 —— 拒绝生成（顺序错位 = 静默写错表）：\n"
                 f"  头文件: {members}\n"
                 f"  生成器: {expected}\n"
                 "  改头文件成员后，请同步本脚本 HW_FIELDS 的顺序/命名再重跑。")


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
        # 先收成"名字 → kwarg"：一来下面的逐字段取值不再依赖迭代顺序，二来能拒绝
        # `**kwargs` 展开（字段名静态读不出来）与重复 kwarg（Python 语义是后者覆盖前者——
        # 静默取哪个都可能在写错表）。
        kw_by_name = {}
        for kw in value.keywords:
            if kw.arg is None:
                sys.exit(f"hwconfig[{hex(hw)}] 出现 `**` 展开 —— 字段名静态读不出来，拒绝生成")
            if kw.arg in kw_by_name:
                sys.exit(f"hwconfig[{hex(hw)}] 的 kwarg {kw.arg} 重复出现 —— 拒绝生成"
                         "（后者覆盖先者，静默取哪个都可能在写错表）")
            kw_by_name[kw.arg] = kw

        damode, dacode, iot = None, None, False
        if "damode" in kw_by_name:
            # 只认 DAmodes.<ATTR> 这一种形态：其它写法（字符串/表达式）取不到 → 由下面的
            # 硬失败拦住，而不是静默落到 LEGACY。
            kw = kw_by_name["damode"]
            if isinstance(kw.value, ast.Attribute):
                damode = DAMODE.get(kw.value.attr)
        if "dacode" in kw_by_name:
            kw = kw_by_name["dacode"]
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
        if "iot" in kw_by_name:
            kw = kw_by_name["iot"]
            # 只认 bool 字面量：`bool("False")` 是 True —— 字符串形态会静默把非 IoT 芯片标成 IoT
            if not (isinstance(kw.value, ast.Constant)
                    and isinstance(kw.value.value, bool)):
                sys.exit(f"0x{hw:04X} 的 iot 不是 bool 字面量（{ast.unparse(kw.value)}）—— 拒绝生成")
            iot = kw.value.value                # 上游 hwconfig 的 iot=True（如 0x6226）

        # —— 逐芯片硬件地址（W8-T2）：**忠实转写"有没有"**，缺的字段不许填 0 冒充 ——
        # 上游 Chipconfig.__init__ 的形参默认值全是 None ⇒ "kwarg 缺失"与"显式写 None"是同一件事，
        # 两者都算**该字段缺失**（把 None 当 0 写进表，往 watchdog 写 0 就是砖机）。
        hw_values = {}
        for upstream_name, _cpp_name, _has_name in HW_FIELDS:
            kw = kw_by_name.get(upstream_name)
            if kw is None or (isinstance(kw.value, ast.Constant) and kw.value.value is None):
                hw_values[upstream_name] = (False, 0)
                continue
            v = kw.value
            if not (isinstance(v, ast.Constant) and isinstance(v.value, int)
                    and not isinstance(v.value, bool)):
                sys.exit(f"0x{hw:04X} 的 {upstream_name} 不是整数字面量（{ast.unparse(v)}）"
                         "—— 读不懂就拒绝生成（不回填、不猜）")
            val = v.value
            # 0 与"缺失"在产物里无法区分（值槽本来就写 0），且往 watchdog 写 0 是砖机；
            # 负数不是地址。上游 71b0175 的这 7 个字段无 0/负值 —— 真出现了先人工复核再放宽。
            if not 0 < val <= 0xFFFFFFFF:
                sys.exit(f"0x{hw:04X} 的 {upstream_name} = {val} 不在 (0, 0xFFFFFFFF] —— 拒绝生成"
                         "（0 与'缺失'无法区分；负数不是地址）")
            hw_values[upstream_name] = (True, val)
        # 对称不变量（emit 与用例都依赖它）：值槽为 0 **当且仅当** 存在位为假。
        # 上面已保证 present ⇒ 值 > 0、absent ⇒ 值 = 0，这里再核一次，防以后有人改上面时漏掉。
        # ⚠️ 用 sys.exit 而不是 assert（W8-T2 审查 Minor-3 / 控制方裁决）：assert 在 `python3 -O`
        # 下会被整个剥掉（实测退出码 0、什么都不打印）—— "静默"正是 fail-closed 最不能有的性质；
        # 且裸 AssertionError 不指名芯片与字段，与其余 25 条拒绝路径的姿态不一致。
        for upstream_name, _cpp_name, _has_name in HW_FIELDS:
            present, val = hw_values[upstream_name]
            if (val == 0) != (not present):
                sys.exit(f"0x{hw:04X} 的 {upstream_name} 破坏了对称不变量：present={present}、值={val}"
                         "（值槽为 0 必须当且仅当存在位为假）—— 拒绝生成")
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
        entries.append((hw, dacode, damode, iot, hw_values))

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
    # 按 hw_code 排（key= 而不是裸元组比较：元组里现在有 dict，万一哪天 hw_code 重复，
    # 裸比较会在 dict 上抛 TypeError，把"重复键"的清晰报错变成难懂的崩溃）
    entries.sort(key=lambda e: e[0])
    return entries, dacode_fallbacks


def require_output_dir(out_path) -> None:
    """输出路径的父目录必须存在 —— 在解析上游**之前**就核一次，免得白跑一遍再拒绝。

    同一个检查在 write_artifact() 里再走一遍（那才是真正落盘的关口）；两处都具名 sys.exit，
    不靠"反正前面查过了"。
    """
    if out_path is not None and not out_path.parent.is_dir():
        sys.exit(f"输出目录不存在：{out_path.parent} —— 拒绝生成（未创建/未改动任何文件）")


def write_artifact(text: str, out_path):
    """产物落地：给了路径就**原子写**（同目录临时文件 + os.replace），否则写 stdout。

    为什么不让调用方用 `> 文件` 重定向：shell 在 exec **之前**就把目标截断了，而本脚本有
    30 处具名拒绝路径 —— 一旦拒绝，入库文件就变成 0 字节（bash 实测 17 → 0）。
    "拒绝生成"和"工作树被破坏"必须分开；原子写让**任何**调用式都不会毁数据。

    out_path is None → 旧行为（stdout，`print` 补尾换行）。
    """
    if out_path is None:
        print(text)
        return
    require_output_dir(out_path)       # 落盘前的最后一道（main 里已早查过一次）
    # 临时文件必须与目标**同目录**：os.replace 只在同一文件系统内才是原子的。
    fd, tmp_name = tempfile.mkstemp(dir=str(out_path.parent),
                                    prefix=out_path.name + ".", suffix=".tmp")
    tmp = Path(tmp_name)
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as f:
            f.write(text + "\n")
        os.replace(tmp, out_path)      # 原子换入：此刻之前目标一直是旧内容
    except BaseException:
        tmp.unlink(missing_ok=True)    # 失败不留残骸，目标保持原样
        raise


def main():
    # 只接受 0 或 1 个参数（输出路径；`-` 同"不给"）。用法见模块 docstring。
    if len(sys.argv) > 2:
        sys.exit(f"参数过多（收到 {len(sys.argv) - 1} 个）—— 用法: "
                 "gen_mtk_chip_table.py [输出路径|-]，拒绝生成")
    out_arg = sys.argv[1] if len(sys.argv) == 2 else None
    out_path = None if out_arg in (None, "-") else Path(out_arg)
    require_output_dir(out_path)       # 早拒绝：白跑一遍再报"目录不存在"没有意义
    if not SRC.exists():
        sys.exit(f"缺少 {SRC}（mtkclient 子模块未初始化？git submodule update --init）")
    entries, dacode_fallbacks = extract()
    # 生成期核对 struct ChipHwAddrs 的成员顺序（不一致 → sys.exit）：
    # 表体是按位置初始化的聚合，成员顺序错位 = 89 行整表错位且编译器不吭声。
    header_struct_members()
    if len(entries) < MIN_ENTRIES:
        sys.exit(f"只解析到 {len(entries)} 个条目（上游 89）—— 疑似表被换成 stub，拒绝生成")
    commit = source_commit()
    iot_count = sum(1 for e in entries if e[3])
    # —— 覆盖统计（W8-T2）：既写进产物头注释，也走 stderr 供复跑核对 ——
    # 期望值对着上游 71b0175 独立量过（ast 逐 kwarg 判缺失/None）：89/71/66/68/74/66/38/68。
    counts = {name: sum(1 for e in entries if e[4][name][0]) for name, _, _ in HW_FIELDS}
    def _usable(e):
        f = e[4]
        return (f["watchdog"][0] and f["uart"][0]
                and any(f[p][0] for p in ("brom_payload_addr", "da_payload_addr", "pl_payload_addr")))
    n_usable = sum(1 for e in entries if _usable(e))
    n_all7 = sum(1 for e in entries if all(e[4][n][0] for n, _, _ in HW_FIELDS))
    n_all_missing = sum(1 for e in entries if not any(e[4][n][0] for n, _, _ in HW_FIELDS))
    all_missing = [hex(e[0]) for e in entries if not any(e[4][n][0] for n, _, _ in HW_FIELDS)]
    # 诊断一律走 stderr：stdout 是产物本身。
    print(f"转写 {len(entries)} 条（iot=True {iot_count} 条；dacode 回填 {len(dacode_fallbacks)} 条"
          f"{'：' + ' '.join(hex(h) for h in dacode_fallbacks) if dacode_fallbacks else ''}）",
          file=sys.stderr)
    print("硬件地址覆盖（显式给出/总条数）："
          + "  ".join(f"{n}={counts[n]}" for n, _, _ in HW_FIELDS)
          + f"；七字段全齐={n_all7}；kamakiri 三要素(watchdog+uart+任一 payload)齐全={n_usable}；"
          + f"三要素缺={len(entries) - n_usable}（其中七字段全缺={n_all_missing}：{' '.join(all_missing)}）",
          file=sys.stderr)
    out = []
    out.append("// src/core/modes/mtk_chip_table.cpp")
    out.append("//")
    out.append("// ⚠️ **本文件由脚本生成，请勿手改** —— 改表请改生成脚本后重跑：")
    out.append("//     python3 tools/gen_mtk_chip_table.py src/core/modes/mtk_chip_table.cpp")
    out.append("// 生成器写同目录临时文件、成功后原子换入 ⇒ 它**任何**拒绝生成都不会碰本文件。")
    out.append("// ⚠️ **不要**用 `python3 … > src/core/modes/mtk_chip_table.cpp`：bash 下重定向先截断再 exec，")
    out.append("//     生成器一旦拒绝，本文件会被留成 0 字节（实测 17 → 0）—— 该调用式自 W8-T2 起弃用。")
    out.append("//")
    out.append("// 数据来源：bkerler/mtkclient（GPL-3.0）的 mtkclient/config/brom_config.py 的 `hwconfig` 表")
    out.append(f"//     commit: {commit}")
    out.append("//     上游 URL: https://github.com/bkerler/mtkclient")
    out.append(f"//     上游原文: https://github.com/bkerler/mtkclient/blob/{commit}/mtkclient/config/brom_config.py")
    out.append("// 本项目为 GPLv3，与本表许可兼容；转写只取静态事实（hw_code / dacode / damode / iot +")
    out.append("// 7 个硬件地址字段），不复制上游代码。")
    out.append(f"// 本表 {len(entries)} 条 = 上游 hwconfig 的全部条目（其中 iot=True {iot_count} 条、"
               f"dacode 按 hw_code 回填 {len(dacode_fallbacks)} 条）；damode 取值同上游 DAmodes。")
    out.append("//")
    out.append("// 本文件含**两张同源表**（同一次生成、同一个 hwconfig）：kChips（代际判定）与")
    out.append("// kHwAddrs（逐芯片硬件地址，W8-T2）。两张表的 hw_code 集合必须完全相同 ——")
    out.append("// 否则 DA 上传路径与漏洞利用路径会对同一颗芯片给出互相矛盾的结论。")
    out.append('#include "mtk_chip_table.h"')
    out.append("")
    out.append("namespace mtkbrom {")
    out.append("namespace {")
    out.append("const ChipInfo kChips[] = {")
    for hw, dacode, damode, iot, _hwf in entries:
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
    out.append("// —— 逐芯片硬件地址（W8-T2）：同一张上游表的另 7 个字段 ——")
    out.append("// 列序同 mtk_chip_table.h 的 ChipHwAddrs（生成期已核对该 struct 的成员顺序，不一致即拒绝生成）：")
    out.append("//   hwCode, " + ", ".join(cpp for _, cpp, _ in HW_FIELDS) + ",")
    out.append("//   " + ", ".join(has for _, _, has in HW_FIELDS))
    out.append("// **缺的字段写「值槽 0 + 存在位 false」** —— 值槽里的 0 不是「地址 0」，是「上游没给」；")
    out.append("// 反过来 present ⇒ 值必非 0（生成器拒绝 0 值）。谁要往硬件写地址，先查存在位。")
    out.append("// 覆盖（生成器实测，改上游后以 tests/test_mtk_chip_hwaddrs.cpp 的复核为准）：")
    out.append("//   " + "  ".join(f"{n} {counts[n]}/{len(entries)}" for n, _, _ in HW_FIELDS))
    out.append(f"//   七字段全齐 {n_all7}；kamakiri 三要素（watchdog+uart+任一 payload）齐全 {n_usable}"
               f"（其中 {n_usable - n_all7} 条字段不全但可用）；")
    out.append(f"//   三要素缺 {len(entries) - n_usable}（其中七字段全缺 {n_all_missing} 条：{' '.join(all_missing)}）"
               " ⇒ 漏洞利用对它们**不可用**。")
    out.append("const ChipHwAddrs kHwAddrs[] = {")
    for hw, _dacode, _damode, _iot, hwf in entries:
        values = ", ".join(f"0x{hwf[n][1]:08X}" for n, _, _ in HW_FIELDS)
        hasbits = ", ".join("true" if hwf[n][0] else "false" for n, _, _ in HW_FIELDS)
        out.append(f"    {{0x{hw:04X}, {values}, {hasbits}}},")
    out.append("};")
    out.append("constexpr int kHwAddrsCount = int(sizeof(kHwAddrs) / sizeof(kHwAddrs[0]));")
    out.append("")
    out.append("const ChipHwAddrs *lookupChipHwAddrs(quint16 hwCode)")
    out.append("{")
    out.append("    // 线性查找，理由同 lookupChip（表小、调用点极少）。只按 hw_code 命中：")
    out.append("    // 字段不全的芯片**照样返回非空**（缺哪个由 hasXxx 表达），只有未收录才是 nullptr。")
    out.append("    for (int i = 0; i < kHwAddrsCount; ++i)")
    out.append("        if (kHwAddrs[i].hwCode == hwCode)")
    out.append("            return &kHwAddrs[i];")
    out.append("    return nullptr;")
    out.append("}")
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
    write_artifact("\n".join(out), out_path)


if __name__ == "__main__":
    main()
