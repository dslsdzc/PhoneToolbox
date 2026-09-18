#pragma once

// MTK 帧层共用的**小端**字节助手（le32 编码 / le16At / le32At 解码）。
//
// 为什么有本文件：这三个函数此前在 mtk_xflash_session.cpp / mtk_xml_session.cpp /
// mtk_xflash_payload.cpp 里各抄一份，注释里自陈"跨文件不共享匿名命名空间，各留一份"——
// 而同族模块（session 层与 payload 层）的帧格式判据必须逐字节一致，多份副本只会
// 在改动时漂移（改一处、漏一处 → 两层的"同一"帧格式实际不同）。故收敛到本头。
//
// ⚠️ **测试侧刻意不复用本头**：`tests/mtk_bytes.h` 是**另一份独立实现**。
//   理由：测试用这些助手构造"期望帧字节"，若与生产共用同一个函数，则"编码方向"的
//   判据（如把 le32 改成大端）会同时改变生产输出与期望值 → 用例照样绿。
//   两份实现互为交叉校验，是这些帧级断言甄别力的来源，**不要**把测试改成 include 本头。
//
// ⚠️ 收敛 `leToU32`（`mtk_xflash_session.cpp`）/ `le64` 之前先看这条：`test_mtk_xflash_session`
//   目前**还能**抓到"编码方向被改"这类变异，靠的是这两个函数**尚未收敛到本头**（另有
//   `leToU32` 是 xflash 侧独有的解码副本），加上用例里对 `QByteArray("OK\0\0", 4)` 这类
//   **硬编码字面量**的比对 —— 属**偶然，非设计**。若把它们也收敛进来，xflash 侧会落进与
//   `mtk_xml_session` 相同的处境：**编码方向变异整卷假绿**（W4-T2 复审的 A-22 对照组实测：
//   xml_session 26 passed / 0 failed，一条不红）。真要收敛，必须**同时**给测试侧留一份独立
//   实现（`tests/mtk_bytes.h` 的姿态），否则那组判据会**静默消失**。

#include <QByteArray>
#include <QtGlobal>

namespace mtkbrom {

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

} // namespace mtkbrom
