// src/core/modes/edl_handler.cpp
//
// EDL（9008）通道的薄封装：设备侧全部委托给 src/core/edl（libusb 传输 + 会话编排）。
// 本文件与 src/core/edl/edl_libusb_transport.cpp 是全仓**仅有的两处** libusb 依赖。
//
// 旧单体实现已整体删除（手搓 Sahara 帧 / 手搓 firehose XML / 4 字节长度前缀 / 恒假 ACK 判定 /
// 无数据面），其协议逻辑的替代物与逐条修正见 src/core/edl/{sahara,firehose,edl_session}.cpp 的
// 头注释（对照协议速查 §6 的 5 处缺陷）。本文件只剩三件事：
//   ① 连接生命周期（Sahara 引导 → 等重枚举 → configure）；
//   ② EDLPartition ↔ 单条目 FlashPlan 的适配（listPartitions/readPartition/writePartition/writeRaw）；
//   ③ 中文错误与进度落既有两条信号（outputMessage/progress）。
#include "edl_handler.h"

#include <QFile>
#include <QFileInfo>

#include <limits>

#include "core/edl/firehose.h"
#include "core/edl/sahara.h"
#include "image_engine/sparse_image.h"

namespace {

// 命令响应等待（与 edl_session.cpp 的 kCmdTimeoutMs 同量级：qdl firehose.c:1217）
constexpr int kCmdTimeoutMs = 10000;
// disconnect() 的复位等待：设备常在 ACK 后立刻重启（或直接掉线）→ 短等即可，结果一律不报错
constexpr int kDisconnectResetTimeoutMs = 3000;
// 调用方没给扇区大小时的兜底（旧实现的 <program SECTOR_SIZE_IN_BYTES> 也是按调用方传入）
constexpr quint32 kDefaultSectorSize = 4096;

// "lun<N>"（listPartitions 的产物）→ LUN 号。EDLPartition 没有 lun 字段（公开 API 不能改），
// 故读/写路径只能从名字反解（listPartitions 的头注释写明了这个约定）。
bool lunFromName(const QString &name, quint32 *lun)
{
    if (!name.startsWith(QLatin1String("lun"), Qt::CaseInsensitive))
        return false;
    bool ok = false;
    const uint n = name.mid(3).toUInt(&ok);
    if (!ok)
        return false;
    *lun = n;
    return true;
}

} // namespace

EDLHandler::EDLHandler(QObject *parent)
    : QObject(parent)
    // m_session 绑定 m_transport；configure 协商结果（载荷上限）留在会话内，写路径复用，不重复 configure
    , m_session(m_transport, [this](const edl::SessionProgress &p) {
          // 会话进度 → 既有两条信号。日志只在 (stage, detail) **变化**时落一条：write 阶段每块都会
          // 回调一次，逐条落会把输出面板刷爆；百分比始终转发给 progress(int)。
          const QString key = p.stage + QLatin1Char('\x1f') + p.detail;
          if (key != m_lastProgressKey) {
              m_lastProgressKey = key;
              if (!p.detail.isEmpty())
                  emit outputMessage(QStringLiteral("[%1] %2").arg(p.stage, p.detail), false);
          }
          emit progress(p.percent);
      })
{
}

EDLHandler::~EDLHandler()
{
    disconnect();
}

// 失败统一出口：落 lastError() + 一条错误日志（旧实现同样是"设 m_lastError 再 outputMessage"）
bool EDLHandler::fail(const QString &msg)
{
    m_lastError = msg;
    emit outputMessage(msg, true);
    return false;
}

// ==================== 连接生命周期 ====================

