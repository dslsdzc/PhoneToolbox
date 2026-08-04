#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "root_patcher/root_patcher.h"
#include "root_patcher/magisk_patcher.h"
#include "root_patcher/kernelsu_patcher.h"
#include "root_patcher/ramdisk_utils.h"
#include "image_engine/boot_image.h"

#include <zlib.h>

namespace {

// ---- 测试工具：newc cpio（070701）构建/解析 ----
// 布局与 Magisk cpio 实现一致：110B 头（"070701" + 13×8 hex 字段）、
// 名字 NUL 结尾、数据按 4 字节对齐，TRAILER!!! 结尾。
struct TestCpioEntry {
    QByteArray name;
    quint32 mode = 0;
    QByteArray data;
};

constexpr quint32 kRegMode = 0100000; // S_IFREG
constexpr quint32 kDirMode = 0040000; // S_IFDIR

QByteArray buildCpio(const QList<TestCpioEntry> &entries)
{
    QByteArray out;
    quint32 ino = 300000;
    auto putHeader = [&](quint32 mode, quint32 filesize, quint32 namesize) {
        char hdr[111]; // 110 字符 + NUL
        std::snprintf(hdr, sizeof(hdr),
                      "070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x",
                      static_cast<unsigned>(ino++), static_cast<unsigned>(mode), 0u, 0u, 1u,
                      0u, static_cast<unsigned>(filesize), 0u, 0u, 0u, 0u,
                      static_cast<unsigned>(namesize), 0u);
        out.append(hdr, 110);
    };
    for (const auto &e : entries) {
        putHeader(e.mode, static_cast<quint32>(e.data.size()),
                  static_cast<quint32>(e.name.size()) + 1);
        out.append(e.name);
        out.append('\0');
        while (out.size() % 4)
            out.append('\0');
        if (!e.data.isEmpty()) {
            out.append(e.data);
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

bool parseCpio(const QByteArray &buf, QList<TestCpioEntry> *out)
{
    auto hex8 = [&buf](int off, quint32 *v) {
        quint32 r = 0;
        for (int i = 0; i < 8; ++i) {
            const char c = buf[off + i];
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
    };
    int pos = 0;
    while (pos + 110 <= buf.size()) {
        if (buf.mid(pos, 6) != "070701")
            return false;
        quint32 mode, filesize, namesize;
        if (!hex8(pos + 14, &mode) || !hex8(pos + 54, &filesize) || !hex8(pos + 94, &namesize))
            return false;
        if (namesize == 0 || pos + 110 + namesize > buf.size())
            return false;
        const QByteArray name = buf.mid(pos + 110, namesize - 1);
        int p = pos + 110 + namesize;
        p = (p + 3) & ~3;
        if (name == "TRAILER!!!")
            break;
        if (name == "." || name == "..") {
            pos = p;
            continue;
        }
        if (static_cast<qint64>(p) + filesize > buf.size())
            return false;
        TestCpioEntry e;
        e.name = name;
        e.mode = mode;
        e.data = buf.mid(p, static_cast<int>(filesize));
        out->append(e);
        p += static_cast<int>(filesize);
        pos = (p + 3) & ~3;
    }
    return true;
}

const TestCpioEntry *findEntry(const QList<TestCpioEntry> &entries, const QByteArray &name)
{
    for (const auto &e : entries)
        if (e.name == name)
            return &e;
    return nullptr;
}

// ---- 测试工具：最小 ZIP（STORE/DEFLATE）构建 ----
struct TestZipEntry {
    QByteArray name;
    QByteArray data;
    int method = 0; // 0=store 8=deflate
};

void putLE32(QByteArray &b, quint32 v)
{
    b.append(char(v)).append(char(v >> 8)).append(char(v >> 16)).append(char(v >> 24));
}

void putLE16(QByteArray &b, quint16 v)
{
    b.append(char(v)).append(char(v >> 8));
}

QByteArray buildZip(const QList<TestZipEntry> &entries)
{
    QByteArray body;
    struct CdEnt {
        QByteArray name;
        quint32 crc = 0;
        quint32 compSize = 0;
        quint32 uncompSize = 0;
        quint32 localOff = 0;
        int method = 0;
    };
    QList<CdEnt> cd;
    for (const auto &e : entries) {
        QByteArray comp;
        int method = e.method;
        if (e.method == 8) {
            // raw DEFLATE（与真实 ZIP/APK 一致：无 zlib 封装头；
            // zlib compress2 输出的是 zlib 封装流，不能用于 mock 真实 APK）
            z_stream strm = {};
            if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8,
                             Z_DEFAULT_STRATEGY) != Z_OK)
                return {};
            const uLong bound = deflateBound(&strm, static_cast<uLong>(e.data.size()));
            comp.resize(static_cast<int>(bound));
            strm.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(e.data.constData()));
            strm.avail_in = static_cast<uInt>(e.data.size());
            strm.next_out = reinterpret_cast<Bytef *>(comp.data());
            strm.avail_out = static_cast<uInt>(comp.size());
            const int rc = deflate(&strm, Z_FINISH);
            const bool ok = (rc == Z_STREAM_END);
            comp.truncate(static_cast<int>(strm.total_out));
            deflateEnd(&strm);
            if (!ok)
                return {};
        } else {
            comp = e.data;
            method = 0;
        }
        const quint32 off = static_cast<quint32>(body.size());
        const QByteArray name = e.name;
        QByteArray lh;
        putLE32(lh, 0x04034b50);
        putLE16(lh, 20); // version needed
        putLE16(lh, 0);  // flags
        putLE16(lh, static_cast<quint16>(method));
        putLE16(lh, 0); // mtime
        putLE16(lh, 0); // mdate
        putLE32(lh, crc32(0, reinterpret_cast<const Bytef *>(e.data.constData()),
                          static_cast<uInt>(e.data.size())));
        putLE32(lh, static_cast<quint32>(comp.size()));
        putLE32(lh, static_cast<quint32>(e.data.size()));
        putLE16(lh, static_cast<quint16>(name.size()));
        putLE16(lh, 0); // extra len
        lh.append(name);
        body.append(lh);
        body.append(comp);
        CdEnt c;
        c.name = name;
        c.crc = crc32(0, reinterpret_cast<const Bytef *>(e.data.constData()),
                      static_cast<uInt>(e.data.size()));
        c.compSize = static_cast<quint32>(comp.size());
        c.uncompSize = static_cast<quint32>(e.data.size());
        c.localOff = off;
        c.method = method;
        cd.append(c);
    }
    QByteArray cdb;
    for (const auto &c : cd) {
        QByteArray cb;
        putLE32(cb, 0x02014b50);
        putLE16(cb, 20); // version made by
        putLE16(cb, 20); // version needed
        putLE16(cb, 0);  // flags
        putLE16(cb, static_cast<quint16>(c.method));
        putLE16(cb, 0); // mtime
        putLE16(cb, 0); // mdate
        putLE32(cb, c.crc);
        putLE32(cb, c.compSize);
        putLE32(cb, c.uncompSize);
        putLE16(cb, static_cast<quint16>(c.name.size()));
        putLE16(cb, 0); // extra len
        putLE16(cb, 0); // comment len
        putLE16(cb, 0); // disk start
        putLE16(cb, 0); // internal attr
        putLE32(cb, 0); // external attr
        putLE32(cb, c.localOff);
        cb.append(c.name);
        cdb.append(cb);
    }
    QByteArray eocd;
    putLE32(eocd, 0x06054b50);
    putLE16(eocd, 0); // disk
    putLE16(eocd, 0); // cd disk
    putLE16(eocd, static_cast<quint16>(cd.size()));
    putLE16(eocd, static_cast<quint16>(cd.size()));
    putLE32(eocd, static_cast<quint32>(cdb.size()));
    putLE32(eocd, static_cast<quint32>(body.size()));
    putLE16(eocd, 0); // comment len
    return body + cdb + eocd;
}

// ---- 测试工具：boot v0 镜像 ----
// 布局：1632B 头补零到 4096 页 → kernel（页对齐，默认 4096B 'K' 填充）→
// ramdisk（页对齐结尾）。cmdline 写于 offset 64（512B，NUL 填充）。
static QByteArray buildBootV0(const QByteArray &ramdisk,
                              const QByteArray &kernel = QByteArray(4096, 'K'),
                              const QByteArray &cmdline = QByteArray())
{
    QByteArray hdr(1632, 0);
    hdr.replace(0, 8, "ANDROID!");
    auto put32 = [&](int off, quint32 v) {
        hdr[off] = char(v);
        hdr[off + 1] = char(v >> 8);
        hdr[off + 2] = char(v >> 16);
        hdr[off + 3] = char(v >> 24);
    };
    put32(8, static_cast<quint32>(kernel.size()));   // kernel_size
    put32(16, static_cast<quint32>(ramdisk.size())); // ramdisk_size
    put32(24, 0);                                    // second_size
    put32(36, 4096);                                 // page_size
    put32(40, 0);                                    // header_version
    if (!cmdline.isEmpty())
        hdr.replace(64, qMin(511, cmdline.size()), cmdline); // cmdline@64 (512B)
    QByteArray out = hdr;
    while (out.size() % 4096)
        out.append('\0');
    out.append(kernel);
    while (out.size() % 4096)
        out.append('\0'); // 各段页对齐（parseBootImage 按页偏移读段）
    out.append(ramdisk);
    while (out.size() % 4096)
        out.append('\0');
    return out;
}

} // namespace

class TestPatcher : public QObject
{
    Q_OBJECT
private slots:
    void factoryCreate();
    void missingApkFails();
    void nonexistentApkFails();
    void apkWithoutMagiskinitFails();
    void invalidBootFails();
    void repatchFails();
    void nonCpioRamdiskFails();
    void injectUncompressedRamdisk();
    void injectGzipRamdisk();
    void injectDeflatedApk();
    void injectFallsBackAbi();
    void downloadUrlKnown();

