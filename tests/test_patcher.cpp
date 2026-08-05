#include <QtTest>
#include <QTemporaryDir>
#include <QFile>
#include "root_patcher/root_patcher.h"
#include "root_patcher/magisk_patcher.h"
#include "root_patcher/kernelsu_patcher.h"
#include "root_patcher/apatch_patcher.h"
#include "root_patcher/ramdisk_su_patcher.h"
#include "root_patcher/module_installer.h"
#include "root_patcher/zip_util.h"
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

// ---- C5 测试工具：mock kptools ----
// 行为完全由输入内容驱动（无外部开关文件），实现与官方 kptools 的
// 命令面一致：unpack <boot> / -i kernel -f / -p -i ... -k ... -o ... /
// -l -i kernel / repack <boot>。每次调用把全部参数追加进 mock-calls.log
// （repack 时并入 new-boot.img，供测试断言官方参数形态）。
// 开关（写入 boot 镜像内核段/内容）：
//   "UNPACK_FAIL" → unpack 退出非零
//   "REPACK_FAIL" → repack 退出非零
//   "FLAG_FAIL"   → -f 自身退出非零（kptools 解析失败，门禁 rc≠0 分支）
//   "NO_KALLSYMS" → -f 不输出 CONFIG_KALLSYMS=y（门禁内容分支）
//   "PATCH_FAIL"  → -p 退出非零
//   "NOPATCH"     → -p 成功但不写入 KP1158 标记（-l 报 patched=false）
QByteArray apatchMockKptoolsScript()
{
    return QByteArray(R"(#!/bin/sh
echo "invoked: $*" >> mock-calls.log
if [ "$1" = "unpack" ]; then
  grep -q "UNPACK_FAIL" "$2" && exit 7
  cp "$2" kernel
  printf 'MOCKKERNEL' >> kernel
  exit 0
fi
if [ "$1" = "repack" ]; then
  grep -q "REPACK_FAIL" "$2" && exit 8
  cp "$2" new-boot.img
  printf 'MOCKREPACK' >> new-boot.img
  cat mock-calls.log >> new-boot.img 2>/dev/null
  [ -f kernel ] && cat kernel >> new-boot.img
  exit 0
fi
img=; out=; cmd=
while [ $# -gt 0 ]; do
  case "$1" in
    -i) img="$2"; shift 2 ;;
    -k) shift 2 ;;
    -o) out="$2"; shift 2 ;;
    -p) cmd=patch; shift ;;
    -f) cmd=flag; shift ;;
    -l) cmd=list; shift ;;
    -s|-S) shift 2 ;;
    *) shift ;;
  esac
done
if [ "$cmd" = "patch" ]; then
  grep -q "PATCH_FAIL" "$img" && exit 9
  cp "$img" "$out"
  grep -q "NOPATCH" "$img" || printf 'KP1158MOCKPATCH' >> "$out"
  exit 0
fi
if [ "$cmd" = "flag" ]; then
  grep -q "FLAG_FAIL" "$img" && exit 10
  grep -q "NO_KALLSYMS" "$img" || printf 'CONFIG_KALLSYMS=y\nCONFIG_KALLSYMS_ALL=y\n'
  exit 0
fi
if [ "$cmd" = "list" ]; then
  if grep -q "KP1158" "$img"; then printf 'patched=true\n'; else printf 'patched=false\n'; fi
  exit 0
fi
exit 0
)");
}

// 构造含 libkptools.so（mock 脚本）+ assets/kpimg 的 APatch 管理器 APK。
QByteArray buildApatchApk(const QByteArray &mockKptools,
                          const QByteArray &kpimg = QByteArray("mock-kpimg-data"))
{
    return buildZip({
        {"lib/arm64-v8a/libkptools.so", mockKptools, 0},
        {"lib/arm64-v8a/libkpatch.so", QByteArray("mock"), 0},
        {"assets/kpimg", kpimg, 0},
    });
}

// 构造 C5 测试用 boot 镜像：kernel 段内容可控（mock 开关经此处写入）。
QByteArray buildApatchBoot(const QByteArray &kernelMark = QByteArray())
{
    QByteArray kernel = QByteArray(4096, 'K');
    if (!kernelMark.isEmpty())
        kernel = kernelMark;
    return buildBootV0(buildCpio({}), kernel, QByteArray("androidboot.test=1"));
}

// ---- C7 测试工具：SuperSU 式刷入包（update-binary + su + Superuser.apk）----
// 结构对齐真实产物（联网验证 SuperSU-v2.82-SR5 zip）：su 按 ABI 目录存放
//（arm64/su、arm/su、armv7/su、x86/su、x64/su、mips/su、mips64/su），
// META-INF/com/google/android/update-binary 为安装脚本。
QByteArray buildSupersuZip(const QByteArray &suBytes, int suMethod = 0)
{
    return buildZip({{"META-INF/com/google/android/update-binary", "#!/sbin/sh\n", 0},
                     {"arm64/su", suBytes, suMethod},
                     {"common/Superuser.apk", QByteArray("mock-superuser-apk"), 0}});
}

// 写 SuperSU zip 到临时目录，返回路径（空表示失败）。
QString writeSupersuZip(const QString &dirPath, const QByteArray &suBytes, int suMethod = 0)
{
    const QString path = dirPath + "/UPDATE-SuperSU.zip";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(buildSupersuZip(suBytes, suMethod));
    f.close();
    return path;
}

// ---- C8 测试工具：最小 Magisk 模块 zip（官方规范，联网验证 2026-08）----
// 结构对齐 topjohnwu/Magisk docs/guides.md：根级 module.prop（Magisk v28+ 静态
// 读取位置）+ 文件树 + META-INF/com/google/android/{update-binary,updater-script}
//（updater-script 须为 "#MAGISK"，recovery 刷入标识）。字段序为官方严格格式：
// id/name/version/versionCode/author/description/updateJson，LF 行尾。
const QByteArray kModuleProp =
    QByteArray("id=zygisk_lsposed\n"
               "name=LSPosed\n"
               "version=v1.9.2\n"
               "versionCode=7024\n"
               "author=LSPosed Developers\n"
               "description=Enhanced Xposed Framework\n"
               "updateJson=https://example.com/lsposed/update.json\n");

QByteArray buildModuleZip(const QByteArray &prop = kModuleProp,
                          const QByteArray &hosts = QByteArray("127.0.0.1 localhost\n"))
{
    return buildZip({{"module.prop", prop, 0},
                     {"system/etc/hosts", hosts, 0},
                     {"zygisk/arm64-v8a.so", QByteArray("\x7f"
                                                        "ELF" "mock-zygisk-lib"),
                      8}, // DEFLATE（真实模块条目压缩形态）
                     {"service.sh", QByteArray("#!/system/bin/sh\n"), 0},
                     {"META-INF/com/google/android/update-binary",
                      QByteArray("#MAGISK module installer\n"), 0},
                     {"META-INF/com/google/android/updater-script", QByteArray("#MAGISK\n"), 0}});
}

// 写模块 zip 到临时目录，返回路径（空表示失败）。
QString writeModuleZip(const QString &dirPath, const QByteArray &zip)
{
    const QString path = dirPath + "/module.zip";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(zip);
    f.close();
    return path;
}

// 已 Magisk 修补 ramdisk（C3 注入产物形态：.backup 链标记 → magiskd 运行时加载器）
QByteArray buildMagiskPatchedRamdisk()
{
    return buildCpio({{"init", kRegMode | 0750, "magiskinit-mock"},
                      {".backup", kDirMode, {}},
                      {".backup/init", kRegMode | 0750, "original-init"}});
}

