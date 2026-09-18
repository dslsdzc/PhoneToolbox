#pragma once

// 测试侧的小端字节助手（le32 编码 / le16At / le32At 解码），供 5 个 MTK 帧层用例文件共用。
//
// ⚠️ 本文件是**刻意的独立实现**，**不要**改成 include `core/modes/mtk_le_bytes.h`：
//   用例用这些助手构造"期望帧字节"，与生产共用一份实现会让**编码方向**失去判据
//   （把 le32 改成大端 → 生产输出与期望值一起变 → 帧级断言照样绿）。
//   两份实现互为交叉校验；`mtk_gpt.cpp` 的 crc32 提升为公开 API 时同理（见 A-30 的注释）。
//
// 收敛范围：此前 le32 在本目录 5 个文件里各抄一份、le32At 另有 1 份，改一处漏一处的风险
// 与生产侧相同（同因收敛，理由见上）。

#include <QByteArray>
#include <QtGlobal>

namespace mtkbytes {

inline QByteArray le32(quint32 v)
{
    QByteArray b(4, '\0');
    b[0] = char(v & 0xFF); b[1] = char((v >> 8) & 0xFF);
    b[2] = char((v >> 16) & 0xFF); b[3] = char((v >> 24) & 0xFF);
    return b;
}

inline quint16 le16At(const QByteArray &b, int off)
{
    return quint16(quint8(b.at(off))) | (quint16(quint8(b.at(off + 1))) << 8);
}

inline quint32 le32At(const QByteArray &b, int off)
{
    return quint32(quint8(b.at(off))) | (quint32(quint8(b.at(off + 1))) << 8)
         | (quint32(quint8(b.at(off + 2))) << 16) | (quint32(quint8(b.at(off + 3))) << 24);
}

} // namespace mtkbytes
