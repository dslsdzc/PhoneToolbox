// src/core/modes/mtk_da_file.cpp
//
// AllInOne DA 解析 + 条目选择（纯函数）。布局与判据全部来自实测：
//   6 个真实文件 / 161 条目 = reference/mtk-samples/（gitignored），
//   独立解析器基准 = reference/mtk-samples/parse_da.py，
//   逐字段实测表 = reference/mtk-samples/da_parse_report.json。
// 出处（mtkclient，GPL-3.0，只读引用不复制代码）：
//   Library/DA/daconfig.py:120-205            解析（字段序 / count_da / 条目步长探测）
//   Library/DA/daconfig.py:200                装载阶段剔除 hw_code==0 占位条目
//   Library/DA/daconfig.py:207-218            选择：按 hw_code + hw/sw 版本过滤，取**文件顺序首个**满足者
//   Library/DA/legacy/dalegacy_lib.py:563-571 region[1] = DA1 / region[2] = DA2 硬编码
#include "mtk_da_file.h"

#include <climits>   // INT_MAX（>2GiB 切片守卫）
#include <utility>   // std::as_const（遍历 Qt 容器，不得用 qAsConst）

#include <QDir>      // writeExtractedDAs 的落盘目录（存在性检查 + 拼路径）
#include <QFile>     // 落盘与失败清理

#include "mtk_err.h"       // setErr（与 mtk_preloader_emi.cpp 共用一份）
#include "mtk_le_bytes.h"  // le16At / le32At（小端解码，与各帧层共用一份）