// 已 KernelSU 修补 ramdisk（ksud boot-patch 形态，联网验证：原 init 改名
// init.real + 新 init 包装 + kernelsu.ko）
QByteArray buildKsuPatchedRamdisk()
{
    return buildCpio({{"init", kRegMode | 0755, "ksuinit-wrapper-mock"},
                      {"init.real", kRegMode | 0755, "original-init"},
                      {"kernelsu.ko", kRegMode | 0755, "mock-kernelsu-ko"}});
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
    void ksuMissingWrapperNextAnnotatesFallback();
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

    // ---- C5: APatch/KernelPatch 系 ----
    void apatchInjectFromApk();
    void apatchInjectFromManualDir();
    void apatchMissingSourceFails();
    void apatchBothSourcesFails();
    void apatchApkMissingEntryFails();
    void apatchApkNotZipFails();
    void apatchKpatchDirMissingFilesFails();
    void apatchKpatchPathNotDirFails();
    void apatchManualDirPrefersHostPlatform();
    void apatchManualDirSkips7zArchive();
    void apatchInvalidBootFails();
    void apatchNoKallsymsFails();
    void apatchFlagFailFails();
    void apatchUnpackFailFails();
    void apatchPatchFailFails();
    void apatchNotPatchedFails();
    void apatchRepackFailFails();
    void apatchNullErrorNoCrash();
    void apatchFactoryAndSources();

    // ---- C6: 文件级入口 patchFile + 自动备份 ----
    void patchFileMagiskHappyPath();
    void patchFileNonexistentFails();
    void patchFileInvalidBootFails();
    void patchFileExistingBackupFails();
    void patchFileKitsuneMissingApkHintsManual();
    void patchFileUnimplementedTypeFails();
    void patchFileFactoryDispatchKernelSu();
    void patchFileNullArgsNoCrash();

    // ---- C7: SuperSU 老设备 ramdisk 注入 ----
    void suInjectHappyPath();
    void suInjectGzipRamdisk();
    void suInjectFallsBackAbi();
    void suInjectDeflatedSu();
    void suMissingZipFails();
    void suNonexistentZipFails();
    void suZipNotZipFails();
    void suZipWithoutSuFails();
    void suNotElfFails();
    void suInvalidBootFails();
    void suNoRamdiskFails();
    void suNoInitRcFails();
    void suRepatchFails();
    void suMagiskPatchedFails();
    void suNullErrorNoCrash();
    void suPatchFileHappyPath();
    void suPatchFileRollbackNoBackup();
    void moduleFactoryCreate();
    void moduleInjectHappyPath();
    void moduleInjectGzipFrameworkGate();
    void moduleKsuFrameworkGate();
    void moduleNoBootImageOk();
    void moduleMissingZipFails();
    void moduleNonexistentZipFails();
    void moduleZipNotZipFails();
    void moduleWithoutPropFails();
    void moduleBadIdFails();
    void moduleBadVersionCodeFails();
    void moduleInstallShFails();
    void moduleUpdaterScriptNotMagiskFails();
    void moduleUnpatchedImageFails();
    void moduleInvalidBootFails();
    void moduleNullErrorNoCrash();
    void modulePatchFileRejectsModuleType();
    void moduleOversizeZipFails();
    void moduleTraversalEntryFails();
    void zipPrefixPreferExact();
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
    // C5：APatch/KernelPatch 两入口映射到 APatchPatcher（详见
    // apatchFactoryAndSources）
    for (patcher::RootType t : {patcher::RootType::APatch,
                                patcher::RootType::KernelPatch}) {
        std::unique_ptr<patcher::RootPatcher> p(patcher::RootPatcher::create(t));
        QVERIFY2(p.get(), "create() 返回 nullptr");
        QVERIFY(dynamic_cast<patcher::APatchPatcher *>(p.get()));
    }
    // C7：RamdiskSu 映射到 RamdiskSuPatcher；未实现类型返回 nullptr（不崩溃）
    {
        std::unique_ptr<patcher::RootPatcher> p(patcher::RootPatcher::create(
            patcher::RootType::RamdiskSu));
        QVERIFY2(p.get(), "create() 返回 nullptr");
        QVERIFY(dynamic_cast<patcher::RamdiskSuPatcher *>(p.get()));
    }
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

void TestPatcher::ksuMissingWrapperNextAnnotatesFallback()
{
    // 审查 Minor：next/suki 不发布独立 ksuinit 资产 → 回退官方 KernelSU
    // ksuinit。缺 wrapper 的错误文案必须标注该来源与"fork 定制行为不保证"
    //（修复前文案无来源标注，用户误以为下载的是 Next 定制版）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString koPath = dir.path() + "/kernelsu.ko";
    QFile f(koPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("fake kernelsu.ko module bytes");
    f.close();

    patcher::KernelSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelSU_Next;
    cfg.koPath = koPath; // apkPath 未指定
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(buildCpio({{"init", kRegMode | 0750, "init-data"}})), cfg, out,
                     &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("ksuinit", Qt::CaseInsensitive));
    QVERIFY(err.contains("官方", Qt::CaseInsensitive));
    QVERIFY(err.contains("不保证"));
    // official 变体（无回退语义）不附加该标注
    patcher::PatchConfig officialCfg = cfg;
    officialCfg.type = patcher::RootType::KernelSU;
    err.clear();
    QVERIFY(!p.patch(buildBootV0(buildCpio({{"init", kRegMode | 0750, "init-data"}})),
                     officialCfg, out, &err));
    QVERIFY(!err.contains("不保证"));
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

// ================= C5: APatch/KernelPatch 系 =================

void TestPatcher::apatchInjectFromApk()
{
    // 官方路径：APatch 管理器 APK 提取 libkptools.so + assets/kpimg（与
    // APatch App prepare() 一致）→ 官方 kptools 流程（mock）。断言：
    //   1) 输出 boot 镜像含原始 boot 段与 MOCKREPACK（repack 产物）
    //   2) 含 KP1158 标记 —— 注入后 boot 镜像含 kpatch（kpimg KP_MAGIC）标记
    //   3) mock-calls.log（并入输出）证明官方命令形态：
    //      unpack boot.img → -i kernel -f → -p -i kernel.ori -k kpimg
    //      -o kernel → -l -i kernel → repack boot.img
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/apatch.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildApatchApk(apatchMockKptoolsScript()));
    f.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.apkPath = apkPath;
    const QByteArray boot = buildApatchBoot();
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(boot, cfg, out, &err), qPrintable(err));
    QVERIFY(err.isEmpty());
    QVERIFY(out.contains(boot)); // 原始 boot 段透传进 repack 产物
    QVERIFY(out.contains("KP1158")); // kpatch 标记
    QVERIFY(out.contains("KP1158MOCKPATCH"));
    QVERIFY(out.contains("MOCKREPACK"));
    QVERIFY(out.contains("MOCKKERNEL"));
    QVERIFY(out.contains("invoked: unpack"));
    QVERIFY(out.contains("invoked: repack"));
    QVERIFY(out.contains("-i kernel -f"));
    QVERIFY(out.contains("-p -i kernel.ori -k kpimg -o kernel"));
    QVERIFY(out.contains("-l -i kernel"));
}

void TestPatcher::apatchInjectFromManualDir()
{
    // 手动路径：kpatchPath 目录含 kptools+kpimg 文件（KernelPatch release
    // 预编译资产解包形态，文件名前缀匹配）；kptools 无执行位也须成功
    //（patcher 负责 chmod +x）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile kp(dir.path() + "/kptools-linux");
    QVERIFY(kp.open(QIODevice::WriteOnly));
    kp.write(apatchMockKptoolsScript());
    kp.close();
    QFile ki(dir.path() + "/kpimg-android");
    QVERIFY(ki.open(QIODevice::WriteOnly));
    ki.write("mock-kpimg-data");
    ki.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelPatch;
    cfg.kpatchPath = dir.path();
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildApatchBoot(), cfg, out, &err), qPrintable(err));
    QVERIFY(err.isEmpty());
    QVERIFY(out.contains("KP1158"));
}

void TestPatcher::apatchMissingSourceFails()
{
    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch; // apkPath 与 kpatchPath 均未指定
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildApatchBoot(), cfg, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestPatcher::apatchBothSourcesFails()
{
    // 两种来源互斥：同时指定会误导（版本不一致风险）→ 明确报错
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/apatch.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildApatchApk(apatchMockKptoolsScript()));
    f.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.apkPath = apkPath;
    cfg.kpatchPath = dir.path();
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildApatchBoot(), cfg, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestPatcher::apatchApkMissingEntryFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/apatch.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildZip({{"lib/arm64-v8a/libmagiskinit.so", "wrong patcher", 0}}));
    f.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildApatchBoot(), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("libkptools", Qt::CaseInsensitive));
}