// Sahara 引导：打开 9008 → 载 programmer → 关句柄 → 等重枚举（返回时设备已重新打开且在 Firehose）
bool EDLHandler::connectSahara(const QString &programmerPath)
{
    emit outputMessage(QStringLiteral("EDL: 连接 Sahara 模式..."), false);
    emit progress(5);

    // programmer 先读进内存：文件打不开/为空在**碰设备之前**就拒（旧实现是先开设备再读盘）
    QFile prog(programmerPath);
    if (!prog.open(QIODevice::ReadOnly)) {
        return fail(QStringLiteral("无法打开 programmer 文件：%1（%2）")
                        .arg(programmerPath, prog.errorString()));
    }
    const QByteArray programmer = prog.readAll();
    prog.close();
    if (programmer.isEmpty())
        return fail(QStringLiteral("programmer 文件为空：%1").arg(programmerPath));
    emit outputMessage(QStringLiteral("Programmer 大小: %1 字节").arg(programmer.size()), false);

    // 从 Sahara 阶段重新开始：传输层的阶段状态可能还停在上一轮的 Firehose
    // （EdlUsbStage 是传输层的模式状态，见 edl_libusb_transport.h）
    m_transport.setStage(edl::EdlUsbStage::Sahara);
    m_connected = false;
    m_configured = false;
    m_lastProgressKey.clear();

    // ① 打开 + 载 programmer。失败一律**不发 reset**（设备留在 EDL 便于重试；spec §4 错误矩阵）
    {
        QString openErr;
        if (!m_transport.open(&openErr))
            return fail(QStringLiteral("EDL: 无法打开 9008 设备：%1").arg(openErr));
        emit progress(15);

        QString loadErr;
        if (!edl::saharaLoadProgrammer(m_transport, programmer, &loadErr)) {
            m_transport.close();
            return fail(loadErr);
        }
    }
    emit outputMessage(QStringLiteral("Sahara 握手与 programmer 加载完成"), false);
    emit progress(60);

    // ② 关句柄 + 等重枚举。契约（edl_transport.h:14-19）：waitReenumerate 调用前须已 close()，
    //    返回 true 时设备**已重新 open()** —— 故这里不再 open 一次。
    m_transport.close();
    emit outputMessage(QStringLiteral("等待设备切换到 Firehose 模式..."), false);
    {
        // 预算取 FlashOptions 的默认值（45000 = 既有 3s 初等 + 15 次重试；轮询间隔由传输实现定）
        const edl::FlashOptions opt;
        QString reenumErr;
        if (!m_transport.waitReenumerate(opt.reenumerateTimeoutMs, &reenumErr)) {
            m_transport.close();
            return fail(reenumErr);
        }
    }

    m_connected = true;
    emit outputMessage(QStringLiteral("Firehose 模式连接成功"), false);
    emit progress(70);
    return true;
}

// 断开：best-effort 复位 + 释放句柄（失败不报错 —— 设备常常已经自己重启了）
void EDLHandler::disconnect()
{
    if (m_connected && m_transport.isOpen()) {
        edl::FirehoseResponse resp;
        QString ignored;
        edl::firehoseSendCommand(m_transport, edl::xmlReset(), resp,
                                 kDisconnectResetTimeoutMs, &ignored);
    }
    m_transport.close();          // 幂等（未打开时是 no-op），契约见 edl_transport.h
    m_connected = false;
    m_configured = false;
    m_lastProgressKey.clear();
}

// ==================== Firehose 会话 ====================

// configure 协商（**发完等响应** —— 旧实现只发不读，缺陷 3）。同一会话只做一次。
bool EDLHandler::ensureFirehoseConfigured()
{
    if (m_configured)
        return true;

    // 首选 eMMC（与旧实现一致）；设备不支持时 firehoseConfigure 会自动换型重试一次（firehose.h）
    QString memoryName = QStringLiteral("emmc");
    quint32 payload = 0;
    QString cfgErr;
    if (!m_session.beginFirehose(memoryName, payload, &cfgErr))
        return fail(QStringLiteral("Firehose 配置失败: %1").arg(cfgErr));

    m_configured = true;
    if (payload == 0) {
        emit outputMessage(QStringLiteral("Firehose 会话已配置（MemoryName=%1）："
                                          "设备未回报载荷上限，按保守默认分块").arg(memoryName), false);
    } else {
        emit outputMessage(QStringLiteral("Firehose 会话已配置（MemoryName=%1，载荷上限 %2 字节）")
                               .arg(memoryName).arg(payload), false);
    }
    return true;
}

