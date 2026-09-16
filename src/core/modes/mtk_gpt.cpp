#include "core/modes/mtk_gpt.h"

namespace mtkgpt {
namespace {

// UEFI：备份 GPT 占磁盘末尾若干扇区（条目表 ≤ 32 扇区 + 1 个头部扇区）。本模块用它做两件事：
//   ① diskSectors 未知/过小时的早退判据；② 备份窗口大小的**上限**（头部字段损坏时别发超大读）。
constexpr quint64 kBackupWindowSectors = 34;

// 探测扇区大小的读窗口：2 × 4096 —— LBA1 的头在 512 与 4096 两种布局下都落在窗口内
//（4096 布局需要 0x1000 + 0x5C = 4188 字节）。
constexpr int kProbeLen = 0x2000;

// 一次读主 GPT 的上限 = GPT 保留区（上游按 0x22 个扇区读整张表：realtime.py:88-92 的 "gpt"
// 特判 filesize = 0x22 × pagesize）—— 只作"头部字段异常时别发超大读"的护栏。
constexpr quint64 kPrimaryAreaMax = 0x22ull * 4096;

// 条目参数判据（UEFI：条目大小 ≥ 128、条数上限 4096；"2 的幂"这条不查 —— 与本模块的
// 解析正确性无关）—— "读多少"（primaryExtent）与"解析多少"（parseAt）共用同一份，
// 避免两处判据漂移。
constexpr quint32 kMinEntrySize = 128;
constexpr quint32 kMaxEntrySize = 4096;
constexpr quint32 kMaxEntries = 4096;

quint32 rdU32(const QByteArray &b, int off)
{
    return quint32(quint8(b.at(off))) | (quint32(quint8(b.at(off + 1))) << 8)
         | (quint32(quint8(b.at(off + 2))) << 16) | (quint32(quint8(b.at(off + 3))) << 24);
}

void wrU32(QByteArray &b, int off, quint32 v)
{
    b[off] = char(v & 0xFF); b[off + 1] = char((v >> 8) & 0xFF);
    b[off + 2] = char((v >> 16) & 0xFF); b[off + 3] = char((v >> 24) & 0xFF);
}

void wrU64(QByteArray &b, int off, quint64 v)
{
    wrU32(b, off, quint32(v & 0xFFFFFFFFu));
    wrU32(b, off + 4, quint32(v >> 32));
}

quint64 rdU64(const QByteArray &b, int off)
{
    return quint64(rdU32(b, off)) | (quint64(rdU32(b, off + 4)) << 32);
}

// CRC-32/ISO-HDLC（与 zlib.crc32 同参数；Qt 的 qChecksum 是 CRC-16，不能用）
quint32 crc32(const QByteArray &data)
{
    quint32 crc = 0xFFFFFFFFu;
    for (const char ch : data) {
        crc ^= quint8(ch);
        for (int i = 0; i < 8; ++i)
            crc = (crc >> 1) ^ (0xEDB88320u & (quint32(0) - (crc & 1u)));
    }
    return ~crc;
}

bool guidAllZero(const QByteArray &g) { return g.size() == 16 && g == QByteArray(16, '\0'); }

QString nameOf(const QByteArray &entry)
{
    const QByteArray raw = entry.mid(56, 72);                 // UTF-16LE，\x00\x00 终止（上游 ustring(72)）
    int end = 0;
    while (end + 1 < raw.size() && !(raw.at(end) == '\0' && raw.at(end + 1) == '\0'))
        end += 2;
    return QString::fromUtf16(reinterpret_cast<const char16_t *>(raw.constData()), end / 2);
}

bool entryGeometry(const QByteArray &hdr, quint32 *count, quint32 *size)
{
    const quint32 c = rdU32(hdr, 0x50);
    const quint32 s = rdU32(hdr, 0x54);
    if (c == 0 || c > kMaxEntries || s < kMinEntrySize || s > kMaxEntrySize)
        return false;
    *count = c;
    *size = s;
    return true;
}

// 解析主 GPT 所需字节数 = 条目区起点 × 扇区 + 条数 × 条目大小（头字段不可用 → false）
bool primaryExtent(const QByteArray &head, quint32 sectorSize, quint64 *out)
{
    if (head.size() < int(sectorSize) + 0x5C)
        return false;
    const QByteArray hdr = head.mid(int(sectorSize), 0x5C);
    quint32 count = 0;
    quint32 size = 0;
    if (!entryGeometry(hdr, &count, &size))
        return false;
    *out = rdU64(hdr, 0x48) * quint64(sectorSize) + quint64(count) * size;
    return true;
}

// headerPos = 头在 raw 里的偏移；entriesBase = **条目区在 raw 里的偏移**
bool parseAt(const QByteArray &raw, quint32 sectorSize, int headerPos, int entriesBase,
             quint64 diskSectors, bool isBackup, Table &out, QString *error)
{
    if (sectorSize != 512 && sectorSize != 4096) {
        if (error) *error = QStringLiteral("扇区大小不支持（%1）").arg(sectorSize);
        return false;
    }
    if (headerPos < 0 || headerPos + 0x5C > raw.size()) {
        if (error) *error = QStringLiteral("GPT 头读取不足（raw %1 字节，头偏移 %2）").arg(raw.size()).arg(headerPos);
        return false;
    }
    const QByteArray hdr = raw.mid(headerPos, 0x5C);
    if (hdr.left(8) != QByteArray("EFI PART", 8)) {
        if (error) *error = QStringLiteral("GPT 签名不符（读到 %1）").arg(QString::fromLatin1(hdr.left(8).toHex()));
        return false;
    }
    const quint32 revision = rdU32(hdr, 0x08);
    if (revision != 0x00010000u) {
        if (error) *error = QStringLiteral("GPT revision 不符（0x%1，期望 0x10000）")
                                .arg(revision, 8, 16, QLatin1Char('0'));
        return false;
    }
    const quint32 headerSize = rdU32(hdr, 0x0C);
    if (headerSize < 0x5C || headerSize > sectorSize) {
        if (error) *error = QStringLiteral("GPT header_size 异常（%1）").arg(headerSize);
        return false;
    }
    // 头部 CRC：**把 crc32 字段清零后再算**（UEFI 规定；上游从不校验）。
    // 范围取 raw 里的 header_size 字节（本地 hdr 只有 0x5C 字节，header_size > 0x5C 的盘
    // 直接 left() 会算在截断数据上 → 合法盘被误拒）；raw 不够长即按现有字节算，CRC 自然不符。
    QByteArray hdrForCrc = raw.mid(headerPos, qMin<int>(int(headerSize), raw.size() - headerPos));
    hdrForCrc.replace(0x10, 4, QByteArray(4, '\0'));
    const quint32 crcStored = rdU32(hdr, 0x10);
    const quint32 crcCalc = crc32(hdrForCrc);
    if (crcStored != crcCalc) {
        if (error) *error = QStringLiteral("GPT 头部 CRC 不符（存 0x%1，算 0x%2）")
                                .arg(crcStored, 8, 16, QLatin1Char('0')).arg(crcCalc, 8, 16, QLatin1Char('0'));
        return false;
    }

    quint32 entryCount = 0;
    quint32 entrySize = 0;
    if (!entryGeometry(hdr, &entryCount, &entrySize)) {
        if (error) *error = QStringLiteral("GPT 条目参数异常（count=%1 size=%2）")
                                .arg(rdU32(hdr, 0x50)).arg(rdU32(hdr, 0x54));
        return false;
    }
    const quint64 entryBytes = quint64(entryCount) * entrySize;
    if (entriesBase < 0 || quint64(entriesBase) + entryBytes > quint64(raw.size())) {
        if (error) *error = QStringLiteral("GPT 条目表读取不足（需要到 %1，raw 只有 %2）")
                                .arg(quint64(entriesBase) + entryBytes).arg(raw.size());
        return false;
    }
    const QByteArray entries = raw.mid(entriesBase, int(entryBytes));
    const quint32 entriesCrcStored = rdU32(hdr, 0x58);
    const quint32 entriesCrcCalc = crc32(entries);
    if (entriesCrcStored != entriesCrcCalc) {
        if (error) *error = QStringLiteral("GPT 条目表 CRC 不符（存 0x%1，算 0x%2）")
                                .arg(entriesCrcStored, 8, 16, QLatin1Char('0')).arg(entriesCrcCalc, 8, 16, QLatin1Char('0'));
        return false;
    }

    Table t;
    t.sectorSize = sectorSize;
    t.diskSectors = diskSectors;
    t.usedBackup = isBackup;
    t.diskGuid = hdr.mid(0x38, 16);
    for (quint32 i = 0; i < entryCount; ++i) {
        const QByteArray e = entries.mid(int(i) * int(entrySize), int(entrySize));
        if (guidAllZero(e.left(16)))
            continue;                                   // UEFI：type GUID 全 0 = 未使用（上游用 unique GUID，不复刻）
        Partition p;
        p.typeGuid = e.left(16);
        p.uniqueGuid = e.mid(16, 16);
        p.firstLba = rdU64(e, 32);
        p.lastLba = rdU64(e, 40);
        p.flags = rdU64(e, 48);
        p.name = nameOf(e);
        if (p.lastLba < p.firstLba)
            continue;                                   // 端点反了的条目跳过（不静默产出负长度）
        t.partitions.append(p);
    }
    out = t;
    return true;
}

} // namespace

quint64 offsetBytes(const Partition &p, quint32 sectorSize) { return p.firstLba * quint64(sectorSize); }

quint64 sizeBytes(const Partition &p, quint32 sectorSize)
{
    return p.lastLba >= p.firstLba ? (p.lastLba - p.firstLba + 1) * quint64(sectorSize) : 0;
}

bool parsePrimary(const QByteArray &raw, quint32 sectorSize, Table &out, QString *error)
{
    if (raw.size() < int(2 * sectorSize) + 0x5C) {
        if (error) *error = QStringLiteral("主 GPT 读取不足（%1 字节）").arg(raw.size());
        return false;
    }
    if (raw.mid(int(sectorSize), 8) != QByteArray("EFI PART", 8)) {
        if (error) *error = QStringLiteral("主 GPT 签名不符（LBA1 读到 %1）")
                                .arg(QString::fromLatin1(raw.mid(int(sectorSize), 8).toHex()));
        return false;
    }
    const quint64 entryLba = rdU64(raw.mid(int(sectorSize), 0x5C), 0x48);
    return parseAt(raw, sectorSize, int(sectorSize), int(entryLba * sectorSize), 0, false, out, error);
}

bool parseBackup(const QByteArray &raw, quint32 sectorSize, quint64 windowFirstLba, Table &out, QString *error)
{
    if (raw.size() < int(2 * sectorSize) + 0x5C || raw.size() % int(sectorSize) != 0) {
        if (error) *error = QStringLiteral("备份 GPT 窗口大小异常（%1 字节）").arg(raw.size());
        return false;
    }
    const int headerPos = raw.size() - int(sectorSize);          // 头在窗口**最后一扇区**（真样本实测）
    if (raw.mid(headerPos, 8) != QByteArray("EFI PART", 8)) {
        if (error) *error = QStringLiteral("备份 GPT 签名不符（窗口末扇区读到 %1）")
                                .arg(QString::fromLatin1(raw.mid(headerPos, 8).toHex()));
        return false;
    }
    const quint64 entryLba = rdU64(raw.mid(headerPos, 0x5C), 0x48);
    if (entryLba < windowFirstLba) {
        if (error) *error = QStringLiteral("备份 GPT 条目起始 LBA（%1）在窗口（%2 起）之前")
                                .arg(entryLba).arg(windowFirstLba);
        return false;
    }
    const int entriesBase = int((entryLba - windowFirstLba) * quint64(sectorSize));
    const quint64 windowSectors = quint64(raw.size()) / quint64(sectorSize);
    return parseAt(raw, sectorSize, headerPos, entriesBase, windowFirstLba + windowSectors, true, out, error);
}

bool readTable(const ReadFn &read, quint64 diskSectors, Table &out, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };
    auto fail = [error](const QString &m) { if (error) *error = m; return false; };

