// src/core/eub/eub_loadout.cpp
//
// 8 张布局表的**数值本体** + 查表/切段/摘要。
//
// 数值的唯一来源是设计 spec §5.4（docs/superpowers/specs/2026-09-17-exynos-eub-design.md），
// spec 的表逐条来自 facts §C3–§C6；每条 sourceNote 落到 reference/ 里的 file:line
// （reference/ 是 gitignored 的只读参照：本文件只**记录**出处，不复制其代码）。
// 改动任何一个数字前先改 spec —— tests/test_eub_loadout.cpp 对 8 张表共 41 个段逐值硬断言。
//
// ⚠️ 本线**无真机、也无真样本**（sboot.bin 是三星签名二进制，不进仓库，facts §F1/§F8）：
// 下表只到"照抄参照实现 + 与 spec 逐值对齐"这一层证据，**未在设备上验证过**。布局表绑定
// 固件修订（facts §C9），换修订即可能错位；本层不判断镜像与表是否匹配，只按表切。
#include "eub_loadout.h"

#include <QCryptographicHash>
#include <QRegularExpression>

namespace eub {
namespace {

// ---- 各表构造。evidence 取值只能是 eub_loadout.h 顶部列的四档之一（测试会核对）。----

// spec §5.4 行 1。出处 reference/exynos-usbdl/scripts/split-sboot-8890.sh:2-5（单源 dd 四行）：
//   2: skip=0 bs=512 count=16 → 0x2000 @ 0x0
//   3: skip=16 bs=512 count=288 → 0x24000 @ 0x2000
//   4: skip=155648 bs=1 count=158992 → 0x26D10 @ 0x26000（该段非 512 对齐，故用 bs=1）
//   5: skip=776 bs=512 count=1672 → 0xD1000 @ 0x61000
EubLoadout make8890()
{
    EubLoadout lo;
    lo.soc = QStringLiteral("Exynos8890");
    lo.models = {QStringLiteral("G930W8")};   // split-sboot-8890.sh:1 的固件串 G930W8VLS6CSH1（机型码部分）
    lo.evidence = QStringLiteral("单源+实战报告");
    lo.sourceNote = QStringLiteral(
        "reference/exynos-usbdl/scripts/split-sboot-8890.sh:2-5（四行 dd，单源）；"
        "实战佐证：reference/exynos8890-exynos-usbdl-recovery/exynos-usbdl-recover.sh:87-93"
        "（按同序逐段发送）+ README.md:45 —— 该包直接引用同一脚本，**不构成独立第二源**（facts §C3）；"
        "sboot 修订与 sha1 出处：split-sboot-8890.sh:1");
    lo.sbootSha1 = QByteArray("9322ccb4e9b382b8cc67ff9ef989c459a763621f");
    lo.style = zeroStyle();   // exynos-usbdl 路径（facts §B4 头 00 00 00 00 / §B5 尾 00 00）
    lo.segments = {
        {QStringLiteral("fwbl1"),      0x0,     0x2000},
        {QStringLiteral("el3_mon"),    0x2000,  0x24000},
        {QStringLiteral("bl2"),        0x26000, 0x26D10},
        {QStringLiteral("bootloader"), 0x61000, 0xD1000},
    };
    return lo;
}

// spec §5.4 行 2。出处 reference/exynos-usbdl/scripts/split-sboot-8895.sh:2-7：
//   2: skip=0 bs=512 count=16 → 0x2000 @ 0x0
//   3: skip=16 bs=512 count=320 → 0x28000 @ 0x2000
//   4: skip=336 bs=512 count=384 → 0x30000 @ 0x2A000
//   5: `cp $1.1.bin $1.4.bin` → **再发一次第 1 段**（facts §C5 的重发段；作者自注抄自 astarasikov dltool.c）
//   6: skip=912 bs=512 count=1672 → 0xD1000 @ 0x72000
//   7: skip=2584 bs=512 count=1024 → 0x80000 @ 0x143000
// 第 5/6 段脚本未命名（facts §C6）—— 下表取名 part5/part6 仅占位，语义以偏移为准（facts §C2）。
EubLoadout make8895()
{
    EubLoadout lo;
    lo.soc = QStringLiteral("Exynos8895");
    lo.models = {QStringLiteral("G950F")};    // split-sboot-8895.sh:1 的固件串 G950FXXU1AQJ5
    lo.evidence = QStringLiteral("单源");
    lo.sourceNote = QStringLiteral(
        "reference/exynos-usbdl/scripts/split-sboot-8895.sh:2-7（单源；第 5 行 cp 即「重发 fwbl1」；"
        "第 5/6 段脚本未命名，本表取名 part5/part6 仅占位）；"
        "sboot 修订与 sha1 出处：split-sboot-8895.sh:1");
    lo.sbootSha1 = QByteArray("648a3e2c4de149250c575b4f14de096e147cc799");
    lo.style = zeroStyle();   // 同为 exynos-usbdl 路径
    lo.segments = {
        {QStringLiteral("fwbl1"),  0x0,     0x2000},
        {QStringLiteral("bl31"),   0x2000,  0x28000},
        {QStringLiteral("bl2"),    0x2A000, 0x30000},
        {QStringLiteral("fwbl1"),  0x0,     0x2000},   // 重发段（facts §C5）
        {QStringLiteral("part5"),  0x72000, 0xD1000},
        {QStringLiteral("part6"),  0x143000, 0x80000},
    };
    return lo;
}

// spec §5.4 行 3：**双源分歧**（facts §C4）。前两段与末段两源完全一致（仅命名不同），
// 分歧在第 3 段 bl2 的长度：hubble 表作 0x8000（32768，**恰好填到下一段起点 0x3A000**），
// ananjaser 脚本作 0x7D10（32016，其后留 752B 空隙）。**谁对未定** —— 本仓采信 hubble 的
// 连续切法（理由：与下一段连续，且 hubble 是仅有的给出完整 JSON 表的一方），另一源写进
// sourceNote 保留分歧记录（facts §F3：表项必须写明采信哪一源）。
// sbootSha1 **留空**：唯一记了修订的是**未被采信的那一源**（A510FXXS8CTI7 / 466852d1…，
// 其 bl2=0x7D10 正被本表的 0x8000 否决）；拿被否决源的修订当"本表所用修订"是**方向相反**的
// 保证 —— 用户文件若与它相符，UI 会报"sha1 一致：你的固件与该表所用修订相同"，而两表的切法
// 本身就不一致。hubble（本表采信方）未记录所用 sboot 修订，故本表**不记 sha1**（facts §C9），
// 分歧与另一源的修订只留在 sourceNote 里（比较两边前 8 位仍可人工核对）。
EubLoadout make7580()
{
    EubLoadout lo;
    lo.soc = QStringLiteral("Exynos7580");
    lo.models = {QStringLiteral("A510F")};    // split-sboot-7580.sh:1 的固件串 A510FXXS8CTI7
    lo.evidence = QStringLiteral("双源分歧（采信 hubble 连续切法）");
    lo.sourceNote = QStringLiteral(
        "reference/hubble/ExynosData/Exynos7580.json:4-23（采信：bl2 长度 0x8000 与下一段起点连续；"
        "该源未记录所用 sboot 修订，故本表 sbootSha1 留空）；"
        "另一源（未采信其 bl2 值）reference/exynos8890-exynos-usbdl-recovery/A510F/split-sboot-7580.sh:4 "
        "作 0x7D10（其后留 752B 空隙），其第 1 行记录修订 A510FXXS8CTI7 / sha1 "
        "466852d13fa02d51729d21633f47708308579f58 —— 该 sha1 属**另一源**，与本表采信的切法不构成"
        "同源对应，仅供人工比对；facts §C4 记录该分歧，谁对未定");
    lo.style = dnwStyle();    // hubble 路径（facts §B4 头 1B 44 4E 57 / §B5 尾 FF FF）
    lo.segments = {
        {QStringLiteral("fwbl1"),  0x0,    0x2000},
        {QStringLiteral("bl31"),   0x2000, 0x30000},
        {QStringLiteral("bl2"),    0x32000, 0x8000},
        {QStringLiteral("u-boot"), 0x3A000, 0xD1000},
    };
    return lo;
}

// spec §5.4 行 4。出处 reference/hubble/ExynosData/Exynos7885.json:5-28（单源，未对拍）。
EubLoadout make7885()
{
    EubLoadout lo;
    lo.soc = QStringLiteral("Exynos7885");
    lo.evidence = QStringLiteral("单源");
    lo.sourceNote = QStringLiteral(
        "reference/hubble/ExynosData/Exynos7885.json:5-28（bootloader_splits 五项，含 repeat-fwbl1；"
        "单源，未对拍，facts §C6 —— 该源未记录所用 sboot 修订，故 sha1 留空）");
    lo.style = dnwStyle();
    lo.segments = {
        {QStringLiteral("fwbl1"),  0x0,     0x2000},
        {QStringLiteral("bl31"),   0x2000,  0x25000},
        {QStringLiteral("bl2"),    0x27000, 0x2A000},
        {QStringLiteral("fwbl1"),  0x0,     0x2000},   // 重发段（facts §C5）
        {QStringLiteral("u-boot"), 0x61800, 0xD1000},
    };
    return lo;
}

// spec §5.4 行 5：**双源一致**（facts §C5）。两源 = hubble 表（ExynosData/Exynos9610.json）
// 与 astarasikov 救援包的 split_bootloader_a505.sh + dltool.c 的 files[] 顺序（part1 出现两次
// = 重发段）。第二源另有第 7 段 part6 0x21A000/0x101000 —— 本表**不采纳**（hubble 表无此项）。
EubLoadout make9610()
{
    EubLoadout lo;
    lo.soc = QStringLiteral("Exynos9610");
    lo.evidence = QStringLiteral("双源一致");
    lo.sourceNote = QStringLiteral(
        "reference/hubble/ExynosData/Exynos9610.json:5-34 ↔ "
        "reference/exynos9610-usb-emergency-recovery/split_bootloader_a505.sh:1-6 + "
        "reference/exynos9610-usb-emergency-recovery/dltool/dltool.c:311-319（双源一致，facts §C5）；"
        "该第二源另有 part6 0x21A000/0x101000，本表不采纳（hubble 表无对应项）");
    lo.style = dnwStyle();
    lo.segments = {
        {QStringLiteral("fwbl1"),  0x0,     0x2000},
        {QStringLiteral("epbl"),   0x2000,  0x13000},
        {QStringLiteral("bl2"),    0x15000, 0x2F000},
        {QStringLiteral("fwbl1"),  0x0,     0x2000},   // 重发段（facts §C5）
        {QStringLiteral("u-boot"), 0x5A000, 0x180000},
        {QStringLiteral("el3_mon"), 0x1DA000, 0x40000},
    };
    return lo;
}

// spec §5.4 行 6。出处 reference/hubble/ExynosData/Exynos9810.json:5-33（单源，未对拍）。
EubLoadout make9810()
{
    EubLoadout lo;
    lo.soc = QStringLiteral("Exynos9810");
    lo.evidence = QStringLiteral("单源");
    lo.sourceNote = QStringLiteral(
        "reference/hubble/ExynosData/Exynos9810.json:5-33（含 repeat-fwbl1；单源，未对拍，facts §C6）");
    lo.style = dnwStyle();
    lo.segments = {
        {QStringLiteral("fwbl1"),  0x0,     0x2000},
        {QStringLiteral("bl31"),   0x2000,  0x13000},
        {QStringLiteral("bl2"),    0x15000, 0x4F000},
        {QStringLiteral("fwbl1"),  0x0,     0x2000},   // 重发段（facts §C5）
        {QStringLiteral("u-boot"), 0x7D000, 0x180000},
        {QStringLiteral("el3_mon"), 0x1FD000, 0x40000},
    };
    return lo;
}

// spec §5.4 行 7。出处 reference/hubble/ExynosData/Exynos9820.json:5-28（单源，未对拍）。
EubLoadout make9820()
{
    EubLoadout lo;
    lo.soc = QStringLiteral("Exynos9820");
    lo.evidence = QStringLiteral("单源");
    lo.sourceNote = QStringLiteral(
        "reference/hubble/ExynosData/Exynos9820.json:5-28（单源，未对拍；response_support=true，facts §C7/§C8）");
    lo.style = dnwStyle();
    lo.segments = {
        {QStringLiteral("fwbl1"),  0x0,     0x3000},
        {QStringLiteral("epbl"),   0x3000,  0x13000},
        {QStringLiteral("bl2"),    0x16000, 0x52000},
        {QStringLiteral("u-boot"), 0xA4000, 0x180000},
        {QStringLiteral("el3_mon"), 0x224000, 0x40000},
    };
    lo.responseSupport = true;   // 设备会回显（facts §C7/§C8）
    return lo;
}

// spec §5.4 行 8。出处 reference/hubble/ExynosData/Exynos9830.json:2-28（单源，未对拍）。
// 该表是唯一带 extraFiles 的：段发完后另发 BL 包内的 ldfw.img / tzsw.img（facts §C7）。
EubLoadout make9830()
{
    EubLoadout lo;
    lo.soc = QStringLiteral("Exynos9830");
    lo.evidence = QStringLiteral("单源");
    lo.sourceNote = QStringLiteral(
        "reference/hubble/ExynosData/Exynos9830.json:2-28（单源，未对拍；"
        "files_to_send + response_support=true，facts §C7）；hubble.py:329-341 为发送逻辑");
    lo.style = dnwStyle();
    lo.segments = {
        {QStringLiteral("fwbl1"),  0x0,     0x3000},
        {QStringLiteral("epbl"),   0x3000,  0x13000},
        {QStringLiteral("bl2"),    0x16000, 0x6C000},
        {QStringLiteral("lk"),     0xDB000, 0x280000},
        {QStringLiteral("el3_mon"), 0x35B000, 0x40000},
    };
    lo.extraFiles = {QStringLiteral("ldfw.img"), QStringLiteral("tzsw.img")};
    lo.responseSupport = true;
    return lo;
}

} // namespace

QList<EubLoadout> allLoadouts()
{
    // 顺序 = spec §5.4 的表行序（测试钉住该顺序）。全表只有 41 个段，每次重建比静态缓存
    // 更简单，也没有静态初始化顺序问题。
    return {make8890(), make8895(), make7580(), make7885(),
            make9610(), make9810(), make9820(), make9830()};
}

bool eubLoadoutFor(const QString &socName, EubLoadout &out, QString *error)
{
    const QList<EubLoadout> all = allLoadouts();
    for (const EubLoadout &lo : all) {
        if (lo.soc.compare(socName, Qt::CaseInsensitive) == 0) {
            out = lo;
            return true;
        }
    }
    out = EubLoadout{};   // fail-closed：失败不留陈旧表项（调用方忽略返回值也拿不到旧表）
    if (error) {
        QStringList names;
        names.reserve(all.size());
        for (const EubLoadout &lo : all)
            names << lo.soc;
        *error = QStringLiteral("未知 SoC「%1」：该 SoC 无公开布局表（本工具不做猜测）。支持的 SoC：%2")
                     .arg(socName, names.join(QStringLiteral("、")));
    }
    return false;
}

bool splitSboot(const QByteArray &sboot, const EubLoadout &lo,
                QList<QPair<QString, QByteArray>> &out, QString *error)
{
    out.clear();
    // 第一遍：全表校验（**绝不截断** —— 任何一段越界都整体失败；spec §7）
    const quint64 size = quint64(sboot.size());
    quint64 needed = 0;        // 表所需的最小尺寸 = max(offset + length)
    bool fits = true;
    for (const EubSegment &s : lo.segments) {
        const quint64 end = s.offset + s.length;
        if (end > needed)
            needed = end;
        if (size < end)
            fits = false;
    }
    if (!fits) {
        if (error)
            *error = QStringLiteral("sboot.bin 太短：表（%1）需要 ≥ 0x%2 字节，你的文件是 0x%3 字节"
                                    "（不做截断，请换用与表匹配的固件修订）")
                         .arg(lo.soc, QString::number(needed, 16), QString::number(size, 16));
        return false;
    }
    // 第二遍：切段（校验已过，mid() 必然取满长度）
    out.reserve(lo.segments.size());
    for (const EubSegment &s : lo.segments)
        out.append({s.name, sboot.mid(qsizetype(s.offset), qsizetype(s.length))});
    return true;
}

QString detectSocFromImage(const QByteArray &sboot)
{
    // facts §A4（hubble.py:192-202）：b'EXYNOS[0-9]+' 的**首个**匹配，再 .title() 成 "Exynos9610"。
    // 照抄两处行为：①大小写敏感（参照只认大写 EXYNOS）；②取首个匹配（findall()[0]）。
    static const QRegularExpression re(QStringLiteral("EXYNOS[0-9]+"));
    const QRegularExpressionMatch m = re.match(QString::fromLatin1(sboot));
    if (!m.hasMatch())
        return QString();
    // 参照用 Python 的 .title()；对 "EXYNOS" + 数字而言，其结果等价于固定前缀 "Exynos" + 数字
    return QStringLiteral("Exynos") + m.captured(0).mid(6);
}

QString sha1Hex(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex());
}

} // namespace eub
