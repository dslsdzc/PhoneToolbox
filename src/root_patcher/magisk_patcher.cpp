#include "root_patcher/magisk_patcher.h"
#include "root_patcher/kernelsu_patcher.h"
#include "root_patcher/apatch_patcher.h"
#include "root_patcher/ramdisk_utils.h"
#include "image_engine/boot_image.h"

#include <QFile>
#include <QList>
#include <QMap>

#include <zlib.h>

#include <cstdio>
#include <cstring>

namespace patcher {
namespace {

// ============================================================
// 最小 ZIP 读取（PKWARE 规范，覆盖 APK 实际使用的 STORE/DEFLATE）
// ============================================================

quint16 le16(const QByteArray &d, int pos)
{
    return static_cast<quint16>(static_cast<uchar>(d[pos])) |
           (static_cast<quint16>(static_cast<uchar>(d[pos + 1])) << 8);
}

quint32 le32(const QByteArray &d, int pos)
{
    return static_cast<quint32>(static_cast<uchar>(d[pos])) |
           (static_cast<quint32>(static_cast<uchar>(d[pos + 1])) << 8) |
           (static_cast<quint32>(static_cast<uchar>(d[pos + 2])) << 16) |
           (static_cast<quint32>(static_cast<uchar>(d[pos + 3])) << 24);
}

struct ZipEntry {
    QString name;
    quint16 method = 0; // 0=store 8=deflate
    quint32 compSize = 0;
    quint32 uncompSize = 0;
    quint32 localOff = 0;
};

// 从文件尾 64 KiB + EOCD 内扫描 EOCD（0x06054b50），校验注释长度落在文件尾。
bool findEocd(const QByteArray &zip, quint16 *entryCount, quint32 *cdSize, quint32 *cdOffset)
{
    if (zip.size() < 22) // 最小 EOCD 22B
        return false;
    const int scan = qMin(zip.size(), 65536 + 22);
    for (int i = zip.size() - 22; i >= zip.size() - scan; --i) {
        if (i < 0)
            break;
        if (le32(zip, i) == 0x06054b50u) {
            const quint16 commentLen = le16(zip, i + 20);
            if (i + 22 + commentLen == zip.size()) {
                *entryCount = le16(zip, i + 10);
                *cdSize = le32(zip, i + 12);
                *cdOffset = le32(zip, i + 16);
                return true;
            }
        }
    }
    return false;
}

// 解析中央目录。zip64 标记（entryCount/偏移为 0xFFFF/0xFFFFFFFF）明确报错：
// Magisk 系官方 APK 均为普通 ZIP（联网验证 v25.2/v30.7 产物）。
bool loadZipEntries(const QByteArray &zip, QList<ZipEntry> *out, QString *err)
{
    auto fail = [err](const QString &msg) {
        if (err)
            *err = msg;
        return false;
    };
    quint16 count;
    quint32 cdSize, cdOffset;
    if (!findEocd(zip, &count, &cdSize, &cdOffset))
        return fail(QStringLiteral("ZIP: 未找到 EOCD 记录"));
    if (count == 0xFFFF || cdSize == 0xFFFFFFFFu || cdOffset == 0xFFFFFFFFu)
        return fail(QStringLiteral("ZIP: zip64 归档暂不支持"));
    if (static_cast<quint64>(cdOffset) + cdSize > static_cast<quint64>(zip.size()))
        return fail(QStringLiteral("ZIP: 中央目录越界"));
    // 位置运算全程 qint64：cdOffset 可接近 INT_MAX（QByteArray 上限），
    // 与 46/nameLen 等相加时避免 int 溢出（理论边缘，防御性处理）
    qint64 pos = cdOffset;
    for (int i = 0; i < count; ++i) {
        if (pos + 46 > zip.size() || le32(zip, static_cast<int>(pos)) != 0x02014b50u)
            return fail(QStringLiteral("ZIP: 中央目录损坏"));
        const quint16 nameLen = le16(zip, static_cast<int>(pos) + 28);
        const quint16 extraLen = le16(zip, static_cast<int>(pos) + 30);
        const quint16 commentLen = le16(zip, static_cast<int>(pos) + 32);
        const qint64 entryEnd = pos + 46 + nameLen + extraLen + commentLen;
        if (entryEnd > zip.size())
            return fail(QStringLiteral("ZIP: 中央目录条目越界"));
        ZipEntry e;
        e.name = QString::fromUtf8(zip.constData() + static_cast<int>(pos) + 46, nameLen);
        e.method = le16(zip, static_cast<int>(pos) + 10);
        e.compSize = le32(zip, static_cast<int>(pos) + 20);
        e.uncompSize = le32(zip, static_cast<int>(pos) + 24);
        e.localOff = le32(zip, static_cast<int>(pos) + 42);
        out->append(e);
        pos = entryEnd;
    }
    return true;
}

// 按中央目录大小切片条目数据（对 bit3 data descriptor 同样成立），
// STORE 直取，DEFLATE 用 zlib 解压（输出大小以目录字段为准，防恶意头）。
bool extractZipEntry(const QByteArray &zip, const QString &entryName, QByteArray &out,
                     QString *err)
{
    auto fail = [err](const QString &msg) {
        if (err)
            *err = msg;
        return false;
    };
    QList<ZipEntry> entries;
    if (!loadZipEntries(zip, &entries, err))
        return false;
    const ZipEntry *found = nullptr;
    for (const auto &e : entries) {
        if (e.name == entryName) {
            found = &e;
            break;
        }
    }
    if (!found)
        return fail(QStringLiteral("ZIP: 未找到条目 %1").arg(entryName));
    if (static_cast<quint64>(found->localOff) + 30 > static_cast<quint64>(zip.size()) ||
        le32(zip, static_cast<int>(found->localOff)) != 0x04034b50u)
        return fail(QStringLiteral("ZIP: 本地文件头损坏"));
    const quint16 lhNameLen = le16(zip, static_cast<int>(found->localOff) + 26);
    const quint16 lhExtraLen = le16(zip, static_cast<int>(found->localOff) + 28);
    const qint64 dataOff = static_cast<qint64>(found->localOff) + 30 + lhNameLen + lhExtraLen;
    if (dataOff + found->compSize > zip.size())
        return fail(QStringLiteral("ZIP: 条目数据越界"));
    const QByteArray comp = zip.mid(static_cast<int>(dataOff), static_cast<int>(found->compSize));
    if (found->method == 0) {
        out = comp;
        return true;
    }
    if (found->method == 8) {
        // raw DEFLATE：ZIP 条目是无 zlib 头的原始 deflate 流，zlib 的
        // uncompress()（期望 zlib 封装流）不适用，须用 inflateInit2(-MAX_WBITS)。
        // 用真实 Magisk APK 验证过（v25.2 产物，见 task-C3-report.md）。
        // uncompSize 来自中央目录（攻击者可控）：16MB 上限防恶意 APK 强制
        // 分配内存致 bad_alloc terminate（真实 magiskinit 仅 ~200-500KB）
        if (found->uncompSize > 16u * 1024 * 1024)
            return fail(QStringLiteral("ZIP: 条目解压后过大"));
        QByteArray buf(static_cast<int>(found->uncompSize), Qt::Uninitialized);
        z_stream strm = {};
        if (inflateInit2(&strm, -MAX_WBITS) != Z_OK)
            return fail(QStringLiteral("ZIP: inflate 初始化失败"));
        strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(comp.constData()));
        strm.avail_in = static_cast<uInt>(comp.size());
        strm.next_out = reinterpret_cast<Bytef *>(buf.data());
        strm.avail_out = static_cast<uInt>(buf.size());
        const int rc = inflate(&strm, Z_FINISH);
        const bool streamOk = (rc == Z_STREAM_END);
        const int produced = static_cast<int>(buf.size()) - static_cast<int>(strm.avail_out);
        inflateEnd(&strm);
        if (!streamOk)
            return fail(QStringLiteral("ZIP: DEFLATE 解压失败"));
        out = buf.left(produced);
        return true;
    }
    return fail(QStringLiteral("ZIP: 不支持的压缩方法 %1").arg(found->method));
}

// ============================================================
// newc cpio（070701/070702）读写
// 布局与 Magisk 的 cpio 实现逐字节一致（联网验证 v25.2 的 cpio.cpp 与
// master 的 cpio.rs）：110B 头（"070701" + 13×8 hex 字段）、名字 NUL 结尾、
// 名字与数据均按 4 字节对齐；TRAILER!!! 结尾；Android 支持多段 cpio 拼接
// （TRAILER 后继续搜索下一个 cpio 魔数）。硬链接条目与 Magisk 同样按
// 原始数据透传（不特殊处理，nlink 重序列化时固定为 1）。
// ============================================================

struct CpioEntry {
    quint32 mode = 0;
    QByteArray data;
};

bool hex8Value(const char *p, quint32 *v)
{
    quint32 r = 0;
    for (int i = 0; i < 8; ++i) {
        const char c = p[i];
        r <<= 4;
        if (c >= '0' && c <= '9')
            r |= static_cast<quint32>(c - '0');
        else if (c >= 'a' && c <= 'f')
            r |= static_cast<quint32>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            r |= static_cast<quint32>(c - 'A' + 10);
        else
            return false;
    }
    *v = r;
    return true;
}

class CpioArchive
{
public:
    // 解析失败返回 false 并写 err（绝不崩溃；所有偏移均做边界校验）
    bool parse(const QByteArray &buf, QString *err)
    {
        auto fail = [err](const char *msg) {
            if (err)
                *err = QString::fromUtf8(msg);
            return false;
        };
        m_entries.clear();
        if (buf.size() < 110)
            return fail("cpio: 数据过短（非 cpio 镜像）"); // 合法 cpio 至少含一个 110B 头
        int pos = 0;
        while (pos + 110 <= buf.size()) {
            const QByteArray magic = buf.mid(pos, 6);
            if (magic != "070701" && magic != "070702")
                return fail("cpio: 魔数错误");
            quint32 mode, filesize, namesize;
            // 字段偏移：magic@0，ino@6，mode@14，...，filesize@54，...，namesize@94
            if (!hex8Value(buf.constData() + pos + 14, &mode) ||
                !hex8Value(buf.constData() + pos + 54, &filesize) ||
                !hex8Value(buf.constData() + pos + 94, &namesize))
                return fail("cpio: 头部字段损坏");
            const qint64 nameOff = static_cast<qint64>(pos) + 110;
            if (namesize == 0 || nameOff + namesize > buf.size())
                return fail("cpio: 名字越界");
            const QByteArray name = buf.mid(static_cast<int>(nameOff), namesize - 1);
            qint64 p = nameOff + namesize;
            p = (p + 3) & ~qint64(3);
            if (name == "." || name == "..") {
                pos = static_cast<int>(p);
                continue;
            }
            if (name == "TRAILER!!!") {
                // 多段 cpio 拼接：TRAILER 后搜索下一个 cpio 魔数
                const int next = buf.indexOf("070701", static_cast<int>(p));
                if (next < 0)
                    break;
                pos = next;
                continue;
            }
            if (p + filesize > buf.size())
                return fail("cpio: 数据越界");
            CpioEntry e;
            e.mode = mode;
            e.data = buf.mid(static_cast<int>(p), static_cast<int>(filesize));
            m_entries.insert(QString::fromUtf8(name), e); // 同名替换（cpio add 语义）
            p += filesize;
            pos = static_cast<int>((p + 3) & ~qint64(3));
        }
        return true;
    }

