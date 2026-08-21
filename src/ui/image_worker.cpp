#include "ui/image_worker.h"
#include "core/resource_monitor.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryFile>
#include <limits>

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

// ---- G5 流式进度换算与路径判定（worker 内共享）----

// 进度百分比换算：done/total → [0, 99]。封顶 99：100 只由任务完成/失败路径
// 统一发射（面板在 percent >= 100 时隐藏进度条，中途出现 100 会被提前隐藏）。
// total 为 0（空输入/未知）时返回 0，进度保持初始档位。
int pct(quint64 done, quint64 total)
{
    if (total == 0)
        return 0;
    const double p = 100.0 * double(qMin(done, total)) / double(total);
    return qBound(0, int(p), 99);
}

// 路径同一性判定（流式路径守卫用）：两路径指向同一文件时返回 true。
// 存在时用 canonicalFilePath（解析符号链接/相对成分），否则比绝对路径。
bool sameFile(const QString &a, const QString &b)
{
    if (a == b)
        return true;
    const QFileInfo fa(a), fb(b);
    const QString ca = fa.exists() ? fa.canonicalFilePath() : fa.absoluteFilePath();
    const QString cb = fb.exists() ? fb.canonicalFilePath() : fb.absoluteFilePath();
    return !ca.isEmpty() && ca == cb;
}

// 分区 blob 消费字节（与 imgpayload 引擎 processOps 的 blobTotal 公式逐字
// 一致：ZERO/DISCARD 不计；diff 系仅 SOURCE_BSDIFF 消费 blob；累计用引擎同款
// 饱和加法 —— 恶意超大 manifest 下保持单调不溢出）。多分区解包的总进度分母
// = 各分区该值之和。
quint64 partitionBlobTotal(const imgpayload::Partition &part)
{
    quint64 t = 0;
    for (const imgpayload::InstallOp &op : part.ops) {
        if (op.type == imgpayload::OP_ZERO || op.type == imgpayload::OP_DISCARD)
            continue;
        const bool diff = op.type == imgpayload::OP_BSDIFF ||
                          op.type == imgpayload::OP_SOURCE_COPY ||
                          op.type == imgpayload::OP_SOURCE_BSDIFF ||
                          op.type == imgpayload::OP_PUFFDIFF ||
                          op.type == imgpayload::OP_BROTLI_BSDIFF ||
                          op.type == imgpayload::OP_ZUCCHINI;
        if (!diff || op.type == imgpayload::OP_SOURCE_BSDIFF)
            t = (op.dataLength > std::numeric_limits<quint64>::max() - t)
                ? std::numeric_limits<quint64>::max()
                : t + op.dataLength;
    }
    return t;
}

// 递归增量遍历文件系统镜像（listTreeLazy 按目录粒度，不整镜像读入内存）：
// 收集全部非目录条目（含子目录内的）并累计文件总字节（流式解包进度分母）。
// 目录条目沿途 mkpath（与内存路径 doUnpack 一致）；symlink 条目按文件处理
// （ext4 提取链接目标文本、erofs 拒绝非普通文件 → 上层报错，与内存路径对齐）。
// 深度受引擎目录深度上限约束（128），递归安全。
bool collectFsTree(const QString &imagePath, const QString &dir,
                   QList<imgfs::FsEntry> &files, quint64 &totalBytes,
                   QString *error)
{
    QList<imgfs::FsEntry> entries;
    if (!imgfs::listTreeLazy(imagePath, dir, entries, error))
        return false;
    for (const imgfs::FsEntry &e : entries) {
        if (e.isDir) {
            if (!collectFsTree(imagePath, e.path, files, totalBytes, error))
                return false;
        } else {
            files.append(e);
            totalBytes += e.size;
        }
    }
    return true;
}

// 递归收集目录下所有文件绝对路径（tar 流式解包产物统计用：extractTarStream
// 只回调字节进度不回调条目名 → 快照差集即本批产物）。
QSet<QString> snapshotFiles(const QString &dir)
{
    QSet<QString> set;
    QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext())
        set.insert(QDir::cleanPath(it.next()));
    return set;
}

// 快照后新增的文件（流式 tar 解包产物）；覆盖已有同名文件的条目不计数
// （仅影响日志列表，不影响解包结果）。
QStringList diffFiles(const QString &dir, const QSet<QString> &before)
{
    QStringList out;
    QDirIterator it(dir, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString p = QDir::cleanPath(it.next());
        if (!before.contains(p))
            out << p;
    }
    return out;
}

} // namespace