void TestPatcher::apatchApkNotZipFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/apatch.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("not a zip archive");
    f.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildApatchBoot(), cfg, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestPatcher::apatchKpatchDirMissingFilesFails()
{
    // 目录存在但缺 kptools / kpimg
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile ki(dir.path() + "/kpimg");
    QVERIFY(ki.open(QIODevice::WriteOnly));
    ki.write("mock-kpimg-data");
    ki.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.kpatchPath = dir.path();
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildApatchBoot(), cfg, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestPatcher::apatchKpatchPathNotDirFails()
{
    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.kpatchPath = "/nonexistent/patch-dir";
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildApatchBoot(), cfg, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestPatcher::apatchManualDirPrefersHostPlatform()
{
    // 审查 Minor：手动目录含多平台 kptools 候选（kptools-linux/mac/win +
    // kptools-msys2-win.7z）时按 Q_OS_* 选宿主平台二进制 —— 修复前按字母序
    // 取首个（macOS/Windows 宿主会选到 kptools-linux 而非本平台二进制）。
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString hostTag;
#if defined(Q_OS_LINUX)
    hostTag = QStringLiteral("linux");
#elif defined(Q_OS_MACOS)
    hostTag = QStringLiteral("mac");
#elif defined(Q_OS_WIN)
    hostTag = QStringLiteral("win");
#else
    hostTag = QStringLiteral("linux"); // 未知平台：退化为常见名
#endif
    // 宿主平台候选：基 mock + repack 时补打选中标记（断言用）
    QByteArray good = apatchMockKptoolsScript();
    QVERIFY(good.contains("printf 'MOCKREPACK' >> new-boot.img"));
    good.replace("printf 'MOCKREPACK' >> new-boot.img",
                 "printf 'MOCKREPACK' >> new-boot.img\n"
                 "printf 'SEL=" + hostTag.toLatin1() + "' >> new-boot.img");
    // 其余平台候选：恒失败脚本（若被选中 → 修补失败）。另加按字母序排最前
    // 的无关前缀候选 kptools-abc —— 修复前按字母序取首个会选中它而失败；
    // 修复后按平台排名选中宿主平台候选（该 decoy 使测试在 Linux 宿主也能
    // 复现旧缺陷，不依赖 linux/mac/win 字母序恰好与宿主一致的巧合）
    const QByteArray bad = QByteArray("#!/bin/sh\nexit 3\n");

    auto writeFile = [&dir](const QString &name, const QByteArray &data) -> bool {
        QFile f(dir.path() + QLatin1Char('/') + name);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(data);
        f.close();
        return true;
    };
    QVERIFY(writeFile(QStringLiteral("kptools-") + hostTag, good));
    QVERIFY(writeFile(QStringLiteral("kptools-abc"), bad)); // 字母序 decoy
    for (const QString &o : {QStringLiteral("linux"), QStringLiteral("mac"),
                             QStringLiteral("win")}) {
        if (o != hostTag)
            QVERIFY(writeFile(QStringLiteral("kptools-") + o, bad));
    }
    QVERIFY(writeFile(QStringLiteral("kptools-msys2-win.7z"),
                      QByteArray("7z-archive-garbage-not-an-elf")));
    QVERIFY(writeFile(QStringLiteral("kpimg-android"), QByteArray("mock-kpimg-data")));

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelPatch;
    cfg.kpatchPath = dir.path();
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildApatchBoot(), cfg, out, &err), qPrintable(err));
    QVERIFY(err.isEmpty());
    QVERIFY(out.contains("SEL=" + hostTag.toLatin1())); // 选中宿主平台候选
    QVERIFY(out.contains("KP1158"));
}

void TestPatcher::apatchManualDirSkips7zArchive()
{
    // 审查 Minor 负例：目录仅含 kptools-msys2-win.7z（压缩包不可执行）→
    // 明确报错而非把 .7z 当二进制 exec（修复前 .7z 会被按前缀选中）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QFile kp(dir.path() + "/kptools-msys2-win.7z");
    QVERIFY(kp.open(QIODevice::WriteOnly));
    kp.write("7z-compressed-archive-bytes-not-an-elf");
    kp.close();
    QFile ki(dir.path() + "/kpimg-android");
    QVERIFY(ki.open(QIODevice::WriteOnly));
    ki.write("mock-kpimg-data");
    ki.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.kpatchPath = dir.path();
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildApatchBoot(), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    // 新行为：候选全部被跳过 → 目录扫描报"未找到"（修复前会把 .7z 当
    // 二进制 exec，错误来自 kptools 无法启动而非扫描层）
    QVERIFY(err.contains("未找到"));
    QVERIFY(out.isEmpty());
}

void TestPatcher::apatchInvalidBootFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/apatch.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildApatchApk(apatchMockKptoolsScript()));
    f.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray("not a boot image at all"), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("boot", Qt::CaseInsensitive));
}

void TestPatcher::apatchNoKallsymsFails()
{
    // CONFIG_KALLSYMS 门禁（官方 boot_patch.sh: kptools -i kernel -f |
    // grep CONFIG_KALLSYMS=y）：内核未启用 → 明确报错，不得继续
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/apatch.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildApatchApk(apatchMockKptoolsScript()));
    f.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildApatchBoot(QByteArray("NO_KALLSYMS-kernel")), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("CONFIG_KALLSYMS", Qt::CaseInsensitive));
}

void TestPatcher::apatchFlagFailFails()
{
    // kptools -f 自身失败（rc≠0，如内核解析异常）→ 门禁 rc≠0 分支：
    // 必须走"IKCONFIG 解析失败"错误路径而非继续
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/apatch.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildApatchApk(apatchMockKptoolsScript()));
    f.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildApatchBoot(QByteArray("FLAG_FAIL-kernel")), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("IKCONFIG", Qt::CaseInsensitive));
}

void TestPatcher::apatchUnpackFailFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/apatch.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildApatchApk(apatchMockKptoolsScript()));
    f.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildApatchBoot(QByteArray("UNPACK_FAIL-kernel")), cfg, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestPatcher::apatchPatchFailFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/apatch.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildApatchApk(apatchMockKptoolsScript()));
    f.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildApatchBoot(QByteArray("PATCH_FAIL-kernel")), cfg, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestPatcher::apatchNotPatchedFails()
{
    // -p 成功但 -l 未检测到 patched=true（如 kpimg 无效被 kptools 拒绝
    // 或布局不符）→ 必须拒绝交付，不得把未成功修补的镜像当成功
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/apatch.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildApatchApk(apatchMockKptoolsScript()));
    f.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildApatchBoot(QByteArray("NOPATCH-kernel")), cfg, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestPatcher::apatchRepackFailFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/apatch.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildApatchApk(apatchMockKptoolsScript()));
    f.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.apkPath = apkPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildApatchBoot(QByteArray("REPACK_FAIL-kernel")), cfg, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestPatcher::apatchNullErrorNoCrash()
{
    // error=nullptr 契约（审查修复回归护栏）：全部失败分支不得解引用 error。
    // 各开关内核触发对应失败分支，error 传 nullptr —— 仅断言不崩溃且返回 false。
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString apkPath = dir.path() + "/apatch.apk";
    QFile f(apkPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildApatchApk(apatchMockKptoolsScript()));
    f.close();

    patcher::APatchPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::APatch;
    cfg.apkPath = apkPath;
    QByteArray out;
    // 流程失败分支：门禁 rc≠0 / 门禁内容 / unpack / patch / 未修补 / repack
    for (const QByteArray &mark : {QByteArray("FLAG_FAIL-kernel"),
                                   QByteArray("NO_KALLSYMS-kernel"),
                                   QByteArray("UNPACK_FAIL-kernel"),
                                   QByteArray("PATCH_FAIL-kernel"),
                                   QByteArray("NOPATCH-kernel"),
                                   QByteArray("REPACK_FAIL-kernel")}) {
        out.clear();
        QVERIFY(!p.patch(buildApatchBoot(mark), cfg, out, nullptr));
    }
    // 来源/前置校验失败分支同样不得崩溃
    patcher::PatchConfig noSource;
    noSource.type = patcher::RootType::APatch;
    QVERIFY(!p.patch(buildApatchBoot(), noSource, out, nullptr));
    QVERIFY(!p.patch(QByteArray("garbage, not boot"), cfg, out, nullptr));
}

