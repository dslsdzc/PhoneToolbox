#include "core/modes/mtk_payload.h"

#include <QFile>
#include <QHash>
#include <QThread>
#include <optional>
#include <utility>   // std::as_const（遍历 Qt 容器，不得用 qAsConst）

#include "core/modes/mtk_chip_table.h"     // lookupChip / ChipInfo / DaMode（代际判定）
#include "core/modes/mtk_gpt.h"            // mtkgpt::readTable / ReadFn（XFlash 分区表）
#include "core/modes/mtk_xflash_payload.h" // 七步握手 / bring-up / EMI / boot_to / 读写 / SHUTDOWN
#include "core/mtk_flash_plan.h"

namespace mtkbrom {
namespace {

// 读满 n 字节（走 IBromUsb::readExact：真机由传输层累加拆包，见 mtk_brom.h）。
// dst 可为 nullptr（只读走、不解析 —— 上游那些"读了就扔"的字段）。
bool readExactBytes(IBromUsb *u, int n, const QString &what, QByteArray *dst, QString *error)
{
    QByteArray b;
    if (!u->readExact(b, n, 3000, error)) {
        if (error && error->isEmpty())
            *error = QStringLiteral("%1 读取不足（要 %2 字节）").arg(what).arg(n);
        return false;
    }
    if (dst) *dst = b;
    return true;
}

quint16 be16At(const QByteArray &b, int off)
{
    return quint16((quint8(b.at(off)) << 8) | quint8(b.at(off + 1)));
}

quint32 be32At(const QByteArray &b, int off)
{
    return (quint32(quint8(b.at(off))) << 24) | (quint32(quint8(b.at(off + 1))) << 16)
         | (quint32(quint8(b.at(off + 2))) << 8) | quint32(quint8(b.at(off + 3)));
}

// 大端 4B 编码（**本文件唯一的实现**：sendStage2Config / sendEmiLegacy / bootToDa2Legacy 共用）
QByteArray be32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char((v >> 24) & 0xFF); b[1] = char((v >> 16) & 0xFF);
    b[2] = char((v >> 8) & 0xFF);  b[3] = char(v & 0xFF);
    return b;
}

// 读 4B 大端字段（EMI 各档位的 dramlength 共用 —— 逐档读的位置不同，但字段本身同构）
bool readBe32Field(IBromUsb *u, quint32 &out, const QString &what, QString *error)
{
    QByteArray b;
    if (!readExactBytes(u, 4, what, &b, error))
        return false;
    out = be32At(b, 0);
    return true;
}

constexpr quint8 kAck = 0x5A;        // Rsp.ACK（dalegacy_param.py:139）
constexpr quint8 kNack = 0xA5;       // Rsp.NACK（:140）

// XFlash 存储寻址口径（STORAGE_PARAM 的 storage/parttype，ST:16-49）：eMMC(0x1) 的 user 区(0x8)。
// 两者都是设备侧枚举值（不是本仓自造）：0x1 = eMMC、0x8 = user —— 传给 xflashStorageParam。
// （kXStorageEmmc / kXEmmcPartUser 已在 mtk_payload.h 定义 —— T11 审查 Minor 收敛为单一来源）

} // namespace

// ---- DA1 ----

// DA1：取代旧 sendPayload。地址/长度/签名长度**全部来自 region[1]**（三代共同硬编码，spec §2 P5）。
bool sendDa1(BromSession &s, const DaSelection &sel, QString *error)
{
    if (sel.da1Bytes.isEmpty()) {
        if (error) *error = QStringLiteral("DA1 载荷为空（region[1].m_len == 0 的条目应在选择阶段被跳过）");
        return false;
    }
    if (sel.da1Bytes.size() != int(sel.da1.len)) {
        if (error) *error = QStringLiteral("DA1 切片长度异常（%1 != m_len %2）")
                                .arg(sel.da1Bytes.size()).arg(sel.da1.len);
        return false;
    }
    if (!s.sendDa(sel.da1.startAddr, sel.da1.len, sel.da1.sigLen, sel.da1Bytes, error))
        return false;
    return s.jumpDa(sel.da1.startAddr, error);
}

// DA1 → DA2 的完整引导（顺序来自 dalegacy_lib.py:556-650，逐条见各函数注释）。
// **不含**枚举/打开 USB —— 故可用 mock 逐帧测；真机段（枚举/libusb/握手）在 runBromFlash。
bool bromBringUpDa(BromSession &s, const DaSelection &sel, quint16 hwCode,
                   quint8 bromVer, quint8 blVer, const PreloaderResult &pre,
                   QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };

    if (!sendDa1(s, sel, error))
        return false;
    say(QStringLiteral("DA1 已上传（地址 0x%1，%2 字节）")
            .arg(sel.da1.startAddr, 8, 16, QLatin1Char('0')).arg(sel.da1Bytes.size()));
    if (!waitDa1Ready(s, 10000, error))
        return false;                                        // 不是 0xC0 = 明确失败（DA2 一个字节都不发）
    say(QStringLiteral("DA1 就绪（0xC0）"));

    QString flashtype;
    if (!exchangeDa1StorageInfo(s, &flashtype, log, error))
        return false;

    bool emiNeeded = false;
    if (!sendStage2Config(s, hwCode, bromVer, blVer, flashtype, &emiNeeded, log, error))
        return false;

    if (emiNeeded) {
        if (!beginEmiDramInfo(s, nullptr, log, error))
            return false;
        if (pre.origin == PreloaderOrigin::None) {
            // 上游同姿态："Preloader needed due to dram config." —— 继续只会让 DA2 起不来
            if (error)
                *error = QStringLiteral("DA1 要求 DRAM 配置（errorcode = 0xBC3），但没有可用的 preloader/EMI —— "
                                        "中止（%1）").arg(pre.skipReason);
            return false;
        }
        // 注：pre.log 由调用方（bromFlashOnSession）落一次 —— 这里不重复落（T9 审查 M1）
        EmiData emi;
        QString emiError;
        if (!extractEmiLegacy(pre.bytes, emi, &emiError)) {
            if (error) *error = QStringLiteral("DA1 要求 DRAM 配置，但 EMI 提取失败：%1").arg(emiError);
            return false;
        }
        if (!sendEmiLegacy(s, emi, hwCode, error))
            return false;                                   // 版本被拒/DRAM 初始化失败 = 明确失败（上游同）
        say(QStringLiteral("DRAM 初始化完成（EMI 版本 0x%1）").arg(emi.ver, 2, 16, QLatin1Char('0')));
    } else {
        say(QStringLiteral("DA1 报告不需要 DRAM 配置（errorcode = 0）—— 跳过 EMI"));
        if (pre.origin == PreloaderOrigin::None && !pre.skipReason.isEmpty())
            say(pre.skipReason);                            // 信息级：不需要 DRAM 配置，缺 preloader 无妨
    }

    if (!bootToDa2Legacy(s, sel, error))
        return false;
    say(QStringLiteral("DA2 已上传（%1 字节，含尾部签名）").arg(sel.da2Bytes.size()));
    if (!readFlashInfoDa2(s, hwCode, log, error))
        return false;
    say(QStringLiteral("DA2 存活确认"));
    return true;
}