    if (!read)
        return fail(QStringLiteral("GPT 读取失败：读回调为空"));

    // 1) 主 GPT：扇区大小按 512 → 4096 探测（顺序同上游 gpt.py:219，真样本就是 4096）。
    //    先只读"能覆盖两种布局的 LBA1 头"的 kProbeLen 字节 —— 文件型读回调（整盘镜像、
    //    8 扇区备份窗口）按请求范围严格判越界，一次要 0x22 扇区会在小样本上直接失败。
    QByteArray head;
    QString probeErr;
    if (!read(0, kProbeLen, &head, &probeErr) || head.size() < 0x5C + 512)
        return fail(QStringLiteral("GPT 头部读取失败（读回调返回不足）%1")
                        .arg(probeErr.isEmpty() ? QString() : QStringLiteral("：") + probeErr));
    quint32 sectorSize = 0;
    for (quint32 ss : {512u, 4096u}) {
        if (head.size() >= int(ss) + 8 && head.mid(int(ss), 8) == QByteArray("EFI PART", 8)) {
            sectorSize = ss;
            break;
        }
    }
    if (sectorSize == 0)
        return fail(QStringLiteral("未找到 GPT 签名（LBA1 的 512 与 4096 偏移都不是 EFI PART）"));
    say(QStringLiteral("GPT：扇区大小探测为 %1 字节").arg(sectorSize));