    // ---- C4: KernelSU 系 ----
    void ksuDetectKmiFromKernel();
    void ksuDetectKmiFromCmdline();
    void ksuDetectKmiNone();
    void ksuMissingKoFails();
    void ksuMissingWrapperFails();
    void ksuNonGkiNoKoFails();
    void ksuKmiUnsupportedFails();
    void ksuUnknownVariantFails();
    void ksuInjectLkmUncompressed();
    void ksuInjectLkmGzipRamdisk();
    void ksuInjectLkmIntoRamdisklessBoot();
    void ksuInjectLkmNoInitRamdisk();
    void ksuMagiskPatchedBootFails();
    void ksuRepatchIdempotent();
    void ksuAnyKernel3Replace();
    void ksuAnyKernel3DeflatedImage();
    void ksuAnyKernel3ZipWithoutKernelFails();
    void ksuLkmPackZipExtracts();
    void ksuLkmPackZipWithoutKmiFails();
    void ksuDownloadUrlsKnown();
};

void TestPatcher::factoryCreate()
{
    // Magisk 系三入口均映射到 MagiskPatcher
    for (patcher::RootType t : {patcher::RootType::Magisk, patcher::RootType::MagiskAlpha,
                                patcher::RootType::Kitsune}) {
        std::unique_ptr<patcher::RootPatcher> p(patcher::RootPatcher::create(t));
        QVERIFY2(p.get(), "create() 返回 nullptr");
        QVERIFY(dynamic_cast<patcher::MagiskPatcher *>(p.get()));
    }
    // C4：KernelSU 系四入口均映射到 KernelSuPatcher
    for (patcher::RootType t : {patcher::RootType::KernelSU, patcher::RootType::KernelSU_Next,
                                patcher::RootType::SukiSU, patcher::RootType::ReSukiSU}) {
        std::unique_ptr<patcher::RootPatcher> p(patcher::RootPatcher::create(t));
        QVERIFY2(p.get(), "create() 返回 nullptr");
        QVERIFY(dynamic_cast<patcher::KernelSuPatcher *>(p.get()));
    }
    // 未实现类型返回 nullptr（不崩溃），由后续任务 C5+ 扩展
    for (patcher::RootType t : {patcher::RootType::APatch, patcher::RootType::KernelPatch,
                                patcher::RootType::RamdiskSu,
                                patcher::RootType::ModuleInstall})
        QVERIFY(patcher::RootPatcher::create(t) == nullptr);
}

void TestPatcher::missingApkFails()
{
    patcher::MagiskPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk; // apkPath 未指定
    QByteArray out;
    QString err;
    const QByteArray cpio = buildCpio({});
    QVERIFY(!p.patch(buildBootV0(cpio), cfg, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestPatcher::nonexistentApkFails()
{
    patcher::MagiskPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk;
    cfg.apkPath = "/nonexistent/magisk.apk";
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(buildCpio({})), cfg, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestPatcher::apkWithoutMagiskinitFails()
{
    // 合法 zip 但缺少 libmagiskinit.so
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/magisk.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildZip({{"lib/arm64-v8a/libmagiskboot.so", "not magiskinit", 0}}));
    f.close();

    patcher::MagiskPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk;
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(buildCpio({})), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("libmagiskinit", Qt::CaseInsensitive));
}

void TestPatcher::invalidBootFails()
{
    // 合法 APK（含 magiskinit 条目）+ 垃圾 boot 字节：
    // 必须失败在 boot 镜像解析路径（而非 APK 读取），断言错误信息含 boot
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/magisk.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildZip({{"lib/arm64-v8a/libmagiskinit.so", "fake magiskinit bytes", 0}}));
    f.close();

    patcher::MagiskPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk;
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray("not a boot image at all"), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("boot", Qt::CaseInsensitive));
}

void TestPatcher::repatchFails()
{
    // 重复修补防护：已修补产物（ramdisk 含 .backup 标记）再次 patch 必须拒绝，
    // 防止原 init 备份被覆盖为旧 magiskinit（官方 boot_patch.sh 以
    // "cpio test → 已 Magisk 修补则 restore" 流程防此；本实现直接拒绝并提示还原）
    const QByteArray initPayload("original init payload");
    const QByteArray fakeMagiskinit("magiskinit-bytes-for-repatch-guard");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/magisk.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildZip({{"lib/arm64-v8a/libmagiskinit.so", fakeMagiskinit, 0}}));
    f.close();