    bool find(const QString &name, CpioEntry *out) const
    {
        const auto it = m_entries.constFind(name);
        if (it == m_entries.constEnd())
            return false;
        if (out)
            *out = *it;
        return true;
    }

    void addOrReplace(const QString &name, const CpioEntry &e) { m_entries.insert(name, e); }

    QByteArray serialize() const
    {
        QByteArray out;
        quint32 ino = 300000;
        char hdr[111]; // 110 字符 + NUL（与 Magisk cpio.cpp 的 header[111] 一致）
        auto putHeader = [&](quint32 mode, quint32 filesize, quint32 namesize) {
            std::snprintf(hdr, sizeof(hdr),
                          "070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x",
                          static_cast<unsigned>(ino++), static_cast<unsigned>(mode), 0u, 0u, 1u,
                          0u, static_cast<unsigned>(filesize), 0u, 0u, 0u, 0u,
                          static_cast<unsigned>(namesize), 0u);
            out.append(hdr, 110);
        };
        for (auto it = m_entries.constBegin(); it != m_entries.constEnd(); ++it) {
            const QByteArray name = it.key().toUtf8();
            putHeader(it->mode, static_cast<quint32>(it->data.size()),
                      static_cast<quint32>(name.size()) + 1);
            out.append(name);
            out.append('\0');
            while (out.size() % 4)
                out.append('\0');
            if (!it->data.isEmpty()) {
                out.append(it->data);
                while (out.size() % 4)
                    out.append('\0');
            }
        }
        putHeader(0755, 0, 11);
        out.append("TRAILER!!!\0", 11);
        while (out.size() % 4)
            out.append('\0');
        return out;
    }

private:
    // 键序排列输出，与 Magisk 的 std::map/BTreeMap 行为一致（稳定可复现）
    QMap<QString, CpioEntry> m_entries;
};

// 注入物 ABI 搜索顺序：ramdisk 的 init 运行于目标设备 CPU，PC 端无从得知
// ABI，按现代设备占比优先 arm64-v8a（官方 App 按设备 ro.product.cpu.abi
// 选择，此处给出回退链）。
QStringList abiCandidates()
{
    return {QStringLiteral("arm64-v8a"), QStringLiteral("armeabi-v7a"),
            QStringLiteral("x86_64"), QStringLiteral("x86")};
}

// .backup/.magisk 配置：C3 不做 fstab verity/encryption 剥离（KEEPVERITY/
// KEEPFORCEENCRYPT 保持 true 以如实反映未修补状态）；magiskinit 运行时仅
// 原样透传该文件（v25.2 init/rootdir.cpp magisk_cfg），字段供卸载/还原流程使用。
QByteArray magiskConfig()
{
    return QByteArray("KEEPVERITY=true\nKEEPFORCEENCRYPT=true\n"
                      "PATCHVBMETAFLAG=false\nRECOVERYMODE=false\n");
}

} // namespace