ImageWorker::ImageWorker(QObject *parent)
    : QObject(parent)
{
    // 跨线程 queued 连接要求参数类型运行时已注册（Qt 6 中 Q_DECLARE_METATYPE
    // 生成的 qt_metatype_id() 首次使用即自动注册；此处显式调用双保险）
    qRegisterMetaType<imgreg::Detected>("imgreg::Detected");
    qRegisterMetaType<patcher::PatchConfig>("patcher::PatchConfig");
    qRegisterMetaType<QList<imgfs::FsEntry>>("QList<imgfs::FsEntry>"); // D5 整树投递

    // 工作对象迁移到专用线程：runXxx 投递的 QueuedConnection 事件在此线程
    // 的事件循环中按提交顺序串行处理。先迁移后启动，事件队列已就绪。
    moveToThread(&m_thread);
    m_thread.setObjectName(QStringLiteral("image-worker"));
    m_thread.start();

    // H1 降级接入：整体 CPU 使用率 >80% 时工作线程降为 IdlePriority（仅系统
    // 空闲时被调度，解包/打包这类 CPU 密集任务让位给前台），恢复（<70%）后
    // 回到 NormalPriority（恢复逻辑同此 lambda，未单独分派）。
    //
    // 审查修正（Important）：本对象已在上面 moveToThread(&m_thread)，若沿用
    // 默认 AutoConnection，lambda 会以 QueuedConnection 投递到工作线程事件
    // 循环 —— 长任务（解包/打包）期间工作线程忙于执行、不处理事件，降级在
    // 任务进行中永远不生效。故显式指定 Qt::DirectConnection：cpuHigh 由主
    // 线程的 ResourceMonitor（QTimer 在主线程）发射，lambda 在发射线程（主
    // 线程）内立即执行，直接调用 m_thread.setPriority()。Qt 5.7+ 起
    // QThread::setPriority 线程安全（Qt 6.11 按 QThread 存储的线程 ID 跨线程
    // 生效，内部 pthread_setschedparam），无需切到工作线程执行。context 仍为
    // 本对象 → 析构时自动断开（发射方为单例，生命周期长于本对象，无悬挂）。
    connect(&ResourceMonitor::instance(), &ResourceMonitor::cpuHigh, this,
            [this](bool high, int) {
                m_thread.setPriority(high ? QThread::IdlePriority
                                          : QThread::NormalPriority);
            },
            Qt::DirectConnection);
}

ImageWorker::~ImageWorker()
{
    // 停止工作线程并阻塞等待其退出，防止线程悬挂与资源泄漏。
    // 注意：若正在执行长操作，wait() 会等待其完成（Qt 文档建议常规场景
    // 用 finished() 信号驱动收尾；析构场景 quit()+wait() 是标准做法）。
    m_thread.quit();
    m_thread.wait();
}

bool ImageWorker::useStreamPath(const QString &path)
{
    // size() 对不存在/不可读文件返回 -1 → 恒小于阈值 → 走旧接口（读取失败
    // 以 error 上报，契约不变）。仅按大小判定（格式由引擎侧决定是否提供流式
    // 接口；无流式接口的格式由 worker 内格式分派回退旧路径）。
    const qint64 size = QFileInfo(path).size();
    return size > kStreamThreshold;
}

bool ImageWorker::useStreamPath(const QString &path, imgreg::Format format)
{
    if (!useStreamPath(path))
        return false;
    switch (format) {
    case imgreg::Format::Sparse:
    case imgreg::Format::RawImage:  // 转换/打包（raw→sparse）；解包按钮本就不 enable
    case imgreg::Format::Payload:
    case imgreg::Format::Tar:
    case imgreg::Format::TarMd5:
    case imgreg::Format::Erofs:
    case imgreg::Format::Ext4:
        return true;
    default:
        return false; // 无流式接口（Super/Boot/Dat/Kdz/UpdateApp/Sin/Pac/DiskGpt/TwrpWin）
    }
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
    emit progress(0, QStringLiteral("识别中"));

    imgreg::Detected result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        // 契约：失败以 error 形式上报，绝不崩溃
        result.detail = QStringLiteral("读取失败: ") + file.errorString();
        emit progress(100, QStringLiteral("识别中")); // 失败路径也补发 100（D1 Minor 1）
        emit detectFinished(path, result);
        return;
    }
    // 魔数判定最大读取 2120B（pac 辅助信号 @2116），4096 一次覆盖全部判定点
    const QByteArray header = file.read(4096);
    file.close();

    if (header.isEmpty()) {
        result.detail = QStringLiteral("读取失败: 文件为空");
        emit progress(100, QStringLiteral("识别中")); // 失败路径也补发 100（D1 Minor 1）
        emit detectFinished(path, result);
        return;
    }

    result = imgreg::detect(header, QFileInfo(path).fileName());
    emit progress(100, QStringLiteral("识别中"));
    emit detectFinished(path, result);
}

