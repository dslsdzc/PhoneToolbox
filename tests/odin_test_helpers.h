// tests/odin_test_helpers.h
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include "core/odin/pit.h"

// 合成 PIT 夹具（共享：test_pit / test_samsung_plan / test_odin_session 都用它，勿各写一份）。
// 字段默认值取真样本 J1POP3G.pit 的 BOOT 条目形态（dt=2 MMC / attr=0x5 / upd=1）。
namespace odintest {

inline void putU32(QByteArray &d, int off, quint32 v)
{
    d[off]     = char(v & 0xFF);
    d[off + 1] = char((v >> 8) & 0xFF);
    d[off + 2] = char((v >> 16) & 0xFF);
    d[off + 3] = char((v >> 24) & 0xFF);
}

inline void putU16(QByteArray &d, int off, quint16 v)
{
    d[off]     = char(v & 0xFF);
    d[off + 1] = char((v >> 8) & 0xFF);
}

inline quint32 rdU32(const QByteArray &d, int off)
{
    return quint32(quint8(d.at(off))) | (quint32(quint8(d.at(off + 1))) << 8)
         | (quint32(quint8(d.at(off + 2))) << 16) | (quint32(quint8(d.at(off + 3))) << 24);
}

struct PitSpec {
    QByteArray name;                    // 分区名（≤32；内部会 NUL 填满）
    quint32 binaryType = 0;
    quint32 deviceType = 2;
    quint32 identifier = 0;
    quint32 attributes = 0x5;
    quint32 updateAttributes = 1;
    quint32 blockSizeOrOffset = 0;
    quint32 blockCount = 1024;
    quint32 fileOffset = 0;
    quint32 fileSize = 0;
    QByteArray flashFilename;           // 可含 '\r\n'（用例故意构造）
    QByteArray fotaFilename;
};

inline QByteArray field32(const QByteArray &s)
{
    QByteArray f(32, '\0');
    const QByteArray cut = s.left(32);
    for (int i = 0; i < cut.size(); ++i)
        f[i] = cut.at(i);
    return f;
}

// 28 字节头 + N×132 字节条目 + trailing（默认 1024 字节非零，模拟真实尾部签名块）
inline QByteArray buildPit(const QList<PitSpec> &entries,
                           const QByteArray &comTar2 = QByteArray("COM_TAR2"),
                           const QByteArray &cpuBlId = QByteArray("MSM8974"),
                           quint16 luCount = 0,
                           const QByteArray &trailing = QByteArray(1024, '\x5A'))
{
    QByteArray d(28 + entries.size() * 132, '\0');
    putU32(d, 0, 0x12349876);
    putU32(d, 4, quint32(entries.size()));
    const QByteArray com = field32(comTar2);
    for (int i = 0; i < 8; ++i) d[8 + i] = com.at(i);
    const QByteArray cpu = field32(cpuBlId);
    for (int i = 0; i < 8; ++i) d[16 + i] = cpu.at(i);
    putU16(d, 24, luCount);
    putU16(d, 26, 0);
    for (int i = 0; i < entries.size(); ++i) {
        const PitSpec &s = entries.at(i);
        const int o = 28 + i * 132;
        putU32(d, o + 0, s.binaryType);
        putU32(d, o + 4, s.deviceType);
        putU32(d, o + 8, s.identifier);
        putU32(d, o + 12, s.attributes);
        putU32(d, o + 16, s.updateAttributes);
        putU32(d, o + 20, s.blockSizeOrOffset);
        putU32(d, o + 24, s.blockCount);
        putU32(d, o + 28, s.fileOffset);
        putU32(d, o + 32, s.fileSize);
        const QByteArray nm = field32(s.name);
        const QByteArray ff = field32(s.flashFilename);
        const QByteArray fa = field32(s.fotaFilename);
        for (int k = 0; k < 32; ++k) {
            d[o + 36 + k]  = nm.at(k);
            d[o + 68 + k]  = ff.at(k);
            d[o + 100 + k] = fa.at(k);
        }
    }
    return d + trailing;
}

} // namespace odintest