// DA1 起来后**只回单字节 0xC0**（dalegacy_lib.py:596-601）。LEGACY 没有 SYNC/SETUP_ENVIRONMENT/
// SETUP_HW_INIT_PARAMS —— 那些是 XFlash 的；这里只等这一个字节。
bool waitDa1Ready(BromSession &s, int timeoutMs, QString *error)
{
    QByteArray b;
    if (!s.usb()->read(b, 1, timeoutMs, error))
        return false;
    if (b.size() != 1 || quint8(b.at(0)) != 0xC0) {
        if (error)
            *error = QStringLiteral("DA1 就绪信号不是 0xC0（收到 %1 字节%2）")
                         .arg(b.size())
                         .arg(b.isEmpty() ? QString()
                                          : QStringLiteral("，首字节 0x%1")
                                                .arg(quint8(b.at(0)), 2, 16, QLatin1Char('0')));
        return false;
    }
    return true;
}

// 存储信息交换（dalegacy_lib.py:607-633 逐句）：
//   读 4B NAND_INFO(期望 0xBC4，只记日志) → 2B id 数 → id数×2B → 4B EMMC_INFO → 4×4B → 写 1B ACK → 读 3×1B
// 顺带按上游规则（:623-628）定存储类型：nandids[0] != 0 → nand；否则 emmcids[0] != 0 → emmc；否则 nor。
// 不做这一步，DA1 不会进入 stage2 配置（后面所有步骤都会错位）。
bool exchangeDa1StorageInfo(BromSession &s, QString *flashtype, QStringList *log, QString *error)
{
    IBromUsb *u = s.usb();
    QByteArray nandInfo;
    if (!readExactBytes(u, 4, QStringLiteral("DA1 存储信息：NAND_INFO"), &nandInfo, error))
        return false;
    QByteArray idsRaw;
    if (!readExactBytes(u, 2, QStringLiteral("DA1 存储信息：NAND id 数量"), &idsRaw, error))
        return false;
    const quint16 ids = be16At(idsRaw, 0);
    QByteArray nandIds;
    if (ids > 0 && !readExactBytes(u, int(ids) * 2, QStringLiteral("DA1 存储信息：NAND id 表"), &nandIds, error))
        return false;
    QByteArray emmcInfo;
    if (!readExactBytes(u, 4, QStringLiteral("DA1 存储信息：EMMC_INFO"), &emmcInfo, error))
        return false;
    QByteArray emmcIds;
    if (!readExactBytes(u, 16, QStringLiteral("DA1 存储信息：EMMC id 表(4×4B)"), &emmcIds, error))
        return false;
    if (!u->write(QByteArray(1, char(kAck)), error))             // usbwrite(ACK)
        return false;
    if (!readExactBytes(u, 3, QStringLiteral("DA1 存储信息：装置确认字节(3×1B)"), nullptr, error))
        return false;

    const bool hasNand = !nandIds.isEmpty() && be16At(nandIds, 0) != 0;
    const bool hasEmmc = be32At(emmcIds, 0) != 0;
    const QString type = hasNand ? QStringLiteral("nand")
                                 : (hasEmmc ? QStringLiteral("emmc") : QStringLiteral("nor"));
    if (flashtype) *flashtype = type;
    if (log)
        *log << QStringLiteral("DA1 存储信息：NAND_INFO=0x%1，NAND id 数=%2，EMMC_INFO=0x%3 → 存储类型 %4")
                    .arg(be32At(nandInfo, 0), 8, 16, QLatin1Char('0'))
                    .arg(ids)
                    .arg(be32At(emmcInfo, 0), 8, 16, QLatin1Char('0'))
                    .arg(type);
    return true;
}

// bmtflag / bmtpartsize（上游 mtk_config.py:231-283 bmtsettings(hwcode) 的 **eMMC** 分支）。
// **D1 只做 eMMC**：上游那些 nand 分支不实现（D1 的存储路径只有 eMMC —— PMT/分区表），
// flashtype != "emmc" 由调用方拒绝。blockcount 上游只用于内部记录、**不发给设备** → 不上线。
//   默认                     → flag 1 / partSize 0
//   [0x6592,0x8127,0x6571]   → partSize 0x1500000（:235-239；0x8127 也在 :240 的列表里，
//                              但 elif 链先命中 :235 → **先命中者胜**）
//   [0x6575]                 → partSize 0x1500000（:253-260）
//   [0x6582]                 → flag 2 + partSize 0x1500000（:261-265，**两个字段都要**）
//   [0x6572]                 → flag 0 + partSize 0xA8（:266-273）
// 其余（含 :240-243 那批不看 flashtype 的芯片）→ 默认值。
namespace {
struct BmtSettings { quint8 flag = 1; quint32 partSize = 0; };
BmtSettings bmtSettings(quint16 hwCode)
{
    BmtSettings s;                                   // 上游默认：bmtflag = 1、bmtpartsize = 0
    switch (hwCode) {
    case 0x6592:
    case 0x8127:
    case 0x6571:
    case 0x6575:
        s.partSize = 0x1500000;
        break;
    case 0x6582:
        s.flag = 2;
        s.partSize = 0x1500000;
        break;
    case 0x6572:
        // ⚠️ 上游这里疑似把 blockcount(0xA8) 写进了 partsize（它的 nand 分支才是 0x1500000）。
        // **忠实复刻**：真机收到什么我们就发什么，不"替上游修正"（修正 = 与上游发出不同的字节）。
        s.flag = 0;
        s.partSize = 0xA8;
        break;
    default:
        break;
    }
    return s;
}
} // namespace

