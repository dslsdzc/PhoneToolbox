#include "ui/image_worker.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include "image_engine/sparse_image.h"
#include "image_engine/payload_image.h"
#include "image_engine/tar_image.h"
#include "image_engine/dat_image.h"
#include "image_engine/super_image.h"
#include "image_engine/boot_image.h"
#include "image_engine/kdz_image.h"
#include "image_engine/huawei_image.h"
#include "image_engine/sin_image.h"
#include "image_engine/pac_image.h"
#include "image_engine/disk_image.h"
#include "image_engine/twrp_image.h"
#include "image_engine/fs/fs_image.h"

namespace {

// ---- 工作线程内的小工具（无 UI 依赖）----

// 读文件为 QByteArray（image_engine 各引擎均为内存式 API，整文件载入）。
// 失败返回空并填 error（全局契约：失败不崩溃）。
QByteArray readFile(const QString &path, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("读取失败: ") + f.errorString();
        return {};
    }
    const QByteArray data = f.readAll();
    f.close();
    if (data.isEmpty() && error && error->isEmpty())
        *error = QStringLiteral("文件为空");
    return data;
}

// 写文件；失败返回 false 并填 error。调用方保证父目录已存在。
bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("无法写入 %1: %2").arg(path, f.errorString());
        return false;
    }
    const qint64 written = f.write(data);
    f.close();
    if (written != data.size()) {
        if (error) *error = QStringLiteral("写入 %1 不完整（%2/%3 字节）")
                                .arg(path).arg(written).arg(data.size());
        return false;
    }
    return true;
}

// 产物名消毒：产物名来自镜像内部（分区名/文件名），不可信。
// 去掉目录成分（防 "../" 穿越），再替换路径分隔符与其余非法字符。
QString sanitizeName(QString name)
{
    name = QFileInfo(name).fileName(); // 剥离目录成分（含 ../ 前缀）
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String(".."))
        name = QStringLiteral("_");
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        if (c.isLetterOrNumber() || c == QLatin1Char('.') ||
            c == QLatin1Char('-') || c == QLatin1Char('_'))
            out += c;
        else
            out += QLatin1Char('_');
    }
    return out;
}

// 相对路径安全合并：name 相对 outDir 解析后必须仍在 outDir 内
// （tar/文件系统条目名不可信，防路径穿越）。合法时 *full 为落盘路径。
bool safeJoin(const QString &outDir, const QString &name, QString *full)
{
    const QString cleaned = QDir::cleanPath(outDir + QLatin1Char('/') + name);
    const QString base = QDir::cleanPath(outDir);
    if (cleaned != base && !cleaned.startsWith(base + QLatin1Char('/')))
        return false;
    *full = cleaned;
    return true;
}

} // namespace

ImageWorker::ImageWorker(QObject *parent)
    : QObject(parent)
{
    // 跨线程 queued 连接要求参数类型运行时已注册（Qt 6 中 Q_DECLARE_METATYPE
    // 生成的 qt_metatype_id() 首次使用即自动注册；此处显式调用双保险）
    qRegisterMetaType<imgreg::Detected>("imgreg::Detected");
    qRegisterMetaType<patcher::PatchConfig>("patcher::PatchConfig");

    // 工作对象迁移到专用线程：runXxx 投递的 QueuedConnection 事件在此线程
    // 的事件循环中按提交顺序串行处理。先迁移后启动，事件队列已就绪。
    moveToThread(&m_thread);
    m_thread.setObjectName(QStringLiteral("image-worker"));
    m_thread.start();
}

ImageWorker::~ImageWorker()
{
    // 停止工作线程并阻塞等待其退出，防止线程悬挂与资源泄漏。
    // 注意：若正在执行长操作，wait() 会等待其完成（Qt 文档建议常规场景
    // 用 finished() 信号驱动收尾；析构场景 quit()+wait() 是标准做法）。
    m_thread.quit();
    m_thread.wait();
}

void ImageWorker::runDetect(const QString &path)
{
    QMetaObject::invokeMethod(this, [this, path] { doDetect(path); },
                              Qt::QueuedConnection);
}

