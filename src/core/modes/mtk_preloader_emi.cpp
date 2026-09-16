// src/core/modes/mtk_preloader_emi.cpp
//
// 对照上游 mtkclient/mtkclient/Library/DA/daconfig.py:120-145 的 m_extract_emi（GPL-3.0，只读引用）。
// 实测依据（.superpowers/sdd/mtk-d2d3-facts-report.md §2，统计只作注释证据，不进断言）：
//   832 个真实 preloader 中只有 3 个含 MMM 魔术、829 个的标记在偏移 0；
//   同一 preloader 两代取不同切片。reference/mtk-samples/preloader.bin 实测：
//   MMM@0、标记@254392、MTK_BIN@254492、mlen=0x3EBB8、siglen=0x66C、dramsize=912
//   → LEGACY 切片 800B、XFlash 切片 912B（整块）。
// 832 样本对拍补充（D2 Task 3，/tmp 一次性脚本，统计只作注释证据）：**832/832** 的标记都在
//   **窗口**偏移 0、MTK_BIN 都在窗口偏移 100（⇒ XFlash 切片恒 = LEGACY 切片 + 112B）；含 MMM
//   的 3 个样本裁剪后同样落在窗口偏移 0 —— 两代切片差异与 MMM 分支无关，是取法不同（112B = 头部）。
//
// 与上游 Python 的**有意偏差**（本实现一律 fail-closed 并给出诊断文本；上游靠 extract_emi 的
// try/except 把异常吞成 emi=None、或靠 Python 切片静默 clamp —— 结果同为"不该用这份 EMI"）：
//   1. mlen < siglen：上游 data[:负数] 静默留前段 → 本实现报错（头部自相矛盾）。
//   2. dramsize+4 > 可用数据：上游静默 clamp 成 data[:-4] → 本实现报错。
//   3. dramsize==0 且裁剪后 ≤0x800：上游 data[-4:] 抛异常 → 本实现报错（同结果，另有诊断）。
//   4. MTK_BIN+0xC 越过末尾：上游返回**空** EMI 但算成功 → 本实现报错，
//      不把"空 EMI"当提取成功交给发送端。
//   5. 第 4 条的判据**借 Qt 的 clamp 语义**表达（`mid()` 越界不报错、clamp 成空 ⇔ 起点越界；
//      与 Python 切片同语义），故不另写范围比较 —— 行内注见 extractEmiLegacy（标注"偏差 5"）。
//   6. XFlash 整块分支要求标记在窗口偏移 0（上游 DC:137 的 `idx == 0`）；标记不在起点时上游
//      静默改取 **MTK_BIN+0xC** 切片（DC:141-144，即另一代的切片）→ 本实现报错并指名替代调用，
//      不发出上游在此情形不会发的字节（见 extractEmiXflash）。832 样本全部满足该条件。
//   另（判据差异）：版本字节**只去尾部 NUL**（上游 `rstrip(b"\x00")` 语义；内嵌 NUL 上游
//   `int(b"\x005")` 会抛异常 → 不得当数字收），去尾后须为 1-2 位 ASCII 数字。上游 int() 还会
//   trim 两侧空白（" 5" 上游接受、本实现拒绝）—— 真实 preloader 该字段恒为 "35"/"38" 这类 2 位。
// **版本非数字不属偏差**：上游 int() 抛异常 → extract_emi 捕获后 emi=None（**DC:162-164**；
// 注意 157-160 是 `self.error(...)/exit(1)` 的**硬中止**分支，引错会与"失败不中止刷写"相反）→
// 上层跳过、不发 DRAM 配置；本实现显式 false + error，净效果一致（不得混进合法的 ver==0）。
#include "mtk_preloader_emi.h"

#include <utility>   // std::as_const（遍历 Qt 容器，不得用 qAsConst）