bool EDLHandler::firehoseConnect()
{
    if (!m_connected)
        return fail(QStringLiteral("未连接 EDL 设备"));
    return ensureFirehoseConfigured();
}

// getstorageinfo 单次查询（命令构造/响应解析都在 firehose.cpp；这里只做调用与中文错误包装）
bool EDLHandler::queryStorageInfo(quint32 lun, edl::StorageInfo &out,
                                  quint32 *reportedLunCount, QString *error)
{
    edl::drainResidual(m_transport);      // 每笔写前清 IN 端点残留（qdl firehose.c:249-252）

    edl::FirehoseResponse resp;
    QString sendErr;
    if (!edl::firehoseSendCommand(m_transport, edl::xmlGetStorageInfo(lun), resp,
                                  kCmdTimeoutMs, &sendErr)) {
        if (error)
            *error = QStringLiteral("getstorageinfo 失败（LUN %1）：%2").arg(lun).arg(sendErr);
        return false;
    }
    if (!resp.ack) {
        if (error)
            *error = QStringLiteral("getstorageinfo 被拒（LUN %1）：%2").arg(lun)
                         .arg(resp.errorText.isEmpty() ? resp.raw : resp.errorText);
        return false;
    }
    QString parseErr;
    if (!edl::parseStorageInfo(resp, lun, out, &parseErr)) {
        if (error)
            *error = QStringLiteral("getstorageinfo 响应不可用（LUN %1）：%2").arg(lun).arg(parseErr);
        return false;
    }
    if (reportedLunCount)
        *reportedLunCount = edl::parseLunCount(resp);
    return true;
}

// 分区列表：Firehose **没有**"列出分区"的命令（旧实现的 `<partition>` 不是有效命令，其解析也从未
// 成立过）。能问到的只有存储几何 → 逐 LUN 产出一条 EDLPartition（name = "lun<N>"）。
// LUN 0 先查；响应报了 LUN 数（num_physical_partitions / bNumberLu / UFS Total Active LU）
// 则逐个再查，上限 8（parseLunCount 的防呆）。分区名请以刷写计划/GPT 为准（头文件已写明）。
QList<EDLPartition> EDLHandler::listPartitions()
{
    if (!m_connected) {
        fail(QStringLiteral("未连接 EDL 设备"));
        return {};
    }
    if (!ensureFirehoseConfigured())
        return {};

    QList<EDLPartition> parts;
    quint32 total = 1;                       // 设备没报 LUN 数时只查 LUN 0
    for (quint32 lun = 0; lun < total; ++lun) {
        edl::StorageInfo si;
        QString geoErr;
        quint32 reported = 0;
        if (!queryStorageInfo(lun, si, &reported, &geoErr)) {
            fail(geoErr);                    // 失败即返回空列表：绝不交出"查了一半"的列表给调用方
            return {};
        }
        if (lun == 0 && reported > 0)
            total = reported;                // 设备上报的 LUN 数只在首轮生效（同一次查询的上下文）

        EDLPartition p;
        p.name = QStringLiteral("lun%1").arg(lun);
        p.startSector = 0;
        p.numSectors = si.totalBlocks;       // 整个 LUN 的可写区间
        p.sectorSize = si.blockSize;
        p.isReadOnly = false;
        parts.append(p);
    }

    emit outputMessage(QStringLiteral("Firehose 无法枚举分区名：已按 LUN 列出 %1 个卷"
                                      "（分区名请以刷写计划/GPT 为准）").arg(parts.size()), false);
    return parts;
}

