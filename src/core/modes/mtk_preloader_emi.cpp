// src/core/modes/mtk_preloader_emi.cpp
//
// 对照上游 mtkclient/mtkclient/Library/DA/daconfig.py:120-145 的 m_extract_emi（GPL-3.0，只读引用）。
// 实测依据（.superpowers/sdd/mtk-d2d3-facts-report.md §2，统计只作注释证据，不进断言）：
//   832 个真实 preloader 中只有 3 个含 MMM 魔术、829 个的标记在偏移 0；
//   同一 preloader 两代取不同切片。reference/mtk-samples/preloader.bin 实测：
//   MMM@0、标记@254392、MTK_BIN@254492、mlen=0x3EBB8、siglen=0x66C、dramsize=912
//   → LEGACY 切片 800B（XFlash 的整块取法是 912B）。
//
// 与上游 Python 的**有意偏差**（本实现一律 fail-closed 并给出诊断文本；上游靠 extract_emi 的
// try/except 把异常吞成 emi=None、或靠 Python 切片静默 clamp —— 结果同为"不该用这份 EMI"）：
//   1. mlen < siglen：上游 data[:负数] 静默留前段 → 本实现报错（头部自相矛盾）。
//   2. dramsize+4 > 可用数据：上游静默 clamp 成 data[:-4] → 本实现报错。
//   3. dramsize==0 且裁剪后 ≤0x800：上游 data[-4:] 抛异常 → 本实现报错（同结果，另有诊断）。
//   4. 版本字节非数字：上游 int() 抛异常 → **整个提取失败**；本实现置 ver=0 并继续
//      （切片判据与版本号无关），由发送端（D2）决定 ver==0 是否可发。
//   5. MTK_BIN+0xC 越过末尾：上游返回**空** EMI 但算成功 → 本实现报错，
//      不把"空 EMI"当提取成功交给发送端。
#include "mtk_preloader_emi.h"

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

bool extractEmiLegacy(const QByteArray &preloader, EmiData &out, QString *error)
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
        out.branch = QStringLiteral("偏移0");
    }

    // DC:132-135：搜 MTK_BLOADER_INFO_v，找不到即失败
    const int info = data.indexOf(QByteArray(kBloaderInfo, kBloaderInfoLen));
    if (info == -1) {
        setErr(error, QStringLiteral("未找到 MTK_BLOADER_INFO_v（不是可提取 EMI 的 preloader）"));
        return false;
    }
    // DC:138/143：版本 = 标记后 **2 个 ASCII 字节**（去尾部 NUL），如 "38" → 38；
    // 真样本 preloader.bin 的字节是 "35" → 35（偏差 4：非数字时上游整体失败，本实现置 0 继续）
    const QByteArray verBytes = data.mid(info + kBloaderInfoLen, 2);
    bool verOk = false;
    const int ver = QString::fromLatin1(verBytes).remove(QChar('\0')).trimmed().toInt(&verOk);
    out.ver = verOk ? quint32(ver) : 0;

    // DC:137-139 的 `idx == 0 且 damode == XFLASH` 整块分支属 XFlash —— D1 不实现，
    // 本函数恒走 LEGACY：EMI = data[find("MTK_BIN")+0xC:]
    const int bin = data.indexOf(QByteArray(kMtkBin, kMtkBinLen));
    if (bin == -1) {                                            // DC:141
        setErr(error, QStringLiteral("未找到 MTK_BIN（LEGACY 切片起点未知）"));
        return false;
    }
    out.bytes = data.mid(bin + kMtkBinSkip);                    // DC:142
    // mid() 会 clamp：起点越界 ⇔ 切片为空（偏差 5）
    if (out.bytes.isEmpty()) {
        setErr(error, QStringLiteral("MTK_BIN+0xC(%1) 越过数据末尾（%2 字节），EMI 切片为空")
                          .arg(bin + kMtkBinSkip).arg(data.size()));
        return false;
    }
    return true;
}

} // namespace mtkbrom