void ImageWorker::runUnpack(const QString &path, const QString &outDir,
                            const imgreg::Detected &detected)
{
    QMetaObject::invokeMethod(this,
                              [this, path, outDir, detected] { doUnpack(path, outDir, detected); },
                              Qt::QueuedConnection);
}

void ImageWorker::runPack(const QString &outPath, const QStringList &inputs,
                          const imgreg::Detected &detected)
{
    QMetaObject::invokeMethod(this,
                              [this, outPath, inputs, detected] { doPack(outPath, inputs, detected); },
                              Qt::QueuedConnection);
}

void ImageWorker::runConvert(const QString &path, const QString &outPath,
                             const imgreg::Detected &detected)
{
    QMetaObject::invokeMethod(this,
                              [this, path, outPath, detected] { doConvert(path, outPath, detected); },
                              Qt::QueuedConnection);
}

void ImageWorker::runPatch(const QString &path, const patcher::PatchConfig &config)
{
    QMetaObject::invokeMethod(this, [this, path, config] { doPatch(path, config); },
                              Qt::QueuedConnection);
}

void ImageWorker::doDetect(const QString &path)
{
    emit progress(0, QStringLiteral("识别"));

    imgreg::Detected result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // 契约：失败以 error 形式上报，绝不崩溃
        result.detail = QStringLiteral("读取失败: ") + file.errorString();
        emit progress(100, QStringLiteral("识别")); // 失败路径也补发 100（D1 Minor 1）
        emit detectFinished(path, result);
        return;
    }
    // 魔数判定最大读取 2120B（pac 辅助信号 @2116），4096 一次覆盖全部判定点
    const QByteArray header = file.read(4096);
    file.close();

    if (header.isEmpty()) {
        result.detail = QStringLiteral("读取失败: 文件为空");
        emit progress(100, QStringLiteral("识别")); // 失败路径也补发 100（D1 Minor 1）
        emit detectFinished(path, result);
        return;
    }

    result = imgreg::detect(header, QFileInfo(path).fileName());
    emit progress(100, QStringLiteral("识别"));
    emit detectFinished(path, result);
}

