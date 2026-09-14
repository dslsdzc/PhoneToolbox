// src/core/odin/odin_session.cpp
//
// 会话编排：顺序、时机与字节。协议帧构造在 odin_protocol.cpp、计划在 samsung_plan.cpp、
// 设备访问只经 IOdinTransport —— 本文件不碰 libusb、不解析 XML。
// 流程参照：heimdall/source/FlashAction.cpp:540-571（Initialise → BeginSession →（tflash）
// → TotalBytes → getPitData → flashPartitions → EndSession）与 odin4-llucs/src/odin4.cpp:165-189。
#include "odin_session.h"

#include <utility>   // std::as_const（项目约定：不得用 qAsConst）

#include <QFile>

namespace odin {
namespace {

// 设备回报的 PIT 大小上限（odin4-llucs/src/usb/odin_protocol.cpp:634 同款；真 PIT 2924..18492 B）
constexpr quint32 kMaxPitBytes = 1048576;

// 控制类应答的 IN 缓冲上限（odin4 odin_protocol.cpp:288 `rsp.assign(512, 0)`）：普通应答 8 字节、
// 机型查询应答 12 字节，都必须能**整包**收下 —— 短读会把余下的字节留在 IN 端点里，毒化下一次读。
constexpr int kControlResponseMaxBytes = 512;

void setErr(QString *error, const QString &msg) { if (error) *error = msg; }

void prefixErr(QString *error, const QString &prefix)
{
    if (error && !error->isEmpty())
        *error = prefix + QStringLiteral("：") + *error;
    else if (error)
        *error = prefix;
}

QString hexBytes(const QByteArray &b) { return QString::fromLatin1(b.toHex(' ')); }

} // namespace

OdinSession::OdinSession(IOdinTransport &transport, OdinProgressFn progress)
    : m_t(transport), m_progress(std::move(progress))
{
}

void OdinSession::report(const QString &stage, const QString &detail, int percent)
{
    if (m_progress)
        m_progress(OdinProgress{stage, detail, percent});
}

int OdinSession::percentFor(quint64 writtenBytes) const
{
    if (m_totalBytes == 0)
        return 100;
    return int(qMin<quint64>(writtenBytes * 100 / m_totalBytes, 100));
}

bool OdinSession::run(const SamsungPlan &plan, const OdinOptions &opt, QString *error)
{
    m_opt = opt;
    m_writtenBytes = 0;
    m_totalBytes = plan.totalBytes;

    if (plan.entries.isEmpty()) {              // 空计划：一条命令都不发（不 open）
        setErr(error, QStringLiteral("计划为空：没有可刷写的分区"));
        return false;
    }
    if (!m_t.open(error)) {
        prefixErr(error, QStringLiteral("打开设备失败"));
        return false;
    }

    bool ok = handshake(error) && beginSession(error);
    if (ok && m_opt.queryDeviceType)
        queryDeviceType();                     // best-effort：不影响 ok
    if (ok && !setTotalBytes(plan.totalBytes, error))
        ok = false;

    PitTable devicePit;
    bool haveDevicePit = false;
    if (ok && m_opt.dumpDevicePit) {
        QString derr;
        const DumpOutcome outcome = dumpDevicePit(devicePit, &derr);
        if (outcome == DumpOutcome::Ok) {
            haveDevicePit = true;
        } else if (outcome == DumpOutcome::StreamInterrupted) {
            // **不回退**：设备可能仍在把余下的 PIT 片发进 IN 端点。改道包内 PIT 继续刷写，
            // 会让设备停在半开会话里（后续命令与残留数据交错）—— 直接失败，让用户重试整次会话。
            setErr(error, QStringLiteral("设备 PIT 读取在数据阶段中断：%1 —— 设备可能停在半开会话，"
                                         "请重试（未下发任何分区数据）").arg(derr));
            ok = false;
        } else {
            report(QStringLiteral("pit"),
                   QStringLiteral("读取设备 PIT 失败，改用包内 PIT：%1").arg(derr),
                   percentFor(m_writtenBytes));
        }
    }

    QList<ResolvedEntry> todo;
    if (ok && !resolveEntries(plan, haveDevicePit ? &devicePit : nullptr, todo, error))
        ok = false;

    if (ok) {
        for (const ResolvedEntry &r : std::as_const(todo)) {
            if (!writeEntry(r, error)) { ok = false; break; }
        }
    }

    if (ok) {
        endSession(m_opt.reboot);              // 收尾失败只落日志，不改结论
        report(QStringLiteral("done"), QStringLiteral("刷写完成"), 100);
    }
    m_t.close();                               // 所有路径都关句柄
    return ok;
}

bool OdinSession::handshake(QString *error)
{
    report(QStringLiteral("handshake"), QStringLiteral("发送 ODIN 握手"), 0);
    QString werr;
    if (!m_t.write(QByteArray("ODIN", 4), &werr)) {
        setErr(error, QStringLiteral("握手失败：%1").arg(werr));
        return false;
    }
    // 读只给 handshakeTimeoutMs（1s）：设备不在 Odin 模式时必须快速失败
    // （Heimdall BridgeManager.cpp:306/316 的 1000ms；odin4 用 10s —— 取短的一侧，失败更快）
    const QByteArray rsp = m_t.read(4, m_opt.handshakeTimeoutMs, error);
    if (rsp.size() != 4 || rsp != QByteArray("LOKE", 4)) {
        setErr(error, rsp.isEmpty()
                          ? QStringLiteral("握手失败：未收到设备响应（期望 LOKE）")
                          : QStringLiteral("握手失败：收到 %1（期望 LOKE）").arg(hexBytes(rsp)));
        return false;
    }
    // 握手后清一次 IN 端点（odin4 odin_protocol.cpp:304-326 的 drain）：残留字节会让后续读错位。
    // timeoutMs = 0 = 非阻塞轮询，拿不到东西**不置 error**。
    m_t.read(64, 0, nullptr);
    return true;
}

bool OdinSession::readAck(quint32 expectedId, bool allowProgressCodes,
                          const QString &context, QString *error)
{
    QString rerr;
    const QByteArray rsp = m_t.read(kAckSize, m_opt.controlTimeoutMs, &rerr);
    if (rsp.size() < kAckSize) {
        setErr(error, QStringLiteral("%1：未收到完整应答（%2 字节）%3")
                          .arg(context).arg(rsp.size())
                          .arg(rerr.isEmpty() ? QString() : QStringLiteral("；传输层报：") + rerr));
        return false;
    }
    Ack ack;
    if (!parseAck(rsp, ack, error)) {
        prefixErr(error, context);
        return false;
    }
    const AckVerdict v = judgeAck(ack, expectedId, allowProgressCodes);
    if (!v.ok) {
        setErr(error, QStringLiteral("%1：%2").arg(context, v.reason));
        return false;
    }
    if (ack.code != 0)
        // 非零但非负：不判失败（odin4/Thor 只要求非负；Heimdall 要求必须为 0 —— 采用前者**但记账**）
        report(QStringLiteral("info"),
               QStringLiteral("%1：设备返回状态码 %2（非零且非失败码，继续）").arg(context).arg(ack.code),
               percentFor(m_writtenBytes));
    return true;
}

bool OdinSession::beginSession(QString *error)
{
    QString werr;
    if (!m_t.write(frameBeginSession(), &werr)) {
        setErr(error, QStringLiteral("起会话失败：%1").arg(werr));
        return false;
    }
    const QByteArray rsp = m_t.read(kAckSize, m_opt.controlTimeoutMs, error);
    BeginSessionAck a;
    if (!parseBeginSessionAck(rsp, a, error)) {
        prefixErr(error, QStringLiteral("起会话失败"));
        return false;
    }
    // ⚠️ 这个应答的 code 字段**是版本号**：只判 id（不能做 code<0 判定，见 odin_protocol.h）
    const AckVerdict v = judgeAckIdOnly(Ack{a.id, a.code}, kControlSession);
    if (!v.ok) {
        setErr(error, QStringLiteral("起会话被设备拒绝：%1").arg(v.reason));
        return false;
    }
    m_profile = profileForVersion(a.version);
    report(QStringLiteral("session"),
           QStringLiteral("协议版本 %1：片大小 %2 KiB / 每序列 %3 片 / 刷写超时 %4 ms%5")
               .arg(a.version).arg(m_profile.packetSize / 1024).arg(m_profile.sequenceCount)
               .arg(m_profile.flashTimeoutMs)
               .arg(a.compressedSupported ? QStringLiteral("（设备支持压缩传输，本期发原始数据）")
                                          : QString()),
           0);

    if (a.version >= 2) {
        // D4：只有 version>=2 才协商片大小（odin4:399-406 / Thor:58-71）
        if (!m_t.write(frameFilePartSize(m_profile.packetSize), &werr)) {
            setErr(error, QStringLiteral("协商片大小失败：%1").arg(werr));
            return false;
        }
        if (!readAck(kControlSession, false, QStringLiteral("协商片大小"), error))
            return false;
    }
    return true;
}

void OdinSession::queryDeviceType()
{
    // best-effort（请求本身见 odin4 odin_protocol.cpp:433-452）。**调用点与 odin4 不同**：
    // odin4 在 odin4.cpp:172 于「握手后、**起会话前**」调用，本期按 spec §5 的流程在
    // `beginSession()` **之后**、上报总字节之前调用 —— 同一请求号、不同时机（审查 Minor 7）。
    // ⚠️ D13：同一个 0x64/0x01 在 Thor 是"重置刷写计数"（刷完才发），在 odin4 里既当机型查询
    // 又当重置 —— 本期只做**一次查询**，刷完**不**发重置。语义冲突留给持机人裁定。
    QString err;
    if (!m_t.write(frameDeviceTypeQuery(), &err)) {
        report(QStringLiteral("info"), QStringLiteral("机型查询发送失败（跳过）：%1").arg(err),
               percentFor(m_writtenBytes));
        return;
    }
    // ⚠️ 必须按 512（odin4 的 `rsp.assign(512, 0)`，:288）读，**不能**按 kAckSize(8)：
    //    机型应答是 12 字节（odin4:437 `if (rsp.size() < 12) return false;`）—— 按 8 读会永远
    //    拿不到 ≥12 字节（真机发的是短包），且余下 4 字节留在 IN 端点里毒化下一次应答读。
    const QByteArray rsp = m_t.read(kControlResponseMaxBytes, m_opt.controlTimeoutMs, &err);
    if (rsp.size() < 12) {
        report(QStringLiteral("info"),
               QStringLiteral("设备未回报机型（应答 %1 字节）").arg(rsp.size()),
               percentFor(m_writtenBytes));
        return;
    }
    Ack ack;
    if (!parseAck(rsp, ack, nullptr) || !judgeAckIdOnly(ack, kControlSession).ok) {
        report(QStringLiteral("info"),
               QStringLiteral("机型查询被拒（应答 id 0x%1）").arg(ack.id, 8, 16, QLatin1Char('0')),
               percentFor(m_writtenBytes));
        return;
    }
    // odin4 把该值读成 "SM-<十进制>"（:450）—— 那是它的约定、不是权威定义；这里只记原始值。
    const quint32 raw = quint32(quint8(rsp.at(8))) | (quint32(quint8(rsp.at(9))) << 8)
                      | (quint32(quint8(rsp.at(10))) << 16) | (quint32(quint8(rsp.at(11))) << 24);
    report(QStringLiteral("info"),
           QStringLiteral("设备机型原始值 0x%1（odin4 记为 SM-%2；语义未验证，仅记录）")
               .arg(raw, 8, 16, QLatin1Char('0')).arg(raw),
           percentFor(m_writtenBytes));
}

bool OdinSession::setTotalBytes(quint64 total, QString *error)
{
    QString werr;
    if (!m_t.write(frameTotalBytes(total), &werr)) {
        setErr(error, QStringLiteral("上报总字节失败：%1").arg(werr));
        return false;
    }
    return readAck(kControlSession, false, QStringLiteral("上报总字节"), error);
}

OdinSession::DumpOutcome OdinSession::dumpDevicePit(PitTable &out, QString *error)
{
    QString werr;
    if (!m_t.write(framePitDumpRequest(), &werr)) {
        setErr(error, QStringLiteral("请求读取设备 PIT 失败：%1").arg(werr));
        return DumpOutcome::RejectedNoStream;   // 命令没发出去 → 设备未开始发流
    }
    // ① 请求应答：id 回显 0x65 + u32 文件大小（Heimdall PitFileResponse；odin4:626-637）
    QString rerr;
    const QByteArray rsp = m_t.read(kAckSize, m_opt.controlTimeoutMs, &rerr);
    if (rsp.size() < kAckSize) {
        setErr(error, QStringLiteral("读取设备 PIT 失败：未收到大小应答（%1 字节）%2")
                          .arg(rsp.size())
                          .arg(rerr.isEmpty() ? QString() : QStringLiteral("；") + rerr));
        return DumpOutcome::RejectedNoStream;   // 大小应答是数据流的前置 → 设备尚未发流
    }
    Ack ack;
    if (!parseAck(rsp, ack, error)) {
        prefixErr(error, QStringLiteral("读取设备 PIT 失败"));
        return DumpOutcome::RejectedNoStream;
    }
    const AckVerdict v = judgeAck(ack, kControlPitFile, false);
    if (!v.ok) {
        setErr(error, QStringLiteral("读取设备 PIT 失败：%1").arg(v.reason));
        return DumpOutcome::RejectedNoStream;   // 请求阶段被拒（如 BOOTLOADER_FAIL）
    }
    const quint32 size = ack.code;
    if (size == 0 || size > kMaxPitBytes) {
        setErr(error, QStringLiteral("设备回报的 PIT 大小不合理（%1 字节）").arg(size));
        return DumpOutcome::RejectedNoStream;   // 同上：还没请求过任何一片
    }
    // ② 逐片取：0x65/0x02 + 片序号 → 应答是**裸的 ≤500 字节数据**（无 8 字节头）
    //    （Heimdall 的 ReceiveFilePartPacket 是 500 字节变长包；odin4:643-652 同样把应答当原始数据）
    //    ⚠️ 从这里起进入**数据阶段**：任何失败都返回 StreamInterrupted（不可回退，见 DumpOutcome）。
    QByteArray data;
    data.reserve(int(size));
    const quint32 parts = (size + kPitPartSize - 1) / kPitPartSize;
    for (quint32 i = 0; i < parts; ++i) {
        if (!m_t.write(framePitPartRequest(i), &werr)) {
            setErr(error, QStringLiteral("读取设备 PIT 第 %1 片失败：%2").arg(i).arg(werr));
            return DumpOutcome::StreamInterrupted;
        }
        const QByteArray chunk = m_t.read(kPitPartSize, m_opt.controlTimeoutMs, &rerr);
        if (chunk.isEmpty()) {
            setErr(error, QStringLiteral("读取设备 PIT 第 %1/%2 片失败：%3")
                              .arg(i + 1).arg(parts)
                              .arg(rerr.isEmpty() ? QStringLiteral("设备未回数据") : rerr));
            return DumpOutcome::StreamInterrupted;
        }
        data.append(chunk);
    }
    // ③ 末片之后设备会再发一次空包（Heimdall 末片带 kEmptyTransferAfter；odin4:654-657 紧随一次 IN 空读）
    //    —— best-effort，拿不到不算错。但**要落日志**（审查 Minor 5）：设备不发空包时这一读会白等到
    //    超时（10s）且此前没有任何输出；"有没有尾空包""收到的是不是空包"是判断参照口径是否
    //    与真机一致的现场证据（本期无真机，未验证）。**不改变控制流**。
    QString eerr;
    const QByteArray emptyPkt = m_t.read(1, m_opt.controlTimeoutMs, &eerr);
    report(QStringLiteral("info"),
           !eerr.isEmpty()
               ? QStringLiteral("设备 PIT 尾空包：超时未收到（%1；真机未验证）").arg(eerr)
               : (emptyPkt.isEmpty()
                      ? QStringLiteral("设备 PIT 尾空包：已收到")
                      : QStringLiteral("设备 PIT 尾空包：收到 %1 字节**非空**包（流可能未对齐）")
                            .arg(emptyPkt.size())),
           percentFor(m_writtenBytes));
    // ④ 结束：0x65/0x03 → 应答
    if (!m_t.write(framePitEndRequest(), &werr)) {
        setErr(error, QStringLiteral("结束读取设备 PIT 失败：%1").arg(werr));
        return DumpOutcome::StreamInterrupted;
    }
    if (!readAck(kControlPitFile, false, QStringLiteral("结束读取设备 PIT"), error))
        return DumpOutcome::StreamInterrupted;

    // 至此：每一片都请求过且都有应答、结束请求也已确认 —— 流已按 size 对齐消费完，
    // 端点里不可能还有"设备正在发"的字节。以下是**内容**问题（长度不符/解析失败），
    // 改道包内 PIT 是安全的 → 归入可回退。
    data.truncate(int(size));                  // 末片可能多读（设备按 500 发，我们只要 size 字节）
    if (quint32(data.size()) != size) {
        setErr(error, QStringLiteral("设备 PIT 数据不完整（期望 %1 字节，收到 %2）")
                          .arg(size).arg(data.size()));
        return DumpOutcome::RejectedNoStream;
    }
    if (!parsePit(data, out, error))
        return DumpOutcome::RejectedNoStream;
    return DumpOutcome::Ok;
}

bool OdinSession::resolveEntries(const SamsungPlan &plan, const PitTable *devicePit,
                                 QList<ResolvedEntry> &out, QString *error)
{
    out.clear();
    QStringList missing;
    for (const SamsungPlanEntry &e : plan.entries) {
        if (e.fileIndex < 0 || e.fileIndex >= plan.files.size()) {
            setErr(error, QStringLiteral("计划条目 %1 的来源包索引非法（%2）")
                              .arg(e.partition).arg(e.fileIndex));
            return false;
        }
        ResolvedEntry r;
        r.partition = e.partition;
        r.imageFile = e.imageFile;
        r.path = plan.files.at(e.fileIndex).path;
        r.offset = e.sourceOffset;
        r.size = e.sizeBytes;
        r.pit = e.pit;
        if (r.size == 0) {
            setErr(error, QStringLiteral("计划条目 %1 的镜像 %2 大小为 0，拒绝刷写")
                              .arg(e.partition, e.imageFile));
            return false;
        }
        if (devicePit) {
            // spec §5：以设备 PIT 为准（设备自身布局才是真相；信包内 PIT 可能写错位置）
            const PitEntry *de = devicePit->findByName(e.partition);
            if (!de) {
                missing << e.partition;         // 收集齐再一次性报（用户能看到全部缺哪些）
                continue;
            }
            if (de->identifier != e.pit.identifier || de->deviceType != e.pit.deviceType
                || de->binaryType != e.pit.binaryType) {
                report(QStringLiteral("validate"),
                       QStringLiteral("设备 PIT 与包内 PIT 不一致，以设备为准：%1"
                                      "（id %2→%3 / deviceType %4→%5 / binaryType %6→%7）")
                           .arg(e.partition).arg(e.pit.identifier).arg(de->identifier)
                           .arg(e.pit.deviceType).arg(de->deviceType)
                           .arg(e.pit.binaryType).arg(de->binaryType),
                       percentFor(m_writtenBytes));
            }
            r.pit = *de;
        }
        out.append(r);
    }
    if (!missing.isEmpty()) {
        setErr(error, QStringLiteral("设备 PIT 中不存在以下分区：%1 —— 拒刷（未下发任何数据）。"
                                     "请核对固件包与机型是否配套")
                          .arg(missing.join(QStringLiteral("、"))));
        return false;
    }
    return true;
}

bool OdinSession::writeEntry(const ResolvedEntry &r, QString *error)
{
    // 先开镜像：**任何设备命令之前**失败（不把设备带进半途状态）
    QFile image(r.path);
    if (!image.open(QIODevice::ReadOnly)) {
        setErr(error, QStringLiteral("打开镜像失败：%1（分区 %2）").arg(r.path, r.partition));
        return false;
    }
    if (!image.seek(qint64(r.offset))) {
        setErr(error, QStringLiteral("定位镜像数据失败：%1（分区 %2，偏移 %3）")
                          .arg(r.path, r.partition).arg(r.offset));
        return false;
    }

    QString werr;
    if (!m_t.write(frameRequestFlash(), &werr)) {
        setErr(error, QStringLiteral("申请文件传输失败（分区 %1）：%2").arg(r.partition, werr));
        return false;
    }
    if (!readAck(kControlFileTransfer, false,
                 QStringLiteral("申请文件传输（分区 %1）").arg(r.partition), error))
        return false;

    const quint64 seqBytes = quint64(m_profile.packetSize) * quint64(m_profile.sequenceCount);
    const quint64 sequences = (r.size + seqBytes - 1) / seqBytes;
    quint64 sent = 0;

    for (quint64 si = 0; si < sequences; ++si) {
        const bool isLast = (si + 1 == sequences);
        const quint64 realSize = isLast ? (r.size - si * seqBytes) : seqBytes;
        const quint32 aligned = alignedSequenceBytes(realSize, m_profile.packetSize);
        if (aligned == 0) {
            setErr(error, QStringLiteral("分区 %1 的序列 %2 对齐后长度为 0").arg(r.partition).arg(si));
            return false;
        }
        if (!m_t.write(frameRequestSequence(aligned), &werr)) {
            setErr(error, QStringLiteral("申请序列失败（分区 %1，序列 %2）：%3")
                              .arg(r.partition).arg(si).arg(werr));
            return false;
        }
        if (!readAck(kControlFileTransfer, false,
                     QStringLiteral("申请序列（分区 %1，序列 %2）").arg(r.partition).arg(si), error))
            return false;

        const quint32 parts = aligned / m_profile.packetSize;
        for (quint32 pi = 0; pi < parts; ++pi) {
            const quint64 remaining = r.size - sent;
            const int toRead = int(qMin<quint64>(remaining, quint64(m_profile.packetSize)));
            QByteArray part(int(m_profile.packetSize), '\0');   // D9：末片零填充到整片（三方一致）
            if (toRead > 0) {
                const QByteArray chunk = image.read(toRead);
                if (chunk.size() != toRead) {
                    setErr(error, QStringLiteral("读取镜像失败（分区 %1，偏移 %2，需 %3 字节，读到 %4）")
                                      .arg(r.partition).arg(r.offset + sent).arg(toRead).arg(chunk.size()));
                    return false;
                }
                part.replace(0, toRead, chunk);
            }
            if (!m_t.write(part, &werr)) {
                setErr(error, QStringLiteral("写入分片失败（分区 %1，已写 %2 字节）：%3")
                                  .arg(r.partition).arg(sent).arg(werr));
                return false;
            }
            QString rerr;
            const QByteArray rsp = m_t.read(kAckSize, m_profile.flashTimeoutMs, &rerr);
            if (rsp.size() < kAckSize) {
                setErr(error, QStringLiteral("分片应答缺失（分区 %1，分片 %2，已写 %3 字节）：%4")
                                  .arg(r.partition).arg(pi).arg(sent)
                                  .arg(rerr.isEmpty() ? QStringLiteral("设备未回应答") : rerr));
                return false;
            }
            Ack ack;
            if (!parseAck(rsp, ack, error)) {
                prefixErr(error, QStringLiteral("分片应答解析失败（分区 %1）").arg(r.partition));
                return false;
            }
            const AckVerdict v = judgeAck(ack, kResponseSendFilePart, false);
            if (!v.ok) {
                setErr(error, QStringLiteral("分片被拒（分区 %1，分片 %2，已写 %3 字节）：%4")
                                  .arg(r.partition).arg(pi).arg(sent).arg(v.reason));
                return false;
            }
            if (ack.code != pi) {              // 严格序号：错位继续发会把数据写到别处
                setErr(error, QStringLiteral("分片序号不符（分区 %1，分片 %2）：期望 %3，设备回 %4")
                                  .arg(r.partition).arg(pi).arg(pi).arg(ack.code));
                return false;
            }
            sent += quint64(toRead);
            report(QStringLiteral("write"), QStringLiteral("%1（%2）").arg(r.partition, r.imageFile),
                   percentFor(m_writtenBytes + sent));
        }

        // D7：结束序列命令**前**发一次空写（Heimdall kEmptyTransferBeforeAndAfter + odin4
        // send_empty_transfer；Thor 不发 → 2:1 取"发"）。命令后**不**发（after 仅 Heimdall，D6/D8）。
        if (!m_t.write(QByteArray(), &werr))
            // 空写失败按容忍处理：odin4 只落 verbose 日志（odin_protocol.cpp:296-301）
            report(QStringLiteral("info"),
                   QStringLiteral("结束序列前的空写失败（忽略）：%1").arg(werr),
                   percentFor(m_writtenBytes + sent));

        if (!m_t.write(frameEndSequence(r.pit, quint32(realSize), isLast), &werr)) {
            setErr(error, QStringLiteral("结束序列失败（分区 %1，序列 %2）：%3")
                              .arg(r.partition).arg(si).arg(werr));
            return false;
        }
        // 结束序列允许 -7..-2 的"进度码"（odin4:363-366：Ext4/Size/Auth/Write/Erase/WP 是分类不是失败）
        if (!readAck(kControlFileTransfer, true,
                     QStringLiteral("结束序列（分区 %1，序列 %2）").arg(r.partition).arg(si), error))
            return false;
    }

    m_writtenBytes += sent;
    report(QStringLiteral("write"),
           QStringLiteral("%1 完成（%2 字节）").arg(r.partition).arg(sent),
           percentFor(m_writtenBytes));
    return true;
}

void OdinSession::endSession(bool reboot)
{
    QString werr;
    if (!m_t.write(frameEndSession(false), &werr)) {
        report(QStringLiteral("end"), QStringLiteral("结束会话失败（忽略）：%1").arg(werr),
               percentFor(m_writtenBytes));
        return;
    }
    QString err;
    if (!readAck(kControlEndSession, false, QStringLiteral("结束会话"), &err))
        report(QStringLiteral("end"), QStringLiteral("结束会话未被确认（忽略）：%1").arg(err),
               percentFor(m_writtenBytes));
    if (!reboot)
        return;
    if (!m_t.write(frameEndSession(true), &werr)) {
        report(QStringLiteral("end"), QStringLiteral("重启命令发送失败（忽略）：%1").arg(werr),
               percentFor(m_writtenBytes));
        return;
    }
    err.clear();
    if (!readAck(kControlEndSession, false, QStringLiteral("重启"), &err))
        report(QStringLiteral("end"), QStringLiteral("重启未被确认（忽略）：%1").arg(err),
               percentFor(m_writtenBytes));
    else
        report(QStringLiteral("end"), QStringLiteral("已请求设备重启"), percentFor(m_writtenBytes));
}

} // namespace odin
