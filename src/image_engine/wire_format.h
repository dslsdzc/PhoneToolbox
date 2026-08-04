#pragma once
#include <QByteArray>
#include <QList>

namespace pbwire {

struct Field {
    int number = 0;
    int wireType = -1;   // 0=varint, 1=64bit, 2=len-delimited, 5=32bit
    quint64 varint = 0;
    QByteArray bytes;
};

// 解析一条消息的全部字段（顶层）；ok=false 表示非法数据
QList<Field> parseMessage(const QByteArray &data, bool &ok);
QByteArray encodeVarintRaw(quint64 value);
QByteArray encodeVarint(int number, quint64 value);
QByteArray encodeBytes(int number, const QByteArray &data);
QByteArray encodeMessage(int number, const QByteArray &message);

} // namespace pbwire
