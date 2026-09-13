// src/core/edl/edl_session.cpp
//
// 会话编排 + 数据面。执行顺序（spec §4）：
//   Sahara(载 programmer) → 关设备 → 等重枚举 → 重开 → configure → getstorageinfo（逐 LUN）
//   → validatePlan（不过则拒刷）→ 逐条目 Program/Erase/Patch → setbootablestoragedrive（含 xbl/sbl1）
//   → reset（**仅成功路径一次**）
//
// 数据面不变量（每条都能在 tests/test_edl_session.cpp 里按字节断言）：
//   1. 命令一律走 firehoseSendCommand（唯一发送出口，线帧 = `<?xml …?><data>…</data>`，**无长度前缀**）；
//      数据是**裸写 OUT**（无帧头、无长度前缀）—— program 命令之后就是原始字节流
//      （reference/qdl/src/firehose.c:1021-1035 发命令、:1101-1125 的 qdl_write 发数据）。
//   2. 分块 chunkSectors = max(1, maxPayload / sectorSize)：qdl 用
//      MIN(max_payload_size / sector_size, left)（firehose.c:1080），但 max_payload_size < sector_size
//      时 qdl 会算出 chunk_size=0 并死循环 —— 取 max(1, …) 兜底。
//   3. `len % maxPacketSize() == 0` → 补一次 0 字节写（ZLP）：reference/qdl/src/usb.c:548-553。
//      **由本数据面负责**（qdl 的 usb 层对所有写都补；这里只对数据块补，命令帧的 ZLP 留给
//      libusb 传输层 —— Task 7 的 write() 不得对数据块再补一次，否则真机上每块多一个 ZLP）。
//   4. 一条 program 的**数据总量恒为 numSectors × sectorSize**：源头不足补零（qdl 的
//      memset 残段：firehose.c:1089-1097），源头超出截断（qdl 只按 num_sectors 发）并告警。
//   5. **整条 program 的数据发完只等一个 ACK**（firehose.c:1132-1137），块间不额外等待。
//   6. 每笔写之前 drain 掉 IN 端点残留：firehoseSendCommand 见到 <response> 即停读
//      （firehose.cpp 的注释），而 qdl 明确"不消费完后续写会超时"（firehose.c:249-252）。
#include "edl_session.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>

#include "firehose.h"
#include "image_engine/sparse_image.h"
#include "sahara.h"

