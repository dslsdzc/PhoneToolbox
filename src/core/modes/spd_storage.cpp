#include "core/modes/spd_storage.h"

#include <QFile>

#include <climits>

#include "image_engine/pac_image.h"

// 实现对照（核实记录见头文件与计划文档"协议核实记录"）
namespace spd {
namespace {

void putBe32(QByteArray &out, quint32 v)
{
    out.append(char((v >> 24) & 0xFF));
    out.append(char((v >> 16) & 0xFF));
    out.append(char((v >> 8) & 0xFF));
    out.append(char(v & 0xFF));
}

// MIDST_DATA 块上限：计划选值 2048（参照默认 528，≤4096 可观察范围内）
constexpr int kFdlBlock = 2048;
constexpr int kPartBlock = 4096; // 分区写 MIDST_DATA 块（行为观察 write_part 默认 4096）
// 命令响应超时：EXEC_DATA 设备执行可达 15s；writePartition 的 MIDST 每块
// 同样用 15s（行为观察：分区数据块上传等待）
constexpr int kExecTimeoutMs = 15000;

} // namespace

// 分区选择包（行为观察：分区选择/加载/擦除流程 核实）：name 36×UTF-16LE + size LE32
// （mode64 时另含 size_hi LE32 + dummy u64）——载荷 76B 或 88B。
// 名称超 36 单元时截断（调用方 selectPartition 已前置拒绝）。
// 公共纯函数：单测直接验证 mode64 88B 布局（writePartition 经 selectPartition 使用）
QByteArray selectPartitionPacket(const QString &name, quint64 size, bool mode64)
{
    QByteArray pkt(mode64 ? 88 : 76, '\0');
    // name 36×UTF-16LE（显式小端写入，不依赖宿主字节序；行为观察：小端写入）
    int off = 0;
    for (const QChar c : name) {
        if (off >= 72) break;
        const quint16 u = c.unicode();
        pkt[off++] = char(u & 0xFF);
        pkt[off++] = char(u >> 8);
    }
    // size LE32（低 32 位；高 32 位入 size_hi，行为观察：小端写入）
    pkt[72] = char(size & 0xFF);
    pkt[73] = char((size >> 8) & 0xFF);
    pkt[74] = char((size >> 16) & 0xFF);
    pkt[75] = char((size >> 24) & 0xFF);
    if (mode64) {
        pkt[76] = char((size >> 32) & 0xFF);
        pkt[77] = char((size >> 40) & 0xFF);
        pkt[78] = char((size >> 48) & 0xFF);
        pkt[79] = char((size >> 56) & 0xFF);
        // dummy 8B 保持零（行为观察 pkt 零初始化）
    }
    return pkt;
}

bool SpdFlasher::sendAndExpectAck(quint16 type, const QByteArray &payload, QString *error,
                                  int timeoutMs)
{
    QByteArray reply;
    quint16 replyType = 0;
    if (!m_session.sendCommand(type, payload, reply, 64, error, &replyType, timeoutMs))
        return false;
    // 强制校验响应 type == BSL_REP_ACK（行为观察：发后强制校验 ACK 响应）：
    // 设备错误响应（如 0x84 OPERATION_FAILED）不得被当作成功
    if (replyType != BSL_REP_ACK) {
        if (error) *error = QStringLiteral("命令 0x%1 未获 ACK（响应 0x%2）")
                                .arg(type, 4, 16, QLatin1Char('0'))
                                .arg(replyType, 4, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

bool SpdFlasher::uploadFdl(const QString &fdlPath, quint32 loadAddr, bool execAfter,
                           QString *error)
{
    QFile f(fdlPath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 FDL 文件: %1").arg(fdlPath);
        return false;
    }
    // 明确上限：块偏移循环仅支持 < 2GiB（qsizetype 比较，先于 readAll 读入；
    // 项目先例 update_app.cpp 的 INT_MAX 守卫，7ad80f8）
    if (f.size() > qsizetype(INT_MAX)) {
        if (error) *error = QStringLiteral("FDL 文件过大（≥2GiB 暂不支持）");
        return false;
    }
    const QByteArray data = f.readAll();
    f.close();
    if (data.isEmpty()) {
        if (error) *error = QStringLiteral("FDL 文件为空");
        return false;
    }
    // START_DATA：addr BE32 + size BE32（每步响应强制 ACK，行为观察：发后强制校验 ACK 响应）
    QByteArray payload;
    putBe32(payload, loadAddr);
    putBe32(payload, quint32(data.size()));
    if (!sendAndExpectAck(BSL_CMD_START_DATA, payload, error))
        return false;
    // MIDST_DATA×N（块 ≤2048，每步响应强制 ACK）
    for (qsizetype off = 0; off < data.size(); off += kFdlBlock) {
        const QByteArray block = data.mid(off, kFdlBlock);
        if (!sendAndExpectAck(BSL_CMD_MIDST_DATA, block, error))
            return false;
    }
    // END_DATA
    if (!sendAndExpectAck(BSL_CMD_END_DATA, QByteArray(), error))
        return false;
    // EXEC_DATA（execAfter 时；设备执行耗时可达 15s，行为观察：响应超时 15s）。
    // 参照对 FDL2 EXEC 的 INCOMPATIBLE_PARTITION(0x96) 宽容；实现严格 ACK
    // （保守，真机遇 0x96 将失败）
    if (execAfter
        && !sendAndExpectAck(BSL_CMD_EXEC_DATA, QByteArray(), error, kExecTimeoutMs))
        return false;
    return true;
}

bool SpdFlasher::eraseFlash(quint32 addr, quint32 size, QString *error)
{
    QByteArray payload;
    putBe32(payload, addr);
    putBe32(payload, size);
    return sendAndExpectAck(BSL_CMD_ERASE_FLASH, payload, error);
}

bool SpdFlasher::readFlash(quint32 addr, quint32 len, quint32 offset, QByteArray &out,
                           QString *error)
{
    // 单次读取上限 0xFFFF（响应 len 字段 16 位，行为观察）；更大须分块
    if (len > 0xFFFF) {
        if (error) *error = QStringLiteral("单次读取超过 0xFFFF 字节，需分块");
        return false;
    }
    QByteArray payload;
    putBe32(payload, addr);
    putBe32(payload, len);
    putBe32(payload, offset);
    QByteArray reply;
    quint16 rtype = 0;
    if (!m_session.sendCommand(BSL_CMD_READ_FLASH, payload, reply, int(len) + 16, error,
                               &rtype))
        return false;
    // sendCommand 不校验响应 type——此处断言 BSL_REP_READ_FLASH(0x93)（行为观察）
    if (rtype != BSL_REP_READ_FLASH) {
        if (error) *error = QStringLiteral("READ_FLASH 响应类型不符 (0x%1)")
                                .arg(rtype, 4, 16, QLatin1Char('0'));
        return false;
    }
    out = reply;
    // 短读报错而非静默截断：返回数据量须与请求一致（0xFFFF 上限内可比）。
    // 严格等长报错（参照允许短读为区域末尾正常结束）；真机边界读可能误报，待验证
    if (out.size() != int(len)) {
        if (error) *error = QStringLiteral("READ_FLASH 响应数据量不符（%1/请求 %2 字节）")
                                .arg(out.size()).arg(len);
        return false;
    }
    return true;
}

bool SpdFlasher::resetDevice(QString *error)
{
    return sendAndExpectAck(BSL_CMD_NORMAL_RESET, QByteArray(), error);
}

bool SpdFlasher::selectPartition(quint16 cmd, const QString &name, quint64 size,
                                 QString *error)
{
    // 分区名 36 单元上限（行为观察：名称 36 单元上限（UTF-16 单元）超长报错——此处显式拒绝）
    if (name.size() > 36) {
        if (error) *error = QStringLiteral("分区名过长（>36 单元）: %1").arg(name);
        return false;
    }
    const bool mode64 = (size >> 32) != 0;
    return sendAndExpectAck(cmd, selectPartitionPacket(name, size, mode64), error);
}

bool SpdFlasher::erasePartition(const QString &name, QString *error)
{
    // 行为观察：分区选择/加载/擦除流程（擦除）：ERASE_FLASH + 选择包（size=0，非 64 位，载荷 76B）
    return selectPartition(BSL_CMD_ERASE_FLASH, name, 0, error);
}

bool SpdFlasher::writePartition(const QString &name, const QByteArray &data,
                                QString *error)
{
    // 行为观察：分区选择/加载/擦除流程（写分区）：START_DATA + 选择包（size=数据长度）→ ACK →
    // MIDST_DATA×N（块 ≤4096，逐块 ACK，15s 超时）→ END_DATA → ACK。
    // 分区写无 EXEC_DATA（区别于 FDL 上传）；真机验证待后续（诚实边界）
    if (!selectPartition(BSL_CMD_START_DATA, name, quint64(data.size()), error))
        return false;
    for (qsizetype off = 0; off < data.size(); off += kPartBlock) {
        if (!sendAndExpectAck(BSL_CMD_MIDST_DATA, data.mid(off, kPartBlock), error,
                              kExecTimeoutMs))
            return false;
    }
    return sendAndExpectAck(BSL_CMD_END_DATA, QByteArray(), error);
}

bool isFdlPartition(const QString &partitionName)
{
    const QString n = partitionName.toLower();
    return n == QStringLiteral("fdl") || n == QStringLiteral("fdl1")
        || n == QStringLiteral("fdl2");
}

bool runSpdFlash(const QString &pacPath, const QString &fdl1Path, const QString &fdl2Path,
                 std::function<void(const QString &, int)> progress, QString *error)
{
    // 1. 解析 pac（B8 imgpac 复用）
    QFile pacFile(pacPath);
    if (!pacFile.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("无法打开 pac: %1").arg(pacPath);
        return false;
    }
    const QByteArray pacData = pacFile.readAll();
    QList<imgpac::PacPartition> parts;
    if (!imgpac::parsePac(pacData, parts, error))
        return false;
    if (parts.isEmpty()) {
        if (error) *error = QStringLiteral("pac 无分区");
        return false;
    }

    // 2. 枚举 + 打开会话
    QList<QPair<int, int>> devs;
    if (!enumerateUsb(devs, error))
        return false;
    if (devs.isEmpty()) {
        if (error) *error = QStringLiteral("未检测到展锐设备（VID 0x1782）");
        return false;
    }
    std::unique_ptr<IUsbChannel> usb;
    if (!openLibusbUsb(devs.first().first, devs.first().second, usb, error))
        return false;
    SpdSession session(std::move(usb), devs.first().first, devs.first().second);
    if (!session.connect(error))
        return false;
    SpdFlasher flasher(session);

    // 3. FDL1 + FDL2 上传（诚实边界：二进制用户提供）
    // 3a. FDL1 握手（行为观察核实线序）：CHECK_BAUD(1B)→REP_VER(0x81)→CONNECT→ACK
    if (!session.handshake(1, 1, error))
        return false;
    if (progress) progress(QStringLiteral("fdl1"), 5);
    if (!flasher.uploadFdl(fdl1Path, 0x40004000, true, error))
        return false;
    // 3b. FDL2 就绪等待（行为观察核实）：CHECK_BAUD(4B) 重试 ≤10 次直至
    // REP_VER→CONNECT→ACK（FDL1 EXEC 后设备加载 FDL2，无响应则重试）；
    // 成功后同一会话继续上传 FDL2（参照实现不重新枚举/不等待新端口——
    // 若真机出现断线重枚举，需重新连接会话，待真机验证）
    if (!session.handshake(4, 10, error))
        return false;
    if (progress) progress(QStringLiteral("fdl2"), 10);
    if (!flasher.uploadFdl(fdl2Path, 0x14000000, true, error))
        return false;

    // 4. 逐分区（fdl 分区跳过——已单独上传）
    // 写路径（行为观察核实，对照参考实现）：先擦除后写（行为观察：分区选择/加载/擦除流程）；
    // 分区选择包 name 36×UTF-16LE + size LE32（mode64 时
    // + size_hi + dummy，载荷 76/88B）；写数据经 START_DATA/MIDST_DATA×N/
    // END_DATA 上传，无 EXEC_DATA
    int done = 0;
    for (const imgpac::PacPartition &p : parts) {
        if (isFdlPartition(p.name)) continue;
        // 操作型条目无数据（行为观察 nFileFlag=0，如 "FLASH"）——跳过
        if (p.size == 0) continue;
        if (progress) progress(p.name, 10 + 80 * done / parts.size());
        // 分区数据从 pac 提取（B8 布局：p.offset/p.size；parsePac 已校验数据范围）
        const QByteArray pdata = pacData.mid(qsizetype(p.offset), qsizetype(p.size));
        // 逐分区先擦后写（计划流程；行为观察：参考实现 write_part 本身不预擦除，
        // 擦除为独立命令 erase_part）
        if (!flasher.erasePartition(p.name, error))
            return false;
        if (!flasher.writePartition(p.name, pdata, error))
            return false;
        ++done;
    }
    if (progress) progress(QString(), 95);
    if (!flasher.resetDevice(error))
        return false;
    if (progress) progress(QString(), 100);
    return true;
}

} // namespace spd