// stage2 配置写入 + errorcode 握手（dalegacy_lib.py:228-403 逐句）。
// ⚠️ 顺序/取值必须与上游逐字节一致（真机不接受就整条链失败）：
//   B bromver → B blver → >H 0x0008 → B 0x00 → >I 0x7007FFFF → B bmtflag → >I bmtpartsize
//   → B 0x01(force_charge) → B resetkeys(1；0x6583 为 0) → B 0x02(ext_clock) → B 0x00(msdc_boot_ch)
//   → 按 hwcode 追加（:256-278）：0x6592 写 >I 0；{0x6580,0x8163,0x8127} 写 [0x8127 先 >I 0]
//     + >I 0x1 + 20B 常量；{0x6583,0x6589} 写 >I forcedram(0/1)；0x6582 写 >I 1
//     （上游 :273 的 `elif hwcode == 0x8127` 分支在 :258 已被捕获 → **不可达**，本实现同序）
//   → sleep(0.350) → 读 4B errorcode（大端）
//     0x0   → *emiNeeded = false（不需要 DRAM 配置，本步正常结束；0x6592 另读 5×4B）
//     0xBC3 → *emiNeeded = true（调用方再接 beginEmiDramInfo + sendEmiLegacy）
//     其它  → 失败（上游 eh.status(errorcode) 报错）
bool sendStage2Config(BromSession &s, quint16 hwCode, quint8 bromVer, quint8 blVer,
                      const QString &flashtype, bool *emiNeeded, QStringList *log, QString *error)
{
    if (emiNeeded) *emiNeeded = false;
    if (flashtype != QStringLiteral("emmc")) {
        if (error) *error = QStringLiteral("存储类型为 %1 —— Phase D1 只支持 eMMC（NAND/NOR 的 BMT 分支未实现）")
                                .arg(flashtype);
        return false;
    }
    IBromUsb *u = s.usb();
    const BmtSettings bmt = bmtSettings(hwCode);

    if (!u->write(QByteArray(1, char(bromVer)), error))
        return false;
    if (!u->write(QByteArray(1, char(blVer)), error))
        return false;
    if (!u->write(QByteArray("\x00\x08", 2), error))             // m_nor_chip = 0x08
        return false;
    if (!u->write(QByteArray(1, '\x00'), error))                 // m_nor_chip_select = CS_0
        return false;
    if (!u->write(be32(0x7007FFFF), error))                      // m_nand_acccon
        return false;
    if (!u->write(QByteArray(1, char(bmt.flag)), error))
        return false;
    if (!u->write(be32(bmt.partSize), error))
        return false;
    if (!u->write(QByteArray(1, '\x01'), error))                 // force_charge = 1
        return false;
    if (!u->write(QByteArray(1, char(hwCode == 0x6583 ? 0 : 1)), error))   // resetkeys
        return false;
    if (!u->write(QByteArray(1, '\x02'), error))                 // ext_clock = EXT_26M
        return false;
    if (!u->write(QByteArray(1, '\x00'), error))                 // msdc_boot_ch = 0
        return false;

    if (hwCode == 0x6592) {
        if (!u->write(be32(0), error))                           // is_gpt_solution = 0
            return false;
    } else if (hwCode == 0x6580 || hwCode == 0x8163 || hwCode == 0x8127) {
        if (hwCode == 0x8127 && !u->write(be32(0), error))       // is_gpt_solution（仅 0x8127）
            return false;
        if (!u->write(be32(1), error))                           // slc_percent = 1
            return false;
        // 上游 :264 的常量 = **20 字节**（4646 + 00×14 + ff000000）
        if (!u->write(QByteArray::fromHex("46460000000000000000000000000000ff000000"), error))
            return false;
    } else if (hwCode == 0x6583 || hwCode == 0x6589) {
        if (!u->write(be32(hwCode == 0x6589 ? 1u : 0u), error))  // forcedram
            return false;
    } else if (hwCode == 0x6582) {
        if (!u->write(be32(1), error))                           // newcombo = 1
            return false;
    }

    QThread::msleep(350);                                        // 上游 time.sleep(0.350)
    quint32 errorcode = 0;
    if (!readBe32Field(u, errorcode, QStringLiteral("stage2 errorcode"), error))
        return false;
    if (errorcode == 0x0) {
        if (hwCode == 0x6592)                                    // 上游特例：另读 5×4B（:286-291）
            if (!readExactBytes(u, 20, QStringLiteral("stage2（0x6592 特例 5×4B）"), nullptr, error))
                return false;
        if (log)
            *log << QStringLiteral("stage2 配置完成：不需要 DRAM 配置（errorcode = 0）");
        return true;
    }
    if (errorcode == 0xBC3) {
        if (emiNeeded) *emiNeeded = true;
        if (log)
            *log << QStringLiteral("stage2：DA1 要求 DRAM 配置（errorcode = 0xBC3）");
        return true;
    }
    if (error) *error = QStringLiteral("stage2 配置被拒绝：errorcode = 0x%1")
                            .arg(errorcode, 8, 16, QLatin1Char('0'));
    return false;
}

// errorcode == 0xBC3 之后的读取（dalegacy_lib.py:295-327 逐句）：
//   读 4B（丢弃）→ 读 16B draminfo（**大端**；上游还会做 4 字节组反转的第二种排列用于本地 preloader 匹配）
//   → 读 4B **必须 == 0xBC4** → 读 2B nand_id_count → count×2B。
// draminfo 交回调用方 —— **仅用于日志**（D1 不做"按 draminfo 在固件目录里找 preloader"的自动匹配：
// 那是上游在 EMI 未定时的补救路径，我们把"EMI 必须显式给出"作为诚实边界）。
bool beginEmiDramInfo(BromSession &s, QByteArray *dramInfo, QStringList *log, QString *error)
{
    IBromUsb *u = s.usb();
    if (!readExactBytes(u, 4, QStringLiteral("dram 配置前的 4B"), nullptr, error))
        return false;
    QByteArray info;
    if (!readExactBytes(u, 16, QStringLiteral("draminfo(16B)"), &info, error))
        return false;
    if (dramInfo) *dramInfo = info;
    quint32 retval = 0;
    if (!readBe32Field(u, retval, QStringLiteral("dram read 回执"), error))
        return false;
    if (retval != 0xBC4) {
        if (error) *error = QStringLiteral("dram read 回执不是 0xBC4（收到 0x%1）")
                                .arg(retval, 8, 16, QLatin1Char('0'));
        return false;
    }
    QByteArray cnt;
    if (!readExactBytes(u, 2, QStringLiteral("dram nand id 数量"), &cnt, error))
        return false;
    const quint16 nandIdCount = be16At(cnt, 0);
    if (nandIdCount > 0 && !readExactBytes(u, int(nandIdCount) * 2,
                                           QStringLiteral("dram nand id 表"), nullptr, error))
        return false;
    if (log)
        *log << QStringLiteral("DRAM info（16B，仅供参考）：%1")
                    .arg(QString::fromLatin1(info.toHex(' ')));
    return true;
}