namespace edl {
namespace {

// 设备未回 MaxPayloadSizeToTargetInBytesSupported 时的保守默认 —— **不得**把 0 当"无上限"
// （那会让数据面试图一次发完整个镜像）。依据：qdl 的载荷上限只来自 configure 协商
// （reference/qdl/src/firehose.c:534-548），未协商到值时是编译期默认 1048576
// （usb.c:580 初始化即 1048576；auto.c:162、sim.c:635、qud.c:545 同）。
constexpr quint32 kDefaultMaxPayloadBytes = 1024 * 1024;

constexpr int kCmdTimeoutMs       = 10000;    // 命令响应等待（qdl 同量级：firehose.c:1217）
constexpr int kDataAckTimeoutMs   = 120000;   // program 数据发完的 ACK（qdl：firehose.c:1132-1137）
constexpr int kReadTimeoutMs      = 30000;    // 回读数据阶段单次读超时（qdl：firehose.c:1241）
constexpr int kReenumTimeoutMs    = 30000;    // Sahara→Firehose 重枚举等待
constexpr int kReadChunkBytes     = 4096;     // 单次 IN 传输读取上限（与 firehose.cpp:462 同款）
constexpr int kMaxReadsPerResponse = 64;      // 读次数上限（防"永远不含 <response"时死循环）
// drain 轮询的次数上限：每次轮询都是非阻塞的（0 超时），正常最多几次就静默；
// 给设备"话痨日志"留足余量的同时保证**不会挂死**（上限 64×4096 字节）。
constexpr int kMaxDrainPolls      = 64;
constexpr quint64 kZeroPieceBytes = 1024 * 1024;   // 补零/填充的生成片大小（不 materialize 整段）

QString actionName(PlanEntry::Action a)
{
    switch (a) {
    case PlanEntry::Action::Program: return QStringLiteral("program");
    case PlanEntry::Action::Erase:   return QStringLiteral("erase");
    case PlanEntry::Action::Patch:   return QStringLiteral("patch");
    }
    return QStringLiteral("?");
}

// 条目在日志/错误里的名字：partitionName 优先，Patch 用文件名，都空时退化为动作名
QString entryName(const PlanEntry &e)
{
    if (!e.partitionName.isEmpty()) return e.partitionName;
    if (!e.imageFile.isEmpty())     return e.imageFile;
    return actionName(e.action);
}

// 起始扇区的展示形态：表达式原样（主机侧不解释，flash_plan.h:17-19）
QString startSectorText(const PlanEntry &e)
{
    return e.startSectorExpr.isEmpty() ? QString::number(e.startSector) : e.startSectorExpr;
}

// 数据面/命令失败的统一文案（spec §4 表：必须给出"已写入 N 扇区、失败于偏移 M"与条目名）
QString entryFailText(const PlanEntry &e, quint64 writtenSectors, quint64 offsetBytes, const QString &why)
{
    return QStringLiteral("条目 %1（%2，LUN %3，起始扇区 %4）失败：已写入 %5 扇区、失败于偏移 %6 字节；%7")
        .arg(entryName(e), actionName(e.action))
        .arg(e.lun)
        .arg(startSectorText(e))
        .arg(writtenSectors)
        .arg(offsetBytes)
        .arg(why);
}

// 写之前的 drain：把 IN 端点里"响应之后还跟着的字节"读干净。0 超时 = 非阻塞轮询
// （真机 = libusb_bulk_transfer(timeout=0) 立即返回，见 edl_transport.h 的 read 说明）；
// 空返回即端点静默。qdl 的做法是"见到响应后继续读到超时"（firehose.c:249-252、:274-276），
// 差别只是它给每次读 100ms —— 我们不等（写之前没有待等的数据）。
void drainResidual(IEdlTransport &t)
{
    QString ignored;
    for (int i = 0; i < kMaxDrainPolls; ++i) {
        if (t.read(kReadChunkBytes, 0, &ignored).isEmpty())
            break;
    }
}

// 只等响应、不发命令：数据面 raw 数据之后的那一个 ACK（firehose.c:1132-1137）与回读操作的
// 收尾响应都走这里。**不重复实现解析** —— 复用 Task 5 的 parseFirehoseResponse（严格 ACK/NAK）。
bool waitResponse(IEdlTransport &t, FirehoseResponse &resp, int timeoutMs,
                  const QString &what, QString *error)
{
    QByteArray buf;
    QElapsedTimer clock;
    clock.start();
    int reads = 0;
    while (!buf.contains("<response") && reads < kMaxReadsPerResponse && clock.elapsed() < timeoutMs) {
        QString readErr;
        const QByteArray chunk = t.read(kReadChunkBytes, timeoutMs, &readErr);
        if (chunk.isEmpty())
            break;                    // 超时/掉线（read 约定：超时 → 空 + error）
        buf += chunk;
        ++reads;
    }
    resp = parseFirehoseResponse(buf);
    if (!resp.ack && !resp.nak) {
        if (error) {
            *error = QStringLiteral("%1：等待响应超时（%2 ms 内未收到 <response>）").arg(what).arg(timeoutMs);
            if (!buf.isEmpty())
                *error += QStringLiteral("；设备返回：%1").arg(QString::fromUtf8(buf.left(200)));
        }
        return false;
    }
    return true;
}

// 裸写一块数据（无长度前缀、无帧头）+ ZLP（usb.c:548-553）
bool writeRaw(IEdlTransport &t, const QByteArray &data, QString *error)
{
    if (!t.write(data, error))
        return false;
    const int maxPacket = t.maxPacketSize();
    if (!data.isEmpty() && maxPacket > 0 && data.size() % maxPacket == 0) {
        if (!t.write(QByteArray(), error))    // 长度恰为端点包长整数倍 → 补 0 字节写
            return false;
    }
    return true;
}

// 数据面推流器：把"已展开的 raw 字节流"按块裸写出去，并把**实际推送的字节**（含补零）同步喂
// 给 sha256。总量硬性钉在 declaredBytes = numSectors × sectorSize：源头不足补零、超出截断
// （第 4 条不变量）—— 少发/多发都会让设备侧的字节计数错位。
struct DataPusher {
    explicit DataPusher(IEdlTransport &transport) : t(transport) {}
    IEdlTransport &t;
    quint64 chunkBytes = 0;          // max(1, maxPayload/sectorSize) × sectorSize
    quint64 declaredBytes = 0;
    quint64 pushedBytes = 0;         // 已落笔（不含 buf 里未满一块的部分）
    bool    truncated = false;       // 源头比声明的长 → 只发声明量
    QCryptographicHash *hash = nullptr;                       // 非空时同步喂
    std::function<void(quint64)> advance;                      // 参数 = 全局已推送字节数
    QByteArray buf;                                            // 攒块缓冲

    bool flush(QString *error)
    {
        if (buf.isEmpty()) return true;
        if (!writeRaw(t, buf, error)) return false;
        if (hash) hash->addData(buf);
        pushedBytes += quint64(buf.size());
        buf.clear();
        if (advance) advance(pushedBytes);
        return true;
    }

    bool push(const char *data, qint64 len, QString *error)
    {
        while (len > 0) {
            const quint64 accounted = pushedBytes + quint64(buf.size());
            if (accounted >= declaredBytes) { truncated = true; return true; }   // 声明量已满
            // 每块**恰好** chunkBytes：room 同时受"块内余量""声明量余量""本次输入长度"三重约束，
            // 否则一次 push 进来（如 1 MiB 文件读缓冲）会把整段塞进 buf、冲出一个超长块
            const quint64 roomInChunk = chunkBytes > quint64(buf.size())
                    ? chunkBytes - quint64(buf.size()) : 1;
            const quint64 room = qMin<quint64>(qMin<quint64>(quint64(len), roomInChunk),
                                               declaredBytes - accounted);
            buf.append(data, qsizetype(room));
            data += room;
            len -= qint64(room);
            if (quint64(buf.size()) >= chunkBytes && !flush(error)) return false;
        }
        return true;
    }