namespace mtkbrom {
namespace {

// 长度一律由 sizeof 推出，**不要**手写数字（mtk_da_file.cpp 有同类坑：字面量长度写错会读越界）。
constexpr char kMmm[] = "\x4D\x4D\x4D\x01\x38\x00\x00\x00";   // DC:121 的魔术（含内嵌 NUL，8 字节）
constexpr int kMmmLen = int(sizeof(kMmm)) - 1;
constexpr char kBloaderInfo[] = "MTK_BLOADER_INFO_v";          // DC:132（18 字节）
constexpr int kBloaderInfoLen = int(sizeof(kBloaderInfo)) - 1;
constexpr char kMtkBin[] = "MTK_BIN";                          // DC:141（7 字节）
constexpr int kMtkBinLen = int(sizeof(kMtkBin)) - 1;
constexpr int kMmmMlenOff = 0x20;        // DC:124 mlen（<u32，小端）
constexpr int kMmmSiglenOff = 0x2C;      // DC:125 siglen（<u32，小端）
constexpr int kMtkBinSkip = 0xC;         // DC:142 LEGACY 切片起点 = MTK_BIN+0xC
constexpr int kDramRetry = 0x800;        // DC:129 dramsize==0 时的重读步长

quint32 rdLe32(const QByteArray &d, int off)
{
    return quint32(quint8(d.at(off))) | (quint32(quint8(d.at(off + 1))) << 8)
         | (quint32(quint8(d.at(off + 2))) << 16) | (quint32(quint8(d.at(off + 3))) << 24);
}

void setErr(QString *error, const QString &msg) { if (error) *error = msg; }

} // namespace

// 公共前缀（两代共用）—— D1 的 extractEmiLegacy 前段**原样提取**：判据、分支顺序、失败文案
// **逐字未改**（D1 的既有用例就是这次重构的回归网）。上游把这段写在一个函数里、最后按
// `damode`/`idx` 分叉；本实现把它拆成"公共前缀 + 两个各自取切片的公开函数"，让**调用方**按代选择。
// 成功返回时：out 已重置并填好 branch（D1 语义）；window = 裁到 dramsize 的窗口（未命中 MMM
// 时即整块输入）；version = 已校验的 1-2 位 ASCII 数字（落在各自的切片函数里 toUInt()）。
bool emiCommon(const QByteArray &preloader, EmiData &out, QByteArray &window, QString &version,
               QString *error)
{
    out = EmiData{};                       // fail-closed：失败路径不留上一次的半份结果
    if (preloader.isEmpty()) {
        setErr(error, QStringLiteral("preloader 为空，无法提取 EMI"));
        return false;
    }
    QByteArray data = preloader;
    const int mmm = data.indexOf(QByteArray(kMmm, kMmmLen));
    if (mmm != -1) {
        // DC:121-123：命中 MMM 魔术才做裁剪（实测仅 3/832 个真实 preloader 命中此分支）
        data = data.mid(mmm);
        if (data.size() < kMmmSiglenOff + 4) {          // 最大读取区间是 [0x2C, 0x30)
            setErr(error, QStringLiteral("MMM 分支数据太短（%1 字节，至少需要 %2）")
                              .arg(data.size()).arg(kMmmSiglenOff + 4));
            return false;
        }
        const quint32 mlen = rdLe32(data, kMmmMlenOff);        // DC:124（小端）
        const quint32 siglen = rdLe32(data, kMmmSiglenOff);    // DC:125（小端）
        if (mlen < siglen) {                                   // 偏差 1
            setErr(error, QStringLiteral("MMM 分支 mlen(%1) < siglen(%2)，头部自相矛盾")
                              .arg(mlen).arg(siglen));
            return false;
        }
        // DC:126 data = data[:mlen-siglen]（超过全长时 clamp 到全长 —— 与 Python 切片同语义）
        const quint64 keep = quint64(mlen) - quint64(siglen);
        if (keep < quint64(data.size()))
            data = data.left(qsizetype(keep));
        if (data.size() < 4) {
            setErr(error, QStringLiteral("MMM 分支按 mlen-siglen 裁剪后不足 4 字节（%1）")
                              .arg(data.size()));
            return false;
        }
        quint32 dramsize = rdLe32(data, int(data.size()) - 4);  // DC:127 data[-4:]
        if (dramsize == 0) {
            // DC:128-130：末 4B 为 0 → 砍掉 0x800 再读一次（偏差 3：不足 0x800 时上游必抛异常）
            if (data.size() <= kDramRetry) {
                setErr(error, QStringLiteral("MMM 分支 dramsize==0 且裁剪后仅 %1 字节（≤%2），"
                                             "无法按上游语义重读").arg(data.size()).arg(kDramRetry));
                return false;
            }
            data = data.left(data.size() - kDramRetry);
            dramsize = rdLe32(data, int(data.size()) - 4);
        }
        // DC:131 data = data[-dramsize-4:-4]：取回内层 EMI 块（丢掉尾部 4B dramsize）
        const quint64 take = quint64(dramsize) + 4;
        if (take > quint64(data.size())) {                      // 偏差 2
            setErr(error, QStringLiteral("MMM 分支 dramsize(%1) 超出可用数据（%2 字节）")
                              .arg(dramsize).arg(data.size()));
            return false;
        }
        data = data.mid(qsizetype(quint64(data.size()) - take), qsizetype(dramsize));
        out.branch = QStringLiteral("MMM");
    } else {
        // 分支名只描述"**未命中 MMM 魔术**"，不是"标记在偏移 0"（标记不在偏移 0 时同样成立）
        out.branch = QStringLiteral("未命中MMM");
    }

    // DC:132-135：搜 MTK_BLOADER_INFO_v，找不到即失败
    const int info = data.indexOf(QByteArray(kBloaderInfo, kBloaderInfoLen));
    if (info == -1) {
        setErr(error, QStringLiteral("未找到 MTK_BLOADER_INFO_v（不是可提取 EMI 的 preloader）"));
        return false;
    }
    // DC:138/143：版本 = 标记后 **2 个 ASCII 字节**（去尾部 NUL），如 "38" → 38；
    // 真样本 preloader.bin 的字节是 "35" → 35（**整体**两位，不是末位 5）。
    // **只去尾部 NUL**（上游 `rstrip(b"\x00")` 语义：内嵌 NUL 留着 —— 上游 `int(b"\x005")`
    // 抛异常，不得当 5 收），去尾后须为 1-2 位 ASCII 数字，否则**整体提取失败**：
    // 上游 int() 抛异常时是 `except Exception: self.emiver = 0; self.emi = None`（**DC:162-164**）
    // → 上层 `if self.daconfig.emi is not None:` 整段跳过、**根本不发 DRAM 配置**；
    // 而 ver==0 在上游是**合法档位**（tier-0）—— 把"读不懂"混进"合法的 0"会让伪造版本驱动协议分档。
    // "0" / "00" 仍接受（上游 int("00") == 0）。
    const QByteArray verBytes = data.mid(info + kBloaderInfoLen, 2);
    QByteArray digits = verBytes;
    while (digits.endsWith('\0'))
        digits.chop(1);                                   // rstrip 语义：只去尾部（内嵌 NUL 视作非数字）
    bool verOk = digits.size() == 1 || digits.size() == 2;   // 去尾后须 1-2 位
    for (const char c : std::as_const(digits))
        if (c < '0' || c > '9')
            verOk = false;                                // 按字节判 ASCII 数字（不经 QString/Unicode 语义）
    if (!verOk) {
        setErr(error, QStringLiteral("EMI 版本字节不是数字（读到 \"%1\"）—— 与上游一致按**提取失败**"
                                     "处理（上游此时 emi = None，不发 DRAM 配置）")
                          .arg(QString::fromLatin1(verBytes)));
        return false;
    }
    // digits 已校验为 1-2 位 ASCII 数字，"QString::toUInt()" 与 D1 原本的 "QByteArray::toUInt()"
    // 在该取值域等价（两者都只接受数字/前导空白/正负号，取值域内无差别）—— 版本号留在调用方落盘，
    // 使两代各自的赋值点都停在各自的切片函数里。
    version = QString::fromLatin1(digits);
    window = data;
    return true;
}

bool extractEmiLegacy(const QByteArray &preloader, EmiData &out, QString *error)
{
    QByteArray window;
    QString version;
    if (!emiCommon(preloader, out, window, version, error))
        return false;
    out.ver = version.toUInt();                                 // 与 D1 同序：仍在 MTK_BIN 搜索**之前**

    // 本函数恒走 LEGACY（DC:140-144）：EMI = data[find("MTK_BIN")+0xC:]
    const int bin = window.indexOf(QByteArray(kMtkBin, kMtkBinLen));
    if (bin == -1) {                                            // DC:141
        setErr(error, QStringLiteral("未找到 MTK_BIN（LEGACY 切片起点未知）"));
        return false;
    }
    out.bytes = window.mid(bin + kMtkBinSkip);                  // DC:142
    // mid() 会 clamp：起点越界 ⇔ 切片为空（偏差 5）
    if (out.bytes.isEmpty()) {
        setErr(error, QStringLiteral("MTK_BIN+0xC(%1) 越过数据末尾（%2 字节），EMI 切片为空")
                          .arg(bin + kMtkBinSkip).arg(window.size()));
        return false;
    }
    return true;
}

bool extractEmiXflash(const QByteArray &preloader, EmiData &out, QString *error)
{
    QByteArray window;
    QString version;
    if (!emiCommon(preloader, out, window, version, error))
        return false;

    // 上游 DC:137 把"整块返回"系于 `idx == 0`（标记正在窗口起点）。标记不在起点时上游落 else
    // 分支、静默改取 `MTK_BIN+0xC` 切片（DC:141-144）—— 那是**另一代的切片**。本实现不静默换
    // 切片（否则发出的字节上游在此情形不会发），改明确失败并指名替代调用（偏差 6）。
    // emiCommon 成功 ⇒ 窗口内必有标记 ⇒ 此处 info != -1，只判"是否在偏移 0"。
    const int info = window.indexOf(QByteArray(kBloaderInfo, kBloaderInfoLen));
    if (info != 0) {
        setErr(error, QStringLiteral("XFlash 切片要求标记在 dramsize 窗口偏移 0（实测 %1）："
                                     "上游此情形改取 MTK_BIN+0xC 切片，本实现不静默换切片 —— "
                                     "需要该切片请改调 extractEmiLegacy").arg(info));
        return false;
    }

    out.ver = version.toUInt();
    // **整块窗口**（真样本 912B）—— 不找 MTK_BIN、不做任何切片。此处不另判空：窗口内已含标记
    // （≥18B）恒非空，空切片分支在 XFlash 路径不可达（与 LEGACY 的 MTK_BIN+0xC 越界不同）。
    out.bytes = window;
    out.branch = QStringLiteral("XFLASH");
    return true;
}

} // namespace mtkbrom