// DA2 存活判据（dalegacy_lib.py:526-552 逐句；调用点 :640）。
// 上游把这 200+ 字节全读走再解析 —— **必须读走**，否则残留字节会污染随后的 PMT 读取（错位）。
// 本实现只解析 PassInfo（存活判据），NOR/NAND/EMMC 详情只记字节数（存储详情解析属 D2/D3）。
// ⚠️ nandcount 两级都为 0 时上游走 `usbread(-4)` —— **读 0 字节**（`usblib.py:462-483`：
//    `resplen <= 0` 只 warning，`bytestoread = resplen` 让 `while bytestoread > 0` 直接不成立）。
//    这里必须**同上游一样什么都不读**（审查 S1：nandcount == 0 是 eMMC 机器的常态，
//    多读一包就会把 info2/EMMC/SDC/flashconfig/PassInfo 一起吃掉 → 后续全错位）。
bool readFlashInfoDa2(BromSession &s, quint16 hwCode, QStringList *log, QString *error)
{
    IBromUsb *u = s.usb();
    if (!readExactBytes(u, 0x1C, QStringLiteral("read_flash_info：NOR info"), nullptr, error))
        return false;
    QByteArray nand;
    if (!readExactBytes(u, 0x11, QStringLiteral("read_flash_info：NAND info"), &nand, error))
        return false;

    // Legacy_NandInfo64（dalegacy_flash_param.py:69-78）：dword(4)+bytes(1)+short(2)+qword(8)+short(2)
    //   → m_nand_flash_id_count 在偏移 15
    quint16 nandcount = be16At(nand, 15);
    if (nandcount == 0) {
        // Legacy_NandInfo32（:170-179）：dword(4)+bytes(1)+short(2)+dword(4)+short(2) → count 在偏移 11
        nandcount = be16At(nand, 11);
        if (nandcount > 2) {
            // 上游 :534 `nc = data[-4:] + self.usbread(nandcount * 2 - 4)` —— 复用 NAND info 尾部 4B
            if (!readExactBytes(u, nandcount * 2 - 4,
                                QStringLiteral("read_flash_info：NAND id 表"), nullptr, error)) {
                return false;
            }
        } else if (log) {
            // nandcount <= 2 → 上游 `usbread(nandcount*2-4)`（0 或**负数**）**读 0 字节**（见函数头注释）
            *log << QStringLiteral("read_flash_info：NAND id 计数 %1（上游 usbread(%2) ≤ 0 → 读 0 字节）")
                        .arg(nandcount).arg(int(nandcount) * 2 - 4);
        }
    } else if (!readExactBytes(u, nandcount * 2,
                               QStringLiteral("read_flash_info：NAND id 表"), nullptr, error)) {
        return false;
    }

    if (!readExactBytes(u, 9, QStringLiteral("read_flash_info：NAND info2"), nullptr, error))
        return false;
    if (!readExactBytes(u, 0x5C, QStringLiteral("read_flash_info：EMMC info"), nullptr, error))
        return false;
    if (!readExactBytes(u, 0x1C, QStringLiteral("read_flash_info：SDC info"), nullptr, error))
        return false;
    if (!readExactBytes(u, 0x26, QStringLiteral("read_flash_info：flashconfig"), nullptr, error))
        return false;
    // hwcode ∈ {0x8127, 0x8163} 时上游另读 4B（dalegacy_lib.py:543-545）
    if (hwCode == 0x8127 || hwCode == 0x8163) {
        if (!readExactBytes(u, 4, QStringLiteral("read_flash_info：hwcode 附加 4B"), nullptr, error))
            return false;
    }

    // PassInfo（dalegacy_lib.py:28-39，共 0xA）：ack(1B) + m_download_status(4B BE)
    //   + m_boot_style(4B BE) + soc_ok(1B)
    QByteArray pass;
    if (!readExactBytes(u, 0xA, QStringLiteral("read_flash_info：PassInfo"), &pass, error))
        return false;
    const quint8 ack = quint8(pass.at(0));
    const quint32 dlStatus = be32At(pass, 1);
    if (log)
        *log << QStringLiteral("DA2 存活判据：PassInfo ack=0x%1，download_status=0x%2，boot_style=0x%3")
                    .arg(ack, 2, 16, QLatin1Char('0'))
                    .arg(dlStatus, 8, 16, QLatin1Char('0'))
                    .arg(be32At(pass, 5), 8, 16, QLatin1Char('0'));
    if (ack == 0x5A)
        return true;                                             // 上游第一分支（:547-548）
    if ((dlStatus & 0xFF) == 0x5A) {                             // 上游第二分支（:549-551）：再读 1B
        if (!readExactBytes(u, 1, QStringLiteral("read_flash_info：状态补充字节"), nullptr, error))
            return false;
        return true;
    }
    if (error)
        *error = QStringLiteral("DA2 未就绪：PassInfo ack=0x%1 且 download_status=0x%2（都不含 0x5A）")
                     .arg(ack, 2, 16, QLatin1Char('0'))
                     .arg(dlStatus, 8, 16, QLatin1Char('0'));
    return false;
}

// LEGACY EMI/DRAM 初始化（dalegacy_lib.py:333-398 逐句对应；地址/长度全**大端**）。
bool sendEmiLegacy(BromSession &s, const EmiData &emi, quint16 hwCode, QString *error)
{
    if (emi.bytes.isEmpty()) {
        if (error) *error = QStringLiteral("EMI 数据为空");
        return false;
    }
    IBromUsb *u = s.usb();

    if (!u->write(QByteArray(1, char(0xE8)), error))                        // ENABLE_DRAM（:334）
        return false;
    if (!u->write(be32(emi.ver == 0 ? 0xFFFFFFFFu : emi.ver), error))       // emiver==0 → 0xFFFFFFFF
        return false;
    QByteArray ret;
    if (!readExactBytes(u, 1, QStringLiteral("EMI 版本应答"), &ret, error))
        return false;
    if (quint8(ret.at(0)) == kNack) {
        if (error) *error = QStringLiteral("设备拒绝 EMI 配置（NACK）—— preloader 可能不匹配");
        return false;
    }
    if (quint8(ret.at(0)) != kAck) {
        if (error) *error = QStringLiteral("EMI 阶段期望 ACK，收到 0x%1")
                                .arg(quint8(ret.at(0)), 2, 16, QLatin1Char('0'));
        return false;
    }

    // ---- 逐档差异（dalegacy_lib.py:345-371）----
    // ⚠️ **读 dramlength 的位置逐档不同**，不是统一在分档之前：
    //   档 A [0xF,0x10,0x11,0x14,0x15]：先读 4B dramlength → 写 ACK → （非 0x8127）写 >I lendram
    //   档 B [0x0A,0x0B]：**先读 0x10 字节 info** → 再读 4B dramlength → 写 ACK
    //   档 C [0x0C,0x0D]：读 4B dramlength → 写 ACK → 改写 EMI 本体（>I 0x100 + emi[4:dramlength]）
    //   档 D [0x00]：读 4B dramlength → 写 ACK → 截断 EMI → 写 >I dramlength
    //   其它档：上游只 warning，**不读不写**（EMI 原样发出）
    QByteArray emiBytes = emi.bytes;
    quint32 dramlength = 0;
    if (emi.ver == 0x0F || emi.ver == 0x10 || emi.ver == 0x11 || emi.ver == 0x14 || emi.ver == 0x15) {
        if (!readBe32Field(u, dramlength, QStringLiteral("EMI 档 A：RAM-Length"), error))
            return false;
        if (!u->write(QByteArray(1, char(kAck)), error))
            return false;
        if (hwCode != 0x8127)                                        // 上游对 0x8127 特例**不写** lendram（:350）
            if (!u->write(be32(quint32(emiBytes.size())), error))
                return false;                                        // lendram = len(emi)（未截断）
    } else if (emi.ver == 0x0A || emi.ver == 0x0B) {
        if (!readExactBytes(u, 0x10, QStringLiteral("EMI 档 B：RAM-Info(0x10B)"), nullptr, error))
            return false;
        if (!readBe32Field(u, dramlength, QStringLiteral("EMI 档 B：RAM-Length"), error))
            return false;
        if (!u->write(QByteArray(1, char(kAck)), error))
            return false;                                            // 本档**不写** dramlength（:353-356）
    } else if (emi.ver == 0x0C || emi.ver == 0x0D) {
        if (!readBe32Field(u, dramlength, QStringLiteral("EMI 档 C：RAM-Length"), error))
            return false;
        if (!u->write(QByteArray(1, char(kAck)), error))
            return false;
        if (dramlength > quint32(emiBytes.size())) {
            if (error) *error = QStringLiteral("EMI 长度不足（dramlength=%1 > %2）")
                                    .arg(dramlength).arg(emiBytes.size());
            return false;
        }
        // 上游 :361-362：先 emi[:dramlength]，再 >I 0x100 + emi[4:dramlength]（dramlength < 4 时后半为空）
        emiBytes = be32(0x100) + emiBytes.left(int(dramlength)).mid(4);
    } else if (emi.ver == 0x00) {
        if (!readBe32Field(u, dramlength, QStringLiteral("EMI 档 D：RAM-Length"), error))
            return false;
        if (!u->write(QByteArray(1, char(kAck)), error))
            return false;
        if (dramlength > quint32(emiBytes.size())) {
            if (error) *error = QStringLiteral("EMI 长度不足（dramlength=%1 > %2）")
                                    .arg(dramlength).arg(emiBytes.size());
            return false;
        }
        emiBytes = emiBytes.left(int(dramlength));                   // 上游 :367-369
        if (!u->write(be32(dramlength), error))
            return false;
    }
    // else：未知版本 → 上游 warning 后**继续**（不读不写、EMI 原样发出）

    if (!u->write(emiBytes, error))                                  // 发 EMI 本体（:372）
        return false;
    if (!readExactBytes(u, 2, QStringLiteral("EMI checksum"), nullptr, error))
        return false;                                                // checksum（>H）只记日志（上游同）
    if (!u->write(QByteArray(1, char(kAck)), error))
        return false;
    if (!u->write(be32(0x80000001), error))                          // "Send DRAM config"（:376）
        return false;
    quint32 mExtRamRet = 0;
    if (!readBe32Field(u, mExtRamRet, QStringLiteral("M_EXT_RAM_RET"), error))
        return false;
    if (mExtRamRet != 0) {
        if (error) *error = QStringLiteral("DRAM 初始化失败：M_EXT_RAM_RET = 0x%1")
                                .arg(mExtRamRet, 8, 16, QLatin1Char('0'));
        return false;
    }
    if (!readExactBytes(u, 1, QStringLiteral("M_EXT_RAM_TYPE"), nullptr, error)
        || !readExactBytes(u, 1, QStringLiteral("M_EXT_RAM_CHIP_SELECT"), nullptr, error)
        || !readExactBytes(u, 8, QStringLiteral("M_EXT_RAM_SIZE"), nullptr, error))
        return false;                                                // 三者都只记日志（上游同）
    if (emi.ver == 0x0D) {
        for (int i = 0; i < 5; ++i)                                  // 上游固定读 5×4B（Raw/CJ，:392-398）
            if (!readExactBytes(u, 4, QStringLiteral("EMI 档 D 附加字段"), nullptr, error))
                return false;
    }
    return true;
}