    // 生成 n 个零字节并推送（FILL 的 fillValue 同样按片生成，见 pushFill）
    bool pushZeros(quint64 n, QString *error)
    {
        static const QByteArray zeros(int(kZeroPieceBytes), '\0');
        while (n > 0) {
            const quint64 take = qMin<quint64>(n, quint64(zeros.size()));
            if (!push(zeros.constData(), qint64(take), error)) return false;
            n -= take;
        }
        return true;
    }

    // FILL 块：按 4 字节 pattern 逐片生成（不 materialize 整段 —— reference/qdl/src/firehose.c:1055-1066 同）
    bool pushFill(quint32 fillValue, quint64 n, QString *error)
    {
        char pat[4];
        for (int j = 0; j < 4; ++j) pat[j] = char((fillValue >> (8 * j)) & 0xFF);
        QByteArray piece(int(kZeroPieceBytes), '\0');
        for (int j = 0; j < piece.size(); ++j) piece[j] = pat[j % 4];
        while (n > 0) {
            const quint64 take = qMin<quint64>(n, quint64(piece.size()));
            // 片长可能不是 chunkBytes 的整数倍：按 take 推送即可（push 内部攒块）
            if (!push(piece.constData(), qint64(take), error)) return false;
            n -= take;
        }
        return true;
    }

    // 收尾：补零到声明总量后**整块冲出** —— qdl 同样是"读到 EOF 就把残段补零、按块一次写出"
    // （reference/qdl/src/firehose.c:1089-1097），故文件尾的补零与文件尾同属一笔写。
    // 补零按 chunkBytes 切片，避免"源文件很短 + 声明量很大"（如 5000 字节文件声明 4 GiB）
    // 时把整段零放进内存。
    bool finish(QString *error)
    {
        while (pushedBytes + quint64(buf.size()) < declaredBytes) {
            const quint64 want = declaredBytes - (pushedBytes + quint64(buf.size()));
            const quint64 room = chunkBytes > quint64(buf.size()) ? chunkBytes - quint64(buf.size()) : 1;
            buf.append(QByteArray(qsizetype(qMin<quint64>(want, room)), '\0'));
            if (quint64(buf.size()) >= chunkBytes && !flush(error)) return false;
        }
        return flush(error);
    }
};

// 把**将下发到设备的字节流**喂进 hash：与数据面推的内容逐字节同口径（源头截断到
// numSectors×sectorSize、不足补零）。「刷前完整校验」与「边写边校」比的是同一个量 —— 否则
// sparse 条目会在两处得出不同结果（包内文件 vs 展开后的镜像），出现"一处过一处不过"的假失败。
bool hashDeclaredStream(const PlanEntry &e, quint64 declaredBytes, QCryptographicHash &hash, QString *error)
{
    quint64 fed = 0;
    const auto feed = [&hash, &fed, declaredBytes](const char *p, qint64 n) {
        const quint64 room = declaredBytes > fed ? declaredBytes - fed : 0;
        const quint64 take = qMin<quint64>(room, quint64(n));
        if (take > 0) {
            hash.addData(QByteArrayView(p, qsizetype(take)));
            fed += take;
        }
    };
    const auto feedZeros = [&feed](quint64 n) {
        QByteArray zeros(int(kZeroPieceBytes), '\0');
        while (n > 0) {
            const qint64 take = qint64(qMin<quint64>(n, quint64(zeros.size())));
            feed(zeros.constData(), take);
            n -= quint64(take);
        }
    };
    const auto padToDeclared = [&feedZeros, &fed, declaredBytes]() {
        if (declaredBytes > fed) feedZeros(declaredBytes - fed);
    };

    if (e.sparse) {
        const bool ok = imgsparse::sparseWalk(e.imageFile, [&](const imgsparse::SparseChunk &c) {
            if (c.raw) {
                feed(c.data.constData(), c.data.size());
            } else if (c.fill) {
                char pat[4];
                for (int j = 0; j < 4; ++j) pat[j] = char((c.fillValue >> (8 * j)) & 0xFF);
                QByteArray piece(int(kZeroPieceBytes), '\0');
                for (int j = 0; j < piece.size(); ++j) piece[j] = pat[j % 4];
                quint64 left = c.rawBytes;
                while (left > 0) {
                    const qint64 take = qint64(qMin<quint64>(left, quint64(piece.size())));
                    feed(piece.constData(), take);
                    left -= quint64(take);
                }
            } else {
                feedZeros(c.rawBytes);            // DONT_CARE → 零（与数据面一致）
            }
            return true;
        }, error);
        if (!ok) return false;
        padToDeclared();
        return true;
    }

    QFile f(e.imageFile);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开镜像文件 %1：%2").arg(e.imageFile, f.errorString());
        return false;
    }
    QByteArray piece(1024 * 1024, Qt::Uninitialized);
    while (fed < declaredBytes) {
        const qint64 n = f.read(piece.data(), piece.size());
        if (n < 0) {
            if (error) *error = QStringLiteral("读取镜像文件 %1 失败：%2").arg(e.imageFile, f.errorString());
            return false;
        }
        if (n == 0) break;                        // EOF → 余量由 padToDeclared 补
        feed(piece.constData(), n);
    }
    padToDeclared();
    return true;
}

} // namespace