namespace mtkbrom {
namespace {

constexpr int kHeaderSize = 0x68;      // 头部 0x68 字节（前 16 字节 "MTK_DOWNLOAD_AGENT"）
constexpr int kEntryStepDc = 0xDC;     // 现代表目步长
constexpr int kEntryStepD8 = 0xD8;     // 老格式步长（**无真实样本**，仅合成夹具覆盖）
constexpr quint16 kEntryMagic = 0xDADA;
constexpr int kMaxRegions = 64;        // 防 count 损坏导致的天量分配

// 该条目的 region 是否可用于两阶段（regionCount>=3 且 region[1]/region[2] 非空）。
// 真实文件里大量 region[1].m_len == 0（da_parse_report.json 的 warnings）—— DA1/DA2 缺一不可，
// 缺了就只能跳过（跳过的原因写进 whyNot，供 warnings 诊断）。
bool hasUsableDaRegions(const DaEntry &e, QString *whyNot)
{
    if (e.regionCount < 3 || e.regions.size() < 3) {
        if (whyNot) *whyNot = QStringLiteral("region 数不足 3（DA1/DA2 缺位）");
        return false;
    }
    if (e.regions.at(1).len == 0) {
        if (whyNot) *whyNot = QStringLiteral("region[1]（DA1）长度为 0");
        return false;
    }
    if (e.regions.at(2).len == 0) {
        if (whyNot) *whyNot = QStringLiteral("region[2]（DA2）长度为 0");
        return false;
    }
    return true;
}

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
    const quint32 count = le32At(data, 0x68);
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
        e.magic = le16At(data, o + 0x00);
        if (e.magic != kEntryMagic) {   // 条目边界的唯一锚点 → fail-closed
            setErr(error, QStringLiteral("第 %1 条目的 magic 不是 0xDADA（读到 0x%2）—— 条目表可能错位")
                              .arg(i).arg(e.magic, 4, 16, QLatin1Char('0')));
            return false;
        }
        e.hwCode           = le16At(data, o + 0x02);
        e.hwSubCode        = le16At(data, o + 0x04);
        e.hwVersion        = le16At(data, o + 0x06);
        e.swVersion        = le16At(data, o + 0x08);
        e.pagesize         = le16At(data, o + 0x0C);   // P1 不做常量校验（实测 {0,1,4096,12288}）
        e.entryRegionIndex = le16At(data, o + 0x10);
        e.regionCount      = le16At(data, o + 0x12);
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
            reg.fileOffset  = le32At(data, ro + 0);
            reg.len         = le32At(data, ro + 4);
            reg.startAddr   = le32At(data, ro + 8);
            reg.startOffset = le32At(data, ro + 12);   // P3 原样读取，不得推导
            reg.sigLen      = le32At(data, ro + 16);
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

bool selectDaEntry(const DaFile &f, quint16 dacode, quint16 deviceHwVer, quint16 deviceSwVer,
                   QStringList *warnings, DaSelection &out, QString *error)
{
    // 失败时 out 回到默认值（fail-closed：不残留上一次的成功结果，也不留"entry 有值、字节为空"的半份选择）。
    // ⚠️ isXmlForced **只在成功路径末尾**赋值 —— 在早退之前赋值会让 V6 文件失败时留下 isXmlForced=true
    //    （复审 F2：与"失败时不返回半份选择"的契约矛盾）。
    out = DaSelection{};

    if (dacode == 0) {   // 设备报 hw_code=0 = 判不出芯片；且 0 会与占位条目（hw_code==0）相撞
        setErr(error, QStringLiteral("无法判定芯片（dacode == 0）—— 拒绝选择：上游在装载阶段就把 "
                                     "hw_code==0 的占位条目剔除（daconfig.py:189/200），占位条目不得被选中"));
        return false;
    }
    QList<int> candidates;
    QStringList availableCodes;
    for (int i = 0; i < f.entries.size(); ++i) {
        const DaEntry &e = f.entries.at(i);
        const QString code = QStringLiteral("0x%1").arg(e.hwCode, 4, 16, QLatin1Char('0'));
        if (!availableCodes.contains(code))
            availableCodes << code;
        if (e.hwCode != dacode)
            continue;
        // 版本过滤：设备值为 0 时该维旁路（IoT/取不到版本的真实情形）
        if (deviceHwVer != 0 && e.hwVersion > deviceHwVer)
            continue;
        if (deviceSwVer != 0 && e.swVersion > deviceSwVer)
            continue;
        QString whyNot;
        if (!hasUsableDaRegions(e, &whyNot)) {
            if (warnings)
                *warnings << QStringLiteral("条目[%1] hw_code=%2 跳过：%3").arg(i).arg(code, whyNot);
            continue;
        }
        candidates << i;
    }
    if (candidates.isEmpty()) {
        setErr(error, QStringLiteral("DA 文件里没有可用于 hw_code=0x%1（设备版本 hw=%2/sw=%3）的条目；"
                                     "文件内的 hw_code：%4")
                          .arg(dacode, 4, 16, QLatin1Char('0'))
                          .arg(deviceHwVer, 4, 16, QLatin1Char('0'))
                          .arg(deviceSwVer, 4, 16, QLatin1Char('0'))
                          .arg(availableCodes.join(QStringLiteral(", "))));
        return false;
    }
    // 取**文件顺序上首个满足者**（上游 daconfig.py:207-218 的 first-match：`if self.da_loader is None`）
    // —— **不是**"取最大版本"（Task 2 审查 Important-1 已核上游）。并列只记告警（P2：5 元组才是唯一键）。
    const int best = candidates.first();
    const bool multiple = candidates.size() > 1;   // 告警留到**选择成功后**再记（见下，F4①）

    // 硬编码 region[1]=DA1 / region[2]=DA2（三代共同；不得改用 entryRegionIndex —— P5）
    const DaRegion da1 = f.entries.at(best).regions.at(1);
    const DaRegion da2 = f.entries.at(best).regions.at(2);
    // >2GiB 的 region 在 int 截断下会静默给出空切片（"上传 0 字节后静默成功"家族）—— 明确拒绝。
    // 守卫放在**填 out 之前**：失败时 out 保持默认，不返回半份选择（真实样本 1–4 MiB，此路不可达）
    if (da1.len > quint32(INT_MAX) || da2.len > quint32(INT_MAX)
        || da1.fileOffset > quint32(INT_MAX) || da2.fileOffset > quint32(INT_MAX)) {
        setErr(error, QStringLiteral("region 切片超出 int 范围（>2GiB）—— 拒绝（真实样本 1–4 MiB，不可达）"));
        return false;
    }
    out.entryIndex = best;
    out.entry = f.entries.at(best);
    out.da1 = da1;
    out.da2 = da2;
    out.da1Bytes = f.raw.mid(int(da1.fileOffset), int(da1.len));
    // da2 **保留尾部签名**（LEGACY 语义）：长度取 m_len，绝不裁 sigLen
    out.da2Bytes = f.raw.mid(int(da2.fileOffset), int(da2.len));
    out.isXmlForced = f.isV6;   // **成功路径**才赋值（早退路径不得泄漏 —— F2）
    // 多条满足：只在选择成功后才告警（失败路径留"已选中条目[x]"会误导排障 —— F4①）
    if (multiple && warnings)
        *warnings << QStringLiteral("hw_code=0x%1 有 %2 个条目满足版本过滤，取**文件顺序首个**（条目[%3]）——"
                                    "上游同语义；两条目仅 pagesize 不同时唯一键才是 5 元组（含 pagesize），"
                                    "本层不做 pagesize 匹配")
                         .arg(dacode, 4, 16, QLatin1Char('0')).arg(candidates.size()).arg(best);
    return true;
}

namespace {

// 写文件并查到"落盘确实成功"这一层：write() 的返回值 + flush()/close() 的错误状态都要看 ——
// QFile 带缓冲：磁盘满时数据可能先停在用户态缓冲，失败在 flush() 现形，**此时 write() 的返回值
// 仍是满额** —— 照"实写 N 字节"写会自相矛盾，故两种情况分开说（用例：符号链接指向 /dev/full）。
// 与 `spd_pac.cpp` 的 `writeFileChecked`（同名同形，两份已逐行核过**语义等价**）是**有意各自持有
// 一份**：那份在匿名命名空间里（跨 TU 取不到），提到公共头要动已完成面（SPD 模块）；本模块多这
// 30 行，好过为一个单列小任务去改别人的文件。
// **触发条件：第三处出现时抽公共内部头，三处一起改**（对面那份上方有对称的指引）。
bool writeFileChecked(const QString &path, const QByteArray &data, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {   // WriteOnly 隐含 Truncate：同名旧文件不留旧字节
        setErr(error, QStringLiteral("无法写入 DA 文件 %1：%2").arg(path, f.errorString()));
        return false;
    }
    const qint64 written = f.write(data);
    const bool shortWrite = written != qint64(data.size());
    if (shortWrite || !f.flush() || f.error() != QFileDevice::NoError) {
        const QString why = f.errorString();
        f.close();
        setErr(error, shortWrite
                          ? QStringLiteral("写入 DA 文件 %1 不完整（应写 %2 字节，实写 %3 字节）：%4")
                                .arg(path).arg(data.size()).arg(written).arg(why)
                          : QStringLiteral("DA 文件 %1 刷盘失败（%2 字节未能落盘）：%3")
                                .arg(path).arg(data.size()).arg(why));
        return false;
    }
    f.close();
    if (f.error() != QFileDevice::NoError) {
        setErr(error, QStringLiteral("关闭 DA 文件 %1 失败：%2").arg(path, f.errorString()));
        return false;
    }
    return true;
}

} // namespace

bool writeExtractedDAs(const DaSelection &sel, const QString &dir, QString &da1Path, QString &da2Path,
                       QString *error)
{
    // 失败不留半份产物（仓内约定）：出参在入口清空，只有两个文件都写成才写入。
    // 边界（同上，见头文件契约）：只覆盖**被处理到的失败**（本函数返回 false 的路径）—— SIGKILL/
    // 掉电时清理代码没机会跑，且清理依赖 QFile::remove() 成功；本层不是原子写。
    da1Path.clear();
    da2Path.clear();
    if (error)
        error->clear();

    if (sel.da1Bytes.isEmpty() || sel.da2Bytes.isEmpty()) {
        setErr(error, QStringLiteral("不落盘：选中内容含空载荷（DA1 %1 字节 / DA2 %2 字节）——"
                                     "空 DA 无法上传")
                          .arg(sel.da1Bytes.size()).arg(sel.da2Bytes.size()));
        return false;
    }
    const QDir outDir(dir);
    if (!outDir.exists()) {
        // 不替调用方建目录：路径由调用方/用户给，猜错地方比失败更糟（同 spd::writeExtractedFdls）
        setErr(error, QStringLiteral("DA 落盘目录不存在或不是目录：%1").arg(dir));
        return false;
    }
    const QString path1 = outDir.filePath(QStringLiteral("da1.bin"));
    const QString path2 = outDir.filePath(QStringLiteral("da2.bin"));

    QString why;
    if (!writeFileChecked(path1, sel.da1Bytes, &why)) {
        QFile::remove(path1);   // 可能已写出部分内容（写一半失败）——不留半份产物
        setErr(error, why);
        return false;
    }
    if (!writeFileChecked(path2, sel.da2Bytes, &why)) {
        // 只落成 da1.bin 会让调用方以为拿到了可用的一对（da2.bin 的字节落到设备侧才现形）——
        // 两个都删（与 spd::writeExtractedFdls 同姿态；QFile::remove 对"本来就没写成/是目录"
        // 返回 false，不影响失败结论）
        QFile::remove(path1);
        QFile::remove(path2);
        setErr(error, why);
        return false;
    }
    da1Path = path1;
    da2Path = path2;
    return true;
}

} // namespace mtkbrom