// DA2：`>I addr` → `>I size` → `>I packetsize(0x1000)` → 读 1B ACK → 分块写（每块读 1B ACK）→
//   sleep(0.5) → 写 ACK → 读 1B ACK（dalegacy_lib.py:907-940 的 brom_send(…, stage=2)）。
// ⚠️ **LEGACY 保留尾部签名**：发送的是 region[2] 的完整 m_len 字节（不裁 sigLen）——
//   签名在 DA2 内部由它自己校验，裁了反而起不来。
bool bootToDa2Legacy(BromSession &s, const DaSelection &sel, QString *error)
{
    if (sel.da2Bytes.isEmpty()) {
        if (error) *error = QStringLiteral("DA2 载荷为空（region[2].m_len == 0）");
        return false;
    }
    IBromUsb *u = s.usb();
    const quint32 kPacketsize = 0x1000;
    if (!u->write(be32(sel.da2.startAddr), error))
        return false;
    if (!u->write(be32(quint32(sel.da2Bytes.size())), error))        // size = m_len（不是 m_len - sigLen）
        return false;
    if (!u->write(be32(kPacketsize), error))
        return false;
    QByteArray ack;
    if (!readExactBytes(u, 1, QStringLiteral("boot_to 头部 ACK"), &ack, error))
        return false;
    if (quint8(ack.at(0)) != kAck) {
        if (error) *error = QStringLiteral("boot_to 头部后未收到 ACK（收到 0x%1）")
                                .arg(quint8(ack.at(0)), 2, 16, QLatin1Char('0'));
        return false;
    }
    for (int off = 0; off < sel.da2Bytes.size(); off += int(kPacketsize)) {
        if (!u->write(sel.da2Bytes.mid(off, int(kPacketsize)), error))
            return false;
        if (!readExactBytes(u, 1, QStringLiteral("boot_to 数据块 ACK"), &ack, error))
            return false;
        if (quint8(ack.at(0)) != kAck) {
            if (error) *error = QStringLiteral("boot_to 第 %1 块后未收到 ACK（收到 0x%2）")
                                    .arg(off / int(kPacketsize))
                                    .arg(quint8(ack.at(0)), 2, 16, QLatin1Char('0'));
            return false;
        }
    }
    QThread::msleep(500);                                           // 上游 time.sleep(0.5)
    if (!u->write(QByteArray(1, char(kAck)), error))
        return false;
    if (!readExactBytes(u, 1, QStringLiteral("boot_to 收尾 ACK"), &ack, error))
        return false;
    if (quint8(ack.at(0)) != kAck) {
        if (error) *error = QStringLiteral("boot_to 收尾未收到 ACK（DA2 可能没起来）");
        return false;
    }
    return true;
}

bool patchPreloaderSecurity(QByteArray &preloader)
{
    // SBC 修补字节模式（GPLv3 子模块来源标注；规格 §2.3 SBC 位背景，§4.2 历史绕过路径）：
    // ram blacklist / seclib_sec_usbdl_enabled / Patched loader msg / sec_img_auth /
    // get_vfy_policy。模式为十六进制字节串，全文替换（可多次出现）。
    struct Patch { const char *hexFrom; const char *hexTo; const char *name; };
    static const Patch kPatches[] = {
        {"10B50C680268", "10B5012010BD", "ram blacklist"},
        {"08B5104B7B441B681B68", "00207047000000000000", "seclib_sec_usbdl_enabled"},
        {"5072656C6F61646572205374617274", "50617463686564204C205374617274", "Patched loader msg"},
        {"F0B58BB002AE20250C460746", "002070470000000000205374617274", "sec_img_auth"},
        {"FFC0F3400008BD", "FF4FF0000008BD", "get_vfy_policy"},
    };
    bool any = false;
    for (const Patch &p : kPatches) {
        const QByteArray from = QByteArray::fromHex(p.hexFrom);
        const QByteArray to = QByteArray::fromHex(p.hexTo);
        int idx = 0;
        while ((idx = preloader.indexOf(from, idx)) != -1) {
            preloader.replace(idx, from.size(), to);
            any = true;
        }
    }
    return any;
}

bool flashPartition(BromSession &s, DaStorage &st, const QString &name,
                    const QByteArray &image, QString *error)
{
    // listPartitions（规格 §3.6 分区表）查名 → emmcWrite（规格 §3.5 Legacy 命令集）
    QList<EmPartition> parts;
    if (!st.listPartitions(parts, error))
        return false;
    for (const EmPartition &p : parts) {
        if (p.name == name) {
            if (!st.emmcWrite(p.offsetBytes, image, kEmmcPartUser, error))
                return false;
            return true;
        }
    }
    if (error) *error = QStringLiteral("未找到分区 %1（分区表 %2 条）")
                            .arg(name).arg(parts.size());
    return false;
}

namespace {

// 设备实读分区表 → 计划层参照表（**写入判据以设备为准**；预览期的 scatter 到这里可能对不上）
QList<mtkplan::PartitionRef> toPartitionRefs(const QList<EmPartition> &parts)
{
    QList<mtkplan::PartitionRef> out;
    out.reserve(parts.size());
    for (const EmPartition &p : std::as_const(parts)) {
        mtkplan::PartitionRef r;
        r.name = p.name;
        r.sizeBytes = p.sizeBytes;
        out << r;
    }
    return out;
}

} // namespace

// ---- 三代路由（D2-T11）----