EdlSession::EdlSession(IEdlTransport &transport, ProgressFn progress)
    : m_t(transport), m_progress(std::move(progress))
{
}

// 进度百分比：已推送字节 / plan.totalBytes。分母为 0（计划里只有 erase/patch，没有 program 数据）
// 时直接算满 —— 否则进度条永远停在 0。writtenBytes < m_totalBytes 时乘积远小于 2^57（真实镜像量级），
// 无溢出之虞。
int EdlSession::percentFor(quint64 writtenBytes) const
{
    if (m_totalBytes == 0 || writtenBytes >= m_totalBytes)
        return 100;
    return int(writtenBytes * 100 / m_totalBytes);
}

// ---------------------------------------------------------------------------
// run：完整刷写
// ---------------------------------------------------------------------------

bool EdlSession::run(const FlashPlan &plan, const QByteArray &programmer,
                     const FlashOptions &opt, QString *error)
{
    const auto report = [this](const QString &stage, const QString &detail, int percent) {
        if (m_progress) m_progress(SessionProgress{stage, detail, percent});
    };
    const auto fail = [error](const QString &msg) {
        if (error) *error = msg;
        return false;
    };

    m_writtenBytes = 0;
    m_maxPayload = 0;
    m_totalBytes = plan.totalBytes;      // 进度分母（finalizePlan 已算：Program 条目 rawBytes 之和）

    // 空计划：fail-closed（计划层已拒，这里是第二道）—— 一条设备命令都不发
    if (plan.entries.isEmpty())
        return fail(QStringLiteral("拒绝刷写：空计划（没有任何条目）"));

    // ① Sahara：载入 programmer。失败 → 中止、不发 reset（spec §4 表）
    report(QStringLiteral("sahara"), QStringLiteral("载入 programmer"), 0);
    {
        QString openErr;
        if (!m_t.open(&openErr))
            return fail(QStringLiteral("Sahara 阶段失败：无法打开 9008 设备（%1）").arg(openErr));
        QString loadErr;
        if (!saharaLoadProgrammer(m_t, programmer, &loadErr)) {
            m_t.close();
            return fail(QStringLiteral("Sahara 阶段失败：%1").arg(loadErr));
        }
    }
    m_t.close();     // 设备随即重枚举为 Firehose（spec §4 步骤 1 末），旧句柄失效

    // ② 等重枚举 → 重开
    report(QStringLiteral("reenumerate"), QStringLiteral("等待设备进入 Firehose"), 0);
    {
        QString reenumErr;
        if (!m_t.waitReenumerate(kReenumTimeoutMs, &reenumErr)) {
            m_t.close();
            return fail(QStringLiteral("等待设备重枚举失败（Sahara→Firehose）：%1").arg(reenumErr));
        }
        QString openErr;
        if (!m_t.open(&openErr)) {
            m_t.close();
            return fail(QStringLiteral("重枚举后无法打开 Firehose 设备：%1").arg(openErr));
        }
    }

    // ③ configure（协商载荷上限）。注意 payload==0 → 用保守默认，**不得**当无上限
    QString memoryName = plan.storageType.isEmpty() ? QStringLiteral("ufs") : plan.storageType;
    quint32 payload = 0;
    report(QStringLiteral("configure"), memoryName, 0);
    {
        QString cfgErr;
        if (!firehoseConfigure(m_t, memoryName, payload, &cfgErr)) {
            m_t.close();
            return fail(QStringLiteral("configure 阶段失败：%1").arg(cfgErr));
        }
    }
    if (payload == 0) {
        m_maxPayload = kDefaultMaxPayloadBytes;
        report(QStringLiteral("configure"),
               QStringLiteral("设备未回报 MaxPayloadSizeToTargetInBytesSupported，"
                              "按保守默认 %1 字节分块").arg(m_maxPayload), 0);
    } else {
        m_maxPayload = payload;
    }

    // ④ getstorageinfo：只查**计划里出现过的 LUN**（去重，保持计划顺序）。qdl 口径是逐条目取
    // physical_partition_number（reference/qdl/src/program.c:259），不依赖设备的 LUN 总数 ——
    // 响应里的 num_physical_partitions/bNumberLu 只作日志，不决定查几个 LUN。
    QList<StorageInfo> device;
    {
        QList<quint32> luns;
        for (const PlanEntry &e : plan.entries) {
            if (!luns.contains(e.lun)) luns.append(e.lun);
        }
        for (quint32 lun : luns) {
            report(QStringLiteral("getstorageinfo"), QStringLiteral("LUN %1").arg(lun), 0);
            FirehoseResponse resp;
            drainResidual(m_t);
            QString sendErr;
            if (!firehoseSendCommand(m_t, xmlGetStorageInfo(lun), resp, kCmdTimeoutMs, &sendErr)) {
                m_t.close();
                return fail(QStringLiteral("getstorageinfo 阶段失败（LUN %1）：%2").arg(lun).arg(sendErr));
            }
            if (!resp.ack) {
                m_t.close();
                return fail(QStringLiteral("getstorageinfo 阶段失败（LUN %1）：设备返回 NAK：%2")
                                .arg(lun).arg(resp.errorText.isEmpty() ? resp.raw : resp.errorText));
            }
            StorageInfo si;
            QString parseErr;
            if (!parseStorageInfo(resp, lun, si, &parseErr)) {
                m_t.close();
                return fail(QStringLiteral("getstorageinfo 阶段失败（LUN %1）：%2").arg(lun).arg(parseErr));
            }
            device.append(si);
        }
    }

    // ⑤ validatePlan：不过则拒刷，**绝不进入写入**
    {
        report(QStringLiteral("validate"), QStringLiteral("%1 条条目").arg(plan.entries.size()), 0);
        const PlanCheck chk = validatePlan(plan, device);
        for (const QString &w : chk.warnings)
            report(QStringLiteral("validate"), w, 0);
        if (!chk.ok) {
            m_t.close();
            return fail(QStringLiteral("计划校验未通过，拒绝写入：\n%1").arg(chk.errors.join(QLatin1Char('\n'))));
        }
    }

    // ⑥ 逐条目执行（Erase → Program → Patch，顺序由 finalizePlan 排定）
    for (const PlanEntry &e : plan.entries) {
        if (!writeEntry(e, opt, error)) {
            m_t.close();     // 失败路径：只关句柄，**不发 reset**（设备留在 EDL 便于重试）
            return false;
        }
    }

    // ⑦ setbootablestoragedrive：计划含 xbl/sbl1 时，用该条目的 LUN 标记 bootloader 分区
    for (const PlanEntry &e : plan.entries) {
        if (e.action != PlanEntry::Action::Program)
            continue;
        const QString name = e.partitionName.toLower();
        if (name != QLatin1String("xbl") && name != QLatin1String("sbl1"))
            continue;
        report(QStringLiteral("bootable"), QStringLiteral("LUN %1（%2）").arg(e.lun).arg(e.partitionName), 0);
        FirehoseResponse resp;
        drainResidual(m_t);
        QString sendErr;
        if (!firehoseSendCommand(m_t, xmlSetBootableStorageDrive(e.lun), resp, kCmdTimeoutMs, &sendErr)) {
            m_t.close();
            return fail(QStringLiteral("setbootablestoragedrive 失败（LUN %1）：%2").arg(e.lun).arg(sendErr));
        }
        if (!resp.ack) {
            m_t.close();
            return fail(QStringLiteral("setbootablestoragedrive 失败（LUN %1）：设备返回 NAK：%2")
                            .arg(e.lun).arg(resp.errorText.isEmpty() ? resp.raw : resp.errorText));
        }
        break;   // 只发一次：取第一个匹配的 bootloader 分区（qdl 也只标记一个 LUN）
    }

    // ⑧ reset：**仅成功路径发一次**。真机上设备往往先 ACK 再重启（或直接掉线），故这里 best-effort：
    // 超时/NAK 只记日志，不把"已经刷完"判成失败（qdl 在这点上会返回错误码，见 firehose.c:1585-1598 ——
    // 照抄会让每次成功刷机都以失败收场）。
    report(QStringLiteral("reset"), QStringLiteral("复位设备"), 100);
    {
        FirehoseResponse resp;
        drainResidual(m_t);
        QString sendErr;
        if (!firehoseSendCommand(m_t, xmlReset(), resp, kCmdTimeoutMs, &sendErr) || !resp.ack) {
            const QString why = sendErr.isEmpty()
                    ? (resp.errorText.isEmpty() ? resp.raw : resp.errorText) : sendErr;
            report(QStringLiteral("reset"),
                   QStringLiteral("设备未确认复位请求（%1）—— 数据已全部写入，可手动重启").arg(why), 100);
        }
        // Firehose 的 <power value="reset"/> 只是"让设备重启"；句柄的退场由传输层负责
        // （IEdlTransport::resetDevice）。同样 best-effort：设备此时多半已开始重启。
        QString resetErr;
        if (!m_t.resetDevice(&resetErr))
            report(QStringLiteral("reset"),
                   QStringLiteral("传输层复位失败（%1）—— 设备可能已自行重启").arg(resetErr), 100);
    }
    m_t.close();
    report(QStringLiteral("done"), QStringLiteral("刷写完成"), 100);
    return true;
}

