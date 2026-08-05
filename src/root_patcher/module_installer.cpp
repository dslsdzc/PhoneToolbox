#include "root_patcher/module_installer.h"
#include "root_patcher/zip_util.h"
#include "root_patcher/cpio_util.h"
#include "root_patcher/ramdisk_utils.h"
#include "image_engine/boot_image.h"

#include <QFile>
#include <QRegularExpression>

#include <zlib.h>

namespace patcher {
namespace {

// 解包总量上限（防 zip bomb 强制分配 OOM）：真实模块 LSPosed ~40MB、
// Zygisk Next ~2MB，512MB 远大于合法上限。单条目 64MB 上限由 zip_util 覆盖。
constexpr quint64 kMaxUnpackedTotal = 512ull * 1024 * 1024;

// 输入 zip 文件大小上限（1GB）：readAll 整读之前先拒绝，防数 GB 恶意文件
// 先触发分配失败 terminate（审查 Important）。合法模块 zip 远小于此。
constexpr qint64 kMaxInputZipSize = 1024ll * 1024 * 1024;

// 模块树条目数上限：EOCD 条目数/写入端为 quint16（65535），输出 zip 条目 =
// 1（module.prop）+ 树条目 ≤ 65535 防回绕（输入 zip 经 EOCD 上限 ≤ 65535，
// 防御性显式声明该不变式，zip64 支持扩展后仍成立）。
constexpr int kMaxTreeEntries = 65534;

// 官方 id 正则（topjohnwu/Magisk docs/guides.md："^[a-zA-Z][a-zA-Z0-9._-]+$"）。
// 目录名安全关键：拒绝 '/' 与 ".."（安装目录 /data/adb/modules/<id>/ 派生自 id）。
const QRegularExpression &moduleIdRegex()
{
    static const QRegularExpression re(QStringLiteral("^[a-zA-Z][a-zA-Z0-9._-]+$"));
    return re;
}

// 条目路径含 '..' 路径段（zip 规范用 '/' 分隔；'..' 段为穿越向量）。
bool hasDotDotSegment(const QString &name)
{
    const QStringList segs = name.split(QLatin1Char('/'));
    for (const QString &s : segs)
        if (s == QLatin1String(".."))
            return true;
    return false;
}

// ---- 最小 ZIP 写入（PKWARE STORE 存储）----
// 与 zip_util 读取侧对称（普通 ZIP，无 zip64）；全部 STORE：合法且确定性
// 输出（便于测试比对）。仅用于重打包已校验的模块树，大小受 kMaxUnpackedTotal
// 约束。
struct OutZipEntry {
    QString name;
    QByteArray data;
};

void putLE32(QByteArray &b, quint32 v)
{
    b.append(char(v)).append(char(v >> 8)).append(char(v >> 16)).append(char(v >> 24));
}

void putLE16(QByteArray &b, quint16 v)
{
    b.append(char(v)).append(char(v >> 8));
}

QByteArray buildZipStore(const QList<OutZipEntry> &entries)
{
    QByteArray body;
    QByteArray cdb;
    quint32 localOff = 0;
    for (const auto &e : entries) {
        const QByteArray name = e.name.toUtf8();
        const quint32 crc =
            crc32(0, reinterpret_cast<const Bytef *>(e.data.constData()),
                  static_cast<uInt>(e.data.size()));
        const quint32 size = static_cast<quint32>(e.data.size());
        QByteArray lh;
        putLE32(lh, 0x04034b50);
        putLE16(lh, 20); // version needed
        putLE16(lh, 0);  // flags
        putLE16(lh, 0);  // method=STORE
        putLE16(lh, 0);  // mtime
        putLE16(lh, 0);  // mdate
        putLE32(lh, crc);
        putLE32(lh, size);
        putLE32(lh, size);
        putLE16(lh, static_cast<quint16>(name.size()));
        putLE16(lh, 0); // extra len
        lh.append(name);
        body.append(lh);
        body.append(e.data);

        QByteArray cb;
        putLE32(cb, 0x02014b50);
        putLE16(cb, 20); // version made by
        putLE16(cb, 20); // version needed
        putLE16(cb, 0);  // flags
        putLE16(cb, 0);  // method
        putLE16(cb, 0);  // mtime
        putLE16(cb, 0);  // mdate
        putLE32(cb, crc);
        putLE32(cb, size);
        putLE32(cb, size);
        putLE16(cb, static_cast<quint16>(name.size()));
        putLE16(cb, 0); // extra len
        putLE16(cb, 0); // comment len
        putLE16(cb, 0); // disk start
        putLE16(cb, 0); // internal attr
        putLE32(cb, 0); // external attr
        putLE32(cb, localOff);
        cb.append(name);
        cdb.append(cb);

        localOff += static_cast<quint32>(lh.size() + e.data.size());
    }
    QByteArray eocd;
    putLE32(eocd, 0x06054b50);
    putLE16(eocd, 0); // disk
    putLE16(eocd, 0); // cd disk
    // 条目数 quint16 安全：entries = 1（module.prop）+ m_tree ≤ 1 + kMaxTreeEntries
    // ≤ 65535（kMaxTreeEntries 防回绕不变式，见解包循环）
    putLE16(eocd, static_cast<quint16>(entries.size()));
    putLE16(eocd, static_cast<quint16>(entries.size()));
    putLE32(eocd, static_cast<quint32>(cdb.size()));
    putLE32(eocd, static_cast<quint32>(body.size()));
    putLE16(eocd, 0); // comment len
    return body + cdb + eocd;
}

} // namespace

// module.prop 严格格式解析（联网验证官方规范）：
//   id=<string>（正则校验）/ name / version / versionCode=<int>（必须整数）/
//   author / description / updateJson=<url>（可选）；未列字段可为任意单行
//   字符串（保留重写）；文件须 UNIX (LF) 行尾（容忍 Windows 打包的 \r 前缀）。
bool ModuleInstaller::parseModuleProp(const QByteArray &raw, QString *error)
{
    auto fail = [error](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    ModuleMeta m;
    bool versionCodeOk = false;
    m_extraProps.clear();
    const QList<QByteArray> lines = raw.split('\n');
    for (QByteArray line : lines) {
        if (line.endsWith('\r'))
            line.chop(1); // 容忍 \r\n（规范为 LF，重写时统一 LF）
        if (line.trimmed().isEmpty())
            continue;
        const int eq = line.indexOf('=');
        if (eq <= 0)
            return fail(QStringLiteral("module.prop 含非法行（须为单行 key=value）：%1")
                            .arg(QString::fromUtf8(line.left(32))));
        const QByteArray key = line.left(eq);
        const QByteArray val = line.mid(eq + 1);
        if (key == "id")
            m.id = QString::fromUtf8(val);
        else if (key == "name")
            m.name = QString::fromUtf8(val);
        else if (key == "version")
            m.version = QString::fromUtf8(val);
        else if (key == "versionCode") {
            bool ok = false;
            m.versionCode = val.toInt(&ok);
            if (!ok)
                return fail(QStringLiteral("module.prop versionCode 必须为整数（官方规范）"));
            versionCodeOk = true;
        } else if (key == "author")
            m.author = QString::fromUtf8(val);
        else if (key == "description")
            m.description = QString::fromUtf8(val);
        else if (key == "updateJson")
            m.updateJson = QString::fromUtf8(val);
        else
            m_extraProps.append({QString::fromUtf8(key), val}); // 未知单行字段保留
    }
    if (m.id.isEmpty())
        return fail(QStringLiteral("module.prop 缺少 id（官方规范必填）"));
    if (!moduleIdRegex().match(m.id).hasMatch())
        return fail(QStringLiteral("module.prop id 非法：%1（官方规范正则 "
                                   "^[a-zA-Z][a-zA-Z0-9._-]+$，安装目录名安全）")
                        .arg(m.id));
    if (!versionCodeOk)
        return fail(QStringLiteral("module.prop 缺少 versionCode（官方规范必填整数）"));
    m_meta = m;
    return true;
}

// 规范化重写 module.prop（官方字段序 + 未知字段按原顺序追加，LF 行尾）。
QByteArray ModuleInstaller::buildModuleZip() const
{
    QByteArray prop;
    prop.append("id=").append(m_meta.id.toUtf8()).append('\n');
    prop.append("name=").append(m_meta.name.toUtf8()).append('\n');
    prop.append("version=").append(m_meta.version.toUtf8()).append('\n');
    prop.append("versionCode=").append(QByteArray::number(m_meta.versionCode)).append('\n');
    prop.append("author=").append(m_meta.author.toUtf8()).append('\n');
    prop.append("description=").append(m_meta.description.toUtf8()).append('\n');
    if (!m_meta.updateJson.isEmpty())
        prop.append("updateJson=").append(m_meta.updateJson.toUtf8()).append('\n');
    for (const auto &extra : m_extraProps)
        prop.append(extra.first.toUtf8()).append('=').append(extra.second).append('\n');

    QList<OutZipEntry> entries;
    entries.append({"module.prop", prop});
    for (auto it = m_tree.constBegin(); it != m_tree.constEnd(); ++it)
        entries.append({it.key(), it.value()});
    return buildZipStore(entries);
}

bool ModuleInstaller::patch(const QByteArray &bootImage, const PatchConfig &cfg,
                            QByteArray &out, QString *error)
{
    auto fail = [error](const QString &msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (error)
        error->clear();
    out.clear();
    m_meta = ModuleMeta{};
    m_tree.clear();
    m_extraProps.clear();

    if (cfg.type != RootType::ModuleInstall)
        return fail(QStringLiteral("ModuleInstaller 仅处理 ModuleInstall RootType"));

    if (cfg.moduleZipPath.isEmpty())
        return fail(QStringLiteral("未提供模块 ZIP：请经 moduleZipPath 指定（标准 "
                                   "Magisk 模块格式，module.prop + 文件树）"));

    QFile zipFile(cfg.moduleZipPath);
    if (!zipFile.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法打开模块 ZIP：%1").arg(cfg.moduleZipPath));
    // 审查 Important：整读前先按文件大小拒绝（readAll 分配发生在解包上限
    // 检查之前，数 GB 文件会先触发 bad_alloc terminate）
    if (zipFile.size() > kMaxInputZipSize)
        return fail(QStringLiteral("模块 ZIP 超过 %1 MB 大小上限（防整读分配失败）：%2")
                        .arg(kMaxInputZipSize / (1024 * 1024))
                        .arg(cfg.moduleZipPath));
    const QByteArray zip = zipFile.readAll();
    zipFile.close();
    if (zip.isEmpty())
        return fail(QStringLiteral("模块 ZIP 文件为空"));
    if (!patcher::isZip(zip))
        return fail(QStringLiteral("模块 ZIP 不是有效 ZIP 归档：%1").arg(cfg.moduleZipPath));

    QString zipErr;
    QStringList names;
    if (!patcher::zipEntryNames(zip, &names, &zipErr))
        return fail(QStringLiteral("模块 ZIP 读取失败：%1").arg(zipErr));

    // 官方规范明令禁止 install.sh（Magisk 拒绝安装该类 zip）
    if (names.contains(QStringLiteral("install.sh")))
        return fail(QStringLiteral("模块 ZIP 含 install.sh（Magisk 官方规范禁止）："
                                   "请使用标准 Magisk 模块 zip"));

    // updater-script 存在时须为 "#MAGISK"（recovery 刷入标识）：普通 recovery
    // 刷入包（assert/package_extract 等脚本）不是 Magisk 模块 zip
    if (names.contains(QStringLiteral("META-INF/com/google/android/updater-script"))) {
        QByteArray us;
        if (!patcher::extractZipEntry(zip, QStringLiteral("META-INF/com/google/android/updater-script"),
                                      us, &zipErr))
            return fail(QStringLiteral("META-INF 读取失败：%1").arg(zipErr));
        if (!us.trimmed().startsWith("#MAGISK"))
            return fail(QStringLiteral("updater-script 非 #MAGISK：这是普通 recovery "
                                       "刷入包而非 Magisk 模块 zip"));
    }

    // module.prop 必须位于 zip 根（Magisk v28+ 静态读取位置，联网验证）
    if (!names.contains(QStringLiteral("module.prop")))
        return fail(QStringLiteral("模块 ZIP 缺少根级 module.prop（Magisk v28+ 规范）："
                                   "请提供标准 Magisk 模块 zip"));
    QByteArray propRaw;
    if (!patcher::extractZipEntry(zip, QStringLiteral("module.prop"), propRaw, &zipErr))
        return fail(QStringLiteral("module.prop 提取失败：%1").arg(zipErr));
    if (!parseModuleProp(propRaw, error))
        return false;

    // ---- 解包文件树（跳过 module.prop 元数据与 META-INF/ 安装脚手架）----
    quint64 total = 0;
    for (const QString &n : names) {
        if (n == QLatin1String("module.prop") || n.startsWith(QLatin1String("META-INF/")))
            continue;
        if (n.endsWith(QLatin1Char('/')))
            continue; // 纯目录条目（文件树隐含，不落字节）
        // 纵深防御（审查 Minor）：zip 规范要求相对路径，真实模块 zip 无前导
        // '/' 或 '..' 段 —— 拒绝防 UI 推送 /data/adb/modules/<id>/ 时路径穿越
        if (n.startsWith(QLatin1Char('/')) || hasDotDotSegment(n))
            return fail(QStringLiteral("模块条目路径非法（含前导 '/' 或 '..' 段）：%1")
                            .arg(n));
        QByteArray data;
        if (!patcher::extractZipEntry(zip, n, data, &zipErr))
            return fail(QStringLiteral("模块条目 %1 提取失败：%2").arg(n, zipErr));
        total += static_cast<quint64>(data.size());
        if (total > kMaxUnpackedTotal)
            return fail(QStringLiteral("模块解包总量超过 %1 MB 上限（疑似 zip bomb，已中止）")
                            .arg(kMaxUnpackedTotal / (1024 * 1024)));
        m_tree.insert(n, data);
        if (m_tree.size() > kMaxTreeEntries)
            return fail(QStringLiteral("模块条目数超过 %1 上限（重打包 EOCD 计数防回绕）")
                            .arg(kMaxTreeEntries));
    }

    // ---- boot 镜像框架门禁（可选输入，诚实边界）----
    // 模块运行时（magiskd/ksud）在设备启动阶段读取 /data/adb/modules/（data
    // 分区）—— 若提供已修补镜像，须能识别其承载的模块框架；未修补镜像上
    // 安装模块必然失败（无运行时加载器），拒绝并提示正确路径。
    if (!bootImage.isEmpty()) {
        imgboot::BootInfo info;
        if (!imgboot::parseBootImage(bootImage, info))
            return fail(QStringLiteral("无法解析 boot 镜像（请提供已修补的 boot/init_boot "
                                       "镜像，或省略镜像走已 root 设备安装路径）"));
        if (info.ramdisk.isEmpty())
            return fail(QStringLiteral("boot 镜像不含 ramdisk：无法承载 Magisk/KernelSU "
                                       "模块框架，请用对应 App 修补"));
        QByteArray ramdiskRaw;
        QString ramdiskErr;
        if (!patcher::decompressRamdisk(info.ramdisk, ramdiskRaw, &ramdiskErr))
            return fail(QStringLiteral("ramdisk 解压失败：%1").arg(ramdiskErr));
        CpioArchive cpio;
        if (!cpio.parse(ramdiskRaw, &ramdiskErr))
            return fail(QStringLiteral("ramdisk 不是有效 cpio：%1").arg(ramdiskErr));
        // Magisk 标记：.backup 链（C3 注入产物）；KernelSU 标记：init.real +
        // kernelsu.ko（ksud boot-patch 产物，原 init 改名 init.real）
        const bool magiskFramework = cpio.isMagiskPatched();
        const bool ksuFramework =
            cpio.exists(QStringLiteral("init.real")) && cpio.exists(QStringLiteral("kernelsu.ko"));
        if (!magiskFramework && !ksuFramework)
            // 文案注明门禁识别范围（审查 Minor）：仅 Magisk 与 KernelSU LKM
            // 形态有 ramdisk 标记；GKI 内核 / APatch 修补镜像无此标记，应省略
            // 镜像输入直接走已 root 设备安装路径（模块运行时依赖设备上的
            // magiskd/ksud，ramdisk 标记缺失不等同于设备未 root）
            return fail(QStringLiteral("镜像未含可识别的 Magisk/KernelSU 模块框架标记"
                                       "（.backup / init.real+kernelsu.ko）：本门禁仅识别"
                                       " Magisk 与 KernelSU LKM 形态；GKI 内核/APatch 修补"
                                       " 镜像无 ramdisk 标记，请省略该镜像输入，直接走已 "
                                       "root 设备安装路径"));
    }

    // ---- 重打包干净模块 zip（module.prop 规范化 + 文件树，剔除 META-INF）----
    out = buildModuleZip();
    return true;
}

} // namespace patcher