void TestPatcher::apatchFactoryAndSources()
{
    // 工厂：APatch/KernelPatch 两入口均映射到 APatchPatcher
    for (patcher::RootType t : {patcher::RootType::APatch,
                                patcher::RootType::KernelPatch}) {
        std::unique_ptr<patcher::RootPatcher> p(patcher::RootPatcher::create(t));
        QVERIFY2(p.get(), "create() 返回 nullptr");
        QVERIFY(dynamic_cast<patcher::APatchPatcher *>(p.get()));
    }
    // 下载源（联网验证 2026-08-05）：APatch 官方 release 单 APK 资产，
    // 缓存 key 基名 apatch（版本化子目录由调用方追加，如 apatch-11219）
    QVERIFY(patcher::APatchPatcher::apkDownloadUrl()
                .toString()
                .contains("bmax121/APatch"));
    QCOMPARE(patcher::APatchPatcher::assetKey(), QStringLiteral("apatch"));
}

// ================= C6: 文件级入口 patchFile + 自动备份 =================

void TestPatcher::patchFileMagiskHappyPath()
{
    // C6 契约：读 boot 文件 → 工厂派发 Magisk 系 → 写 "<基名>_patched.img"
    // 与 "<源文件>.orig.bak"（备份为原文件字节原样）。断言：outPath 指向
    // _patched.img、备份 == 原文件、产物可解析为合法 boot 且注入生效。
    const QByteArray initPayload("original init for patchFile");
    const QByteArray fakeMagiskinit("magiskinit-for-patchfile-entry");
    const QByteArray bootBytes =
        buildBootV0(buildCpio({{"init", kRegMode | 0750, initPayload}}));

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bootPath = dir.path() + "/boot.img";
    const QString apkPath = dir.path() + "/magisk.apk";
    QFile bf(bootPath);
    QVERIFY(bf.open(QIODevice::WriteOnly));
    bf.write(bootBytes);
    bf.close();
    QFile af(apkPath);
    QVERIFY(af.open(QIODevice::WriteOnly));
    af.write(buildZip({{"lib/arm64-v8a/libmagiskinit.so", fakeMagiskinit, 0}}));
    af.close();

    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk;
    cfg.apkPath = apkPath;
    QString outPath;
    QString err;
    QVERIFY2(patcher::patchFile(bootPath, cfg, &outPath, &err), qPrintable(err));
    QVERIFY(err.isEmpty());

    const QString patchedPath = dir.path() + "/boot_patched.img";
    const QString backupPath = bootPath + ".orig.bak";
    QCOMPARE(outPath, patchedPath);
    QVERIFY(QFile::exists(patchedPath));
    QVERIFY(QFile::exists(backupPath));

    QFile bak(backupPath);
    QVERIFY(bak.open(QIODevice::ReadOnly));
    QCOMPARE(bak.readAll(), bootBytes); // 备份 = 原文件原样
    bak.close();

    QFile pf(patchedPath);
    QVERIFY(pf.open(QIODevice::ReadOnly));
    const QByteArray patchedBytes = pf.readAll();
    pf.close();
    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(patchedBytes, info)); // 产物可解析
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));
    const TestCpioEntry *init = findEntry(entries, "init");
    QVERIFY(init);
    QCOMPARE(init->data, fakeMagiskinit); // 注入生效
}