    patcher::MagiskPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk;
    cfg.apkPath = apkPath;
    QByteArray patched;
    QString err;
    QVERIFY2(p.patch(buildBootV0(buildCpio({{"init", kRegMode | 0750, initPayload}})), cfg,
                     patched, &err),
             qPrintable(err));
    // 对已修补产物再次注入：必须失败并提示先还原；失败时输出不得残留
    QByteArray out2;
    QString err2;
    QVERIFY(!p.patch(patched, cfg, out2, &err2));
    QVERIFY(!err2.isEmpty());
    QVERIFY(err2.contains("还原", Qt::CaseInsensitive));
    QVERIFY(out2.isEmpty());
}

void TestPatcher::nonCpioRamdiskFails()
{
    // 压缩格式未知且非 cpio 的 ramdisk：注入必须失败而非输出损坏镜像
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/magisk.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildZip({{"lib/arm64-v8a/libmagiskinit.so", "fake magiskinit bytes", 0}}));
    f.close();

    patcher::MagiskPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk;
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(QByteArray("plain non-cpio ramdisk text")), cfg, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestPatcher::injectUncompressedRamdisk()
{
    const QByteArray initPayload("#!/system/bin/sh\ntouch /data/magisk-flag\n");
    const QByteArray fakeMagiskinit("\x7f"
                                    "ELF" "fake-magiskinit-executable-bytes-0123456789");
    const QByteArray ramdisk = buildCpio({{"init", kRegMode | 0750, initPayload}});
    const QByteArray apkBytes =
        buildZip({{"lib/arm64-v8a/libmagiskinit.so", fakeMagiskinit, 0}});
    QVERIFY(!apkBytes.isEmpty());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/magisk.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(apkBytes);
    f.close();

    patcher::MagiskPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk;
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(ramdisk), cfg, out, &err), qPrintable(err));

    // 输出为合法 boot 镜像
    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    QVERIFY(!info.ramdisk.isEmpty());
    // 未压缩 ramdisk 原样输出
    QString fmt;
    QVERIFY(!patcher::detectRamdiskFormat(info.ramdisk, fmt));

    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));
    // init 被 magiskinit 接管（0750 常规文件）
    const TestCpioEntry *init = findEntry(entries, "init");
    QVERIFY(init);
    QCOMPARE(init->data, fakeMagiskinit);
    QCOMPARE(init->mode & 0170000, kRegMode);
    QCOMPARE(init->mode & 0777, 0750u);
    // 原 init 备份为 .backup/init（官方 backup_init() 运行时契约），
    // 顶层不再保留原 init 内容
    QVERIFY(findEntry(entries, "init.orig") == nullptr);
    const TestCpioEntry *backupInit = findEntry(entries, ".backup/init");
    QVERIFY(backupInit);
    QCOMPARE(backupInit->data, initPayload);
    QCOMPARE(backupInit->mode & 0170000, kRegMode);
    // .backup 目录与配置
    const TestCpioEntry *backupDir = findEntry(entries, ".backup");
    QVERIFY(backupDir);
    QCOMPARE(backupDir->mode & 0170000, kDirMode);
    QVERIFY(backupDir->data.isEmpty());
    const TestCpioEntry *cfgEntry = findEntry(entries, ".backup/.magisk");
    QVERIFY(cfgEntry);
    QVERIFY(cfgEntry->data.contains("KEEPVERITY=true"));
    // 其余条目（kernel 等）不因注入而丢失：只有注入物相关条目
    QCOMPARE(entries.size(), 4);
}

