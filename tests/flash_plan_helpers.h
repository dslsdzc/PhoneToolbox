#pragma once
// Phase B Task 3 共享夹具：合成 GPT 字节（手写字节，**绝不调用被测解析代码**）。
// Task 6（会话编排）可复用本文件。
#include <QByteArray>
#include <QFile>
#include <QString>
#include <QtGlobal>

// 合成带 **一个非空分区项**（其余表项留空）的 GPT 字节 —— 文件名 `gpt_main{N}.bin` 的模拟物。
//
// == 构造依据：先读 `src/image_engine/disk_image.cpp:41-88`（parseGpt）的实际接受条件 ==
//   * 签名 `"EFI PART"`：parseGpt 在**字节偏移 512** 处找头（`disk_image.cpp:8,50`，
//     `isGpt(disk.mid(kSector, 8))`）；整盘 < 1024 字节直接拒绝（:48-49）。
//   * 头字段只读三个：分区表起始 LBA（u64 LE @+72）、表项数（u32 LE @+80）、表项大小
//     （u32 LE @+84，**必须 ≥128**，否则 return false）；另有 `表LBA > size/512` 的越界拒绝（:59）。
//   * 表项：firstLBA u64 LE @+32、lastLBA u64 LE @+40、分区名 UTF-16LE @+56（取 72 字节，首个
//     NUL 处截断，:81-84）；**空项判据 = type GUID 全零 且 first==last==0**（:73-76）。
//   * **不校验**：头 CRC32 / 表 CRC32 / 备份头 / 其余保留字段；**type GUID 也不要求非零**
//     （:73-76 注释明确"全零 GUID 的合法分区须保留"）—— 故这些字段按真实 GPT 形态写上，
//     但**不参与判定**，不要以为它们在起作用。
//   * parseGpt **不检查**分区扇区范围是否落在文件内 —— 本夹具因此可以只含 GPT 区域本身
//     （真实 `gpt_main{N}.bin` 就是这样：它是"GPT 区域"的镜像，不是整盘镜像）。
// ⚠️ 本夹具只与**本仓解析器**自洽。Task 3 要测的是 `buildPlanFromDir` 的**对账逻辑**，GPT 解析器
//    本身已有自己的测试（`tests/test_disk.cpp`）—— 这里不重复测解析器，也不照抄任何"示例偏移"：
//    上面标注的偏移就是解析器**实际读**的偏移。
//
// == lbaSize：逻辑块大小（= 头所在字节偏移 = LBA 编号的字节单位）==
//   * **512（默认）**：解析器原生布局（`tests/test_disk.cpp:13-38` 同款），eMMC 类设备的形态。
//     用例：`opsReconcilesGptLayout512`（显式传 512）。
//   * **4096**：**真实 EDL 包 `gpt_main{N}.bin` 的形态**。证据（仅作**注释引用**，见下）：
//       - `edl/edlclient/Library/TestFiles/gpt_sm8180x.bin`：24576 字节，`EFI PART` 在 **0x1000**，
//         part_entry_lba=2、32 项 ×128B；
//       - `reference/qdl/tests/data/rawprogram1.xml:12`：`gpt_main1.bin num_partition_sectors="6"`
//         —— 6 × 4096 = 24576，与上者完全吻合；
//       - bkerler 读盘时**同时试 512 与 4096**（`edl/edlclient/Library/gpt.py:526-531`）。
//     `imgdisk::parseGpt` 只按 512 定位 → 该布局由 `buildPlanFromDir` 内的**布局适配**
//     （`flash_plan.cpp` 的 readGptPartitions）归一化后再交给 parseGpt。
//     用例：`opsReconcilesGptLayout4096`。
// ⚠️ **测试绝不读真实样本 `gpt_sm8180x.bin`**：它属 `edl/` 子模块，未初始化的克隆（CI/新机器）里不存在
//    → 测试会挂。上面两条只是**证据引用**；4096 布局的夹具是本函数用同样布局手写出来的字节。
// ⚠️ 两种布局的用例都必须断言"**对账真的发生**"（元数据故意写错 → 以 GPT 为准 + warning），
//    而不是只断言 parse 成功 —— 解析器本身另有 tests/test_disk.cpp。
//
// 文件尺寸 = (2 + 表项数组占用的 LBA 数) × lbaSize：LBA0 保护 MBR + LBA1 头 + 表项数组。
inline QByteArray buildGptWithPartition(const QByteArray &name, quint64 firstLba, quint64 lastLba,
                                        quint32 lbaSize = 512, quint32 numEntries = 48)
{
    const quint32 entrySize = 128;
    const quint64 tableBytes = quint64(numEntries) * entrySize;
    const quint64 tableSectors = (tableBytes + lbaSize - 1) / lbaSize;   // 表项数组占用的 LBA 数
    const quint64 totalBytes = (2 + tableSectors) * lbaSize;
    QByteArray g(static_cast<int>(totalBytes), '\0');

    auto put32 = [&g](quint64 off, quint32 v) {
        for (int i = 0; i < 4; ++i)
            g[int(off) + i] = char((v >> (i * 8)) & 0xFF);
    };
    auto put64 = [&g](quint64 off, quint64 v) {
        for (int i = 0; i < 8; ++i)
            g[int(off) + i] = char((v >> (i * 8)) & 0xFF);
    };

    // LBA0 保护 MBR（parseGpt 不读它，写上只为形态真实；test_disk.cpp:17-19 同款）
    g[446 + 4] = char(0xEE);
    g[510] = char(0x55);
    g[511] = char(0xAA);

    const quint64 hdr = lbaSize;             // LBA1：512 布局时正好是 parseGpt 找头的位置
    g.replace(int(hdr), 8, "EFI PART");
    put32(hdr + 8, 0x00010000);              // revision 1.0（解析器不读）
    put32(hdr + 12, 92);                     // header size（解析器不读）
    put32(hdr + 16, 0);                      // header CRC32：**不校验**（见上）
    put64(hdr + 24, 1);                      // current LBA（不读）
    put64(hdr + 32, 0);                      // backup LBA：夹具无备份头（不读）
    put64(hdr + 40, 2 + tableSectors);       // first usable LBA（不读）
    put64(hdr + 48, lastLba);                // last usable LBA（不读）
    for (int i = 0; i < 16; ++i)             // disk GUID（不读）
        g[int(hdr + 56 + i)] = char(0x10 + i);
    put64(hdr + 72, 2);                      // ← 分区表起始 LBA（解析器**读**）
    put32(hdr + 80, numEntries);             // ← 表项数（解析器**读**）
    put32(hdr + 84, entrySize);              // ← 表项大小（解析器**读**）
    put32(hdr + 88, 0);                      // 表 CRC32：**不校验**（见上）

    // 第一个表项 = 目标分区；其余保持全零（= 空项，parseGpt 跳过：:73-76）
    const quint64 e = 2 * lbaSize;
    for (int i = 0; i < 16; ++i)             // type GUID（不要求非零，写上求形态真实）
        g[int(e + i)] = char(0xA0 + i);
    for (int i = 0; i < 16; ++i)             // unique GUID（不读）
        g[int(e + 16 + i)] = char(0xB0 + i);
    put64(e + 32, firstLba);                 // ← firstLBA（解析器**读**）
    put64(e + 40, lastLba);                  // ← lastLBA（解析器**读**）
    // 分区名 UTF-16LE（≤35 字符 + 结尾 NUL 仍在 72 字节字段内）；按 UTF-8 解释入参，与 parseGpt
    // "UTF-16LE → QString" 的解码口径对称（disk_image.cpp:81-84）
    const QString nameStr = QString::fromUtf8(name);
    const int nameChars = qMin(nameStr.size(), 35);
    for (int i = 0; i < nameChars; ++i) {
        const ushort u = nameStr.at(i).unicode();
        g[int(e + 56 + 2 * i)] = char(u & 0xFF);
        g[int(e + 56 + 2 * i + 1)] = char((u >> 8) & 0xFF);
    }
    return g;
}

// 落盘小工具（brief 原样；返回是否写满全部字节）
inline bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const qint64 n = f.write(bytes);
    f.close();
    return n == bytes.size();
}