// ---------------------------------------------------------------------------
// writeEntry：单条目（命令 + 数据面 + 等 ACK）
// ---------------------------------------------------------------------------

bool EdlSession::writeEntry(const PlanEntry &e, const FlashOptions &opt, QString *error)
{
    const auto report = [this, &e](const QString &stage, int percent) {
        if (m_progress) m_progress(SessionProgress{stage, entryName(e), percent});
    };

    const quint32 sectorSize = e.sectorSize;
    const quint64 declaredBytes = (sectorSize == 0)
            ? 0 : e.numSectors * quint64(sectorSize);

    if (e.action != PlanEntry::Action::Program) {
        // Erase / Patch：只有命令，没有数据面
        report(QStringLiteral("write"), percentFor(m_writtenBytes));
        FirehoseResponse resp;
        drainResidual(m_t);
        QString sendErr;
        const QByteArray cmd = (e.action == PlanEntry::Action::Erase) ? xmlErase(e) : xmlPatch(e);
        if (!firehoseSendCommand(m_t, cmd, resp, kCmdTimeoutMs, &sendErr)) {
            if (error)
                *error = entryFailText(e, 0, 0, QStringLiteral("命令发送/响应失败：%1").arg(sendErr));
            return false;
        }
        if (!resp.ack) {
            if (error)
                *error = entryFailText(e, 0, 0,
                                       QStringLiteral("设备返回 NAK：%1")
                                           .arg(resp.errorText.isEmpty() ? resp.raw : resp.errorText));
            return false;
        }
        return true;
    }

    // ---- Program ----
    if (sectorSize == 0) {
        if (error)
            *error = entryFailText(e, 0, 0, QStringLiteral("SECTOR_SIZE_IN_BYTES 为 0，无法分块"));
        return false;
    }
    if (e.numSectors == 0) {
        // Program 的 numSectors==0 没有合法语义（"整 LUN" 语义只属 Erase，见 firehose.h 的 xmlErase）：
        // 真发出去就是一条 num_partition_sectors="0" 的空 program。计划层应已归一化，这里 fail-closed。
        if (error)
            *error = entryFailText(e, 0, 0,
                                   QStringLiteral("num_partition_sectors 为 0（Program 条目无此语义）"));
        return false;
    }
    if (e.numSectors > (std::numeric_limits<quint64>::max)() / quint64(sectorSize)) {
        if (error)
            *error = entryFailText(e, 0, 0, QStringLiteral("扇区数 × 扇区大小 溢出 u64"));
        return false;
    }

    // 刷前完整校验（可选）：比的是**将下发到设备的字节流**（与边写边校同口径）
    if (opt.fullVerifyBeforeWrite && !e.sha256.isEmpty()) {
        QCryptographicHash preCheck(QCryptographicHash::Sha256);
        QString hashErr;
        if (!hashDeclaredStream(e, declaredBytes, preCheck, &hashErr)) {
            if (error)
                *error = entryFailText(e, 0, 0, QStringLiteral("刷前完整校验失败：%1").arg(hashErr));
            return false;
        }
        const QString got = QString::fromLatin1(preCheck.result().toHex());
        if (got.compare(e.sha256, Qt::CaseInsensitive) != 0) {
            if (error)
                *error = entryFailText(e, 0, 0,
                                       QStringLiteral("刷前完整校验不符（计算 %1…，期望 %2…）—— 未写入任何数据")
                                           .arg(got.left(12), e.sha256.left(12)));
            return false;
        }
    }

    report(QStringLiteral("write"), percentFor(m_writtenBytes));

    // ① program 声明：一条 program 命令覆盖整段（numSectors 由计划层按 sparse 头/XML 算好，不重算）
    FirehoseResponse resp;
    drainResidual(m_t);
    QString sendErr;
    if (!firehoseSendCommand(m_t, xmlProgram(e), resp, kCmdTimeoutMs, &sendErr)) {
        if (error)
            *error = entryFailText(e, 0, 0, QStringLiteral("program 命令发送/响应失败：%1").arg(sendErr));
        return false;
    }
    if (!resp.ack) {
        if (error)
            *error = entryFailText(e, 0, 0,
                                   QStringLiteral("program 命令被拒（NAK）：%1")
                                       .arg(resp.errorText.isEmpty() ? resp.raw : resp.errorText));
        return false;
    }

    // ② 数据面：分块裸写 + ZLP + 扇区补零（sparse 在这里展开，不落临时文件）
    QCryptographicHash digest(QCryptographicHash::Sha256);
    const bool wantHash = opt.verifyAfterWrite && !e.sha256.isEmpty();
    DataPusher pusher(m_t);
    // 分块：chunkSectors = max(1, maxPayload / sectorSize)（第 2 条不变量）
    pusher.chunkBytes = qMax<quint64>(1, quint64(m_maxPayload) / quint64(sectorSize)) * quint64(sectorSize);
    pusher.declaredBytes = declaredBytes;
    pusher.hash = wantHash ? &digest : nullptr;
    pusher.advance = [this, &e](quint64 pushed) {   // 百分点：全局已推送字节 / plan.totalBytes
        if (m_progress)
            m_progress(SessionProgress{QStringLiteral("write"), entryName(e),
                                       percentFor(m_writtenBytes + pushed)});
    };

    if (e.sparse) {
        QString walkErr;
        QString pushErr;
        const bool ok = imgsparse::sparseWalk(e.imageFile,
                                              [&pusher, &pushErr](const imgsparse::SparseChunk &c) {
            if (c.raw)   return pusher.push(c.data.constData(), c.data.size(), &pushErr);
            if (c.fill)  return pusher.pushFill(c.fillValue, c.rawBytes, &pushErr);
            // DONT_CARE：**不产出数据但偏移照常推进**（SparseChunk 契约；reference/qdl/src/program.c:139-170）。
            // 本项目按"一条 program 覆盖整段"下发（numSectors 由计划层算好，brief 硬要求 2），字节流没有
            // 空洞可跳 —— 要让设备侧计数不错位，这一段必须补零发出去：写进设备的内容与"先 simg2img
            // （DONT_CARE 零填充）再刷 raw"逐字节一致。qdl 走的是另一条路（per-chunk program op，
            // DONT_CARE 完全不发、靠下一条 op 的 start_sector 跳过）—— spec §3.3 的文字描述的是那条路；
            // 两条路落盘结果相同，本项目选前者是因为它与"计划层的 numSectors"和"一条目一 ACK"自洽。
            return pusher.pushZeros(c.rawBytes, &pushErr);
        }, &walkErr);
        if (!ok) {
            if (error)
                *error = entryFailText(e, pusher.pushedBytes / sectorSize, pusher.pushedBytes,
                                       QStringLiteral("sparse 展开/推送失败：%1")
                                           .arg(pushErr.isEmpty() ? walkErr : pushErr));
            return false;
        }
    } else {
        QFile f(e.imageFile);
        if (!f.open(QIODevice::ReadOnly)) {
            if (error)
                *error = entryFailText(e, 0, 0,
                                       QStringLiteral("无法打开镜像文件 %1：%2").arg(e.imageFile, f.errorString()));
            return false;
        }
        QByteArray piece(1024 * 1024, Qt::Uninitialized);
        for (;;) {
            const qint64 n = f.read(piece.data(), piece.size());
            if (n < 0) {
                if (error)
                    *error = entryFailText(e, pusher.pushedBytes / sectorSize, pusher.pushedBytes,
                                           QStringLiteral("读取镜像文件失败：%1").arg(f.errorString()));
                return false;
            }
            if (n == 0) break;
            QString pushErr;
            if (!pusher.push(piece.constData(), n, &pushErr)) {
                if (error)
                    *error = entryFailText(e, pusher.pushedBytes / sectorSize, pusher.pushedBytes,
                                           QStringLiteral("数据写入失败：%1").arg(pushErr));
                return false;
            }
        }
    }
    QString finishErr;
    if (!pusher.finish(&finishErr)) {
        if (error)
            *error = entryFailText(e, pusher.pushedBytes / sectorSize, pusher.pushedBytes,
                                   QStringLiteral("数据写入失败：%1").arg(finishErr));
        return false;
    }
    if (pusher.truncated && m_progress) {
        // 源头比声明长：只发 numSectors×sectorSize（qdl 同：只按 num_sectors 读），但不能静默
        m_progress(SessionProgress{QStringLiteral("write"),
                                   QStringLiteral("%1：镜像比声明的 %2 字节长，只下发声明量")
                                       .arg(entryName(e)).arg(declaredBytes),
                                   percentFor(m_writtenBytes + pusher.pushedBytes)});
    }

    // ③ 整条数据发完只等**一个** ACK（firehose.c:1132-1137）；块间不额外等待
    FirehoseResponse dataResp;
    QString waitErr;
    if (!waitResponse(m_t, dataResp, kDataAckTimeoutMs,
                      QStringLiteral("条目 %1 数据发完").arg(entryName(e)), &waitErr)) {
        if (error)
            *error = entryFailText(e, pusher.pushedBytes / sectorSize, pusher.pushedBytes,
                                   QStringLiteral("数据面等待 ACK 失败：%1").arg(waitErr));
        return false;
    }
    if (!dataResp.ack) {
        if (error)
            *error = entryFailText(e, pusher.pushedBytes / sectorSize, pusher.pushedBytes,
                                   QStringLiteral("数据面被设备拒绝（NAK）：%1")
                                       .arg(dataResp.errorText.isEmpty() ? dataResp.raw : dataResp.errorText));
        return false;
    }

    // ④ 校验：边写边算的 sha256 与计划的比对（不符 → 错误；**不回滚**）
    if (wantHash) {
        const QString got = QString::fromLatin1(digest.result().toHex());
        if (got.compare(e.sha256, Qt::CaseInsensitive) != 0) {
            if (error)
                *error = entryFailText(e, pusher.pushedBytes / sectorSize, pusher.pushedBytes,
                                       QStringLiteral("sha256 校验不符（计算 %1…，期望 %2…）；"
                                                      "数据已写入设备，本工具不回滚（覆盖写可重刷）")
                                           .arg(got.left(12), e.sha256.left(12)));
            return false;
        }
    }
    m_writtenBytes += pusher.pushedBytes;
    return true;
}