void TestPatcher::injectGzipRamdisk()
{
    const QByteArray initPayload("original init for gzip ramdisk");
    const QByteArray fakeMagiskinit("magiskinit-fake-for-gzip-test");
    const QByteArray cpio = buildCpio({{"init", kRegMode | 0750, initPayload}});
    const QByteArray ramdiskComp = patcher::compressRamdisk(cpio, "gzip");
    QVERIFY(!ramdiskComp.isEmpty());
    QString fmt;
    QVERIFY(patcher::detectRamdiskFormat(ramdiskComp, fmt));
    QCOMPARE(fmt, "gzip");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/magisk.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildZip({{"lib/arm64-v8a/libmagiskinit.so", fakeMagiskinit, 0}}));
    f.close();

    patcher::MagiskPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::MagiskAlpha; // Alpha 注入机制相同
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(ramdiskComp), cfg, out, &err), qPrintable(err));

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    // 重压保持原格式（gzip）
    QString outFmt;
    QVERIFY(patcher::detectRamdiskFormat(info.ramdisk, outFmt));
    QCOMPARE(outFmt, "gzip");
    QByteArray raw;
    QString rErr;
    QVERIFY(patcher::decompressRamdisk(info.ramdisk, raw, &rErr));

    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(raw, &entries));
    const TestCpioEntry *init = findEntry(entries, "init");
    QVERIFY(init);
    QCOMPARE(init->data, fakeMagiskinit);
    const TestCpioEntry *backupInit = findEntry(entries, ".backup/init");
    QVERIFY(backupInit);
    QCOMPARE(backupInit->data, initPayload);
}

void TestPatcher::injectDeflatedApk()
{
    // 真实 Magisk APK 内条目为 DEFLATE 压缩（联网验证 v25.2/v30.7 产物）
    const QByteArray fakeMagiskinit("magiskinit extracted from deflated apk entry");
    const QByteArray apkBytes = buildZip(
        {{"lib/arm64-v8a/libmagiskinit.so", fakeMagiskinit, 8}}); // method=8 deflate
    QVERIFY(!apkBytes.isEmpty());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/magisk.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(apkBytes);
    f.close();

    patcher::MagiskPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk;
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(buildCpio({{"init", kRegMode | 0750, "init-data"}})), cfg, out,
                     &err),
             qPrintable(err));

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));
    const TestCpioEntry *init = findEntry(entries, "init");
    QVERIFY(init);
    QCOMPARE(init->data, fakeMagiskinit);
}

void TestPatcher::injectFallsBackAbi()
{
    // 无 arm64-v8a 时回退到 armeabi-v7a（PC 端无法获知设备 ABI）
    const QByteArray fakeMagiskinit("magiskinit from armeabi-v7a only apk");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/magisk.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildZip({{"lib/armeabi-v7a/libmagiskinit.so", fakeMagiskinit, 0}}));
    f.close();

    patcher::MagiskPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Kitsune; // Kitsune 注入机制相同
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(buildCpio({{"init", kRegMode | 0750, "init"}})), cfg, out,
                     &err),
             qPrintable(err));

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));
    const TestCpioEntry *init = findEntry(entries, "init");
    QVERIFY(init);
    QCOMPARE(init->data, fakeMagiskinit);
}

