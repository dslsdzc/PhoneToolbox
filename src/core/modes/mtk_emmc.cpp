#include "core/modes/mtk_emmc.h"

namespace mtkbrom {
namespace {

void putBe32(QByteArray &out, quint32 v) // 与 mtk_brom.cpp 同构（局部重复，避免跨 TU 依赖）
{
    out.append(char((v >> 24) & 0xFF));
    out.append(char((v >> 16) & 0xFF));
    out.append(char((v >> 8) & 0xFF));
    out.append(char(v & 0xFF));
}

void putBe64(QByteArray &out, quint64 v)
{
    out.append(char((v >> 56) & 0xFF));
    out.append(char((v >> 48) & 0xFF));
    out.append(char((v >> 40) & 0xFF));
    out.append(char((v >> 32) & 0xFF));
    out.append(char((v >> 24) & 0xFF));
    out.append(char((v >> 16) & 0xFF));
    out.append(char((v >> 8) & 0xFF));
    out.append(char(v & 0xFF));
}

quint32 getBe32(const QByteArray &b, int off)
{
    return off + 4 <= b.size()
        ? (quint32(quint8(b[off])) << 24) | (quint32(quint8(b[off + 1])) << 16)
            | (quint32(quint8(b[off + 2])) << 8) | quint32(quint8(b[off + 3])) : 0;
}

quint64 getLe64(const QByteArray &b, int off)
{
    if (off + 8 > b.size())
        return 0;
    quint64 v = 0;
    for (int i = 7; i >= 0; --i)
        v = (v << 8) | quint8(b[off + i]);
    return v;
}

quint32 getLe32(const QByteArray &b, int off)
{
    if (off + 4 > b.size())
        return 0;
    return quint32(quint8(b[off])) | (quint32(quint8(b[off + 1])) << 8)
        | (quint32(quint8(b[off + 2])) << 16) | (quint32(quint8(b[off + 3])) << 24);
}

constexpr quint64 kPacketsize = 0x100000; // 对照 sdmmc_write_data/readflash

} // namespace

// ---- BromSession 内存协议（F1-2）----

bool BromSession::readMemory(quint32 addr, quint32 dwords, QByteArray &out, QString *error)
{
    // 对照 read()：echo 0xD1 → echo addr → echo dwords → status(2B) → 数据 → status2(2B)
    if (!echoCmd(CMD_READ32, error))
        return false;
    if (!echoBe32(addr, error) || !echoBe32(dwords, error))
        return false;
    quint16 status = 0;
    if (!readStatus(status, error))
        return false;
    if (status > 0xFF) {
        if (error) *error = QStringLiteral("READ32 状态错误: 0x%1").arg(status, 4, 16, QLatin1Char('0'));
        return false;
    }
    const int bytes = int(dwords) * 4;
    QByteArray data;
    if (!m_usb->read(data, bytes, 2000, error))
        return false;
    quint16 status2 = 0;
    if (!readStatus(status2, error))
        return false;
    if (status2 > 0xFF) {
        if (error) *error = QStringLiteral("READ32 收尾状态错误: 0x%1").arg(status2, 4, 16, QLatin1Char('0'));
        return false;
    }
    out = data;
    return true;
}

bool BromSession::writeMemory(quint32 addr, const QByteArray &data, QString *error)
{
    // 对照 write()：echo 0xD4 → echo addr → echo count → status(<=3) → 逐值 echo → status2
    if (data.size() % 4 != 0) {
        if (error) *error = QStringLiteral("WRITE32 数据长度须为 4 的倍数");
        return false;
    }
    if (!echoCmd(CMD_WRITE32, error))
        return false;
    const quint32 count = quint32(data.size() / 4);
    if (!echoBe32(addr, error) || !echoBe32(count, error))
        return false;
    quint16 status = 0;
    if (!readStatus(status, error))
        return false;
    if (status > 3) {
        if (error) *error = QStringLiteral("WRITE32 状态错误: 0x%1").arg(status, 4, 16, QLatin1Char('0'));
        return false;
    }
    // 逐 4B 值 echo（对照 write() 内循环 echo(pack(">I", val))）
    for (int off = 0; off < data.size(); off += 4) {
        QByteArray p;
        putBe32(p, quint32(quint8(data[off])) << 24 | quint32(quint8(data[off + 1])) << 16
                       | quint32(quint8(data[off + 2])) << 8 | quint32(quint8(data[off + 3])));
        if (!m_usb->write(p, error))
            return false;
        QByteArray echo;
        if (!m_usb->read(echo, 4, 1000, error) || echo != p) {
            if (error && error->isEmpty())
                *error = QStringLiteral("WRITE32 值回显不符");
            return false;
        }
    }
    quint16 status2 = 0;
    if (!readStatus(status2, error))
        return false;
    if (status2 > 0xFF) {
        if (error) *error = QStringLiteral("WRITE32 收尾状态错误: 0x%1").arg(status2, 4, 16, QLatin1Char('0'));
        return false;
    }
    return true;
}

// ---- DaStorage ----

bool DaStorage::ensureDa(QString *error) const
{
    if (!m_daActive) {
        if (error) *error = QStringLiteral("DA 未激活：需先成功上传并跳转 DA（sendPayload），EMMC 命令属于 DA 阶段");
        return false;
    }
    return true;
}

bool DaStorage::switchPart(int partType, QString *error)
{
    // 对照 sdmmc_switch_part()：发 0x60 → ACK → 发 parttype → ACK（DA 命令无回显，ACK 即应答）
    if (!m_session.usb()->write(QByteArray(1, char(DA_CMD_SDMMC_SWITCH_PART)), error))
        return false;
    QByteArray ack;
    if (!m_session.usb()->read(ack, 1, 1000, error))
        return false;
    if (ack.size() != 1 || quint8(ack[0]) != RSP_ACK) {
        if (error) *error = QStringLiteral("switch_part 首 ACK 缺失");
        return false;
    }
    if (!m_session.usb()->write(QByteArray(1, char(quint8(partType))), error))
        return false;
    if (!m_session.usb()->read(ack, 1, 1000, error))
        return false;
    if (ack.size() != 1 || quint8(ack[0]) != RSP_ACK) {
        if (error) *error = QStringLiteral("switch_part 尾 ACK 缺失");
        return false;
    }
    return true;
}

bool DaStorage::emmcRead(quint64 addr, quint64 length, QByteArray &out,
                         int partType, QString *error)
{
    // 对照 readflash() emmc 分支：switch_part → 发 0xD6（无回显）→ 0x0C(Host Linux) →
    // 0x02(EMMC) → addr 8B → length 8B → packetsize 4B → 读 1B ACK；
    // 循环：读块 → 读 checksum(2B) → 发 ACK
    if (!ensureDa(error))
        return false;
    if (!switchPart(partType, error))
        return false;
    if (!m_session.usb()->write(QByteArray(1, char(DA_CMD_READ)), error))
        return false;
    QByteArray hdr;
    hdr.append(char(0x0C)); // Host: Linux
    hdr.append(char(0x02)); // Storage: EMMC
    putBe64(hdr, addr);
    putBe64(hdr, length);
    putBe32(hdr, quint32(kPacketsize));
    if (!m_session.usb()->write(hdr, error))
        return false;
    QByteArray ack;
    if (!m_session.usb()->read(ack, 1, 1000, error))
        return false;
    if (ack.size() != 1 || quint8(ack[0]) != RSP_ACK) {
        if (error) *error = QStringLiteral("EMMC 读命令未获 ACK");
        return false;
    }
    out.clear();
    quint64 remaining = length;
    while (remaining > 0) {
        const int want = int(qMin<quint64>(remaining, kPacketsize));
        QByteArray block;
        if (!m_session.usb()->read(block, want, 5000, error))
            return false;
        QByteArray chk;
        if (!m_session.usb()->read(chk, 2, 1000, error))
            return false;
        // checksum 校验（对照 readflash：仅读取，不校验失败路径）
        out += block;
        remaining -= quint64(block.size());
        if (!m_session.usb()->write(QByteArray(1, char(RSP_ACK)), error))
            return false;
    }
    return true;
}

bool DaStorage::emmcWrite(quint64 addr, const QByteArray &data,
                          int partType, QString *error)
{
    // 对照 sdmmc_write_data()：发 0x62 → storage(1B=2 EMMC) → parttype → addr 8B →
    // length 8B → packetsize 4B(0x100000) → ACK；循环：发 ACK → 块 → checksum 2B → CONT
    if (!ensureDa(error))
        return false;
    if (!m_session.usb()->write(QByteArray(1, char(DA_CMD_SDMMC_WRITE_DATA)), error))
        return false;
    QByteArray hdr;
    // storage: EMMC。对照 dalegacy_lib.py：write 帧源码用 MTK_DA_STORAGE_EMMC=0x01，
    // 但 read/format 帧硬编码 0x02（源码内部不一致，storage.py 的 0x02 为死代码）——
    // 本实现与 read 帧一致取 0x02，真机验证前标注"待抓包确认"
    hdr.append(char(0x02));
    hdr.append(char(quint8(partType)));
    putBe64(hdr, addr);
    putBe64(hdr, quint64(data.size()));
    putBe32(hdr, quint32(kPacketsize));
    if (!m_session.usb()->write(hdr, error))
        return false;
    QByteArray ack;
    if (!m_session.usb()->read(ack, 1, 1000, error))
        return false;
    if (ack.size() != 1 || quint8(ack[0]) != RSP_ACK) {
        if (error) *error = QStringLiteral("EMMC 写命令未获 ACK");
        return false;
    }
    quint64 pos = 0;
    while (pos < quint64(data.size())) {
        // 循环前置 ACK（对照 sdmmc_write_data：usbwrite(ACK) 后发块）
        if (!m_session.usb()->write(QByteArray(1, char(RSP_ACK)), error))
            return false;
        QByteArray piece = data.mid(int(pos), int(kPacketsize));
        // 512 对齐补零（对照 sdmmc_write_data：len % 512 补零）
        while (piece.size() % 512 != 0)
            piece.append(char(0));
        if (!m_session.usb()->write(piece, error))
            return false;
        quint32 sum = 0;
        for (char c : piece)
            sum += quint8(c);
        QByteArray chk;
        chk.append(char((sum >> 8) & 0xFF));
        chk.append(char(sum & 0xFF));
        if (!m_session.usb()->write(chk, error))
            return false;
        QByteArray cont;
        if (!m_session.usb()->read(cont, 1, 2000, error))
            return false;
        if (cont.size() != 1 || quint8(cont[0]) != RSP_CONF) {
            if (error) *error = QStringLiteral("EMMC 写块未获 CONT 确认");
            return false;
        }
        pos += quint64(piece.size());
    }
    return true;
}

bool DaStorage::listPartitions(QList<EmPartition> &out, QString *error)
{
    // 对照 read_pmt()：发 0xA5 → 读 ack(1B) → 读 length(4B BE) → 发 ACK →
    // 读 partdata → 发 ACK；解析 0x60/0x58/0x4C 条目（小端字段）
    if (!ensureDa(error))
        return false;
    out.clear();
    if (!m_session.usb()->write(QByteArray(1, char(DA_CMD_SDMMC_READ_PMT)), error))
        return false;
    QByteArray ack;
    if (!m_session.usb()->read(ack, 1, 1000, error))
        return false;
    if (ack.size() != 1 || quint8(ack[0]) != RSP_ACK) {
        if (error) *error = QStringLiteral("read_pmt 未获 ACK");
        return false;
    }
    QByteArray lenB;
    if (!m_session.usb()->read(lenB, 4, 1000, error))
        return false;
    const quint32 dataLen = getBe32(lenB, 0);
    if (!m_session.usb()->write(QByteArray(1, char(RSP_ACK)), error))
        return false;
    QByteArray pd;
    if (!m_session.usb()->read(pd, int(dataLen), 3000, error))
        return false;
    m_session.usb()->write(QByteArray(1, char(RSP_ACK)), error); // 收尾 ACK

    // 条目尺寸判定（对照 read_pmt()）：partdata[0x48]==0xFF → 0x60 条目；
    // 否则 mask_flags = <Q partdata[0x48:0x50]，0 < mask_flags < 0xA → 0x58，否则 0x4C
    // 注：mtkclient 自身此判定为崩溃级缺陷（0x48:0x4C 仅 4B 却 unpack("<Q",…) 读 8B
    // 抛 struct.error，0x4C 条目解析同病）——本实现重建为 4B 读（0x4C 条目 flags@0x48
    // 就是 4B 字段），避免并入下一条目名字节；0x58 条目此处为 offset@0x48 低 4B。
    const quint8 marker = pd.size() > 0x48 ? quint8(pd[0x48]) : 0;
    int entrySize = 0x4C;
    if (marker == 0xFF) {
        entrySize = 0x60;
    } else {
        const quint64 maskFlags = getLe32(pd, 0x48);
        entrySize = (maskFlags > 0 && maskFlags < 0xA) ? 0x58 : 0x4C;
    }
    for (int pos = 0; pos + entrySize <= pd.size(); pos += entrySize) {
        EmPartition p;
        // 名字截到首个 NUL（对照 read_pmt：rstrip(b"\x00")；QString::trimmed 不剥离 NUL）
        QByteArray nameRaw = pd.mid(pos, 0x40);
        const int nul = nameRaw.indexOf(char(0));
        if (nul >= 0)
            nameRaw.truncate(nul);
        p.name = QString::fromUtf8(nameRaw).trimmed();
        if (entrySize == 0x60) {
            p.sizeBytes = getLe64(pd, pos + 0x40);
            p.offsetBytes = getLe64(pd, pos + 0x50);
        } else if (entrySize == 0x58) {
            p.sizeBytes = getLe64(pd, pos + 0x40);
            p.offsetBytes = getLe64(pd, pos + 0x48);
        } else {
            // 0x4C 条目：name@0(0x40) + size@0x40(4B) + offset@0x44(4B) + flags@0x48(4B)
            p.sizeBytes = getLe32(pd, pos + 0x40);
            p.offsetBytes = getLe32(pd, pos + 0x44);
        }
        if (!p.name.isEmpty())
            out.append(p);
    }
    return true;
}

} // namespace mtkbrom