// ---------------------------------------------------------------------------
// readBack：只读回（不写入、不复位）
// ---------------------------------------------------------------------------

bool EdlSession::readBack(const QList<ReadRequest> &reqs, const QList<StorageInfo> &device, QString *error)
{
    if (reqs.isEmpty()) {
        if (error) *error = QStringLiteral("拒绝读回：请求列表为空");
        return false;
    }

    // ① 全部请求先过几何校验（越界/未知 LUN/非法扇区大小 → 一条命令都不发）
    for (const ReadRequest &r : reqs) {
        const StorageInfo *geo = nullptr;
        for (const StorageInfo &s : device) {
            if (s.lun == r.lun) { geo = &s; break; }
        }
        if (!geo) {
            if (error)
                *error = QStringLiteral("读回 LUN %1 无设备几何信息（先 getstorageinfo）").arg(r.lun);
            return false;
        }
        if (r.sectorSize == 0 || r.numSectors == 0) {
            if (error)
                *error = QStringLiteral("读回 LUN %1 扇区 %2..%3 的参数非法（扇区大小为 0 或扇区数为 0）")
                             .arg(r.lun).arg(r.startSector).arg(r.startSector + r.numSectors);
            return false;
        }
        if (r.startSector > geo->totalBlocks || r.numSectors > geo->totalBlocks - r.startSector) {
            if (error)
                *error = QStringLiteral("读回越界：LUN %1 需要扇区 %2..%3，设备仅 %4")
                             .arg(r.lun).arg(r.startSector).arg(r.startSector + r.numSectors)
                             .arg(geo->totalBlocks);
            return false;
        }
        if (r.outputPath.isEmpty()) {
            if (error)
                *error = QStringLiteral("读回 LUN %1 扇区 %2 未指定输出文件").arg(r.lun).arg(r.startSector);
            return false;
        }
    }

    // ② 逐请求读回
    QFile out;
    for (int i = 0; i < reqs.size(); ++i) {
        const ReadRequest &r = reqs.at(i);
        const int basePct = i * 100 / reqs.size();
        if (m_progress)
            m_progress(SessionProgress{QStringLiteral("read"), r.outputPath, basePct});

        out.setFileName(r.outputPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error)
                *error = QStringLiteral("无法创建输出文件 %1：%2").arg(r.outputPath, out.errorString());
            return false;
        }

        FirehoseResponse resp;
        drainResidual(m_t);
        QString sendErr;
        if (!firehoseSendCommand(m_t, xmlRead(r.lun, r.startSector, r.numSectors, r.sectorSize),
                                 resp, kCmdTimeoutMs, &sendErr)) {
            out.close();
            QFile::remove(r.outputPath);
            if (error)
                *error = QStringLiteral("读回命令失败（LUN %1，扇区 %2..%3）：%4")
                             .arg(r.lun).arg(r.startSector).arg(r.startSector + r.numSectors).arg(sendErr);
            return false;
        }
        if (!resp.ack) {
            out.close();
            QFile::remove(r.outputPath);
            if (error)
                *error = QStringLiteral("读回命令被拒（LUN %1，扇区 %2..%3）：%4")
                             .arg(r.lun).arg(r.startSector).arg(r.startSector + r.numSectors)
                             .arg(resp.errorText.isEmpty() ? resp.raw : resp.errorText);
            return false;
        }

        // 读数据：设备在响应之后转 rawmode，直接吐 numSectors × sectorSize 字节（qdl: firehose.c:1229-1265）
        const quint64 want = r.numSectors * quint64(r.sectorSize);
        quint64 got = 0;
        while (got < want) {
            QString readErr;
            const int ask = int(qMin<quint64>(kReadChunkBytes, want - got));
            const QByteArray chunk = m_t.read(ask, kReadTimeoutMs, &readErr);
            if (chunk.isEmpty()) {
                out.close();
                QFile::remove(r.outputPath);
                if (error)
                    *error = QStringLiteral("读回数据失败（LUN %1，扇区 %2..%3）：已收 %4/%5 字节（%6）")
                                 .arg(r.lun).arg(r.startSector).arg(r.startSector + r.numSectors)
                                 .arg(got).arg(want).arg(readErr);
                return false;
            }
            const qint64 written = out.write(chunk);
            if (written != chunk.size()) {
                out.close();
                QFile::remove(r.outputPath);
                if (error)
                    *error = QStringLiteral("写输出文件失败 %1：%2").arg(r.outputPath, out.errorString());
                return false;
            }
            got += quint64(chunk.size());
        }
        out.close();

        // 读操作收尾响应（qdl: firehose.c:1267-1272）
        FirehoseResponse done;
        QString waitErr;
        if (!waitResponse(m_t, done, kCmdTimeoutMs,
                          QStringLiteral("读回收尾（LUN %1）").arg(r.lun), &waitErr)) {
            QFile::remove(r.outputPath);
            if (error) *error = QStringLiteral("读回收尾失败：%1").arg(waitErr);
            return false;
        }
        if (!done.ack) {
            QFile::remove(r.outputPath);
            if (error)
                *error = QStringLiteral("读回被设备拒绝（LUN %1）：%2")
                             .arg(r.lun).arg(done.errorText.isEmpty() ? done.raw : done.errorText);
            return false;
        }
        if (m_progress)
            m_progress(SessionProgress{QStringLiteral("read"), r.outputPath,
                                       int((i + 1) * 100 / reqs.size())});
    }
    if (m_progress)
        m_progress(SessionProgress{QStringLiteral("read"), QStringLiteral("读回完成"), 100});
    return true;
}

} // namespace edl