void TestPatcher::downloadUrlKnown()
{
    // 官方/Alpha 提供可解析的下载源（GitHub Releases），Kitsune 官方源已下线 → 空 URL
    QVERIFY(!patcher::MagiskPatcher::downloadUrl("official").isEmpty());
    QVERIFY(!patcher::MagiskPatcher::downloadUrl("alpha").isEmpty());
    QVERIFY(patcher::MagiskPatcher::downloadUrl("kitsune").isEmpty());
    QCOMPARE(patcher::MagiskPatcher::assetKey("official"), QStringLiteral("magisk"));
    QCOMPARE(patcher::MagiskPatcher::assetKey("alpha"), QStringLiteral("magisk-alpha"));
    QCOMPARE(patcher::MagiskPatcher::assetKey("kitsune"), QStringLiteral("magisk-kitsune"));
}

// ============================================================
// C4: KernelSU 系（官方/Next/SukiSU/ReSukiSU 参数化 + 非 GKI 路径）
// 注入机制依 ksud boot_patch.rs（联网验证 2026-08）：init→init.real 改名、
// init=ksuinit(0755)、kernelsu.ko(0755)
// ============================================================

void TestPatcher::ksuDetectKmiFromKernel()
{
    // GKI 内核版本串（"5.15.137-android13-8-00001-g..."）→ android13-5.15；
    // kernel 内扫描优先于 cmdline（权威路径），与 ksud parse_kmi 一致
    imgboot::BootInfo info;
    info.kernel = "Linux version 5.15.137-android13-8-00001-g97e1e0cd3750 "
                  "(gcc version 12.2) #1 SMP PREEMPT";
    info.cmdline = "console=tty0 androidboot.kmi=android13-5.10"; // 冲突 → kernel 优先
    QCOMPARE(patcher::KernelSuPatcher::detectKmi(info), QStringLiteral("android13-5.15"));
}

void TestPatcher::ksuDetectKmiFromCmdline()
{
    // kernel 无 KMI 串 → cmdline androidboot.kmi= 兜底
    imgboot::BootInfo info;
    info.kernel = QByteArray(4096, 'K');
    info.cmdline = "console=ttyS0 androidboot.kmi=android14-6.1 slot_suffix=_a";
    QCOMPARE(patcher::KernelSuPatcher::detectKmi(info), QStringLiteral("android14-6.1"));
}

void TestPatcher::ksuDetectKmiNone()
{
    imgboot::BootInfo info;
    info.kernel = QByteArray(4096, 'K');
    info.cmdline = "console=ttyS0 no_console_suspend";
    QVERIFY(patcher::KernelSuPatcher::detectKmi(info).isEmpty());
    // 空 BootInfo 不崩溃、返回空
    imgboot::BootInfo empty;
    QVERIFY(patcher::KernelSuPatcher::detectKmi(empty).isEmpty());
}

void TestPatcher::ksuMissingKoFails()
{
    // KMI 可识别但未提供 kernelsu.ko → 明确错误（提示下载源），不崩溃
    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelSU;
    QByteArray out;
    QString err;
    const QByteArray kernel = "Linux version 5.15.137-android13-8-00001-g97e1e0cd3750 "
                              "kernel-bytes";
    QVERIFY(!p.patch(buildBootV0(buildCpio({}), kernel), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("kernelsu.ko", Qt::CaseInsensitive));
    QVERIFY(out.isEmpty());
}

void TestPatcher::ksuMissingWrapperFails()
{
    // 有 ko 无 ksuinit wrapper → 错误提示 ksuinit
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString koPath = dir.path() + "/kernelsu.ko";
    QFile f(koPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("fake kernelsu.ko module bytes");
    f.close();

    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelSU;
    cfg.koPath = koPath; // apkPath 未指定
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(buildCpio({{"init", kRegMode | 0750, "init-data"}})), cfg, out,
                     &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("ksuinit", Qt::CaseInsensitive));
}

void TestPatcher::ksuNonGkiNoKoFails()
{
    // 非 GKI（kernel 无 KMI 串、cmdline 无 androidboot.kmi）且无注入物：
    // 诚实边界 —— 明确错误并建议 APatch/Magisk，不假装支持
    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelSU;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(buildCpio({})), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("APatch", Qt::CaseInsensitive));
    QVERIFY(err.contains("Magisk", Qt::CaseInsensitive));
}

void TestPatcher::ksuKmiUnsupportedFails()
{
    // KMI 可扫描但不在变体支持列表（LKM 仅覆盖 5.10+ GKI 资产）→ 错误 +
    // APatch 兜底建议
    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelSU;
    QByteArray out;
    QString err;
    const QByteArray kernel = "Linux version 5.10.101-android11-9-g30979850fc20 non-gki-err";
    QVERIFY(!p.patch(buildBootV0(buildCpio({}), kernel), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("android11-5.10"));
    QVERIFY(err.contains("APatch", Qt::CaseInsensitive));
}

void TestPatcher::ksuUnknownVariantFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString koPath = dir.path() + "/kernelsu.ko";
    const QString initPath = dir.path() + "/ksuinit";
    QFile f(koPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("ko-bytes");
    f.close();
    QFile f2(initPath);
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write("ksuinit-bytes");
    f2.close();

    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelSU;
    cfg.koPath = koPath;
    cfg.apkPath = initPath;
    cfg.variant = "bogus";
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(buildCpio({})), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("bogus"));
}