    // parsePrimary 的契约是"头 + 整张条目表在同一个缓冲里"：条目区起点/条数/条目大小都在头里，
    // 按头部字段把所需范围一次算清并按扇区上取整补读（头字段不可用/超上限则不补读，交给
    // parsePrimary 报精确原因）。设备侧读回调可能短读，短读结果原样收下由 parsePrimary 判尺寸。
    quint64 needBytes = 0;
    if (primaryExtent(head, sectorSize, &needBytes) && needBytes > quint64(head.size())
        && needBytes <= kPrimaryAreaMax) {
        QByteArray bigger;
        QString extErr;
        if (read(0, int((needBytes + sectorSize - 1) / sectorSize * sectorSize), &bigger, &extErr)
            && bigger.size() > head.size())
            head = bigger;
    }

    QString primaryErr;
    if (parsePrimary(head, sectorSize, out, &primaryErr)) {
        out.diskSectors = diskSectors;
        return true;
    }
    say(QStringLiteral("主 GPT 不可用（%1）").arg(primaryErr));

    // 2) 备份兜底（上游这段实际不生效：partition.py:45 的 seek 被 gpt.py:161 的绝对 seek 覆盖，
    //    本实现按头部字段真正读末尾）。UEFI 备份布局 = 头在**磁盘末扇区**、条目区在它前面若干
    //    扇区（真样本：条目区起点 = 头里的 part_entry_start_lba = 窗口起始 LBA，窗口 8 扇区）。
    //    先读末扇区拿条目区起点，再按"条目起点 → 磁盘末尾"整段读 —— windowFirstLba 必须**正好**
    //    是窗口第一扇区的 LBA：写死"末尾 34 扇区"会在真样本这种更小的窗口上取错起点，请求整体
    //    落到窗口之外（文件型读回调直接失败）。
    if (diskSectors < kBackupWindowSectors)
        return fail(QStringLiteral("主 GPT 不可用且磁盘扇区数未知/过小（%1）—— 无法读备份 GPT").arg(diskSectors));
    const quint64 headerLba = diskSectors - 1;
    QByteArray backHdr;
    QString backErr;
    if (!read(headerLba * sectorSize, int(sectorSize), &backHdr, &backErr) || backHdr.size() < 0x5C)
        return fail(QStringLiteral("备份 GPT 读取失败（末扇区 LBA %1）%2")
                        .arg(headerLba).arg(backErr.isEmpty() ? QString() : QStringLiteral("：") + backErr));
    if (backHdr.left(8) != QByteArray("EFI PART", 8))
        return fail(QStringLiteral("主 GPT 与备份 GPT 都不可用：主（%1）；备（末扇区 LBA %2 无 EFI PART）")
                        .arg(primaryErr).arg(headerLba));
    const quint64 windowFirstLba = rdU64(backHdr, 0x48);   // 备份条目区起点 = 备份窗口起始 LBA
    if (windowFirstLba >= diskSectors || diskSectors - windowFirstLba > kBackupWindowSectors)
        return fail(QStringLiteral("备份 GPT 条目区起点异常（LBA %1，磁盘 %2 扇区）")
                        .arg(windowFirstLba).arg(diskSectors));
    const quint64 windowSectors = diskSectors - windowFirstLba;
    QByteArray tail;
    QString winErr;
    if (!read(windowFirstLba * sectorSize, int(windowSectors * sectorSize), &tail, &winErr))
        return fail(QStringLiteral("备份 GPT 读取失败（LBA %1 起 %2 扇区）%3")
                        .arg(windowFirstLba).arg(windowSectors)
                        .arg(winErr.isEmpty() ? QString() : QStringLiteral("：") + winErr));
    QString backupErr;
    if (!parseBackup(tail, sectorSize, windowFirstLba, out, &backupErr))
        return fail(QStringLiteral("主 GPT 与备份 GPT 都不可用：主（%1）；备（%2）").arg(primaryErr, backupErr));
    out.diskSectors = diskSectors;
    say(QStringLiteral("已使用**备份** GPT（窗口 LBA %1 起 %2 扇区）").arg(windowFirstLba).arg(windowSectors));
    return true;
}