// ============================================================
// MagiskPatcher
// ============================================================

bool MagiskPatcher::patch(const QByteArray &bootImage, const PatchConfig &cfg,
                          QByteArray &out, QString *error)
{
    auto fail = [error](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (error)
        error->clear();

    if (cfg.type != RootType::Magisk && cfg.type != RootType::MagiskAlpha &&
        cfg.type != RootType::Kitsune)
        return fail(QStringLiteral("MagiskPatcher 仅处理 Magisk 系 RootType"));

    if (cfg.apkPath.isEmpty())
        return fail(
            QStringLiteral("未提供 Magisk APK：请先经 AssetsDownloader 下载注入物后填入 apkPath"));

    QFile apkFile(cfg.apkPath);
    if (!apkFile.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法打开 APK：%1").arg(cfg.apkPath));
    const QByteArray apk = apkFile.readAll();
    apkFile.close();
    if (apk.isEmpty())
        return fail(QStringLiteral("APK 文件为空"));

    // 从 APK 提取 magiskinit（真实产物为静态链接 ELF，注入逻辑不校验内容）
    QByteArray magiskinit;
    QString zipErr;
    for (const QString &abi : abiCandidates()) {
        if (extractZipEntry(apk, QStringLiteral("lib/%1/libmagiskinit.so").arg(abi), magiskinit,
                            &zipErr))
            break;
    }
    if (magiskinit.isEmpty())
        return fail(QStringLiteral("APK 内未找到 libmagiskinit.so（%1）").arg(zipErr));

    imgboot::BootInfo info;
    if (!imgboot::parseBootImage(bootImage, info))
        return fail(QStringLiteral("无法解析 boot 镜像"));
    if (info.ramdisk.isEmpty())
        return fail(
            QStringLiteral("boot 镜像不含 ramdisk（ramdiskless SAR 暂不支持，请用对应 App 修补）"));

    // 记录原始压缩格式，重压时保持（与 magiskboot repack 行为一致）
    QString fmt;
    patcher::detectRamdiskFormat(info.ramdisk, fmt);
    QByteArray ramdiskRaw;
    QString ramdiskErr;
    if (!patcher::decompressRamdisk(info.ramdisk, ramdiskRaw, &ramdiskErr))
        return fail(QStringLiteral("ramdisk 解压失败：%1").arg(ramdiskErr));

    CpioArchive cpio;
    if (!cpio.parse(ramdiskRaw, &ramdiskErr))
        return fail(QStringLiteral("ramdisk 不是有效 cpio：%1").arg(ramdiskErr));

    // 重复修补防护：官方 boot_patch.sh 先 "cpio test"（1=已 Magisk 修补）再
    // restore；已修补镜像含 .backup 标记，再次注入会把原 init 备份覆盖成
    // 旧 magiskinit（损坏还原链）→ 明确拒绝，交由调用方还原原厂镜像
    CpioEntry probe;
    if (cpio.find(QStringLiteral(".backup"), &probe))
        return fail(QStringLiteral("镜像已修补过，请先还原为原厂 boot 镜像"));

    // 注入链（与官方 boot_patch.sh "add 0750 init magiskinit" + "backup"
    // 一致）：原 init 条目备份为 .backup/init（运行时 magiskinit 的
    // backup_init() 将其 rename 回 /init 交给真实 init 接管），init 条目
    // 替换为 magiskinit。注意：官方实现不存在 init.orig 条目。
    CpioEntry origInit;
    if (cpio.find(QStringLiteral("init"), &origInit))
        cpio.addOrReplace(QStringLiteral(".backup/init"), origInit);
    CpioEntry magiskinitEntry;
    magiskinitEntry.mode = 0100750; // S_IFREG | 0750
    magiskinitEntry.data = magiskinit;
    cpio.addOrReplace(QStringLiteral("init"), magiskinitEntry);
    CpioEntry backupDir;
    backupDir.mode = 0040000; // S_IFDIR（官方 mkdir 000 .backup）
    cpio.addOrReplace(QStringLiteral(".backup"), backupDir);
    CpioEntry configEntry;
    configEntry.mode = 0100000; // S_IFREG（官方 add 000 .backup/.magisk）
    configEntry.data = magiskConfig();
    cpio.addOrReplace(QStringLiteral(".backup/.magisk"), configEntry);

    const QByteArray patchedCpio = cpio.serialize();
    const QByteArray newRamdisk = patcher::compressRamdisk(patchedCpio, fmt);
    if (newRamdisk.isEmpty())
        return fail(QStringLiteral("ramdisk 重压失败"));

    info.ramdisk = newRamdisk;
    out = imgboot::repackBootImage(info);
    return true;
}

QUrl MagiskPatcher::downloadUrl(const QString &variant)
{
    const QString v = variant.toLower();
    if (v == QLatin1String("alpha"))
        // vvb2060/Magisk：tag=版本号（如 30700），asset 固定 app-release.apk
        // （联网验证 2026-08）
        return QUrl(QStringLiteral("https://github.com/vvb2060/Magisk/releases/latest"));
    if (v == QLatin1String("kitsune"))
        // 原 HuskyDG/magisk-files 与 KitsuneMagisk/KitsuneMagisk 均已下线
        // （GitHub API 404，2026-08 验证），现有源均为个人镜像 → 不内置
        // URL，须手动指定 APK
        return QUrl();
    // 官方：asset 命名随版本变化（Magisk-v30.7.apk / Magisk-v28.1-28100.apk），
    // 由调用方经 GitHub API 解析具体 asset
    return QUrl(QStringLiteral("https://github.com/topjohnwu/Magisk/releases/latest"));
}

QString MagiskPatcher::assetKey(const QString &variant)
{
    const QString v = variant.toLower();
    if (v == QLatin1String("alpha"))
        return QStringLiteral("magisk-alpha");
    if (v == QLatin1String("kitsune"))
        return QStringLiteral("magisk-kitsune");
    return QStringLiteral("magisk");
}

// 工厂：C3 实现 Magisk 系；C4 登记 KernelSU 系（KernelSuPatcher，见
// kernelsu_patcher.h）；C5 登记 APatch/KernelPatch 系（APatchPatcher，见
// apatch_patcher.h）；其余类型由 C6-C8 在各自实现中扩展。
RootPatcher *RootPatcher::create(RootType type)
{
    switch (type) {
    case RootType::Magisk:
    case RootType::MagiskAlpha:
    case RootType::Kitsune:
        return new MagiskPatcher;
    case RootType::KernelSU:
    case RootType::KernelSU_Next:
    case RootType::SukiSU:
    case RootType::ReSukiSU:
        return new KernelSuPatcher;
    case RootType::APatch:
    case RootType::KernelPatch:
        return new APatchPatcher;
    default:
        return nullptr;
    }
}

} // namespace patcher