void TestPatcher::ksuInjectLkmUncompressed()
{
    // 完整 LKM 注入（未压缩 ramdisk）：
    // init→init.real（原样保留）、init=ksuinit(0755)、kernelsu.ko(0755)，
    // 无 .backup 链（与 Magisk 路径区分），kernel/dtb 段不动
    const QByteArray initPayload("original init payload for kernelsu");
    const QByteArray wrapperBytes("ksuinit-wrapper-executable-bytes");
    const QByteArray koBytes("fake kernelsu.ko module bytes");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString koPath = dir.path() + "/kernelsu.ko";
    const QString initPath = dir.path() + "/ksuinit";
    QFile f(koPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(koBytes);
    f.close();
    QFile f2(initPath);
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write(wrapperBytes);
    f2.close();

    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelSU;
    cfg.koPath = koPath;
    cfg.apkPath = initPath;
    QByteArray out;
    QString err;
    const QByteArray ramdisk = buildCpio({{"init", kRegMode | 0750, initPayload}});
    QVERIFY2(p.patch(buildBootV0(ramdisk), cfg, out, &err), qPrintable(err));

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));

    const TestCpioEntry *init = findEntry(entries, "init");
    QVERIFY(init);
    QCOMPARE(init->data, wrapperBytes);
    QCOMPARE(init->mode & 0170000, kRegMode);
    QCOMPARE(init->mode & 0777, 0755u); // ksud add 0755 init
    const TestCpioEntry *ko = findEntry(entries, "kernelsu.ko");
    QVERIFY(ko);
    QCOMPARE(ko->data, koBytes);
    QCOMPARE(ko->mode & 0170000, kRegMode);
    QCOMPARE(ko->mode & 0777, 0755u); // ksud add 0755 kernelsu.ko
    const TestCpioEntry *real = findEntry(entries, "init.real");
    QVERIFY(real);
    QCOMPARE(real->data, initPayload);
    QCOMPARE(real->mode & 0777, 0750u); // mv 保留原模式
    // 与 Magisk 路径区分：无 .backup 链
    QVERIFY(findEntry(entries, ".backup") == nullptr);
    QVERIFY(findEntry(entries, ".backup/init") == nullptr);
    QCOMPARE(entries.size(), 3);
    // kernel 段未动
    QCOMPARE(info.kernel, QByteArray(4096, 'K'));
}

void TestPatcher::ksuInjectLkmGzipRamdisk()
{
    const QByteArray initPayload("original init for gzip ramdisk kernelsu");
    const QByteArray wrapperBytes("ksuinit-fake-for-gzip");
    const QByteArray koBytes("ko-for-gzip-test");
    const QByteArray cpio = buildCpio({{"init", kRegMode | 0750, initPayload}});
    const QByteArray ramdiskComp = patcher::compressRamdisk(cpio, "gzip");
    QVERIFY(!ramdiskComp.isEmpty());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString koPath = dir.path() + "/kernelsu.ko";
    const QString initPath = dir.path() + "/ksuinit";
    QFile f(koPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(koBytes);
    f.close();
    QFile f2(initPath);
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write(wrapperBytes);
    f2.close();

    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelSU_Next; // Next 注入机制相同
    cfg.koPath = koPath;
    cfg.apkPath = initPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(ramdiskComp), cfg, out, &err), qPrintable(err));

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    QString outFmt;
    QVERIFY(patcher::detectRamdiskFormat(info.ramdisk, outFmt));
    QCOMPARE(outFmt, "gzip"); // 重压保持原格式
    QByteArray raw;
    QString rErr;
    QVERIFY(patcher::decompressRamdisk(info.ramdisk, raw, &rErr));
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(raw, &entries));
    const TestCpioEntry *init = findEntry(entries, "init");
    QVERIFY(init);
    QCOMPARE(init->data, wrapperBytes);
    const TestCpioEntry *real = findEntry(entries, "init.real");
    QVERIFY(real);
    QCOMPARE(real->data, initPayload);
    QVERIFY(findEntry(entries, "kernelsu.ko"));
}

void TestPatcher::ksuInjectLkmIntoRamdisklessBoot()
{
    // 无 ramdisk：与 ksud "No ramdisk, create by default" 一致 ——
    // 创建空 cpio 注入 init + kernelsu.ko
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString koPath = dir.path() + "/kernelsu.ko";
    const QString initPath = dir.path() + "/ksuinit";
    QFile f(koPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("ko-bytes");
    f.close();
    QFile f2(initPath);
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write("ksuinit-bytes");
    f2.close();

    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::SukiSU;
    cfg.koPath = koPath;
    cfg.apkPath = initPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(QByteArray()), cfg, out, &err), qPrintable(err));

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    QVERIFY(!info.ramdisk.isEmpty());
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));
    QVERIFY(findEntry(entries, "init"));
    QVERIFY(findEntry(entries, "kernelsu.ko"));
    QVERIFY(findEntry(entries, "init.real") == nullptr); // 无原 init 可改名
}

void TestPatcher::ksuInjectLkmNoInitRamdisk()
{
    // ramdisk 无 init 条目：仍注入 init + ko，不产生 init.real
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString koPath = dir.path() + "/kernelsu.ko";
    const QString initPath = dir.path() + "/ksuinit";
    QFile f(koPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("ko-bytes");
    f.close();
    QFile f2(initPath);
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write("ksuinit-bytes");
    f2.close();

    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ReSukiSU;
    cfg.koPath = koPath;
    cfg.apkPath = initPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(buildCpio({})), cfg, out, &err), qPrintable(err));

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));
    QVERIFY(findEntry(entries, "init"));
    QVERIFY(findEntry(entries, "kernelsu.ko"));
    QVERIFY(findEntry(entries, "init.real") == nullptr);
}

