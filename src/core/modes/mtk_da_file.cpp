// src/core/modes/mtk_da_file.cpp
//
// AllInOne DA 解析（纯函数）。布局与判据全部来自实测：
//   6 个真实文件 / 161 条目 = reference/mtk-samples/（gitignored），
//   独立解析器基准 = reference/mtk-samples/parse_da.py，
//   逐字段实测表 = reference/mtk-samples/da_parse_report.json。
// 出处（mtkclient，GPL-3.0，只读引用不复制代码）：Library/DA/daconfig.py:120-205（解析/选择）、
//   Library/DA/legacy/dalegacy_lib.py:563-571（region[1]/[2] 硬编码）。
#include "mtk_da_file.h"

namespace mtkbrom {
namespace {

constexpr int kHeaderSize = 0x68;      // 头部 0x68 字节（前 16 字节 "MTK_DOWNLOAD_AGENT"）
constexpr int kEntryStepDc = 0xDC;     // 现代表目步长
constexpr int kEntryStepD8 = 0xD8;     // 老格式步长（**无真实样本**，仅合成夹具覆盖）
constexpr quint16 kEntryMagic = 0xDADA;
constexpr int kMaxRegions = 64;        // 防 count 损坏导致的天量分配

quint16 rdLe16(const QByteArray &d, int off)
{
    return quint16(quint8(d.at(off)) | (quint16(quint8(d.at(off + 1))) << 8));
}

quint32 rdLe32(const QByteArray &d, int off)
{
    return quint32(quint8(d.at(off))) | (quint32(quint8(d.at(off + 1))) << 8)
         | (quint32(quint8(d.at(off + 2))) << 16) | (quint32(quint8(d.at(off + 3))) << 24);
}

void setErr(QString *error, const QString &msg) { if (error) *error = msg; }

} // namespace

bool parseDaFile(const QByteArray &data, DaFile &out, QString *error)
{
    out = DaFile{};
    if (data.size() < kHeaderSize + 4) {
        setErr(error, QStringLiteral("DA 文件太短（%1 字节，头部需要 %2）").arg(data.size()).arg(kHeaderSize + 4));
        return false;
    }
    // P10 头部标记：前 16 字节必须是 "MTK_DOWNLOAD_AGENT"（6/6 真实文件）
    if (data.left(16) != QByteArray("MTK_DOWNLOAD_AGENT", 16)) {
        setErr(error, QStringLiteral("不是 AllInOne DA 文件（头部标记不是 MTK_DOWNLOAD_AGENT）"));
        return false;
    }
    // P10 世代判别式**负向**：hdr[:0x68] 内含 "MTK_DA_v6" 才是 V6；横幅版本号不得用于逻辑分支
    out.isV6 = data.left(kHeaderSize).contains(QByteArray("MTK_DA_v6"));   // 9 字节 needle（P10）——
    // ⚠️ 不要写 `QByteArray("MTK_DA_v6", 11)`：字面量只有 9 字符，指定 11 会读越界（ASan global-buffer-overflow）
    //    且功能上是错的（真 V6 头 "MTK_DA_v6_2021-11-03…" 会判不出）。
    out.banner = data.mid(0x20, kHeaderSize - 0x20);
    while (out.banner.endsWith('\0'))
        out.banner.chop(1);
    out.raw = data;      // selectDaEntry 据 raw 切 region 载荷（避免后再读一次文件）

    // P9 count_da 不可信：先按最小步长与 EOF 交叉校验
    const quint32 count = rdLe32(data, 0x68);
    if (count == 0) {                       // 0 条目的 DA 文件没有任何可用条目 —— 明确拒绝
        setErr(error, QStringLiteral("DA 文件声明 0 个条目（count_da == 0）—— 无可用 DA 条目，拒绝解析")
                          + QStringLiteral("（独立基准 parse_da.py 同样把 count_da==0 记为 error）"));
        return false;
    }
    const quint64 minNeed = quint64(0x6C) + quint64(count) * quint64(kEntryStepD8);
    if (quint64(data.size()) < minNeed) {
        setErr(error, QStringLiteral("DA 截断：声明 %1 条目（至少需要 %2 字节），实际 %3 字节")
                          .arg(count).arg(minNeed).arg(data.size()));
        return false;
    }

    // P4 0xD8/0xDC 探测 + 交叉校验：探测点落在 entry[0] 自己的槽内（padding），
    // 若某条目 nregions >= 10 该处会变成真实 region 数据 → 必须与尺寸校验交叉验证。
    // 实测撞车事实：探测点 0x6C+0xD8 == 0x14 + 9*20 + 16，恰好是 region[9].m_sig_len 的低 16 位；
    // 真样本 max regionCount = 6、m_sig_len ∈ {0, 0x80, 0x100, 0x114} → 当前不可达，
    // 万一撞上也是 **fail-closed**（判成老格式 → 明确拒绝，不产错数据）。
    // 注意：尺寸交叉校验对真实 DA **实际不判别**（尾部 payload 远大于 count*4），真正兜底的是
    // 探测本身 —— 这是 P4 规定的固有局限，不是实现缺陷。
    const bool oldFormat = (data.mid(0x6C + kEntryStepD8, 2) == QByteArray("\xDA\xDA", 2));
    const int step = oldFormat ? kEntryStepD8 : kEntryStepDc;
    out.oldFormat = oldFormat;
    const quint64 need = quint64(0x6C) + quint64(count) * quint64(step);
    if (quint64(data.size()) < need) {
        setErr(error, QStringLiteral("DA 条目表与文件长度不自洽（步长 0x%1：需要 %2 字节，实际 %3）——"
                                     "条目尺寸探测可疑，拒绝按探测结果解析")
                          .arg(step, 2, 16, QLatin1Char('0')).arg(need).arg(data.size()));
        return false;
    }
    out.count = count;
    // P8：0xD8 老格式的**字段偏移也不同**（upstream daconfig 在 0xD8 下 pagesize 在 0x08、
    // region 表少 4 字节），而本实现只有新格式偏移。按新偏移硬解 = **静默读出垃圾**
    // （本仓反复出现的"入口判据比实现宽"家族）→ 故：**探测结果照填，解析明确拒绝**。
    if (oldFormat) {
        setErr(error, QStringLiteral("DA 文件是老格式（0xD8，条目 %1 个）：本实现按 P8 明确拒绝解析"
                                     "（无真实样本，且老格式字段偏移不同，硬解会读出垃圾）")
                          .arg(count));
        return false;
    }
    out.entries.reserve(int(count));

    for (quint32 i = 0; i < count; ++i) {
        const int o = 0x6C + int(i) * step;
        DaEntry e;
        e.magic = rdLe16(data, o + 0x00);
        if (e.magic != kEntryMagic) {   // 条目边界的唯一锚点 → fail-closed
            setErr(error, QStringLiteral("第 %1 条目的 magic 不是 0xDADA（读到 0x%2）—— 条目表可能错位")
                              .arg(i).arg(e.magic, 4, 16, QLatin1Char('0')));
            return false;
        }
        e.hwCode           = rdLe16(data, o + 0x02);
        e.hwSubCode        = rdLe16(data, o + 0x04);
        e.hwVersion        = rdLe16(data, o + 0x06);
        e.swVersion        = rdLe16(data, o + 0x08);
        e.pagesize         = rdLe16(data, o + 0x0C);   // P1 不做常量校验（实测 {0,1,4096,12288}）
        e.entryRegionIndex = rdLe16(data, o + 0x10);
        e.regionCount      = rdLe16(data, o + 0x12);
        if (e.regionCount > kMaxRegions) {
            setErr(error, QStringLiteral("第 %1 条目的 region 数异常（%2）").arg(i).arg(e.regionCount));
            return false;
        }
        // 条目槽内放下 region 表才算合法（0x14 + N*20 <= step）
        if (0x14 + int(e.regionCount) * 20 > step) {
            setErr(error, QStringLiteral("第 %1 条目的 region 表超出条目槽（%2 个）").arg(i).arg(e.regionCount));
            return false;
        }
        for (int r = 0; r < e.regionCount; ++r) {
            const int ro = o + 0x14 + r * 20;
            DaRegion reg;
            reg.fileOffset  = rdLe32(data, ro + 0);
            reg.len         = rdLe32(data, ro + 4);
            reg.startAddr   = rdLe32(data, ro + 8);
            reg.startOffset = rdLe32(data, ro + 12);   // P3 原样读取，不得推导
            reg.sigLen      = rdLe32(data, ro + 16);
            // P11 m_buf + m_len 必须落在文件内（0 长度 region 合法：真实文件里存在）
            const quint64 end = quint64(reg.fileOffset) + quint64(reg.len);
            if (end > quint64(data.size())) {
                setErr(error, QStringLiteral("第 %1 条目 region[%2] 越界：m_buf=%3 + m_len=%4 > 文件 %5 字节")
                                  .arg(i).arg(r).arg(reg.fileOffset).arg(reg.len).arg(data.size()));
                return false;
            }
            e.regions.append(reg);
        }
        out.entries.append(e);
    }
    return true;
}

} // namespace mtkbrom