void ImageWorker::doUnpack(const QString &path, const QString &outDir,
                           const imgreg::Detected &detected)
{
    emit progress(0, QStringLiteral("解包"));

    QString error;
    const QByteArray data = readFile(path, &error);
    if (data.isEmpty()) {
        emit progress(100, QStringLiteral("解包"));
        emit unpackFinished(false, QStringList(), error);
        return;
    }
    if (!QDir().mkpath(outDir)) {
        emit progress(100, QStringLiteral("解包"));
        emit unpackFinished(false, QStringList(),
                            QStringLiteral("无法创建输出目录: ") + outDir);
        return;
    }

    QStringList outputs;
    switch (detected.format) {
    // ---- sparse：单产物 raw 镜像 ----
    case imgreg::Format::Sparse: {
        const QByteArray raw = imgsparse::simg2img(data);
        if (raw.isEmpty()) {
            error = QStringLiteral("sparse 解包失败（数据损坏或不受支持）");
            break;
        }
        const QString out = QDir(outDir).filePath(
            QFileInfo(path).completeBaseName() + QStringLiteral(".img"));
        if (!writeFile(out, raw, &error)) break;
        outputs << out;
        break;
    }

    // ---- payload：每分区一个产物（全量 OTA 无旧镜像；diff 系分区报错终止）----
    case imgreg::Format::Payload: {
        imgpayload::PayloadInfo info;
        if (!imgpayload::parseManifest(data, info)) {
            error = QStringLiteral("payload manifest 解析失败");
            break;
        }
        if (info.partitions.isEmpty()) {
            error = QStringLiteral("payload 无分区");
            break;
        }
        const int total = info.partitions.size();
        const QByteArray oldImage; // 空旧镜像：仅 REPLACE 系操作可解
        int done = 0;
        for (const imgpayload::Partition &part : info.partitions) {
            QString perr;
            const QByteArray img = imgpayload::extractPartition(
                data, part, oldImage, &perr, info.blockSize);
            if (img.isEmpty()) {
                error = QStringLiteral("分区 %1 解包失败: %2")
                            .arg(part.name,
                                 perr.isEmpty() ? QStringLiteral("数据为空") : perr);
                break;
            }
            const QString out = QDir(outDir).filePath(
                sanitizeName(part.name) + QStringLiteral(".img"));
            if (!writeFile(out, img, &error)) break;
            outputs << out;
            emit progress(100 * (++done) / total, QStringLiteral("解包"));
        }
        break;
    }

    // ---- tar / tar.md5：目录结构展开（防穿越；符号链接不落盘）----
    case imgreg::Format::Tar:
    case imgreg::Format::TarMd5: {
        QList<imgtar::TarEntry> entries;
        if (!imgtar::extractTar(data, entries)) {
            error = QStringLiteral("tar 解析失败");
            break;
        }
        int skipped = 0;
        for (const imgtar::TarEntry &e : entries) {
            QString full;
            if (!safeJoin(outDir, e.name, &full)) { // 路径穿越防护
                ++skipped;
                continue;
            }
            if (e.isDir) {
                if (!QDir().mkpath(full)) {
                    error = QStringLiteral("无法创建目录: ") + full;
                    break;
                }
                continue;
            }
            if (e.isSymlink) { // 符号链接不落盘（链接目标不可信），仅计数
                ++skipped;
                continue;
            }
            if (!QDir().mkpath(QFileInfo(full).absolutePath())) {
                error = QStringLiteral("无法创建目录: ") + QFileInfo(full).absolutePath();
                break;
            }
            if (!writeFile(full, e.data, &error)) break;
            outputs << full;
        }
        // 被跳过条目（穿越/符号链接）不落盘、不计入产物，静默安全丢弃
        Q_UNUSED(skipped);
        break;
    }

    // ---- dat：需同目录 transfer.list（sdat2img）----
    case imgreg::Format::Dat: {
        const QFileInfo fi(path);
        QStringList candidates;
        candidates << QDir(fi.absolutePath()).filePath(
                          fi.completeBaseName() + QStringLiteral(".transfer.list")) // system.new → system.new.transfer.list
                   << QDir(fi.absolutePath()).filePath(
                          fi.completeBaseName().remove(QStringLiteral(".new"))
                          + QStringLiteral(".transfer.list"));                      // → system.transfer.list
        QString tlist;
        for (const QString &c : candidates) {
            if (QFileInfo::exists(c)) {
                tlist = c;
                break;
            }
        }
        if (tlist.isEmpty()) {
            error = QStringLiteral("未找到 %1（sdat2img 需要 transfer.list 与 .dat 同目录）")
                        .arg(candidates.first());
            break;
        }
        QByteArray raw;
        if (!imgdat::sdat2img(tlist, path, raw, &error)) break; // error 已由引擎填写
        const QString out = QDir(outDir).filePath(
            fi.completeBaseName() + QStringLiteral(".img"));
        if (!writeFile(out, raw, &error)) break;
        outputs << out;
        break;
    }

    // ---- super 动态分区：按元数据分区表逐分区提取 ----
    case imgreg::Format::Super: {
        imgsuper::SuperInfo info;
        if (!imgsuper::parseSuper(data, info)) {
            error = QStringLiteral("super 元数据解析失败");
            break;
        }
        QString serr;
        const QList<QByteArray> parts = imgsuper::extractPartitions(data, info, &serr);
        if (!serr.isEmpty()) {
            error = serr;
            break;
        }
        if (parts.size() != info.partitions.size()) {
            error = QStringLiteral("super 分区产物数与元数据不符");
            break;
        }
        for (int i = 0; i < parts.size(); ++i) {
            const QString out = QDir(outDir).filePath(
                sanitizeName(info.partitions.at(i).name) + QStringLiteral(".img"));
            if (!writeFile(out, parts.at(i), &error)) break;
            outputs << out;
            emit progress(100 * (i + 1) / parts.size(), QStringLiteral("解包"));
        }
        break;
    }

    // ---- boot 镜像：kernel / ramdisk / dtb 三件套 ----
    case imgreg::Format::Boot: {
        imgboot::BootInfo bi;
        if (!imgboot::parseBootImage(data, bi)) {
            error = QStringLiteral("boot 镜像解析失败");
            break;
        }
        if (!bi.kernel.isEmpty()) {
            const QString out = QDir(outDir).filePath(QStringLiteral("kernel"));
            if (!writeFile(out, bi.kernel, &error)) break;
            outputs << out;
        }
        if (!bi.ramdisk.isEmpty()) {
            const QString out = QDir(outDir).filePath(QStringLiteral("ramdisk"));
            if (!writeFile(out, bi.ramdisk, &error)) break;
            outputs << out;
        }
        if (!bi.dtb.isEmpty()) {
            const QString out = QDir(outDir).filePath(QStringLiteral("dtb"));
            if (!writeFile(out, bi.dtb, &error)) break;
            outputs << out;
        }
        if (outputs.isEmpty())
            error = QStringLiteral("boot 镜像无 kernel/ramdisk/dtb 内容");
        break;
    }

    // ---- KDZ：条目为 DZ 则合并为分区镜像，其余原样写出 ----
    case imgreg::Format::Kdz: {
        QList<imgkdz::DzFile> files;
        if (!imgkdz::parseKdz(data, files, &error)) break;
        if (files.isEmpty()) {
            error = QStringLiteral("KDZ 无文件记录");
            break;
        }
        for (const imgkdz::DzFile &f : files) {
            const QString safe = sanitizeName(f.name);
            QList<imgkdz::DzChunk> chunks;
            QString derr;
            if (imgkdz::parseDz(f.data, chunks, &derr)) {
                QByteArray merged = imgkdz::mergeChunks(chunks, &derr);
                if (!merged.isEmpty()) {
                    const QString out = QDir(outDir).filePath(safe + QStringLiteral(".img"));
                    if (!writeFile(out, merged, &error)) break;
                    outputs << out;
                    continue;
                }
            }
            // 非 DZ 条目（或合并失败）：原样落盘，尽力而为
            const QString out = QDir(outDir).filePath(safe);
            if (!writeFile(out, f.data, &error)) break;
            outputs << out;
        }
        break;
    }

    // ---- 华为 update.app：文件表逐条提取 ----
    case imgreg::Format::UpdateApp: {
        QList<imghw::AppFile> files;
        if (!imghw::parseUpdateApp(data, files, &error)) break;
        if (files.isEmpty()) {
            error = QStringLiteral("update.app 无文件表");
            break;
        }
        for (const imghw::AppFile &f : files) {
            QString ferr;
            const QByteArray payload = imghw::extractFile(data, f, &ferr);
            if (payload.isEmpty()) {
                error = QStringLiteral("文件 %1 提取失败: %2")
                            .arg(f.name, ferr.isEmpty() ? QStringLiteral("数据为空") : ferr);
                break;
            }
            const QString out = QDir(outDir).filePath(sanitizeName(f.name));
            if (!writeFile(out, payload, &error)) break;
            outputs << out;
            emit progress(100 * outputs.size() / files.size(), QStringLiteral("解包"));
        }
        break;
    }

    // ---- 索尼 SIN v3：合并块描述为单个 raw 镜像 ----
    case imgreg::Format::Sin: {
        QList<imgsin::BlockDesc> blocks;
        if (!imgsin::parseSin(data, blocks, &error)) break;
        const QByteArray raw = imgsin::extractRaw(data, blocks, &error);
        if (raw.isEmpty()) {
            if (error.isEmpty()) error = QStringLiteral("SIN 解包结果为空");
            break;
        }
        const QString out = QDir(outDir).filePath(
            QFileInfo(path).completeBaseName() + QStringLiteral(".raw"));
        if (!writeFile(out, raw, &error)) break;
        outputs << out;
        break;
    }

    // ---- pac 固件：分区数据段（长度按下一分区偏移推断，见下注）----
    case imgreg::Format::Pac: {
        QList<imgpac::PacPartition> parts;
        if (!imgpac::parsePac(data, parts, &error)) break;
        if (parts.isEmpty()) {
            error = QStringLiteral("pac 无分区");
            break;
        }
        for (int i = 0; i < parts.size(); ++i) {
            // 后端待办: imgpac::PacPartition 公开结构未暴露分区数据长度（内部
            // 已解析 size 但未落进结构），UI 侧按下一分区 offset / EOF 推断。
            // 标准布局（新格式表后连续数据段 / 旧格式头+数据连续）下即真实长度。
            const quint64 next = (i + 1 < parts.size())
                ? parts.at(i + 1).offset
                : static_cast<quint64>(data.size());
            if (next < parts.at(i).offset) {
                error = QStringLiteral("pac 分区表偏移无序，无法解包");
                break;
            }
            const QByteArray payload = data.mid(
                static_cast<qsizetype>(parts.at(i).offset),
                static_cast<qsizetype>(next - parts.at(i).offset));
            const QString name = sanitizeName(
                parts.at(i).fileName.isEmpty() ? parts.at(i).name : parts.at(i).fileName);
            const QString out = QDir(outDir).filePath(name);
            if (!writeFile(out, payload, &error)) break;
            outputs << out;
            emit progress(100 * (i + 1) / parts.size(), QStringLiteral("解包"));
        }
        break;
    }

    // ---- GPT 磁盘镜像：按分区表逐分区提取 ----
    case imgreg::Format::DiskGpt: {
        imgdisk::DiskInfo info;
        if (!imgdisk::parseGpt(data, info)) {
            error = QStringLiteral("GPT 解析失败");
            break;
        }
        if (info.partitions.isEmpty()) {
            error = QStringLiteral("GPT 无分区");
            break;
        }
        for (const imgdisk::Partition &p : info.partitions) {
            QByteArray raw;
            if (!imgdisk::extractPartition(data, p, raw)) {
                error = QStringLiteral("分区 %1 提取失败").arg(p.name);
                break;
            }
            const QString out = QDir(outDir).filePath(
                sanitizeName(p.name) + QStringLiteral(".img"));
            if (!writeFile(out, raw, &error)) break;
            outputs << out;
            emit progress(100 * outputs.size() / info.partitions.size(),
                          QStringLiteral("解包"));
        }
        break;
    }

    // ---- TWRP 备份：单个 win 文件 → raw 镜像 ----
    case imgreg::Format::TwrpWin: {
        QString werr;
        QByteArray raw;
        if (!imgtwrp::extractWin(path, raw, &werr)) {
            error = werr;
            break;
        }
        const QString out = QDir(outDir).filePath(
            QFileInfo(path).completeBaseName() + QStringLiteral(".img"));
        if (!writeFile(out, raw, &error)) break;
        outputs << out;
        break;
    }

    // ---- EROFS / ext4：文件系统遍历 + 按路径提取 ----
    case imgreg::Format::Erofs:
    case imgreg::Format::Ext4: {
        QString ferr;
        imgfs::FsImage *fs = imgfs::openFsImage(data, &ferr);
        if (!fs) {
            error = ferr;
            break;
        }
        QList<imgfs::FsEntry> entries;
        if (!fs->list(QString(), entries)) {
            error = QStringLiteral("文件系统遍历失败");
            delete fs;
            break;
        }
        bool failed = false;
        for (const imgfs::FsEntry &e : entries) {
            QString full;
            if (!safeJoin(outDir, e.path, &full)) continue; // 防穿越，跳过
            if (e.isDir) {
                QDir().mkpath(full);
                continue;
            }
            QByteArray content;
            if (!fs->extract(e.path, content)) {
                error = QStringLiteral("提取 %1 失败").arg(e.path);
                failed = true;
                break;
            }
            if (!QDir().mkpath(QFileInfo(full).absolutePath())) {
                error = QStringLiteral("无法创建目录: ") + QFileInfo(full).absolutePath();
                failed = true;
                break;
            }
            if (!writeFile(full, content, &error)) {
                failed = true;
                break;
            }
            outputs << full;
        }
        delete fs;
        if (failed) break;
        if (outputs.isEmpty())
            error = QStringLiteral("文件系统为空");
        break;
    }

    // ---- 后端缺口：解析器存在但无"解包到目录"产出，UI 侧优雅降级 ----
    case imgreg::Format::Zip:
        error = QStringLiteral("Zip 解包未实现（image_engine 无 zip 引擎，后端扩展待办）");
        break;
    case imgreg::Format::VendorBoot:
        error = QStringLiteral("vendor_boot 解包未实现（imgboot 暂无 vendor_boot 解析器，后端扩展待办）");
        break;
    case imgreg::Format::UpdateBin:
        error = QStringLiteral("update.bin 仅解析分区表、无数据偏移（imghw::parseUpdateBin 后端扩展待办）");
        break;
    case imgreg::Format::Vbmeta:
    case imgreg::Format::Dtb:
        error = QStringLiteral("该格式无解包引擎（后端扩展待办）");
        break;
    case imgreg::Format::RawImage:
        error = QStringLiteral("Raw 镜像本身即为已解包格式，无需解包");
        break;
    case imgreg::Format::Br:
    case imgreg::Format::Lz4:
    case imgreg::Format::Xz:
    case imgreg::Format::Gzip:
    case imgreg::Format::Zstd:
    case imgreg::Format::Brotli:
        error = QStringLiteral("压缩流解包未接线（后端扩展待办）");
        break;
    case imgreg::Format::Unknown:
    default:
        error = QStringLiteral("未知格式，无法解包");
        break;
    }

    emit progress(100, QStringLiteral("解包"));
    emit unpackFinished(error.isEmpty(), outputs, error);
}

