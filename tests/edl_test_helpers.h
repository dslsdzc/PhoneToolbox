// tests/edl_test_helpers.h
#pragma once
#include <QByteArray>
#include <QList>
#include "core/edl/sahara.h"

// Sahara 帧：cmd(4B LE) + 总长(4B LE) + N×4B LE 参数（与 src/core/edl/sahara.cpp 的帧布局一致；
// 既有解析器见 src/core/modes/edl_handler.cpp:201-254）
//
// 本文件是**共享**夹具：Task 4（test_edl_sahara.cpp）与 Task 6（test_edl_session.cpp）都用它，
// 不得在测试文件内各写一份。
inline QByteArray saharaFrame(quint32 cmd, const QList<quint32> &words)
{
    QByteArray f(8 + words.size() * 4, '\0');
    auto put32 = [&f](int off, quint32 v) {
        f[off] = char(v & 0xFF); f[off+1] = char((v >> 8) & 0xFF);
        f[off+2] = char((v >> 16) & 0xFF); f[off+3] = char((v >> 24) & 0xFF);
    };
    put32(0, cmd); put32(4, 8 + words.size() * 4);
    for (int i = 0; i < words.size(); ++i) put32(8 + i * 4, words.at(i));
    return f;
}

// 64 位 READ_DATA 帧：参数 = image_id(u64) + offset(u64) + length(u64) = 6 个 u32 字（低位在前）。
// ⚠️ 别手写这串字：写 {0,12,0,0,4,0} 会编码成 image_id=0xC00000000 / offset=**0** / length=4
//    （本计划早期版本就是这么错的，实测回吐 "0123" 而非期望切片）—— 用本函数。
// 参照解析器：src/core/modes/edl_handler.cpp:302-312（既有 64 位分支，会把 offset 截成 32 位）、
// edl/edlclient/Library/sahara.py:678-700。
inline QByteArray saharaReadData64Frame(quint64 imageId, quint64 offset, quint64 length)
{
    QList<quint32> words;
    const quint64 vals[3] = {imageId, offset, length};
    for (quint64 v : vals) { words << quint32(v & 0xFFFFFFFFu) << quint32(v >> 32); }
    return saharaFrame(quint32(edl::SAHARA_READ_DATA_64), words);
}