void TestPatcher::ksuMagiskPatchedBootFails()
{
    // Magisk 修补产物（.backup 链）→ 拒绝（ksud: "Cannot work with
    // Magisk patched image"）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString koPath = dir.path() + "/kernelsu.ko";
    const QString initPath = dir.path() + "/ksuinit";
    QFile f(koPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("ko-bytes");
    f.close();
    QFile f2(initPath);
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write("ksuinit-bytes");
    f2.close();

    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelSU;
    cfg.koPath = koPath;
    cfg.apkPath = initPath;
    QByteArray out;
    QString err;
    const QByteArray ramdisk =
        buildCpio({{".backup", kDirMode, QByteArray()}, {"init", kRegMode | 0750, "x"}});
    QVERIFY(!p.patch(buildBootV0(ramdisk), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("Magisk", Qt::CaseInsensitive));
    QVERIFY(out.isEmpty());
}

void TestPatcher::ksuRepatchIdempotent()
{
    // 已修补镜像（ramdisk 含 kernelsu.ko）再次注入：与 ksud 一致跳过
    // init→init.real 改名（原 init.real 保留），仅幂等重写 init 与 ko
    const QByteArray initPayload("original init payload");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString koPath = dir.path() + "/kernelsu.ko";
    const QString initPath = dir.path() + "/ksuinit";
    QFile f(koPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("ko-v1");
    f.close();
    QFile f2(initPath);
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write("ksuinit-v1");
    f2.close();

    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelSU;
    cfg.koPath = koPath;
    cfg.apkPath = initPath;
    QByteArray patched;
    QString err;
    QVERIFY2(p.patch(buildBootV0(buildCpio({{"init", kRegMode | 0750, initPayload}})), cfg,
                     patched, &err),
             qPrintable(err));

    // 第二次：换新注入物，成功且 init.real 仍为原 init
    QFile f3(koPath);
    QVERIFY(f3.open(QIODevice::WriteOnly));
    f3.write("ko-v2");
    f3.close();
    QFile f4(initPath);
    QVERIFY(f4.open(QIODevice::WriteOnly));
    f4.write("ksuinit-v2");
    f4.close();
    QByteArray patched2;
    QString err2;
    QVERIFY2(p.patch(patched, cfg, patched2, &err2), qPrintable(err2));

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(patched2, info));
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));
    const TestCpioEntry *init = findEntry(entries, "init");
    QVERIFY(init);
    QCOMPARE(init->data, QByteArray("ksuinit-v2"));
    const TestCpioEntry *ko = findEntry(entries, "kernelsu.ko");
    QVERIFY(ko);
    QCOMPARE(ko->data, QByteArray("ko-v2"));
    const TestCpioEntry *real = findEntry(entries, "init.real");
    QVERIFY(real);
    QCOMPARE(real->data, initPayload); // 原 init 未被二次注入破坏
}

void TestPatcher::ksuAnyKernel3Replace()
{
    // 非 GKI 路径：koPath 为 AnyKernel3 包（anykernel.sh + Image.gz）→
    // 解包提取 Image.gz 替换 boot kernel 段，ramdisk 保持原样
    const QByteArray fakeKernel("REPLACED-KERNEL-IMAGE-GZ-BYTES-0123456789");
    const QByteArray ramdisk = buildCpio({{"init", kRegMode | 0750, "keep-me"}});
    const QByteArray ak3Zip =
        buildZip({{"anykernel.sh", "kernel.string=FakeKernel by test", 0},
                  {"Image.gz", fakeKernel, 0}});
    QVERIFY(!ak3Zip.isEmpty());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = dir.path() + "/anykernel3.zip";
    QFile f(zipPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(ak3Zip);
    f.close();

    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelSU;
    cfg.koPath = zipPath; // AK3 包经 koPath 传入
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(ramdisk), cfg, out, &err), qPrintable(err));

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    QCOMPARE(info.kernel, fakeKernel); // kernel 段替换
    // ramdisk 未动：原 cpio 原样
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));
    QCOMPARE(entries.size(), 1);
    const TestCpioEntry *init = findEntry(entries, "init");
    QVERIFY(init);
    QCOMPARE(init->data, QByteArray("keep-me"));
}

void TestPatcher::ksuAnyKernel3DeflatedImage()
{
    // AK3 包内 Image 为 DEFLATE（真实预置内核包常见压缩）
    const QByteArray fakeKernel("deflated-kernel-image-bytes");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = dir.path() + "/anykernel3.zip";
    QFile f(zipPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildZip({{"META-INF/com/google/android/update-binary", "#!/sbin/sh", 0},
                      {"Image", fakeKernel, 8}})); // method=8 deflate
    f.close();

    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelSU;
    cfg.koPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(buildCpio({})), cfg, out, &err), qPrintable(err));

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    QCOMPARE(info.kernel, fakeKernel);
}