void TestPatcher::patchFileNonexistentFails()
{
    // 路径不存在 / 空路径 / 目录：明确失败且 outPath 置空，不崩溃
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk;
    QString outPath;
    QString err;
    QVERIFY(!patcher::patchFile("/nonexistent/boot.img", cfg, &outPath, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(outPath.isEmpty());

    err.clear();
    QVERIFY(!patcher::patchFile(QString(), cfg, &outPath, &err));
    QVERIFY(!err.isEmpty());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    err.clear();
    QVERIFY(!patcher::patchFile(dir.path(), cfg, &outPath, &err)); // 目录而非文件
    QVERIFY(!err.isEmpty());
}

void TestPatcher::patchFileInvalidBootFails()
{
    // 垃圾 boot 内容：失败且不得留下 _patched.img / .orig.bak（失败不落产物）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bootPath = dir.path() + "/boot.img";
    const QString apkPath = dir.path() + "/magisk.apk";
    QFile bf(bootPath);
    QVERIFY(bf.open(QIODevice::WriteOnly));
    bf.write("not a boot image at all");
    bf.close();
    QFile af(apkPath);
    QVERIFY(af.open(QIODevice::WriteOnly));
    af.write(buildZip({{"lib/arm64-v8a/libmagiskinit.so", "fake magiskinit bytes", 0}}));
    af.close();

    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk;
    cfg.apkPath = apkPath;
    QString outPath;
    QString err;
    QVERIFY(!patcher::patchFile(bootPath, cfg, &outPath, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("boot", Qt::CaseInsensitive));
    QVERIFY(outPath.isEmpty());
    QVERIFY(!QFile::exists(dir.path() + "/boot_patched.img"));
    QVERIFY(!QFile::exists(bootPath + ".orig.bak"));
}

void TestPatcher::patchFileExistingBackupFails()
{
    // 已存在 .orig.bak（上次修补痕迹）→ 拒绝覆盖备份，防止原厂镜像不可再生
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bootPath = dir.path() + "/boot.img";
    QFile bf(bootPath);
    QVERIFY(bf.open(QIODevice::WriteOnly));
    bf.write(buildBootV0(buildCpio({})));
    bf.close();
    QFile bak(bootPath + ".orig.bak");
    QVERIFY(bak.open(QIODevice::WriteOnly));
    bak.write("previous backup");
    bak.close();

    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk;
    QString outPath;
    QString err;
    QVERIFY(!patcher::patchFile(bootPath, cfg, &outPath, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("orig.bak", Qt::CaseInsensitive));
}

void TestPatcher::patchFileKitsuneMissingApkHintsManual()
{
    // 遗留吸收（C3 concern）：Kitsune 无官方下载源 → 错误须提示手动指定 APK
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bootPath = dir.path() + "/boot.img";
    QFile bf(bootPath);
    QVERIFY(bf.open(QIODevice::WriteOnly));
    bf.write(buildBootV0(buildCpio({})));
    bf.close();

    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Kitsune; // apkPath 未指定
    QString outPath;
    QString err;
    QVERIFY(!patcher::patchFile(bootPath, cfg, &outPath, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("手动", Qt::CaseInsensitive));
    QVERIFY(outPath.isEmpty());
}

void TestPatcher::patchFileUnimplementedTypeFails()
{
    // 工厂 create() 返回 nullptr 的类型（ModuleInstall 未实现）→ 明确失败且不落产物
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bootPath = dir.path() + "/boot.img";
    QFile bf(bootPath);
    QVERIFY(bf.open(QIODevice::WriteOnly));
    bf.write(buildBootV0(buildCpio({})));
    bf.close();

    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    QString outPath;
    QString err;
    QVERIFY(!patcher::patchFile(bootPath, cfg, &outPath, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(outPath.isEmpty());
    QVERIFY(!QFile::exists(bootPath + ".orig.bak"));
    QVERIFY(!QFile::exists(dir.path() + "/boot_patched.img"));
}

void TestPatcher::patchFileFactoryDispatchKernelSu()
{
    // 工厂派发证明：KernelSU 系经同一入口走 KernelSuPatcher（LKM 注入）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bootPath = dir.path() + "/boot.img";
    const QString koPath = dir.path() + "/kernelsu.ko";
    const QString initPath = dir.path() + "/ksuinit";
    QFile bf(bootPath);
    QVERIFY(bf.open(QIODevice::WriteOnly));
    bf.write(buildBootV0(buildCpio({{"init", kRegMode | 0750, "init-data"}})));
    bf.close();
    QFile kf(koPath);
    QVERIFY(kf.open(QIODevice::WriteOnly));
    kf.write("ko-bytes-for-patchfile");
    kf.close();
    QFile wf(initPath);
    QVERIFY(wf.open(QIODevice::WriteOnly));
    wf.write("ksuinit-bytes-for-patchfile");
    wf.close();

    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::KernelSU;
    cfg.koPath = koPath;
    cfg.apkPath = initPath;
    QString outPath;
    QString err;
    QVERIFY2(patcher::patchFile(bootPath, cfg, &outPath, &err), qPrintable(err));

    const QString patchedPath = dir.path() + "/boot_patched.img";
    QCOMPARE(outPath, patchedPath);
    QVERIFY(QFile::exists(patchedPath));
    QVERIFY(QFile::exists(bootPath + ".orig.bak"));

    QFile pf(patchedPath);
    QVERIFY(pf.open(QIODevice::ReadOnly));
    const QByteArray patchedBytes = pf.readAll();
    pf.close();
    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(patchedBytes, info));
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));
    const TestCpioEntry *init = findEntry(entries, "init");
    QVERIFY(init);
    QCOMPARE(init->data, QByteArray("ksuinit-bytes-for-patchfile"));
    const TestCpioEntry *ko = findEntry(entries, "kernelsu.ko");
    QVERIFY(ko);
    QCOMPARE(ko->data, QByteArray("ko-bytes-for-patchfile"));
}

void TestPatcher::patchFileNullArgsNoCrash()
{
    // error/outPath 可传 nullptr（全局契约）：全部失败分支不得崩溃
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bootPath = dir.path() + "/boot.img";
    QFile bf(bootPath);
    QVERIFY(bf.open(QIODevice::WriteOnly));
    bf.write("garbage bytes");
    bf.close();

    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::Magisk;
    QVERIFY(!patcher::patchFile(bootPath, cfg, nullptr, nullptr));
    QVERIFY(!patcher::patchFile(bootPath, cfg, nullptr, nullptr));
    // 有效 boot + 未实现类型：同样不得崩溃
    QFile bf2(bootPath);
    QVERIFY(bf2.open(QIODevice::WriteOnly));
    bf2.write(buildBootV0(buildCpio({})));
    bf2.close();
    cfg.type = patcher::RootType::ModuleInstall;
    QVERIFY(!patcher::patchFile(bootPath, cfg, nullptr, nullptr));
}

// ================= C7: SuperSU 老设备 ramdisk 注入 =================
// 机制（联网验证 2026-08-05）：SuperSU 刷入包内 su 与 daemonsu 为同一
// 文件（update-binary cp_perm 同一 $BIN/su 到 /system/xbin/su、
// /system/bin/.ext/.su、/system/xbin/daemonsu）；boot 侧注入为 dkp 内核
// 包 rd/ 目录同款做法 —— ramdisk 放入 init.superuser.rc（service daemonsu
// /system/xbin/daemonsu --auto-daemon）+ init.rc 追加 import /init.superuser.rc。

void TestPatcher::suInjectHappyPath()
{
    // 完整注入链：ZIP 提取 su → ramdisk 加入 sbin/su + init.superuser.rc
    // → init.rc 追加 import。断言：sbin/su 内容与模式（0755）、
    // init.superuser.rc 服务模板（0750）、init.rc import 行、原 init 不动。
    const QByteArray suBytes("\x7f"
                             "ELF" "fake-superuser-su-binary-0123456789");
    const QByteArray initPayload("original init payload for supersu");
    const QByteArray initRc("on boot\n    class_start core\n");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeSupersuZip(dir.path(), suBytes);
    QVERIFY(!zipPath.isEmpty());

    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QByteArray out;
    QString err;
    const QByteArray ramdisk = buildCpio({{"init", kRegMode | 0750, initPayload},
                                          {"init.rc", kRegMode | 0644, initRc}});
    QVERIFY2(p.patch(buildBootV0(ramdisk), cfg, out, &err), qPrintable(err));
    QVERIFY(err.isEmpty());

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));

    // sbin/su：提取的 su 二进制（0755 常规文件，early boot 可执行副本）
    const TestCpioEntry *su = findEntry(entries, "sbin/su");
    QVERIFY(su);
    QCOMPARE(su->data, suBytes);
    QCOMPARE(su->mode & 0170000, kRegMode);
    QCOMPARE(su->mode & 0777, 0755u);

    // init.superuser.rc：daemonsu 服务模板（dkp 内核 rd/ 同款，联网验证）
    const TestCpioEntry *rc = findEntry(entries, "init.superuser.rc");
    QVERIFY(rc);
    QCOMPARE(rc->mode & 0170000, kRegMode);
    QCOMPARE(rc->mode & 0777, 0750u); // sukernel --cpio-add 750 同款
    QVERIFY(rc->data.contains("service daemonsu /system/xbin/daemonsu --auto-daemon"));
    QVERIFY(rc->data.contains("class core"));
    QVERIFY(rc->data.contains("user root"));
    QVERIFY(rc->data.contains("oneshot"));

    // init.rc：原内容保留 + 末尾追加 import 行（仅一行）
    const TestCpioEntry *initRcE = findEntry(entries, "init.rc");
    QVERIFY(initRcE);
    QCOMPARE(initRcE->mode & 0170000, kRegMode);
    QCOMPARE(initRcE->mode & 0777, 0644u); // 原模式保留
    QVERIFY(initRcE->data.startsWith("on boot\n    class_start core\n"));
    QVERIFY(initRcE->data.endsWith("import /init.superuser.rc\n"));
    QCOMPARE(initRcE->data.count("import /init.superuser.rc"), 1);

    // 原 init 未被改动；注入条目恰好 3 个新条目（含 init.rc 替换）
    const TestCpioEntry *init = findEntry(entries, "init");
    QVERIFY(init);
    QCOMPARE(init->data, initPayload);
    QCOMPARE(entries.size(), 4);
}

void TestPatcher::suInjectGzipRamdisk()
{
    // gzip 压缩 ramdisk：重压保持原格式，注入内容一致
    const QByteArray suBytes("\x7f"
                             "ELF" "su-bytes-for-gzip-ramdisk");
    const QByteArray cpio = buildCpio({{"init", kRegMode | 0750, "init-data"},
                                       {"init.rc", kRegMode | 0644, "on boot\n"}});
    const QByteArray ramdiskComp = patcher::compressRamdisk(cpio, "gzip");
    QVERIFY(!ramdiskComp.isEmpty());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeSupersuZip(dir.path(), suBytes);
    QVERIFY(!zipPath.isEmpty());

    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(ramdiskComp), cfg, out, &err), qPrintable(err));

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    QString outFmt;
    QVERIFY(patcher::detectRamdiskFormat(info.ramdisk, outFmt));
    QCOMPARE(outFmt, "gzip");
    QByteArray raw;
    QString rErr;
    QVERIFY(patcher::decompressRamdisk(info.ramdisk, raw, &rErr));
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(raw, &entries));
    const TestCpioEntry *su = findEntry(entries, "sbin/su");
    QVERIFY(su);
    QCOMPARE(su->data, suBytes);
    QVERIFY(findEntry(entries, "init.superuser.rc"));
    const TestCpioEntry *rc = findEntry(entries, "init.rc");
    QVERIFY(rc);
    QVERIFY(rc->data.endsWith("import /init.superuser.rc\n"));
}

void TestPatcher::suInjectFallsBackAbi()
{
    // 无 arm64/su 时回退到 arm/su（真实 2.82 zip 各 ABI 目录形态）
    const QByteArray suBytes("\x7f"
                             "ELF" "su-bytes-from-arm-only-zip");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = dir.path() + "/UPDATE-SuperSU.zip";
    QFile f(zipPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildZip({{"arm/su", suBytes, 0},
                      {"common/Superuser.apk", "mock apk", 0}}));
    f.close();

    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(buildCpio({{"init", kRegMode | 0750, "init"},
                                            {"init.rc", kRegMode | 0644, "on boot\n"}})),
                     cfg, out, &err),
             qPrintable(err));

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));
    const TestCpioEntry *su = findEntry(entries, "sbin/su");
    QVERIFY(su);
    QCOMPARE(su->data, suBytes);
}

void TestPatcher::suInjectDeflatedSu()
{
    // 真实 SuperSU zip 内条目为 DEFLATE（2.82 产物 arm64/su 10.4KB）
    const QByteArray suBytes("\x7f"
                             "ELF" "su-extracted-from-deflated-entry");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeSupersuZip(dir.path(), suBytes, 8); // method=8
    QVERIFY(!zipPath.isEmpty());

    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(buildCpio({{"init", kRegMode | 0750, "init"},
                                            {"init.rc", kRegMode | 0644, "on boot\n"}})),
                     cfg, out, &err),
             qPrintable(err));

    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(out, info));
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));
    const TestCpioEntry *su = findEntry(entries, "sbin/su");
    QVERIFY(su);
    QCOMPARE(su->data, suBytes);
}