// 只读回：几何先查（readBack 用它在发送前拒越界），<read> 命令与数据面都在会话里
bool EDLHandler::readPartition(const EDLPartition &part, const QString &outputPath)
{
    if (!m_connected)
        return fail(QStringLiteral("EDL 未连接"));
    if (!ensureFirehoseConfigured())
        return false;

    quint32 lun = 0;
    if (!lunFromName(part.name, &lun)) {
        return fail(QStringLiteral("分区名 %1 解析不出 LUN —— 本路径要求 listPartitions 产出的 \"lun<N>\"")
                        .arg(part.name));
    }
    if (part.numSectors == 0 || part.sectorSize == 0) {
        return fail(QStringLiteral("分区 %1 的几何非法（扇区数 %2，扇区大小 %3）")
                        .arg(part.name).arg(part.numSectors).arg(part.sectorSize));
    }

    edl::StorageInfo si;
    QString geoErr;
    if (!queryStorageInfo(lun, si, nullptr, &geoErr))
        return fail(geoErr);

    edl::ReadRequest req;
    req.lun = lun;
    req.startSector = part.startSector;
    req.numSectors = part.numSectors;
    req.sectorSize = part.sectorSize;
    req.outputPath = outputPath;

    emit outputMessage(QStringLiteral("读取分区: %1（LUN %2，扇区 %3..%4）")
                           .arg(part.name).arg(lun).arg(part.startSector)
                           .arg(part.startSector + part.numSectors), false);
    QString err;
    if (!m_session.readBack({req}, {si}, &err)) {
        // readBack 失败会删掉写了一半的输出文件（半截备份比没有更危险）
        return fail(err);
    }
    emit outputMessage(QStringLiteral("已读取 %1").arg(outputPath), false);
    return true;
}

// 写入：镜像路径 + 目标区间 → 单条目计划 → 会话的写入路径（<program> 含 physical_partition_number
// + 数据面：分块/ZLP/扇区补零/sparse 展开，缺陷 4 的解）
bool EDLHandler::writeImageEntry(const QString &imageFile, const QString &label, quint32 lun,
                                 quint64 startSector, quint64 numSectors, quint32 sectorSize,
                                 quint64 upperBoundSectors)
{
    if (sectorSize == 0)
        return fail(QStringLiteral("扇区大小为 0，无法计算分块（%1）").arg(imageFile));
    if (!ensureFirehoseConfigured())
        return false;

    // 镜像事实：sparse → **展开后**字节数（数据面边展开边发，不落临时文件）；否则文件大小
    QFile f(imageFile);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("无法打开镜像文件 %1：%2").arg(imageFile, f.errorString()));
    const quint64 fileBytes = quint64(f.size());
    const QByteArray head = f.read(28);      // sparse 头前 28 字节足够算展开后大小
    f.close();

    edl::PlanEntry e;
    e.action = edl::PlanEntry::Action::Program;
    e.partitionName = label;
    e.imageFile = imageFile;
    e.lun = lun;
    e.startSector = startSector;
    e.sectorSize = sectorSize;
    e.sparse = imgsparse::isSparse(head);
    quint64 rawBytes = fileBytes;
    if (e.sparse && !imgsparse::sparseRawSizeFromHeader(head, rawBytes)) {
        return fail(QStringLiteral("sparse 头不可用（%1）：magic 正确但 blk_sz/total_blks 非法")
                        .arg(imageFile));
    }

    // 声明量：调用方给了就用（writeRaw），否则按镜像事实推导（writePartition，避免把整个 LUN 当写入量）
    const quint64 derivedSectors = (rawBytes + sectorSize - 1) / sectorSize;
    if (numSectors == 0)
        numSectors = derivedSectors;
    if (numSectors == 0)
        return fail(QStringLiteral("镜像 %1 为空（0 扇区），拒绝写入").arg(imageFile));
    if (upperBoundSectors > 0 && numSectors > upperBoundSectors) {
        return fail(QStringLiteral("镜像 %1 需要 %2 扇区，超过目标区间上限 %3 扇区")
                        .arg(imageFile).arg(numSectors).arg(upperBoundSectors));
    }
    if (numSectors > derivedSectors) {
        // 明码标价：会话的数据面会把不足的部分**补零**写到设备（qdl 同款：firehose.c:1089-1097）
        emit outputMessage(QStringLiteral("注意：声明 %1 扇区大于镜像实际 %2 扇区 —— "
                                          "余量将按数据面规则补零写入").arg(numSectors).arg(derivedSectors),
                           false);
    }
    if (numSectors > (std::numeric_limits<quint64>::max)() / quint64(sectorSize))
        return fail(QStringLiteral("扇区数 × 扇区大小 溢出 u64（%1）").arg(imageFile));
    e.numSectors = numSectors;
    e.rawBytes = numSectors * quint64(sectorSize);      // 进度分母口径：实际下发的字节量

    edl::FlashPlan plan;
    plan.source = QStringLiteral("EDLHandler 单条目（%1）").arg(label);
    plan.entries.append(e);

    // 归一化（sparse 条目会按文件头复核 numSectors/rawBytes）+ finalizePlan（totalBytes = 进度分母）
    QStringList warnings;
    QString normErr;
    if (!edl::normalizePlan(plan, warnings, &normErr))
        return fail(QStringLiteral("镜像归一化失败：%1").arg(normErr));
    edl::finalizePlan(plan);
    for (const QString &w : warnings)
        emit outputMessage(QStringLiteral("[计划] %1").arg(w), false);

    emit outputMessage(QStringLiteral("写入分区: %1（LUN %2，起始扇区 %3，%4 扇区 × %5 字节%6）")
                           .arg(label).arg(lun).arg(startSector).arg(numSectors).arg(sectorSize)
                           .arg(e.sparse ? QStringLiteral("，sparse 展开") : QString()), false);

    edl::FlashOptions opt;      // 默认：边写边校验（条目无 sha256 时跳过）、不做刷前全量校验
    QString err;
    if (!m_session.writePlan(plan, opt, &err)) {
        // 会话的失败路径会关句柄（edl_session.cpp 的 writePlan）→ 连接的记账同步失效
        m_connected = false;
        m_configured = false;
        return fail(err);
    }
    emit outputMessage(QStringLiteral("写入完成"), false);
    return true;
}