// 代际判定（纯函数）：优先级逐字照上游 daconfig.py:216 的 if/elif 序 ——
// `if da_is_v6: damode = DAmodes.XML` 在查 hwconfig 的 damode **之前**，故 v6 盖过表；
// 表外/IoT 在本实现里是**明确拒绝**（上游会把表外当成默认 DAmodes.LEGACY —— 那正是砖机路径）。
bool decideGeneration(const ChipInfo *chip, bool daIsV6, MtkGeneration &out, QString *error)
{
    if (!chip) {
        if (error) *error = QStringLiteral("芯片表未收录该 hw_code —— 判不出代际（不猜）");
        return false;
    }
    if (chip->iot) {
        if (error) *error = QStringLiteral("IoT 芯片：三代映射均未实现，明确拒绝");   // hw_code 由调用方包装层给出（避免文案重复值）
        return false;
    }
    if (daIsV6 || chip->damode == DaMode::Xml) {   // v6 **强制** XML（DC:216），优先于表
        out = MtkGeneration::Xml;
        return true;
    }
    out = (chip->damode == DaMode::XFlash) ? MtkGeneration::XFlash : MtkGeneration::Legacy;
    return true;
}

QString generationName(MtkGeneration g)
{
    switch (g) {
    case MtkGeneration::Legacy: return QStringLiteral("LEGACY");
    case MtkGeneration::XFlash: return QStringLiteral("XFLASH");
    case MtkGeneration::Xml:    return QStringLiteral("XML");
    }
    return QStringLiteral("?");
}

// GPT 读回调适配器（mtkgpt::ReadFn 的字节偏移 → XFlash READ_DATA 的 (addr, length)）
mtkgpt::ReadFn xflashSectorReader(XFlashSession &x, quint32 storage, quint32 partType)
{
    return [&x, storage, partType](quint64 off, int len, QByteArray *out, QString *e) {
        return xflashReadData(x, off, quint32(len), storage, partType, *out, e);
    };
}

// XFlash 引导链（串起 T4-T6；注释与判据见头文件）
bool xflashBringUpDa(BromSession &brom, XFlashSession &x, const DaSelection &sel,
                     const PreloaderResult &pre, QStringList *log, QString *error)
{
    auto say = [log](const QString &m) { if (log) *log << m; };

    // ① DA1 上传与 0xC0（与 LEGACY 共用 BROM 级帧，XFL:979-985）
    if (!sendDa1(brom, sel, error))
        return false;
    if (!waitDa1Ready(brom, 10000, error))
        return false;
    say(QStringLiteral("XFlash：DA1 已上传并就绪（0xC0）"));

    // ② 七步握手 + 四步 bring-up（T4）
    if (!xflashDa1Handshake(x, log, error))
        return false;
    QByteArray agent;
    if (!xflashBringUpSteps(x, &agent, log, error))
        return false;

    // ③ EMI 判定（铁律 9）：preloader=跳过；brom=需要（缺 EMI 只告警）；其它=失败
    if (agent == QByteArray("preloader")) {
        say(QStringLiteral("XFlash：connection_agent = preloader（设备已初始化过 DRAM）→ 跳过 EMI"));
    } else if (agent == QByteArray("brom")) {
        bool emiSent = false;
        if (pre.origin != PreloaderOrigin::None) {
            EmiData emi;
            QString emiErr;
            if (!extractEmiXflash(pre.bytes, emi, &emiErr)) {
                say(QStringLiteral("XFlash：EMI 提取失败（%1）—— 跳过 DRAM 初始化（DA2 可能起不来）").arg(emiErr));
            } else if (!xflashSendEmi(x, emi.bytes, error)) {
                return false;                        // 发失败 = 真失败（与 LEGACY 的 0xBC3 同姿态）
            } else {
                emiSent = true;
                say(QStringLiteral("XFlash：DRAM 初始化完成（EMI %1 字节，版本 %2）")
                        .arg(emi.bytes.size()).arg(emi.ver));
            }
        } else {
            // 注：pre.log **不在这里落** —— 调用方（bromFlashOnSession）已经落过一次（D1 T9 审查 M1 的不变量，
            //     与 bromBringUpDa 同姿态）。T11 审查抓到本分支曾重复落 → LEGACY 一次、XFLASH 两次。
            say(QStringLiteral("XFlash：无 preloader —— 未做 DRAM 初始化，操作可能失败（上游同姿态）：%1")
                    .arg(pre.skipReason));
        }
        if (!emiSent)
            say(QStringLiteral("XFlash：EMI 未发送（见上）"));
    } else {
        if (error) *error = QStringLiteral("XFlash：connection_agent 非 brom/preloader（收到 %1）—— 失败")
                                .arg(QString::fromLatin1(agent));
        return false;
    }

    // ④ DA2：**剥尾部签名**（XFL:970-978；与 LEGACY 相反）
    if (sel.da2.sigLen >= sel.da2Bytes.size()) {
        if (error) *error = QStringLiteral("XFlash：DA2 切片 %1 字节不足以剥掉签名 %2 字节")
                                .arg(sel.da2Bytes.size()).arg(sel.da2.sigLen);
        return false;
    }
    const QByteArray da2NoSig = sel.da2Bytes.left(sel.da2Bytes.size() - int(sel.da2.sigLen));
    if (!xflashBootTo(x, sel.da2.startAddr, da2NoSig, error))
        return false;
    say(QStringLiteral("XFlash：DA2 已上传（%1 字节，**已剥签名 %2 字节**）")
            .arg(da2NoSig.size()).arg(sel.da2.sigLen));
    return true;
}