void TestPatcher::suMissingZipFails()
{
    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu; // suZipPath 未指定
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(buildCpio({{"init.rc", kRegMode | 0644, "on boot\n"}})), cfg,
                     out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("suZipPath", Qt::CaseInsensitive));
    QVERIFY(out.isEmpty());
}

void TestPatcher::suNonexistentZipFails()
{
    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = "/nonexistent/UPDATE-SuperSU.zip";
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(buildCpio({{"init.rc", kRegMode | 0644, "on boot\n"}})), cfg,
                     out, &err));
    QVERIFY(!err.isEmpty());
}

void TestPatcher::suZipNotZipFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = dir.path() + "/fake.zip";
    QFile f(zipPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("not a zip archive at all");
    f.close();

    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(buildCpio({{"init.rc", kRegMode | 0644, "on boot\n"}})), cfg,
                     out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("ZIP", Qt::CaseInsensitive));
}

void TestPatcher::suZipWithoutSuFails()
{
    // 合法 zip 但无 <abi>/su 条目（诚实边界：明确报错而非注入任意内容）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = dir.path() + "/UPDATE-SuperSU.zip";
    QFile f(zipPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(buildZip({{"META-INF/com/google/android/update-binary", "#!/sbin/sh\n", 0},
                      {"common/Superuser.apk", "mock apk", 0}}));
    f.close();

    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(buildCpio({{"init.rc", kRegMode | 0644, "on boot\n"}})), cfg,
                     out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("su", Qt::CaseInsensitive));
    QVERIFY(out.isEmpty());
}

void TestPatcher::suNotElfFails()
{
    // 条目名符合 <abi>/su 但内容非 ELF：拒绝注入（防恶意/损坏包写入
    // 不可执行文件冒充 su）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeSupersuZip(dir.path(), QByteArray("plain text not an elf"));
    QVERIFY(!zipPath.isEmpty());

    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(buildCpio({{"init.rc", kRegMode | 0644, "on boot\n"}})), cfg,
                     out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("ELF", Qt::CaseInsensitive));
    QVERIFY(out.isEmpty());
}

void TestPatcher::suInvalidBootFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeSupersuZip(
        dir.path(), QByteArray("\x7f" "ELF" "su-bytes"));
    QVERIFY(!zipPath.isEmpty());

    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray("not a boot image at all"), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("boot", Qt::CaseInsensitive));
}

void TestPatcher::suNoRamdiskFails()
{
    // SAR/ramdiskless：诚实边界 —— 明确错误 + 提示改用 Magisk
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeSupersuZip(
        dir.path(), QByteArray("\x7f" "ELF" "su-bytes"));
    QVERIFY(!zipPath.isEmpty());

    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(QByteArray()), cfg, out, &err)); // 空 ramdisk
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("Magisk", Qt::CaseInsensitive));
}

void TestPatcher::suNoInitRcFails()
{
    // ramdisk 无 /init.rc（老设备 ramdisk 约定包含）：路径不符 → 明确错误
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeSupersuZip(
        dir.path(), QByteArray("\x7f" "ELF" "su-bytes"));
    QVERIFY(!zipPath.isEmpty());

    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(buildBootV0(buildCpio({{"init", kRegMode | 0750, "init-data"}})), cfg,
                     out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("init.rc", Qt::CaseInsensitive));
    QVERIFY(out.isEmpty());
}

void TestPatcher::suRepatchFails()
{
    // 重复修补防护（sukernel --patch-test "Already patched, aborting" 同款）：
    // 已含 init.superuser.rc 标记的镜像再次注入必须拒绝，防止 import 重复追加
    const QByteArray suBytes("\x7f"
                             "ELF" "su-for-repatch-guard");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeSupersuZip(dir.path(), suBytes);
    QVERIFY(!zipPath.isEmpty());

    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QByteArray patched;
    QString err;
    const QByteArray ramdisk = buildCpio({{"init", kRegMode | 0750, "init"},
                                          {"init.rc", kRegMode | 0644, "on boot\n"}});
    QVERIFY2(p.patch(buildBootV0(ramdisk), cfg, patched, &err), qPrintable(err));
    // 对已修补产物再次注入：必须失败并提示还原；失败时输出不得残留
    QByteArray out2;
    QString err2;
    QVERIFY(!p.patch(patched, cfg, out2, &err2));
    QVERIFY(!err2.isEmpty());
    QVERIFY(err2.contains("还原", Qt::CaseInsensitive));
    QVERIFY(out2.isEmpty());
}

void TestPatcher::suMagiskPatchedFails()
{
    // Magisk 修补产物（.backup 链）：init 已被 magiskinit 接管，叠加
    // SuperSU import 会破坏 init 链 → 拒绝（与 C3/C4 防护一致）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeSupersuZip(
        dir.path(), QByteArray("\x7f" "ELF" "su-bytes"));
    QVERIFY(!zipPath.isEmpty());

    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QByteArray out;
    QString err;
    const QByteArray ramdisk =
        buildCpio({{".backup", kDirMode, QByteArray()},
                   {"init", kRegMode | 0750, "magiskinit"},
                   {"init.rc", kRegMode | 0644, "on boot\n"}});
    QVERIFY(!p.patch(buildBootV0(ramdisk), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("Magisk", Qt::CaseInsensitive));
    QVERIFY(out.isEmpty());
}

void TestPatcher::suNullErrorNoCrash()
{
    // error=nullptr 契约（全局）：全部失败分支与成功路径均不得解引用 error
    const QByteArray suBytes("\x7f"
                             "ELF" "su-bytes");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeSupersuZip(dir.path(), suBytes);
    QVERIFY(!zipPath.isEmpty());
    const QByteArray goodBoot = buildBootV0(buildCpio({{"init", kRegMode | 0750, "init"},
                                                       {"init.rc", kRegMode | 0644, "on boot\n"}}));

    patcher::RamdiskSuPatcher p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QByteArray out;

    // 失败分支：缺注入物 / 非 zip / 非 boot / 无 ramdisk / 无 init.rc / 重复修补
    patcher::PatchConfig noZip;
    noZip.type = patcher::RootType::RamdiskSu;
    QVERIFY(!p.patch(goodBoot, noZip, out, nullptr));
    cfg.suZipPath = dir.path() + "/nonexistent.zip";
    QVERIFY(!p.patch(goodBoot, cfg, out, nullptr));
    cfg.suZipPath = zipPath;
    QVERIFY(!p.patch(QByteArray("garbage, not boot"), cfg, out, nullptr));
    QVERIFY(!p.patch(buildBootV0(QByteArray()), cfg, out, nullptr));
    QVERIFY(!p.patch(buildBootV0(buildCpio({{"init", kRegMode | 0750, "init"}})), cfg, out,
                     nullptr));
    // 成功路径同样不得崩溃
    QVERIFY(p.patch(goodBoot, cfg, out, nullptr));
}

void TestPatcher::suPatchFileHappyPath()
{
    // patchFile 端到端：RamdiskSu 经工厂派发 → _patched.img + .orig.bak，
    // 产物含 import 行（C6 入口复用）
    const QByteArray suBytes("\x7f"
                             "ELF" "su-for-patchfile-entry");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bootPath = dir.path() + "/boot.img";
    const QString zipPath = writeSupersuZip(dir.path(), suBytes);
    QVERIFY(!zipPath.isEmpty());
    QFile bf(bootPath);
    QVERIFY(bf.open(QIODevice::WriteOnly));
    bf.write(buildBootV0(buildCpio({{"init", kRegMode | 0750, "init"},
                                    {"init.rc", kRegMode | 0644, "on boot\n"}})));
    bf.close();

    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QString outPath;
    QString err;
    QVERIFY2(patcher::patchFile(bootPath, cfg, &outPath, &err), qPrintable(err));

    const QString patchedPath = dir.path() + "/boot_patched.img";
    QCOMPARE(outPath, patchedPath);
    QVERIFY(QFile::exists(patchedPath));
    QVERIFY(QFile::exists(bootPath + ".orig.bak"));

    QFile pf(patchedPath);
    QVERIFY(pf.open(QIODevice::ReadOnly));
    const QByteArray patchedBytes = pf.readAll();
    pf.close();
    imgboot::BootInfo info;
    QVERIFY(imgboot::parseBootImage(patchedBytes, info));
    QList<TestCpioEntry> entries;
    QVERIFY(parseCpio(info.ramdisk, &entries));
    QVERIFY(findEntry(entries, "sbin/su"));
    QVERIFY(findEntry(entries, "init.superuser.rc"));
    const TestCpioEntry *rc = findEntry(entries, "init.rc");
    QVERIFY(rc);
    QVERIFY(rc->data.endsWith("import /init.superuser.rc\n"));
}