void TestPatcher::ksuAnyKernel3ZipWithoutKernelFails()
{
    // AK3 包缺内核文件 → 明确错误（不假装支持）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = dir.path() + "/bad.zip";
    QFile f(zipPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildZip({{"anykernel.sh", "kernel.string=NoImage", 0},
                      {"META-INF/com/google/android/update-binary", "#!/sbin/sh", 0}}));
    f.close();

    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelSU;
    cfg.koPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(buildCpio({})), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("Image", Qt::CaseInsensitive));
    QVERIFY(out.isEmpty());
}

void TestPatcher::ksuLkmPackZipExtracts()
{
    // ReSukiSU 形态：koPath 为 lkm-all.zip（内含 {kmi}_kernelsu.ko）→
    // 按 deviceKmi 提取对应 ko 做 LKM 注入
    const QByteArray koBytes("ko-for-android13-5.15");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = dir.path() + "/lkm-all.zip";
    const QString initPath = dir.path() + "/ksuinit";
    QFile f(zipPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildZip({{"android13-5.15_kernelsu.ko", koBytes, 0},
                      {"android15-6.6_kernelsu.ko", "wrong-ko", 0}}));
    f.close();
    QFile f2(initPath);
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write("ksuinit-bytes");
    f2.close();

    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ReSukiSU;
    cfg.koPath = zipPath;
    cfg.apkPath = initPath;
    cfg.deviceKmi = "android13-5.15"; // 设备指定（kernel 无 KMI 串）
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(buildCpio({{"init", kRegMode | 0750, "init-data"}})), cfg,
                     out, &err),
             qPrintable(err));

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));
    const TestCpioEntry *ko = findEntry(entries, "kernelsu.ko");
    QVERIFY(ko);
    QCOMPARE(ko->data, koBytes);
}

void TestPatcher::ksuLkmPackZipWithoutKmiFails()
{
    // 多 ko 包 + KMI 未知（kernel 无 KMI 串、未指定 deviceKmi）→ 拒绝静默
    // 任选第一个 ko，报错提示 deviceKmi（与 ksud parse_kmi 失败时要求手动
    // 指定 --kmi 一致）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = dir.path() + "/lkm-all.zip";
    QFile f(zipPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildZip({{"android13-5.15_kernelsu.ko", "ko-a", 0},
                      {"android15-6.6_kernelsu.ko", "ko-b", 0}}));
    f.close();

    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ReSukiSU;
    cfg.koPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(buildCpio({{"init", kRegMode | 0750, "init-data"}})), cfg,
                     out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("deviceKmi", Qt::CaseInsensitive));
    QVERIFY(out.isEmpty());
}

void TestPatcher::ksuDownloadUrlsKnown()
{
    // 各变体下载源（联网验证 2026-08）：
    QVERIFY(patcher::KernelSuPatcher::koUrl("official", "android13-5.15")
                .toString()
                .contains("tiann/KernelSU"));
    QVERIFY(patcher::KernelSuPatcher::koUrl("official", "android13-5.15")
                .toString()
                .endsWith("android13-5.15_kernelsu.ko"));
    QVERIFY(patcher::KernelSuPatcher::koUrl("next", "android13-5.15")
                .toString()
                .contains("KernelSU-Next"));
    QVERIFY(patcher::KernelSuPatcher::koUrl("suki", "android13-5.15")
                .toString()
                .contains("SukiSU-Ultra"));
    // ReSukiSU 无主仓 release 资产 → CI 发行通道 lkm-all.zip
    QVERIFY(patcher::KernelSuPatcher::koUrl("resuki", "android13-5.15")
                .toString()
                .contains("lkm-all.zip"));
    QVERIFY(patcher::KernelSuPatcher::koUrl("bogus", "android13-5.15").isEmpty());

    // ksuinit wrapper：official 直接资产；resuki ksuinit.zip；
    // next/suki 无独立资产 → 回退官方（文档化）
    QVERIFY(patcher::KernelSuPatcher::ksuinitUrl("official")
                .toString()
                .contains("tiann/KernelSU"));
    QVERIFY(patcher::KernelSuPatcher::ksuinitUrl("resuki")
                .toString()
                .endsWith("ksuinit.zip"));
    QVERIFY(!patcher::KernelSuPatcher::ksuinitUrl("next").isEmpty());
    QVERIFY(!patcher::KernelSuPatcher::ksuinitUrl("suki").isEmpty());
    QVERIFY(patcher::KernelSuPatcher::ksuinitUrl("bogus").isEmpty());

    // 7 个 KMI 全覆盖（四渠道资产一致，联网验证）
    const QStringList kmis = patcher::KernelSuPatcher::supportedKmis("official");
    QCOMPARE(kmis.size(), 7);
    QVERIFY(kmis.contains("android13-5.15"));
    QVERIFY(kmis.contains("android16-6.12"));
    QCOMPARE(patcher::KernelSuPatcher::supportedKmis("next"), kmis);
    QCOMPARE(patcher::KernelSuPatcher::supportedKmis("suki"), kmis);
    QCOMPARE(patcher::KernelSuPatcher::supportedKmis("resuki"), kmis);
    QVERIFY(patcher::KernelSuPatcher::supportedKmis("bogus").isEmpty());

    QCOMPARE(patcher::KernelSuPatcher::assetKey("resuki"), QStringLiteral("ksu-resuki"));
}

QTEST_APPLESS_MAIN(TestPatcher)
#include "test_patcher.moc"
