#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "root_patcher/root_patcher.h"
#include "root_patcher/magisk_patcher.h"
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
// 布局：1632B 头补零到 4096 页 → kernel 4096B（页对齐）→ ramdisk（页对齐结尾）。
static QByteArray buildBootV0(const QByteArray &ramdisk)
{
    QByteArray hdr(1632, 0);
    hdr.replace(0, 8, "ANDROID!");
    auto put32 = [&](int off, quint32 v) {
        hdr[off] = char(v);
        hdr[off + 1] = char(v >> 8);
        hdr[off + 2] = char(v >> 16);
        hdr[off + 3] = char(v >> 24);
    };
    put32(8, 4096);                    // kernel_size
    put32(16, static_cast<quint32>(ramdisk.size())); // ramdisk_size
    put32(24, 0);                      // second_size
    put32(36, 4096);                   // page_size
    put32(40, 0);                      // header_version
    QByteArray out = hdr;
    while (out.size() % 4096)
        out.append('\0');
    out.append(4096, 'K'); // kernel
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
    void nonCpioRamdiskFails();
    void injectUncompressedRamdisk();
    void injectGzipRamdisk();
    void injectDeflatedApk();
    void injectFallsBackAbi();
    void downloadUrlKnown();
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
    // 未实现类型返回 nullptr（不崩溃），由后续任务 C4+ 扩展
    for (patcher::RootType t : {patcher::RootType::KernelSU, patcher::RootType::KernelSU_Next,
                                patcher::RootType::SukiSU, patcher::RootType::ReSukiSU,
                                patcher::RootType::APatch, patcher::RootType::KernelPatch,
                                patcher::RootType::RamdiskSu, patcher::RootType::ModuleInstall})
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
    patcher::MagiskPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk;
    cfg.apkPath = "/nonexistent.apk";
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray("not a boot image at all"), cfg, out, &err));
    QVERIFY(!err.isEmpty());
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

QTEST_APPLESS_MAIN(TestPatcher)
#include "test_patcher.moc"