bool EDLHandler::writePartition(const EDLPartition &part, const QString &imagePath)
{
    const QFileInfo fi(imagePath);
    if (!fi.exists() || !fi.isFile())
        return fail(QStringLiteral("文件不存在: %1").arg(imagePath));
    if (!m_connected)
        return fail(QStringLiteral("EDL 未连接"));

    quint32 lun = 0;
    if (!lunFromName(part.name, &lun)) {
        return fail(QStringLiteral("分区名 %1 解析不出 LUN —— 本路径要求 listPartitions 产出的 \"lun<N>\"")
                        .arg(part.name));
    }

    // numSectors 由**镜像文件事实**决定（不能拿 part.numSectors：那是整个 LUN 的大小 ——
    // 否则 4 KiB 的镜像会被补零写到 GB 级）；part.numSectors 只作上限（超出即拒）。
    return writeImageEntry(fi.absoluteFilePath(), part.name, lun, part.startSector,
                           /*numSectors=*/0,
                           part.sectorSize > 0 ? part.sectorSize : kDefaultSectorSize,
                           /*upperBoundSectors=*/part.numSectors);
}

bool EDLHandler::writeRaw(const QString &filename, quint64 startSector,
                          quint64 numSectors, quint64 sectorSize)
{
    const QFileInfo fi(filename);        // ⚠️ 语义修正：本参数是**路径**，不再是包内裸文件名
    if (!fi.exists() || !fi.isFile())
        return fail(QStringLiteral("文件不存在: %1").arg(filename));
    if (!m_connected)
        return fail(QStringLiteral("EDL 未连接"));

    // 旧签名没有 LUN（旧实现同样写在 LUN 0：它的 <program> 干脆没有 physical_partition_number）。
    // 需要写其它 LUN 时用 writePartition + listPartitions 的 "lun<N>"。
    return writeImageEntry(fi.absoluteFilePath(), fi.fileName(), /*lun=*/0, startSector,
                           numSectors,
                           sectorSize > 0 ? quint32(sectorSize) : kDefaultSectorSize,
                           /*upperBoundSectors=*/0);
}