void ImageWorker::doPack(const QString &outPath, const QStringList &inputs,
                         const imgreg::Detected &detected)
{
    Q_UNUSED(outPath);
    Q_UNUSED(inputs);
    Q_UNUSED(detected);
    emit progress(0, QStringLiteral("打包"));
    // D6 实现
    emit packFinished(false, QString(),
                      QStringLiteral("打包尚未实现（Task D6 接线）"));
}

void ImageWorker::doConvert(const QString &path, const QString &outPath,
                            const imgreg::Detected &detected)
{
    emit progress(0, QStringLiteral("转换"));

    QString error;
    if (outPath.isEmpty()) {
        emit progress(100, QStringLiteral("转换"));
        emit convertFinished(false, QString(), QStringLiteral("输出路径为空"));
        return;
    }
    const QByteArray data = readFile(path, &error);
    if (data.isEmpty()) {
        emit progress(100, QStringLiteral("转换"));
        emit convertFinished(false, outPath, error);
        return;
    }

    QByteArray out;
    switch (detected.format) {
    case imgreg::Format::Sparse: // sparse → raw
        out = imgsparse::simg2img(data);
        if (out.isEmpty())
            error = QStringLiteral("sparse→raw 转换失败（数据损坏或不受支持）");
        break;
    case imgreg::Format::RawImage: // raw → sparse
        out = imgsparse::img2simg(data);
        if (out.isEmpty())
            error = QStringLiteral("raw→sparse 转换失败");
        break;
    default:
        error = QStringLiteral("当前仅支持 sparse↔raw 转换");
        break;
    }

    if (error.isEmpty() && !writeFile(outPath, out, &error))
        out.clear();

    emit progress(100, QStringLiteral("转换"));
    emit convertFinished(error.isEmpty(), outPath, error);
}

void ImageWorker::doPatch(const QString &path, const patcher::PatchConfig &config)
{
    emit progress(0, QStringLiteral("修补"));

    // D4 接线：patcher::patchFile —— 读 boot 文件 → create() 工厂派发 →
    // 自动备份 "<源文件>.orig.bak" + 产物 "<基名>_patched.img"；失败不落
    // 任何产物（契约见 root_patcher.h：失败返回 false + error，绝不崩溃）。
    // 全部在工作线程执行，UI 线程不阻塞。
    QString error;
    QString outPath;
    const bool ok = patcher::patchFile(path, config, &outPath, &error);
    if (!ok) {
        // 防御：契约要求失败时 error 非空，仍兜底避免空错误日志
        if (error.isEmpty())
            error = QStringLiteral("修补失败（未知错误）");
        emit progress(100, QStringLiteral("修补"));
        emit patchFinished(false, QString(), error);
        return;
    }

    emit progress(100, QStringLiteral("修补"));
    emit patchFinished(true, outPath, QString());
}