// 刷写主体（T9 审查 I1 从 runBromFlash 抽出；**D2-T11 起改为三代路由** —— 选完 DA 之后
// 按 decideGeneration 分派到 LEGACY / XFlash 两条链，XML 代逐段明确拒绝）。
// 只把"枚举/打开/握手"留在 mtk_handler.cpp 的 runBromFlash 里，从而让本函数可离线逐帧测。
bool bromFlashOnSession(BromSession &session, const BromFlashRequest &req,
                        const BromLogFn &log, const BromProgressFn &progress, QString *error)
{
    auto say = [&log](const QString &m) { if (log) log(m, false); };
    auto warn = [&log](const QString &m) { if (log) log(m, true); };

    // 请求级前置（放进本函数而不是 runBromFlash：本函数才是可测入口，直接调用时同样自洽；
    // 代价是 runBromFlash 会先开 USB 再拒空请求 —— 生产入口 flash_tool 侧已先行校验参数）
    if (req.daFile.isEmpty()) {
        if (error) *error = QStringLiteral("DA 文件为空");
        return false;
    }
    if (req.imagePaths.isEmpty()) {
        if (error) *error = QStringLiteral("未选择任何镜像文件（mtk-brom 通道）");
        return false;
    }

    TargetConfig cfg;
    if (session.getTargetConfig(cfg, nullptr) && (cfg.sla || cfg.daa)) {
        if (error) *error = QStringLiteral("设备启用 SLA/DAA 认证，暂不支持（RSA 响应自研为后续任务）");
        return false;
    }

    // hwcode / 版本 → DA 条目选择（5 元组；判不出 → 明确报错，不猜）
    // ⚠️ 版本口径（T4 实施期核上游 `mtk_preloader.py:174-232`）：
    //   • 0xFD 的**低 16 位 hwver** 只用于"关看门狗"等前置动作（`setreg_disablewatchdogtimer(hwcode, hwver)`）
    //     —— 本实现不做那些前置动作，故 hwVer 只读出并落日志（**不参与判定**）；
    //   • 非 IoT 芯片的 `hwver/swver` **以 0xFC 为准**（上游在 0xFC 段把两者**清零后重填**：
    //     `hwver = 0; swver = 0; if res != -1: hw_sub_code = res[0]; hwver = res[1]; swver = res[2]`）；
    //   • 0xFC 失败（-1）时上游**不清零就往下走** → hwver/swver 保持 0 → 版本过滤维**旁路**（`or … == 0`）
    //     → 取首个候选。故这里**不因 0xFC 失败而中止**：记告警 + 用 0/0（与上游同姿态；IoT 读 A2 寄存器的
    //     另一条路我们已在芯片表 iot 位处拒绝）。
    quint16 hwCode = 0, hwVer = 0;
    if (!session.getHwCode(hwCode, hwVer, error))
        return false;
    HwSwVer sw;
    if (!session.getHwSwVer(sw, error)) {
        warn(QStringLiteral("读取 0xFC（hw/sw 版本）失败：%1 —— 版本过滤维按上游口径旁路（hwver/swver = 0）")
                 .arg(error ? *error : QString()));
        if (error) error->clear();
        sw = HwSwVer{};
    }
    quint8 bromVer = 0;
    quint8 blVer = 0;
    if (!session.getBromVer(bromVer, error))          // stage2 配置要写这两个值（顺序敏感）
        return false;
    if (!session.getBlVer(blVer, error))
        return false;
    say(QStringLiteral("芯片 hw_code=0x%1（hw_ver：0xFD=0x%2 / 0xFC=0x%3；sw_ver=0x%4；BROM 0x%5 / BL 0x%6）")
            .arg(hwCode, 4, 16, QLatin1Char('0'))
            .arg(hwVer, 4, 16, QLatin1Char('0'))       // 0xFD 的 hwver：只落日志（见上）
            .arg(sw.hwVer, 4, 16, QLatin1Char('0'))
            .arg(sw.swVer, 4, 16, QLatin1Char('0'))
            .arg(bromVer, 2, 16, QLatin1Char('0')).arg(blVer, 2, 16, QLatin1Char('0')));

    // 代际判定（spec §4）：表外 / IoT → 明确拒绝；v6 强制 XML（DC:216，优先于芯片表）
    const ChipInfo *chip = lookupChip(hwCode);
    DaFile daFile;
    if (!parseDaFile(req.daFile, daFile, error))     // **提前**到判定之前：v6 是判定的输入之一
        return false;
    MtkGeneration gen = MtkGeneration::Legacy;
    if (!decideGeneration(chip, daFile.isV6, gen, error)) {
        // T11 审查 Minor：`decideGeneration` 拿不到 hw_code（它只收 chip），由**调用方**补上值 + 补救提示
        // （恢复 D1 该错误的可诊断性；模式同 D1 的"DA 条目选择失败（%1）：%2"）。
        const QString why = error ? *error : QString();
        if (error) *error = QStringLiteral("代际判定失败（hw_code=0x%1）：%2")
                                .arg(hwCode, 4, 16, QLatin1Char('0')).arg(why);
        return false;
    }
    say(QStringLiteral("代际判定：%1（chip_dacode=0x%2；DA %3）")
            .arg(generationName(gen)).arg(chip->dacode, 4, 16, QLatin1Char('0'))
            .arg(daFile.isV6 ? QStringLiteral("v6") : QStringLiteral("非 v6")));
    DaSelection sel;
    QStringList selWarn;
    // ⚠️ 查找键是 **chip->dacode**，不是设备报的 hwCode（两者在 31/89 个表项上不同）。
    // 上游 `daconfig.py:208-209`：`dacode = self.config.chipconfig.dacode; if dacode in self.dasetup`，
    // 而 `dasetup` 是按**条目自己的** `da.hw_code` 建的（`:190/:192/:201`）→ 键 = dacode。
    // 本仓同口径：`mtk_chip_table.h:29-30`、`mtk_da_file.h` 的 selectDaEntry 参数名。
    // 反例（修前）：设备 0x0321（dacode 0x6735）会拿 0x0321 去查 → 无候选 → 该类机型刷不了。
    if (!selectDaEntry(daFile, chip->dacode, sw.hwVer, sw.swVer, &selWarn, sel, error)) {
        const QString why = error ? *error : QString();            // 先取值再改写（别自引用）
        if (error) *error = QStringLiteral("DA 条目选择失败（%1）：%2").arg(req.daLabel, why);
        return false;
    }
    for (const QString &w : std::as_const(selWarn))
        warn(w);
    say(QStringLiteral("DA 条目：DA1 %1 字节 @0x%2；DA2 %3 字节 @0x%4")
            .arg(sel.da1Bytes.size()).arg(sel.da1.startAddr, 8, 16, QLatin1Char('0'))
            .arg(sel.da2Bytes.size()).arg(sel.da2.startAddr, 8, 16, QLatin1Char('0')));

    // preloader 两路径（显式 > 自动导入 > 网络(默认关) > 跳过）——**这里落一次日志**，
    // bromBringUpDa 不再重复落（T9 审查 M1：同一批 pre.log 曾出现两次）。
    // 注：**跳过 preloader 是否致命由 DA1 决定**（errorcode == 0xBC3 时必须要有）——
    // 判定在 bromBringUpDa 里，这里不预先告警（否则 errorcode==0 的设备会被无谓地吓一跳）。
    PreloaderResult pre;
    if (!resolvePreloader(req.preloader, req.preloaderSources, req.downloader, pre, error))
        return false;
    for (const QString &line : std::as_const(pre.log))
        say(line);

    QStringList bringLog;
    XFlashSession xflash(session.usb(), chip->dacode);   // 通道未打开也能构造（只存指针/码）
    bool brought = false;                                // （T12 会在这里加 XmlSession xml(session.usb());）
    switch (gen) {
    case MtkGeneration::Legacy:
        brought = bromBringUpDa(session, sel, hwCode, bromVer, blVer, pre, &bringLog, error);
        break;
    case MtkGeneration::XFlash:
        brought = xflashBringUpDa(session, xflash, sel, pre, &bringLog, error);
        break;
    case MtkGeneration::Xml:
        // T12 换成： brought = xmlBringUpDa(session, xml, sel, &bringLog, error);
        if (error) *error = QStringLiteral("XML 代：链在 Task 12 接线（当前明确拒绝，不假装支持）");
        break;
    }
    for (const QString &line : std::as_const(bringLog))
        say(line);                                       // **失败也落**：用户要看到卡在哪一步（审查 M4）
    if (!brought)
        return false;

    // 设备分区表（**写入判据以设备为准**；预览期的对照表到这里可能对不上）→ 计划参照表
    QList<mtkplan::PartitionRef> refs;
    QHash<QString, quint64> partAddr;                 // 分区名 → 写入地址（XFlash 用；LEGACY 不用）
    quint32 xWritePacketLength = 0;                   // XFlash 写分块（GET_PACKET_LENGTH）
    std::optional<DaStorage> storage;                 // LEGACY 专用（另两代不构造）

    if (gen == MtkGeneration::Legacy) {
        storage.emplace(session);
        storage->setDaActive(true);
        QList<EmPartition> deviceParts;
        if (!storage->listPartitions(deviceParts, error))
            return false;
        if (deviceParts.isEmpty()) {
            if (error) *error = QStringLiteral("设备分区表为空（read_pmt 无条目）—— 拒绝在未知分区表上写入");
            return false;
        }
        refs = toPartitionRefs(deviceParts);
    } else if (gen == MtkGeneration::XFlash) {
        XPacketLength pl;
        if (!xflashGetPacketLength(xflash, pl, error))     // 铁律 14：拿不到写分块就**不写**
            return false;
        xWritePacketLength = pl.writeLength;
        XChipId id;                                        // 只读诊断，失败只告警（不影响写入）
        if (xflashGetChipId(xflash, id, nullptr)) {
            say(QStringLiteral("XFlash chip id：hw_code=0x%1 hw_ver=0x%2 sw_ver=0x%3 evo=%4")
                    .arg(id.hwCode, 4, 16, QLatin1Char('0')).arg(id.hwVersion, 4, 16, QLatin1Char('0'))
                    .arg(id.swVersion, 4, 16, QLatin1Char('0')).arg(id.chipEvolution));
        } else {
            warn(QStringLiteral("XFlash：GET_CHIP_ID 失败（只读诊断，继续）"));
        }
        PartitionCata cata = PartitionCata::Unknown;
        if (!xflashGetPartitionCata(xflash, cata, error))
            return false;
        if (cata == PartitionCata::Pmt) {
            // 上游 PMT 分支**实际不可达**（事实报告 §7：`DL.partition_table_category()` 无条件回 "GPT"，
            // 且 `partition.py:80-81` 的 PMT 嗅探 int/bytes 比较是 bug）→ **不复刻**：明确拒绝，不猜读法
            if (error) *error = QStringLiteral("XFlash：设备分区表是 PMT（GET_PARTITION_TBL_CATA 回 0x65）——"
                                               "上游该分支不可达（事实报告 §7），本实现明确拒绝（不猜 PMT 读法）");
            return false;
        }
        if (cata != PartitionCata::Gpt) {
            if (error) *error = QStringLiteral("XFlash：分区表类型未知（GET_PARTITION_TBL_CATA 回 0x%1，"
                                               "既非 GPT(0x64) 也非 PMT(0x65)）—— 拒绝在未知表上写入")
                                    .arg(quint32(cata), 0, 16);
            return false;
        }
        mtkgpt::Table tbl;
        QStringList gptLog;
        // diskSectors = 0：XFlash 侧没有"磁盘总扇区数"的可靠来源 → 只走主 GPT（备份兜底不可用；
        // readTable 在需要时会把这一点写进日志/错误，不静默降级）
        if (!mtkgpt::readTable(xflashSectorReader(xflash, kXStorageEmmc, kXEmmcPartUser), 0, tbl, &gptLog, error))
            return false;
        for (const QString &line : std::as_const(gptLog))
            say(line);
        refs = mtkplan::toPartitionRefs(tbl.partitions, tbl.sectorSize);   // T7 的适配器（不重复实现）
        for (const mtkgpt::Partition &p : std::as_const(tbl.partitions))
            partAddr.insert(p.name, mtkgpt::offsetBytes(p, tbl.sectorSize));
        say(QStringLiteral("XFlash：GPT 读出 %1 个分区（扇区 %2 字节）")
                .arg(tbl.partitions.size()).arg(tbl.sectorSize));
        // T11 审查 Minor：空表要**在这里**点名（否则计划层走"derived"分支，最终报成
        // "内部错误：分区 X 不在设备表地址映射里"，把真因（分区表为空）说成内部错位）。
        if (refs.isEmpty()) {
            if (error) *error = QStringLiteral("设备分区表为空（GPT 无有效条目）—— 拒绝在未知分区表上写入");
            return false;
        }
    } else {
        // T12 在此写入 READ-FLASH 读表 + partAddr 填充（T11 阶段明确拒绝，不假装支持）
        if (error) *error = QStringLiteral("XML 代：分区表读取在 Task 12 接线（当前明确拒绝）");
        return false;
    }

    mtkplan::MtkFlashPlan plan;
    if (!mtkplan::buildMtkPlan(refs, req.imagePaths, plan, error))
        return false;
    for (const QString &w : std::as_const(plan.warnings))
        warn(w);
    if (plan.entries.isEmpty()) {
        // **写任何字节之前**就失败（T9 审查 I1 点名的安全门：不许在"计划为空"时往下走）
        if (error) *error = QStringLiteral("按设备分区表没有任何可写入的镜像（见上方告警）");
        return false;
    }
    say(QStringLiteral("计划：%1 个分区，合计 %2 字节").arg(plan.entries.size()).arg(plan.totalBytes));

    // 逐分区写（失败即停；错误含分区名与已写字节 —— 由 flashPartition 填）
    quint64 written = 0;
    for (const mtkplan::PlanEntry &e : std::as_const(plan.entries)) {
        QFile f(e.imagePath);
        if (!f.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("无法读取镜像：%1").arg(e.imagePath);
            return false;
        }
        const QByteArray image = f.readAll();
        f.close();
        if (image.size() != qsizetype(e.imageSize)) {
            if (error) *error = QStringLiteral("镜像 %1 读入字节数与计划不符（%2 != %3）")
                                    .arg(e.imagePath).arg(image.size()).arg(e.imageSize);
            return false;
        }
        say(QStringLiteral("写入分区 %1（%2 字节）…").arg(e.partition).arg(image.size()));
        if (gen == MtkGeneration::Legacy) {
            if (!flashPartition(session, *storage, e.partition, image, error))
                return false;
        } else if (gen == MtkGeneration::XFlash) {
            const auto it = partAddr.constFind(e.partition);
            if (it == partAddr.constEnd()) {     // 计划条目名必来自设备表 → 缺 = 内部错位
                if (error) *error = QStringLiteral("内部错误：分区 %1 不在设备表地址映射里").arg(e.partition);
                return false;
            }
            if (!xflashWriteData(xflash, it.value(), image, kXStorageEmmc, kXEmmcPartUser,
                                 xWritePacketLength, nullptr, error))
                return false;
        } else {
            // T12 换成： if (!xmlWritePartition(xml, e.partition, image, nullptr, error)) return false;
            if (error) *error = QStringLiteral("XML 代：写入在 Task 12 接线（当前明确拒绝）");
            return false;
        }
        written += quint64(image.size());
        if (progress)
            progress(written, plan.totalBytes);
    }

    // 收尾：失败只告警（数据已落盘）
    if (gen == MtkGeneration::Legacy) {
        QString finishErr;
        if (storage->finishFlash(0, &finishErr))
            say(QStringLiteral("FINISH（0xD9）收尾完成"));
        else
            warn(QStringLiteral("FINISH 收尾失败（数据已写入）：%1").arg(finishErr));
    } else if (gen == MtkGeneration::XFlash) {
        QString shutErr;
        if (xflashShutdown(xflash, 0, &shutErr))
            say(QStringLiteral("XFlash：SHUTDOWN 收尾完成"));
        else
            warn(QStringLiteral("XFlash SHUTDOWN 收尾失败（数据已写入）：%1").arg(shutErr));
    } else {
        // T12 换成： QString rbErr; if (xmlReboot(xml, true, &rbErr)) say(...); else warn(...);
        warn(QStringLiteral("XML 代：REBOOT 收尾在 Task 12 接线"));
    }
    return true;
}

} // namespace mtkbrom
