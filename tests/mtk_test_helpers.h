// tests/mtk_test_helpers.h
#pragma once
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>
#include <QString>
#include <QStringList>

#include "core/modes/mtk_da_file.h"

// 合成 DA 夹具（共享：test_mtk_da_file / test_mtk_payload）——布局按 spec §2。
// **不得**复用被测模块的读写原语（夹具与解析器共用同一原语时，方向性错误会自洽通过）。
namespace mtktest {

inline void putLe16(QByteArray &d, int off, quint16 v)
{
    d[off] = char(v & 0xFF);
    d[off + 1] = char((v >> 8) & 0xFF);
}

inline void putLe32(QByteArray &d, int off, quint32 v)
{
    d[off]     = char(v & 0xFF);
    d[off + 1] = char((v >> 8) & 0xFF);
    d[off + 2] = char((v >> 16) & 0xFF);
    d[off + 3] = char((v >> 24) & 0xFF);
}

inline quint32 rdLe32(const QByteArray &d, int off)
{
    return quint32(quint8(d.at(off))) | (quint32(quint8(d.at(off + 1))) << 8)
         | (quint32(quint8(d.at(off + 2))) << 16) | (quint32(quint8(d.at(off + 3))) << 24);
}

struct RegionSpec {
    quint32 len = 16;          // 载荷字节数（builder 会在文件尾追加这么多字节）
    quint32 startAddr = 0x200000;
    quint32 startOffset = 0;
    quint32 sigLen = 0;
};

struct EntrySpec {
    quint16 hwCode = 0x6765;
    quint16 hwSubCode = 0x8A00;
    quint16 hwVersion = 0xCA00;
    quint16 swVersion = 0x0000;
    quint16 pagesize = 4096;
    quint16 entryRegionIndex = 0;
    QList<RegionSpec> regions;
};

// 造一个 AllInOne DA：头 0x68（前 16 字节 MTK_DOWNLOAD_AGENT，横幅自 0x20 起）
// + count(LE@0x68) + N 条目（0xDC；oldFormat=true 时 0xD8）+ 尾部载荷blob（每条目每 region 16B 递增填充）。
// m_buf 由 builder 按实际追加位置回填（要测越界时构造后用 putLe32 直接改字节）。
inline QByteArray buildDa(const QList<EntrySpec> &entries, bool v6 = false, bool oldFormat = false,
                          const QByteArray &banner = QByteArray("MTK_AllInOne_DA_v3.3001_viperbjk"))
{
    const int entrySize = oldFormat ? 0xD8 : 0xDC;
    QByteArray head(0x6C, '\0');
    head.replace(0, 16, QByteArray("MTK_DOWNLOAD_AGENT", 16));
    if (v6)
        head.replace(0x20, 11, QByteArray("MTK_DA_v6  "));   // 真实文件该标记在 0x20（hdr[:0x68] 内）
    head.replace(0x20 + (v6 ? 11 : 0), qMin<int>(banner.size(), 0x68 - 0x20 - (v6 ? 11 : 0)), banner);
    putLe32(head, 0x68, quint32(entries.size()));

    QByteArray body(entrySize * entries.size(), '\0');
    QByteArray payloads;
    quint32 nextPayload = quint32(0x6C + entrySize * entries.size());
    for (int i = 0; i < entries.size(); ++i) {
        const EntrySpec &e = entries.at(i);
        const int o = i * entrySize;
        putLe16(body, o + 0x00, 0xDADA);
        putLe16(body, o + 0x02, e.hwCode);
        putLe16(body, o + 0x04, e.hwSubCode);
        putLe16(body, o + 0x06, e.hwVersion);
        putLe16(body, o + 0x08, e.swVersion);
        putLe16(body, o + 0x0A, 0);
        putLe16(body, o + 0x0C, e.pagesize);
        putLe16(body, o + 0x0E, 0);
        putLe16(body, o + 0x10, e.entryRegionIndex);
        putLe16(body, o + 0x12, quint16(e.regions.size()));
        for (int r = 0; r < e.regions.size(); ++r) {
            const RegionSpec &rs = e.regions.at(r);
            const int ro = o + 0x14 + r * 20;
            putLe32(body, ro + 0, nextPayload);          // m_buf = 文件偏移
            putLe32(body, ro + 4, rs.len);
            putLe32(body, ro + 8, rs.startAddr);
            putLe32(body, ro + 12, rs.startOffset);
            putLe32(body, ro + 16, rs.sigLen);
            payloads.append(QByteArray(int(rs.len), char(1 + i * 3 + r)));
            nextPayload += rs.len;
        }
    }
    return head + body + payloads;
}

// 从内存里的合成 DA 造一个 DaSelection（test_mtk_payload 用；不落盘）
// 返回 false = 夹具 DA 本身不合法（用例应当 QVERIFY 它）
// da2Len 可放大以测 boot_to 的**分块**路径（> 0x1000 才会分块）
inline bool makeSelection(quint16 hwCode, mtkbrom::DaSelection &out, quint32 da2Len,
                          QString *error = nullptr)
{
    EntrySpec e;
    e.hwCode = hwCode;
    RegionSpec r0; r0.startAddr = 0x200000;   r0.len = 16;                    // region[0] = EMI/env（不用）
    RegionSpec r1; r1.startAddr = 0x2007000;  r1.len = 32;                    // region[1] = DA1
    RegionSpec r2; r2.startAddr = 0x80000000; r2.len = da2Len; r2.sigLen = 0x10; // region[2] = DA2（带签名）
    e.regions << r0 << r1 << r2;
    mtkbrom::DaFile f;
    if (!mtkbrom::parseDaFile(buildDa({e}), f, error))
        return false;
    QStringList warn;
    return mtkbrom::selectDaEntry(f, hwCode, 0, 0, &warn, out, error);
}

// 3 参重载：`makeSelection(hw, sel, &err)` 的简写（da2Len = 48 → 单块送完，不分块）。
// 与 4 参版并存是因为用例两种写法都有；两者语义完全一致。
inline bool makeSelection(quint16 hwCode, mtkbrom::DaSelection &out, QString *error)
{
    return makeSelection(hwCode, out, 48, error);
}

// ---- 真样本路径（reference/ 为 gitignored；验证跑带 -DMTK_SAMPLES_REQUIRED=ON 且核对 0 skipped）----
#ifndef MTK_SAMPLES_DIR
#define MTK_SAMPLES_DIR ""
#endif
#ifndef MTK_SAMPLES_REQUIRED
#define MTK_SAMPLES_REQUIRED 0
#endif

inline QString samplesDir() { return QString::fromLatin1(MTK_SAMPLES_DIR); }

// ---- 大端编码（**共享一份**：各用例不要各自再写一遍 lambda）----
inline QByteArray be16(quint16 v)
{
    QByteArray b(2, '\0');
    b[0] = char((v >> 8) & 0xFF);
    b[1] = char(v & 0xFF);
    return b;
}

inline QByteArray be32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char((v >> 24) & 0xFF); b[1] = char((v >> 16) & 0xFF);
    b[2] = char((v >> 8) & 0xFF);  b[3] = char(v & 0xFF);
    return b;
}

// 样本可用性：**按文件判**，不只看目录（目录在而某个样本缺失时，用例应 SKIP 而不是硬 FAIL ——
// 审查 Minor：只判目录会让"目录存在但 da_parse_report.json 缺失"变成误 FAIL）
inline bool sampleFileAvailable(const QString &fileName)
{
    const QString d = samplesDir();
    return !d.isEmpty() && QFile::exists(QDir(d).filePath(fileName));
}

// 目录级判可用性：**留给后续任务（T5/T7 的 brief 会用），勿删**。
// 新用例请优先用 sampleFileAvailable()（按文件判，见上）。
inline bool samplesAvailable()
{
    const QString d = samplesDir();
    return !d.isEmpty() && QDir(d).exists();
}

} // namespace mtktest