void TestPatcher::suPatchFileRollbackNoBackup()
{
    // C6 Minor 回滚分支测试：修补产物写失败（boot_patched.img 被同名目录
    // 占用）→ patchFile 必须移除刚创建的 .orig.bak，保持
    // "存在 .orig.bak ⟺ 存在产物" 不变式
    const QByteArray suBytes("\x7f"
                             "ELF" "su-for-rollback");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bootPath = dir.path() + "/boot.img";
    const QString zipPath = writeSupersuZip(dir.path(), suBytes);
    QVERIFY(!zipPath.isEmpty());
    QFile bf(bootPath);
    QVERIFY(bf.open(QIODevice::WriteOnly));
    bf.write(buildBootV0(buildCpio({{"init", kRegMode | 0750, "init"},
                                    {"init.rc", kRegMode | 0644, "on boot\n"}})));
    bf.close();
    // 产物路径被目录占用：writeFile 打开失败（EISDIR）
    QVERIFY(QDir().mkdir(dir.path() + "/boot_patched.img"));

    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::RamdiskSu;
    cfg.suZipPath = zipPath;
    QString outPath;
    QString err;
    QVERIFY(!patcher::patchFile(bootPath, cfg, &outPath, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(outPath.isEmpty());
    QVERIFY(!QFile::exists(bootPath + ".orig.bak")); // 备份已回滚
}

// ============================================================
// C8: ModuleInstaller（模块框架安装）
// ============================================================

void TestPatcher::moduleFactoryCreate()
{
    // 工厂登记：ModuleInstall → ModuleInstaller；既有类型派发不受影响
    std::unique_ptr<patcher::RootPatcher> p(
        patcher::RootPatcher::create(patcher::RootType::ModuleInstall));
    QVERIFY(p);
    QVERIFY(dynamic_cast<patcher::ModuleInstaller *>(p.get()));
    std::unique_ptr<patcher::RootPatcher> m(patcher::RootPatcher::create(patcher::RootType::Magisk));
    QVERIFY(dynamic_cast<patcher::MagiskPatcher *>(m.get()));
}

void TestPatcher::moduleInjectHappyPath()
{
    // 完整安装链（诚实边界内）：模块 zip 校验（module.prop 官方字段/正则）→
    // 解包文件树（剔除 META-INF 安装脚手架）→ 已修补镜像框架门禁 → 重打包
    // 干净模块 zip。断言：根级 module.prop 规范化位置、文件树、META-INF 剥离、
    // meta()/tree() 访问器。
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeModuleZip(dir.path(), buildModuleZip());
    QVERIFY(!zipPath.isEmpty());

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(buildMagiskPatchedRamdisk()), cfg, out, &err), qPrintable(err));
    QVERIFY(err.isEmpty());

    // out = 干净模块 zip：根级 module.prop（官方字段序规范化重写）
    QStringList names;
    QString zErr;
    QVERIFY(patcher::zipEntryNames(out, &names, &zErr));
    QVERIFY(names.contains("module.prop"));
    for (const QString &n : names)
        QVERIFY2(!n.startsWith("META-INF/"), qPrintable(n)); // 安装脚手架已剥离
    QByteArray prop;
    QVERIFY(patcher::extractZipEntry(out, "module.prop", prop, &zErr));
    QCOMPARE(prop, kModuleProp);

    // 文件树完整（DEFLATE 条目解包正确）
    QByteArray hosts;
    QVERIFY(patcher::extractZipEntry(out, "system/etc/hosts", hosts, &zErr));
    QCOMPARE(hosts, QByteArray("127.0.0.1 localhost\n"));
    QByteArray lib;
    QVERIFY(patcher::extractZipEntry(out, "zygisk/arm64-v8a.so", lib, &zErr));
    QCOMPARE(lib, QByteArray("\x7f"
                             "ELF" "mock-zygisk-lib"));
    QByteArray svc;
    QVERIFY(patcher::extractZipEntry(out, "service.sh", svc, &zErr));
    QCOMPARE(svc, QByteArray("#!/system/bin/sh\n"));

    // 访问器：meta 解析 + 树内容（UI 经 tree() 直推 /data/adb/modules/<id>/）
    QCOMPARE(p.meta().id, QStringLiteral("zygisk_lsposed"));
    QCOMPARE(p.meta().name, QStringLiteral("LSPosed"));
    QCOMPARE(p.meta().version, QStringLiteral("v1.9.2"));
    QCOMPARE(p.meta().versionCode, 7024);
    QCOMPARE(p.meta().author, QStringLiteral("LSPosed Developers"));
    QCOMPARE(p.meta().description, QStringLiteral("Enhanced Xposed Framework"));
    QCOMPARE(p.meta().updateJson, QStringLiteral("https://example.com/lsposed/update.json"));
    QCOMPARE(p.tree().value("system/etc/hosts"), QByteArray("127.0.0.1 localhost\n"));
    QVERIFY(p.tree().contains("zygisk/arm64-v8a.so"));
    QVERIFY(p.tree().contains("service.sh"));
    QVERIFY(!p.tree().contains("module.prop")); // 元数据不入树
    QVERIFY(!p.tree().contains("META-INF/com/google/android/updater-script"));
}

void TestPatcher::moduleInjectGzipFrameworkGate()
{
    // gzip 压缩的已 Magisk 修补 ramdisk：门禁经解压后识别框架标记
    const QByteArray ramdiskComp = patcher::compressRamdisk(buildMagiskPatchedRamdisk(), "gzip");
    QVERIFY(!ramdiskComp.isEmpty());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeModuleZip(dir.path(), buildModuleZip());
    QVERIFY(!zipPath.isEmpty());

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(ramdiskComp), cfg, out, &err), qPrintable(err));

    QStringList names;
    QString zErr;
    QVERIFY(patcher::zipEntryNames(out, &names, &zErr));
    QVERIFY(names.contains("module.prop"));
}

void TestPatcher::moduleKsuFrameworkGate()
{
    // KernelSU 修补形态（init.real + kernelsu.ko，ksud boot-patch 产物）门禁通过
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeModuleZip(dir.path(), buildModuleZip());
    QVERIFY(!zipPath.isEmpty());

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(buildBootV0(buildKsuPatchedRamdisk()), cfg, out, &err), qPrintable(err));
}

void TestPatcher::moduleNoBootImageOk()
{
    // 已 root 设备路径：bootImage 留空跳过框架门禁（模块树经 tree() 供 UI 直推
    // /data/adb/modules/<id>/，官方手动安装流程：push → 重启生效）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeModuleZip(dir.path(), buildModuleZip());
    QVERIFY(!zipPath.isEmpty());

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY2(p.patch(QByteArray(), cfg, out, &err), qPrintable(err));

    QCOMPARE(p.meta().id, QStringLiteral("zygisk_lsposed"));
    QVERIFY(p.tree().contains("system/etc/hosts"));
    QStringList names;
    QString zErr;
    QVERIFY(patcher::zipEntryNames(out, &names, &zErr));
    QVERIFY(names.contains("module.prop"));
}

void TestPatcher::moduleMissingZipFails()
{
    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall; // moduleZipPath 未指定
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray(), cfg, out, &err));
    QVERIFY(!err.isEmpty());
    QVERIFY(err.contains("moduleZipPath"));
    QVERIFY(out.isEmpty());
}

void TestPatcher::moduleNonexistentZipFails()
{
    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = "/nonexistent/module.zip";
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray(), cfg, out, &err));
    QVERIFY(!err.isEmpty());
}

void TestPatcher::moduleZipNotZipFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = dir.path() + "/module.zip";
    QFile f(zipPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("this is not a zip archive at all");
    f.close();

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray(), cfg, out, &err));
    QVERIFY(err.contains("ZIP"));
}