void ImageWorker::doUnpack(const QString &path, const QString &outDir,
                           const imgreg::Detected &detected)
{
    emit progress(0, QStringLiteral("解包中"));

    // G5：大文件（> kStreamThreshold）走流式接口 —— 引擎逐 chunk/块回调字节
    // 进度 → 按"已处理/总量"百分比真实推进进度条（任务中途封顶 99，100 仅由
    // 本函数收尾路径发射，防面板提前隐藏）；小文件保持旧内存接口（两者输出
    // 逐字节一致，G1-G4 测试回归保障）。格式感知判定（审查修复）：仅对有
    // 流式解包接口的格式（Sparse/Payload/Tar/TarMd5/Erofs/Ext4）走流式；
    // 无流式接口的格式（Super/Boot/Dat/Kdz/UpdateApp/Sin/Pac/DiskGpt/
    // TwrpWin）无论大小保持旧内存路径。
    const bool stream = ImageWorker::useStreamPath(path, detected.format);

    QString error;
    QByteArray data;
    if (!stream) {
        // 仅旧内存路径整文件读入 —— 流式路径各引擎随机读/分块读，避免
        // 64MB+ 文件双读 + 峰值内存≈文件大小（G1-G5 内存 O(chunk) 目标）
        data = readFile(path, &error);
        if (data.isEmpty()) {
            emit progress(100, QStringLiteral("解包中"));
            emit unpackFinished(false, QStringList(), error);
            return;
        }
    }
    if (!QDir().mkpath(outDir)) {
        emit progress(100, QStringLiteral("解包中"));
        emit unpackFinished(false, QStringList(),
                            QStringLiteral("无法创建输出目录: ") + outDir);
        return;
    }

    QStringList outputs;
    switch (detected.format) {
    // ---- sparse：单产物 raw 镜像 ----
    case imgreg::Format::Sparse: {
        const QString out = QDir(outDir).filePath(
            QFileInfo(path).completeBaseName() + QStringLiteral(".img"));
        if (stream) {
            // 流式路径（G5）：内存 O(chunk)，进度 = 已消费输入字节 / 输入大小
            if (sameFile(path, out)) {
                // 守卫（G1 Minor 吸收）：输出与输入同路径时流式接口先截断
                // 输出 → 输入被破坏；旧整读路径语义不同（先读后写）
                error = QStringLiteral("输出路径与输入镜像相同，无法流式解包: ") + out;
                break;
            }
            const quint64 total = quint64(qMax<qint64>(QFileInfo(path).size(), 1));
            const auto cb = [this, total](quint64 done) {
                emit progress(pct(done, total), QStringLiteral("解包中"));
            };
            if (!imgsparse::simg2imgStream(path, out, cb, &error)) {
                if (error.isEmpty()) // 契约要求失败时 error 非空，仍兜底
                    error = QStringLiteral("sparse→raw 流式解包失败");
                break;
            }
            outputs << out;
            break;
        }
        const QByteArray raw = imgsparse::simg2img(data);
        if (raw.isEmpty()) {
            error = QStringLiteral("sparse 解包失败（数据损坏或不受支持）");
            break;
        }
        if (!writeFile(out, raw, &error)) break;
        outputs << out;
        break;
    }

    // ---- payload：每分区一个产物（全量 OTA 无旧镜像；diff 系分区报错终止）----
    case imgreg::Format::Payload: {
        imgpayload::PayloadInfo info;
        if (stream) {
            // 流式路径（G5）：parseManifestFile 只读头 + manifest 区（不读 blob
            // 区），逐分区 extractPartitionStream 边读边写，内存 O(op)/O(chunk)
            if (!imgpayload::parseManifestFile(path, info)) {
                error = QStringLiteral("payload manifest 解析失败");
                break;
            }
        } else if (!imgpayload::parseManifest(data, info)) {
            error = QStringLiteral("payload manifest 解析失败");
            break;
        }
        if (info.partitions.isEmpty()) {
            error = QStringLiteral("payload 无分区");
            break;
        }
        if (stream) {
            // 总进度分母 = 各分区 blob 消费字节之和（与引擎 blobTotal 公式一致，
            // 见 partitionBlobTotal）；进度 = 累计已消费 blob 字节 / 总量
            quint64 totalBlob = 0;
            for (const imgpayload::Partition &p : info.partitions)
                totalBlob += partitionBlobTotal(p);
            quint64 acc = 0;
            for (const imgpayload::Partition &part : info.partitions) {
                QString perr;
                const QString out = QDir(outDir).filePath(
                    sanitizeName(part.name) + QStringLiteral(".img"));
                if (sameFile(path, out)) {
                    error = QStringLiteral("分区 %1 输出路径与输入镜像相同，无法流式解包: %2")
                                .arg(part.name, out);
                    break;
                }
                const auto cb = [this, &acc, totalBlob](quint64 done) {
                    emit progress(pct(acc + done, totalBlob), QStringLiteral("解包中"));
                };
                // oldImagePath 为空：仅 REPLACE 系可解，遇 diff 报"需要旧镜像"（与
                // 旧接口 extractPartition(空 oldImage) 语义一致）
                if (!imgpayload::extractPartitionStream(path, part, out, cb, &perr,
                                                        {}, info.blockSize)) {
                    error = QStringLiteral("分区 %1 解包失败: %2")
                                .arg(part.name,
                                     perr.isEmpty() ? QStringLiteral("流式解包失败") : perr);
                    break;
                }
                acc += partitionBlobTotal(part);
                outputs << out;
            }
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
            emit progress(100 * (++done) / total, QStringLiteral("解包中"));
        }
        break;
    }

    // ---- tar / tar.md5：目录结构展开（防穿越；符号链接不落盘）----
    case imgreg::Format::Tar:
    case imgreg::Format::TarMd5: {
        if (stream) {
            // 流式路径（G5）：extractTarStream 按条目流式写盘，内存 O(chunk)，
            // 自动校验 .tar.md5 尾部校验行（无校验行跳过；有且不符 → 失败）。
            // 进度分母 = 归档文件大小（引擎范围 [0, 归档字节数] 不含尾部 MD5
            // 行，分母略偏大 → 封顶 99，收尾补 100）。
            const quint64 total = quint64(qMax<qint64>(QFileInfo(path).size(), 1));
            const auto cb = [this, total](quint64 done) {
                emit progress(pct(done, total), QStringLiteral("解包中"));
            };
            // 产物统计：接口只回调字节进度不回调条目名 → 解包前快照输出目录
            // 现有文件，完成后差集即本批产物（仅日志用，不影响解包结果）
            const QSet<QString> before = snapshotFiles(outDir);
            if (!imgtar::extractTarStream(path, outDir, cb, &error)) {
                if (error.isEmpty())
                    error = QStringLiteral("tar 流式解包失败");
                break;
            }
            outputs = diffFiles(outDir, before);
            break;
        }
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
            emit progress(100 * (i + 1) / parts.size(), QStringLiteral("解包中"));
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
            emit progress(100 * outputs.size() / files.size(), QStringLiteral("解包中"));
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
            // 注: PacPartition.size 已自 F4-3 起公开（新/旧格式解析均回填真实长度）；
            // UI 侧仍按下一分区 offset / EOF 推断（标准布局下与 size 一致，未改行为）
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
            emit progress(100 * (i + 1) / parts.size(), QStringLiteral("解包中"));
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
                          QStringLiteral("解包中"));
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
        if (stream) {
            // 流式路径（G5）：listTreeLazy 按目录增量遍历（不整镜像读入内存，
            // G4 基准：全树 ~1ms/14MB）+ extractFileStream 逐文件 1MB chunk
            // 流式写盘。两趟：第一趟收集文件清单与总字节（进度分母），第二趟
            // 逐文件提取（进度 = 累计已写字节 / 总量）。
            QList<imgfs::FsEntry> files;
            quint64 totalBytes = 0;
            QString terr;
            if (!collectFsTree(path, QString(), files, totalBytes, &terr)) {
                error = terr;
                break;
            }
            if (files.isEmpty()) {
                error = QStringLiteral("文件系统为空");
                break;
            }
            quint64 acc = 0;
            bool failed = false;
            for (const imgfs::FsEntry &e : files) {
                QString full;
                if (!safeJoin(outDir, e.path, &full)) continue; // 防穿越，跳过
                if (sameFile(path, full)) {
                    // 守卫（G1 Minor 吸收）：输出与输入镜像同路径时流式接口
                    // 先截断输出 → 输入被破坏
                    error = QStringLiteral("条目 %1 输出路径与输入镜像相同，无法流式解包: %2")
                                .arg(e.path, full);
                    failed = true;
                    break;
                }
                if (!QDir().mkpath(QFileInfo(full).absolutePath())) {
                    error = QStringLiteral("无法创建目录: ") + QFileInfo(full).absolutePath();
                    failed = true;
                    break;
                }
                const quint64 total = qMax<quint64>(totalBytes, 1);
                const auto cb = [this, &acc, total](quint64 done) {
                    emit progress(pct(acc + done, total), QStringLiteral("解包中"));
                };
                QString ferr;
                if (!imgfs::extractFileStream(path, e.path, full, cb, &ferr)) {
                    error = QStringLiteral("提取 %1 失败: %2")
                                .arg(e.path, ferr.isEmpty() ? QStringLiteral("流式提取失败") : ferr);
                    failed = true;
                    break;
                }
                acc += e.size;
                outputs << full;
            }
            if (failed) break;
            break;
        }
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

    emit progress(100, QStringLiteral("解包中"));
    emit unpackFinished(error.isEmpty(), outputs, error);
}

void ImageWorker::doPack(const QString &outPath, const QStringList &inputs,
                         const imgreg::Detected &detected)
{
    emit progress(0, QStringLiteral("打包中"));

    // 后端打包接口核实（2026-08-05，以 src/image_engine 头文件为准，非计划文档记忆）：
    //   • imgsparse::img2simg(raw, blockSize=4096)（sparse_image.h L7）→ raw→sparse
    //   • imgsparse::simg2img(sparse)（sparse_image.h L6）→ sparse→raw 逆向
    //   • imgtar::buildTar(entries)（tar_image.h L17）+ imgtar::appendMd5Footer(tar)
    //     （tar_image.h L18，三星 Odin 校验尾: [tar][32hex]  firmware.tar.md5\n）
    //     → .img 集合 → tar / tar.md5（目标格式按输出路径后缀判定）
    //   • imghw::buildUpdateAppWithData(QList<QPair<AppFile,QByteArray>>, SignConfig*)
    //     （huawei_image.h L45-46）：需 AppFile 元数据 + 逐文件数据 —— 面板单文件流
    //     无法提供 → 不接入（UI 待办：update.app 文件表编辑器）
    //   • imgpayload：**无任何打包接口**（payload_image.h 仅 isPayload/parseManifest/
    //     extractPartition，buildFullPayload 不存在）→ 后端待办
    //   • imgboot::repackBootImage(BootInfo)（boot_image.h L21）存在，但需 BootInfo
    //     全量部件（kernel/ramdisk/dtb），面板无编辑流 → 不接入（记录备选，见报告）
    //   • imgkdz（仅 mergeChunks 属解包侧）/imgsuper/imgdat/imgsin/imgpac/imgdisk/
    //     imgtwrp：均无打包接口
    // 打包按钮按"后端可用打包接口"enable（image_tool_panel::updateButtonsFor）；
    // 此处对禁用格式仍防御性返回错误（全局契约：失败 ok=false + error，绝不崩溃）。

    QString error;
    if (outPath.isEmpty()) {
        emit progress(100, QStringLiteral("打包中"));
        emit packFinished(false, QString(), QStringLiteral("输出路径为空"));
        return;
    }
    if (inputs.isEmpty()) {
        emit progress(100, QStringLiteral("打包中"));
        emit packFinished(false, QString(), QStringLiteral("输入文件列表为空"));
        return;
    }
    // 保存对话框一般落在已存在目录，仍防御性创建父目录（writeFile 不建父目录）
    if (!QDir().mkpath(QFileInfo(outPath).absolutePath())) {
        emit progress(100, QStringLiteral("打包中"));
        emit packFinished(false, QString(),
                          QStringLiteral("无法创建输出目录: ")
                              + QFileInfo(outPath).absolutePath());
        return;
    }

    // G5：输入合计 > 阈值走流式接口（buildTarStream 内存 O(chunk)，进度 =
    // 已写入输入字节 / 输入合计；sparse 流式同理）。tar 打包按输入合计判定
    //（多文件集合），sparse/raw 单文件按输入文件大小判定。
    quint64 totalIn = 0;
    for (const QString &in : inputs)
        totalIn += quint64(qMax<qint64>(QFileInfo(in).size(), 0));
    const bool stream = totalIn > quint64(ImageWorker::kStreamThreshold);
    if (stream) {
        // G1 Minor 吸收（流式路径守卫）：输出与任一输入同路径时 buildTarStream/
        // simg2imgStream 边读输入边写输出 → 输入被截断破坏；显式拒绝
        for (const QString &in : inputs) {
            if (sameFile(in, outPath)) {
                emit progress(100, QStringLiteral("打包中"));
                emit packFinished(false, QString(),
                                  QStringLiteral("输出路径与输入文件相同: %1").arg(in));
                return;
            }
        }
        QString serr;
        bool ok = false;
        const auto cb = [this, totalIn](quint64 done) {
            emit progress(pct(done, totalIn), QStringLiteral("打包中"));
        };
        switch (detected.format) {
        case imgreg::Format::Sparse: // sparse → raw
            ok = imgsparse::simg2imgStream(inputs.first(), outPath, cb, &serr);
            break;
        case imgreg::Format::RawImage: // raw → sparse
            ok = imgsparse::img2simgStream(inputs.first(), outPath, 4096, cb, &serr);
            break;
        case imgreg::Format::Tar:
        case imgreg::Format::TarMd5: {
            ok = imgtar::buildTarStream(inputs, outPath, cb, &serr);
            // 三星 Odin 校验尾：无逐块进度回调（增量读算 MD5）→ 进度自然停驻
            // 在 pct 封顶值 99，收尾完成后由下方统一发射 100（审查修复：不再
            // 回跳 95，避免进度条可见倒退）
            if (ok && QFileInfo(outPath).suffix() == QLatin1String("md5"))
                ok = imgtar::appendMd5FooterStream(outPath, &serr);
            break;
        }
        default:
            serr = QStringLiteral("该格式后端无打包接口（后端扩展待办）");
            break;
        }
        if (!ok && serr.isEmpty()) // 契约要求失败时 error 非空，仍兜底
            serr = QStringLiteral("流式打包失败");
        emit progress(100, QStringLiteral("打包中"));
        emit packFinished(ok && serr.isEmpty(), outPath, serr);
        return;
    }

    QByteArray out;
    switch (detected.format) {
    // ---- sparse → raw（逆向解包，与转换同向；单文件输入取首个）----
    case imgreg::Format::Sparse: {
        const QByteArray data = readFile(inputs.first(), &error);
        if (!data.isEmpty())
            out = imgsparse::simg2img(data);
        if (out.isEmpty() && error.isEmpty())
            error = QStringLiteral("sparse→raw 打包失败（数据损坏或不受支持）");
        break;
    }

    // ---- raw → sparse ----
    case imgreg::Format::RawImage: {
        const QByteArray data = readFile(inputs.first(), &error);
        if (!data.isEmpty())
            out = imgsparse::img2simg(data);
        if (out.isEmpty() && error.isEmpty())
            error = QStringLiteral("raw→sparse 打包失败");
        break;
    }

    // ---- .img 集合 → tar / tar.md5 ----
    // 输入为多选文件集合，条目名 = 文件名（扁平；重名会互相覆盖 → 拒绝）。
    // 目标格式按输出路径后缀判定: *.md5（含 *.tar.md5）→ appendMd5Footer，
    // 其余 → 纯 tar（允许 tar.md5 源重打为纯 tar 的逆向路径）。
    case imgreg::Format::Tar:
    case imgreg::Format::TarMd5: {
        QList<imgtar::TarEntry> entries;
        entries.reserve(inputs.size());
        QSet<QString> seen;
        for (const QString &in : inputs) {
            const QByteArray data = readFile(in, &error);
            if (data.isEmpty())
                break;
            const QString name = QFileInfo(in).fileName();
            if (seen.contains(name)) {
                error = QStringLiteral("输入文件包含重名条目: %1（tar 内条目名须唯一）")
                            .arg(name);
                break;
            }
            seen.insert(name);
            imgtar::TarEntry e;
            e.name = name;
            e.data = data;
            entries.append(e);
        }
        if (error.isEmpty()) {
            out = imgtar::buildTar(entries);
            if (out.isEmpty())
                error = QStringLiteral("tar 打包失败（buildTar 返回空）");
        }
        if (error.isEmpty() && QFileInfo(outPath).suffix() == QLatin1String("md5"))
            out = imgtar::appendMd5Footer(out);
        break;
    }

    // ---- 后端缺口（按钮已禁用，防御性返回明确错误，不崩溃）----
    case imgreg::Format::Payload:
        error = QStringLiteral("payload 全量打包未实现（imgpayload 无打包接口，"
                               "buildFullPayload 不存在，后端扩展待办）");
        break;
    case imgreg::Format::UpdateApp:
        error = QStringLiteral("update.app 重打包需逐文件数据与 AppFile 元数据"
                               "（buildUpdateAppWithData 存在），当前面板单文件流"
                               "无法提供（UI 文件表编辑器待办）");
        break;
    default:
        error = QStringLiteral("该格式后端无打包接口（后端扩展待办）");
        break;
    }

    if (error.isEmpty() && !writeFile(outPath, out, &error))
        out.clear();

    emit progress(100, QStringLiteral("打包中"));
    emit packFinished(error.isEmpty(), outPath, error);
}

void ImageWorker::doConvert(const QString &path, const QString &outPath,
                            const imgreg::Detected &detected)
{
    emit progress(0, QStringLiteral("转换中"));

    QString error;
    if (outPath.isEmpty()) {
        emit progress(100, QStringLiteral("转换中"));
        emit convertFinished(false, QString(), QStringLiteral("输出路径为空"));
        return;
    }
    // G5：大文件走流式接口（内存 O(chunk)，进度 = 已消费输入字节 / 输入大小）
    const bool stream = ImageWorker::useStreamPath(path);
    if (stream) {
        // G1 Minor 吸收（流式路径守卫）：输入输出同路径时流式接口先截断输出 →
        // 输入被破坏。仅流式路径拒绝 —— 旧整读路径是先读后写，小文件 in-place
        // 安全（审查修复：保持"小文件保持旧接口行为"约束）
        if (sameFile(path, outPath)) {
            emit progress(100, QStringLiteral("转换中"));
            emit convertFinished(false, outPath,
                                 QStringLiteral("输入输出路径相同，请选择其他输出文件"));
            return;
        }
        const quint64 total = quint64(qMax<qint64>(QFileInfo(path).size(), 1));
        const auto cb = [this, total](quint64 done) {
            emit progress(pct(done, total), QStringLiteral("转换中"));
        };
        bool ok = false;
        if (detected.format == imgreg::Format::Sparse)
            ok = imgsparse::simg2imgStream(path, outPath, cb, &error);
        else if (detected.format == imgreg::Format::RawImage)
            ok = imgsparse::img2simgStream(path, outPath, 4096, cb, &error);
        else
            error = QStringLiteral("当前仅支持 sparse↔raw 转换");
        if (!ok && error.isEmpty())
            error = QStringLiteral("sparse↔raw 流式转换失败");
        emit progress(100, QStringLiteral("转换中"));
        emit convertFinished(ok && error.isEmpty(), outPath, error);
        return;
    }

    const QByteArray data = readFile(path, &error);
    if (data.isEmpty()) {
        emit progress(100, QStringLiteral("转换中"));
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

    emit progress(100, QStringLiteral("转换中"));
    emit convertFinished(error.isEmpty(), outPath, error);
}

void ImageWorker::doPatch(const QString &path, const patcher::PatchConfig &config)
{
    emit progress(0, QStringLiteral("修补中"));

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
        emit progress(100, QStringLiteral("修补中"));
        emit patchFinished(false, QString(), error);
        return;
    }

    emit progress(100, QStringLiteral("修补中"));
    emit patchFinished(true, outPath, QString());
}

// ==================== D5 文件系统浏览（会话型） ====================
//
// 后端接口核实（2026-08-05，以 src/image_engine/fs 实际实现为准，非计划文档）：
//   • imgfs::FsImage（fs_image.h L17-24）：list(dir,out)/extract(path,data)/
//     replace(path,data)/repack() —— list/extract/replace 均只有 bool 返回值，
//     无 error 参数；底层 listTree/extractFile/replaceFile 的 QString *error
//     在 FsImage 包装层被吞掉（fs_image.cpp ErofsImage/Ext4Image）。
//   • imgfs::openFsImage(image,error)（fs_image.cpp L98）：按 imgreg::detect
//     分派 EROFS/ext4；其余格式返回 nullptr + error"不是文件系统镜像"。
//   • ErofsImage：replace() 恒 false（fs_image.cpp L55，EROFS 只读）→
//     UI 侧 EROFS 禁用替换/保存；repack() 原样返回。
//   • Ext4Image：replace() 走 imgext4::replaceFile —— metadata_csum 镜像显式
//     拒绝（ext4_reader.cpp L1016-1021）、legacy block map/目录/符号链接拒绝；
//     均因 FsImage::replace 无 error 参数而无法上抛明细 → 本层拼通用文案。
//   • FsEntry.data：listTree 不填充（erofs_reader.cpp L400-406 / ext4 同），
//     跨线程投递整树无大载荷。
// 本层职责：全内存会话管理（读文件 → openFsImage → 全树 list 一次 → 持 FsImage
// 副本），extract/replace/repack 全部在工作线程执行，UI 线程不接触镜像数据。

imgfs::FsImage *ImageWorker::session(const QString &path) const
{
    if (m_fsSession && m_fsSessionPath == path)
        return m_fsSession.get();
    return nullptr;
}

void ImageWorker::closeSession()
{
    m_fsSession.reset();
    m_fsSessionPath.clear();
}

void ImageWorker::runFsOpen(const QString &path)
{
    QMetaObject::invokeMethod(this, [this, path] { doFsOpen(path); },
                              Qt::QueuedConnection);
}

void ImageWorker::runFsExtractTo(const QString &path, const QString &innerPath,
                                 const QString &outPath)
{
    QMetaObject::invokeMethod(this,
                              [this, path, innerPath, outPath] {
                                  doFsExtractTo(path, innerPath, outPath);
                              },
                              Qt::QueuedConnection);
}

void ImageWorker::runFsReplace(const QString &path, const QString &innerPath,
                               const QString &hostFile)
{
    QMetaObject::invokeMethod(this,
                              [this, path, innerPath, hostFile] {
                                  doFsReplace(path, innerPath, hostFile);
                              },
                              Qt::QueuedConnection);
}

void ImageWorker::runFsRepack(const QString &path, const QString &outPath)
{
    QMetaObject::invokeMethod(this, [this, path, outPath] { doFsRepack(path, outPath); },
                              Qt::QueuedConnection);
}

void ImageWorker::runFsClose(const QString &path)
{
    QMetaObject::invokeMethod(this, [this, path] { doFsClose(path); },
                              Qt::QueuedConnection);
}

void ImageWorker::doFsOpen(const QString &path)
{
    emit progress(0, QStringLiteral("文件浏览中"));

    QString error;
    const QByteArray data = readFile(path, &error);
    if (data.isEmpty()) {
        closeSession();
        emit progress(100, QStringLiteral("文件浏览中"));
        emit fsOpened(path, false, QList<imgfs::FsEntry>(), error);
        return;
    }
    imgfs::FsImage *fs = imgfs::openFsImage(data, &error);
    if (!fs) {
        closeSession();
        emit progress(100, QStringLiteral("文件浏览中"));
        emit fsOpened(path, false, QList<imgfs::FsEntry>(), error);
        return;
    }
    // 全树一次遍历（list(dir="") 即整树，FsImage 无增量接口；目录深度由引擎
    // 限 128，失败不崩溃，契约返回 false）
    QList<imgfs::FsEntry> entries;
    if (!fs->list(QString(), entries)) {
        error = QStringLiteral("文件系统遍历失败（镜像损坏或不受支持的特性）");
        delete fs;
        closeSession();
        emit progress(100, QStringLiteral("文件浏览中"));
        emit fsOpened(path, false, QList<imgfs::FsEntry>(), error);
        return;
    }
    closeSession();              // 替换上一会话（同 path 重新打开也重建会话）
    m_fsSession.reset(fs);
    m_fsSessionPath = path;
    emit progress(100, QStringLiteral("文件浏览中"));
    emit fsOpened(path, true, entries, QString());
}

void ImageWorker::doFsExtractTo(const QString &path, const QString &innerPath,
                                const QString &outPath)
{
    emit progress(0, QStringLiteral("文件浏览中"));

    imgfs::FsImage *fs = session(path);
    if (!fs) {
        emit progress(100, QStringLiteral("文件浏览中"));
        emit fsExtracted(path, innerPath, QString(), false,
                         QStringLiteral("会话已关闭（请重新打开镜像）"));
        return;
    }
    QByteArray data;
    if (!fs->extract(innerPath, data)) {
        // FsImage::extract 无 error 参数：erofs 压缩（LZ4）/目录/特殊文件均
        // 返回 false，无法区分（后端扩展待办：错误上抛）→ 按已知语义拼文案
        emit progress(100, QStringLiteral("文件浏览中"));
        emit fsExtracted(path, innerPath, QString(), false,
                         QStringLiteral("提取失败：文件可能为 LZ4 压缩、目录或特殊文件"));
        return;
    }

    QString dest = outPath;
    QString error;
    if (dest.isEmpty()) {
        // 外部打开：写系统临时目录（保留原文件名与扩展名，供系统程序识别类型）
        const QString base = sanitizeName(innerPath);
        const QFileInfo bfi(base);
        QString tmpl = QDir::tempPath() + QStringLiteral("/PhoneToolbox-")
                       + bfi.completeBaseName() + QStringLiteral("-XXXXXX");
        if (!bfi.suffix().isEmpty())
            tmpl += QLatin1Char('.') + bfi.suffix();
        QTemporaryFile tmp(tmpl);
        tmp.setAutoRemove(false); // 外部程序打开期间文件须保留（OS 临时目录自清理）
        if (!tmp.open()) {
            emit progress(100, QStringLiteral("文件浏览中"));
            emit fsExtracted(path, innerPath, QString(), false,
                             QStringLiteral("无法创建临时文件: ") + tmp.errorString());
            return;
        }
        if (tmp.write(data) != data.size()) {
            emit progress(100, QStringLiteral("文件浏览中"));
            emit fsExtracted(path, innerPath, QString(), false,
                             QStringLiteral("写入临时文件不完整"));
            return;
        }
        tmp.close();
        dest = tmp.fileName();
    } else {
        if (!QDir().mkpath(QFileInfo(outPath).absolutePath())) {
            emit progress(100, QStringLiteral("文件浏览中"));
            emit fsExtracted(path, innerPath, QString(), false,
                             QStringLiteral("无法创建目标目录: ")
                                 + QFileInfo(outPath).absolutePath());
            return;
        }
        if (!writeFile(dest, data, &error)) {
            emit progress(100, QStringLiteral("文件浏览中"));
            emit fsExtracted(path, innerPath, QString(), false, error);
            return;
        }
    }

    emit progress(100, QStringLiteral("文件浏览中"));
    emit fsExtracted(path, innerPath, dest, true, QString());
}

void ImageWorker::doFsReplace(const QString &path, const QString &innerPath,
                              const QString &hostFile)
{
    emit progress(0, QStringLiteral("文件浏览中"));

    imgfs::FsImage *fs = session(path);
    if (!fs) {
        emit progress(100, QStringLiteral("文件浏览中"));
        emit fsReplaced(path, innerPath, hostFile, false,
                        QStringLiteral("会话已关闭（请重新打开镜像）"));
        return;
    }
    if (hostFile.isEmpty()) {
        emit progress(100, QStringLiteral("文件浏览中"));
        emit fsReplaced(path, innerPath, hostFile, false,
                        QStringLiteral("替换文件路径为空"));
        return;
    }
    QString error;
    const QByteArray data = readFile(hostFile, &error);
    if (data.isEmpty()) {
        emit progress(100, QStringLiteral("文件浏览中"));
        emit fsReplaced(path, innerPath, hostFile, false, error);
        return;
    }
    if (!fs->replace(innerPath, data)) {
        // FsImage::replace 无 error 参数：ErofsImage 恒 false；Ext4Image 对
        // metadata_csum/legacy block map/目录/符号链接拒绝（明细无法上抛）→
        // UI 侧按已禁用 EROFS 按钮，此处文案合并已知 ext4 拒绝原因
        emit progress(100, QStringLiteral("文件浏览中"));
        emit fsReplaced(path, innerPath, hostFile, false,
                        QStringLiteral("替换失败（ext4 校验和/metadata_csum 镜像、"
                                       "目录/符号链接或 legacy block map 布局暂不支持）"));
        return;
    }
    emit progress(100, QStringLiteral("文件浏览中"));
    emit fsReplaced(path, innerPath, hostFile, true, QString());
}

void ImageWorker::doFsRepack(const QString &path, const QString &outPath)
{
    emit progress(0, QStringLiteral("文件浏览中"));

    imgfs::FsImage *fs = session(path);
    if (!fs) {
        emit progress(100, QStringLiteral("文件浏览中"));
        emit fsRepacked(path, outPath, false,
                        QStringLiteral("会话已关闭（请重新打开镜像）"));
        return;
    }
    if (outPath.isEmpty()) {
        emit progress(100, QStringLiteral("文件浏览中"));
        emit fsRepacked(path, outPath, false, QStringLiteral("输出路径为空"));
        return;
    }
    const QByteArray packed = fs->repack();
    if (packed.isEmpty()) {
        emit progress(100, QStringLiteral("文件浏览中"));
        emit fsRepacked(path, outPath, false,
                        QStringLiteral("重打包失败（后端返回空镜像）"));
        return;
    }
    QString error;
    if (!writeFile(outPath, packed, &error)) {
        emit progress(100, QStringLiteral("文件浏览中"));
        emit fsRepacked(path, outPath, false, error);
        return;
    }
    emit progress(100, QStringLiteral("文件浏览中"));
    emit fsRepacked(path, outPath, true, QString());
}

void ImageWorker::doFsClose(const QString &path)
{
    if (m_fsSessionPath == path)
        closeSession();
}