QByteArray testBuildSyntheticGpt(quint32 sectorSize, quint32 sectorCount)
{
    // 单分区 "boot"（LBA 34..35）：头部合法 + 双 CRC 正确（供正/负向用例）
    const quint32 entryCount = 4;
    const quint32 entrySize = 128;
    QByteArray raw(int(sectorCount * sectorSize), '\0');
    raw[0x1FE] = char(0x55); raw[0x1FF] = char(0xAA);             // 保护性 MBR
    const int hdrPos = int(sectorSize);
    raw.replace(hdrPos, 8, QByteArray("EFI PART", 8));
    wrU32(raw, hdrPos + 0x08, 0x00010000u);                       // revision
    wrU32(raw, hdrPos + 0x0C, 92);                                // header_size
    wrU64(raw, hdrPos + 0x18, 1);                                 // current_lba
    wrU64(raw, hdrPos + 0x20, sectorCount - 1);                   // backup_lba
    wrU64(raw, hdrPos + 0x28, 34);                                // first_usable
    wrU64(raw, hdrPos + 0x30, sectorCount - 34);                  // last_usable
    wrU64(raw, hdrPos + 0x48, 2);                                 // part_entry_start_lba
    wrU32(raw, hdrPos + 0x50, entryCount);
    wrU32(raw, hdrPos + 0x54, entrySize);
    const int entriesPos = int(2 * sectorSize);
    QByteArray entries(int(entryCount * entrySize), '\0');
    entries[0] = char(0xEB); entries[1] = char(0xA0); entries[2] = char(0xD0);   // 非 0 type GUID
    wrU64(entries, 32, 34);
    wrU64(entries, 40, 35);
    const QString nm = QStringLiteral("boot");
    for (int i = 0; i < nm.size(); ++i) {
        entries[56 + i * 2] = char(nm.at(i).unicode() & 0xFF);
        entries[56 + i * 2 + 1] = char(nm.at(i).unicode() >> 8);
    }
    raw.replace(entriesPos, entries.size(), entries);
    wrU32(raw, hdrPos + 0x58, crc32(entries));                    // 条目表 CRC
    QByteArray hdrForCrc = raw.mid(hdrPos, 92);
    hdrForCrc.replace(0x10, 4, QByteArray(4, '\0'));
    wrU32(raw, hdrPos + 0x10, crc32(hdrForCrc));                  // 头部 CRC
    return raw;
}

} // namespace mtkgpt