void TestPatcher::moduleWithoutPropFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeModuleZip(
        dir.path(), buildZip({{"system/etc/hosts", QByteArray("x"), 0}}));
    QVERIFY(!zipPath.isEmpty());

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray(), cfg, out, &err));
    QVERIFY(err.contains("module.prop"));
}

void TestPatcher::moduleBadIdFails()
{
    // id 含 '/'（路径穿越向量）→ 官方正则 ^[a-zA-Z][a-zA-Z0-9._-]+$ 拒绝
    const QByteArray prop = QByteArray(
        "id=a/b\n"
        "name=LSPosed\n"
        "version=v1.9.2\n"
        "versionCode=7024\n"
        "author=dev\n"
        "description=desc\n");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeModuleZip(dir.path(), buildModuleZip(prop));
    QVERIFY(!zipPath.isEmpty());

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray(), cfg, out, &err));
    QVERIFY(err.contains("id"));
}

void TestPatcher::moduleBadVersionCodeFails()
{
    const QByteArray prop = QByteArray(
        "id=zygisk_lsposed\n"
        "name=LSPosed\n"
        "version=v1.9.2\n"
        "versionCode=abc\n" // 规范要求整数
        "author=dev\n"
        "description=desc\n");
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeModuleZip(dir.path(), buildModuleZip(prop));
    QVERIFY(!zipPath.isEmpty());

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray(), cfg, out, &err));
    QVERIFY(err.contains("versionCode"));
}

void TestPatcher::moduleInstallShFails()
{
    // 官方规范明令禁止 install.sh 条目（Magisk 会拒绝安装）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeModuleZip(
        dir.path(), buildZip({{"module.prop", kModuleProp, 0},
                              {"install.sh", QByteArray("#!/system/bin/sh\n"), 0}}));
    QVERIFY(!zipPath.isEmpty());

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray(), cfg, out, &err));
    QVERIFY(err.contains("install.sh"));
}

void TestPatcher::moduleUpdaterScriptNotMagiskFails()
{
    // updater-script 存在但非 "#MAGISK" → 是普通 recovery 刷入包而非 Magisk 模块
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeModuleZip(
        dir.path(),
        buildZip({{"module.prop", kModuleProp, 0},
                  {"META-INF/com/google/android/updater-script",
                   QByteArray("assert(getprop('ro.product.device') == 'foo');\n"), 0}}));
    QVERIFY(!zipPath.isEmpty());

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray(), cfg, out, &err));
    QVERIFY(err.contains("MAGISK"));
}

void TestPatcher::moduleUnpatchedImageFails()
{
    // 未修补镜像（原厂 ramdisk 无框架标记）→ 门禁拒绝：模块运行时依赖
    // magiskd/ksud，须先修补 boot 或在已 root 设备上安装
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeModuleZip(dir.path(), buildModuleZip());
    QVERIFY(!zipPath.isEmpty());

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QString err;
    const QByteArray stockRamdisk = buildCpio({{"init", kRegMode | 0750, "original-init"},
                                               {"init.rc", kRegMode | 0644, "on boot\n"}});
    QVERIFY(!p.patch(buildBootV0(stockRamdisk), cfg, out, &err));
    QVERIFY(err.contains("框架"));
    QVERIFY(out.isEmpty());
}

void TestPatcher::moduleInvalidBootFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeModuleZip(dir.path(), buildModuleZip());
    QVERIFY(!zipPath.isEmpty());

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray("not a boot image at all"), cfg, out, &err));
    QVERIFY(err.contains("boot 镜像"));
}

void TestPatcher::moduleNullErrorNoCrash()
{
    // 全局契约：error=nullptr 时成功/失败路径均不崩溃
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeModuleZip(dir.path(), buildModuleZip());
    QVERIFY(!zipPath.isEmpty());

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QVERIFY(p.patch(QByteArray(), cfg, out, nullptr)); // 成功路径

    patcher::PatchConfig bad = cfg;
    bad.moduleZipPath.clear();
    QVERIFY(!p.patch(QByteArray(), bad, out, nullptr)); // 失败路径
}

void TestPatcher::modulePatchFileRejectsModuleType()
{
    // patchFile 面向 boot 镜像产物（备份/"_patched.img" 命名不适配模块包）→
    // 明确拒绝而非产出误导产物
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString bootPath = dir.path() + "/boot.img";
    const QString zipPath = writeModuleZip(dir.path(), buildModuleZip());
    QVERIFY(!zipPath.isEmpty());
    QFile bf(bootPath);
    QVERIFY(bf.open(QIODevice::WriteOnly));
    bf.write(buildBootV0(buildMagiskPatchedRamdisk()));
    bf.close();

    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QString outPath;
    QString err;
    QVERIFY(!patcher::patchFile(bootPath, cfg, &outPath, &err));
    QVERIFY(err.contains("ModuleInstaller"));
    QVERIFY(outPath.isEmpty());
    QVERIFY(!QFile::exists(bootPath + ".orig.bak")); // 不落任何产物
}

void TestPatcher::moduleOversizeZipFails()
{
    // 审查 Important：readAll 整读前按文件大小拒绝（1GB+ 稀疏文件，内容无关，
    // 验证不会尝试分配）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = dir.path() + "/module.zip";
    QFile f(zipPath);
    QVERIFY(f.open(QIODevice::WriteOnly));
    QVERIFY(f.resize(1024ll * 1024 * 1024 + 1)); // 稀疏文件，即时完成
    f.close();

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray(), cfg, out, &err));
    QVERIFY(err.contains("大小上限"));
    QVERIFY(out.isEmpty());
}

void TestPatcher::moduleTraversalEntryFails()
{
    // 审查 Minor（纵深防御）：'..' 路径段与前导 '/' 条目拒绝（防 UI 推送
    // /data/adb/modules/<id>/ 时路径穿越）
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString zipPath = writeModuleZip(
        dir.path(), buildZip({{"module.prop", kModuleProp, 0},
                              {"../evil.sh", QByteArray("rm -rf /\n"), 0}}));
    QVERIFY(!zipPath.isEmpty());

    patcher::ModuleInstaller p;
    patcher::PatchConfig cfg;
    cfg.type = patcher::RootType::ModuleInstall;
    cfg.moduleZipPath = zipPath;
    QByteArray out;
    QString err;
    QVERIFY(!p.patch(QByteArray(), cfg, out, &err));
    QVERIFY(err.contains("非法"));
    QVERIFY(out.isEmpty());

    // 前导 '/' 同样拒绝（覆盖写同一文件）
    QVERIFY(writeModuleZip(dir.path(),
                           buildZip({{"module.prop", kModuleProp, 0},
                                     {"/abs/path.sh", QByteArray("x\n"), 0}})) == zipPath);
    patcher::PatchConfig cfg2 = cfg; // moduleZipPath 指向覆盖后的同一文件
    QVERIFY(!p.patch(QByteArray(), cfg2, out, &err));
    QVERIFY(err.contains("非法"));
}

void TestPatcher::zipPrefixPreferExact()
{
    // C8 吸收的 C7 Minor（apatch kpimg 精确名优先）：同 zip 同时含精确名与
    // 前缀名时取精确名（与目录扫描 findInDirByPrefix 语义一致）
    const QByteArray zip = buildZip({{"assets/kpimg-old", QByteArray("old"), 0},
                                     {"assets/kpimg", QByteArray("exact"), 0},
                                     {"assets/kpimg-extra", QByteArray("extra"), 0}});
    QByteArray out;
    QString err;
    QVERIFY(patcher::extractZipEntryByPrefix(zip, QStringLiteral("assets/kpimg"), out, &err));
    QCOMPARE(out, QByteArray("exact")); // 精确名优先（旧行为返回 "old"）

    // 无精确名时回退前缀
    const QByteArray zip2 = buildZip({{"assets/kpimg-abc", QByteArray("abc"), 0}});
    QVERIFY(patcher::extractZipEntryByPrefix(zip2, QStringLiteral("assets/kpimg"), out, &err));
    QCOMPARE(out, QByteArray("abc"));
}

QTEST_APPLESS_MAIN(TestPatcher)
#include "test_patcher.moc"
