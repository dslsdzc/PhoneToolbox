#include "disk_image.h"
#include <QtEndian>
#include <algorithm>

namespace imgdisk {

namespace {
// 两种 LBA 布局（逻辑块大小 = 头所在字节偏移 = LBA 编号的字节单位）
constexpr quint64 kLba512 = 512;
constexpr quint64 kLba4096 = 4096;

// UEFI 规范: GPT 分区项的 type_guid 为混合字节序存储
// Data1(4B LE) - Data2(2B LE) - Data3(2B LE) - Data4(2B BE) - Data5(6B 原序)
// 前三个字段需反转字节才能得到规范文本表示 (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)
QString formatTypeGuid(const char *e)
{
    auto hexUp = [](const QByteArray &b) {
        return QString::fromLatin1(b.toHex()).toUpper();
    };
    QByteArray d1(e, 4);
    QByteArray d2(e + 4, 2);
    QByteArray d3(e + 6, 2);
    std::reverse(d1.begin(), d1.end());
    std::reverse(d2.begin(), d2.end());
    std::reverse(d3.begin(), d3.end());
    return QString("%1-%2-%3-%4-%5")
        .arg(hexUp(d1))
        .arg(hexUp(d2))
        .arg(hexUp(d3))
        .arg(hexUp(QByteArray(e + 8, 2)))
        .arg(hexUp(QByteArray(e + 10, 6)));
}

bool isNullGuid(const char *e)
{
    for (int i = 0; i < 16; ++i)
        if (e[i] != 0)
            return false;
    return true;
}
} // namespace

bool isGpt(const QByteArray &header)
{
    return header.size() >= 8 && header.left(8) == "EFI PART";
}

// GPT 布局探测：签名在 0x200 → 512 字节 LBA；在 0x1000 → 4096 字节 LBA。
// 顺序（512 在前）与 bkerler 读取端 `for sectorsize in [512, 4096]`（edl/edlclient/Library/gpt.py:526-531）
// 一致，理由是两边的误判概率**不对称**：
//   * 512 是 eMMC/离线镜像的绝大多数形态，先试即"既有行为零变化"；
//   * 4096 布局里 0x200 落在 LBA0 的**保留区**（保护 MBR 只占 LBA0 的前 512 字节，其余按 UEFI
//     规范保留为 0）—— 要在此撞上签名得让整块零填充被写成 "EFI PART"；
//   * 反过来若先试 4096：512 布局的 0x1000 = LBA8 起就是**真实分区数据**，撞上签名的概率不可忽略。
// 只回答"头读自哪个字节偏移"，不含任何解析语义 —— 调用方（Phase B flash_plan 的失败文案分层）
// 也用它区分"文件被截断"与"表坏了"，故公开在头文件。
bool detectGptLayout(const QByteArray &disk, quint64 &lbaSize)
{
    if (disk.size() >= static_cast<qint64>(kLba512) + 8 &&
        isGpt(disk.mid(static_cast<int>(kLba512), 8))) {
        lbaSize = kLba512;
        return true;
    }
    if (disk.size() >= static_cast<qint64>(kLba4096) + 8 &&
        isGpt(disk.mid(static_cast<int>(kLba4096), 8))) {
        lbaSize = kLba4096;
        return true;
    }
    return false;
}

// 支持两种 LBA 布局：512（eMMC/既有行为）与 4096（真实 EDL 包 gpt_main{N}.bin / UFS）。
// 4096 布局的证据（三源印证，另见 tests/flash_plan_helpers.h 的 lbaSize 注释）：
//   * 真实样本 edl/edlclient/Library/TestFiles/gpt_sm8180x.bin：24576 字节、`EFI PART`@0x1000、
//     part_entry_lba=2、32 项×128B；
//   * reference/qdl/tests/data/rawprogram1.xml:12：`gpt_main1.bin num_partition_sectors="6"`
//     ⇒ 6 × 4096 = 24576 字节，与上者吻合；
//   * bkerler 读取端两种都试（edl/edlclient/Library/gpt.py:526-531）。
// 一旦识别出 lbaSize，**所有** LBA→字节的换算都用它：头的位置、表项数组位置
// （tableOff = tableLba × lbaSize）、以及回填到 DiskInfo::sectorSize 供 extractPartition 使用。
//
// **CRC32：本解析器不校验任何校验和** —— 头 +16（header CRC32）、+88（表项数组 CRC32）连读都没读，
// 也不读备份头（`isGpt` 只看 8 字节签名）。将来若要加校验，**两种布局都必须覆盖**，且注意口径：
// 头 CRC 是对"该头自身的 92 字节"算的、表 CRC 是对"numEntries × entrySize"字节算的（qdl 侧记法
// 见 reference/qdl/tests/data/patch1.xml:27 的 `CRC32(1,92)`），两者都与 lbaSize 无关 ——
// 识别只决定"头从哪个字节偏移开始读"，所以加校验时**不要**在识别之后再做 LBA 换算或字节搬运，
// 否则 4096 布局会开始校验失败。
bool parseGpt(const QByteArray &disk, DiskInfo &out)
{
    quint64 lbaSize = 0;
    if (!detectGptLayout(disk, lbaSize))
        return false;
    // 整盘至少"保护 MBR + 头"两个 LBA（原 2×512 门槛按识别出的 LBA 单位推广：
    // 512 布局仍 < 1024 拒绝，4096 布局 < 8192 拒绝）；截断文件由调用方另行归因。
    if (static_cast<quint64>(disk.size()) < 2 * lbaSize)
        return false;
    const char *h = disk.constData() + lbaSize;
    const quint64 tableLba = qFromLittleEndian<quint64>(h + 72);
    const quint32 numEntries = qFromLittleEndian<quint32>(h + 80);
    const quint32 entrySize = qFromLittleEndian<quint32>(h + 84);
    if (entrySize < 128)
        return false;
    // 分区表 LBA 超出磁盘范围则失败（同时避免 tableLba * lbaSize 溢出）
    if (tableLba > static_cast<quint64>(disk.size()) / lbaSize)
        return false;
    out.sectorSize = lbaSize;
    out.totalSectors = static_cast<quint64>(disk.size()) / lbaSize;
    const quint64 tableOff = tableLba * lbaSize;
    out.partitions.clear();
    // 用 quint64 累加偏移，避免 i * entrySize 有符号溢出
    for (quint32 i = 0; i < numEntries; ++i) {
        const quint64 off = tableOff + static_cast<quint64>(i) * entrySize;
        if (off + 128 > static_cast<quint64>(disk.size()))
            break;
        const char *e = disk.constData() + off;
        const quint64 first = qFromLittleEndian<quint64>(e + 32);
        const quint64 last = qFromLittleEndian<quint64>(e + 40);
        // 空项: 全零 type GUID 且无扇区范围（真实分区 first_lba 远大于 0，
        // 故单看 type GUID 全零不足以判定空项——测试用全零 GUID 的合法分区须保留）
        if (isNullGuid(e) && first == 0 && last == 0)
            continue;
        Partition p;
        p.typeGuid = formatTypeGuid(e);
        p.startSector = first;
        p.numSectors = last >= first ? last - first + 1 : 0;
        QByteArray nameRaw(e + 56, 72);
        // UTF-16LE → UTF-8
        p.name = QString::fromUtf16(reinterpret_cast<const char16_t *>(nameRaw.constData()),
                                    nameRaw.size() / 2).split(QChar(0)).first();
        out.partitions.append(p);
    }
    return true;
}

// lbaSize 必须与本盘一致：parseGpt 回填在 DiskInfo::sectorSize（512 或 4096），调用方原样传回
// （默认 512 只为兼容既有调用点；传错不会报错，只会**静默取错字节区段**）。
bool extractPartition(const QByteArray &disk, const Partition &part, QByteArray &outRaw,
                      quint64 lbaSize)
{
    if (part.numSectors == 0 || lbaSize == 0)   // lbaSize==0 防"调用方漏传"变成除零/全文件
        return false;
    const quint64 diskSize = static_cast<quint64>(disk.size());
    // 除零检查 + 溢出防护: 先按扇区数比较再乘
    if (part.startSector > diskSize / lbaSize)
        return false;
    const quint64 off = part.startSector * lbaSize;
    if (part.numSectors > (diskSize - off) / lbaSize)
        return false;
    const quint64 len = part.numSectors * lbaSize;
    outRaw = disk.mid(static_cast<int>(off), static_cast<int>(len));
    return true;
}

} // namespace imgdisk
