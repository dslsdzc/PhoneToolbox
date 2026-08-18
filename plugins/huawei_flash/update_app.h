#pragma once

// 插件内自包含的 update.app 解析（不依赖主项目 image_engine）。
// 独立实现声明：条目布局（magic/头长/数据长/分区名）为公共领域协议行为
// 观察所得；实现自写。
//
// 布局（行为观察）：条目 magic 55 AA 5A A5 + headerLength(LE32) + 4B + 8B
// + 4B + dataLength(LE32) + 16B + 16B + 32B 分区名(UTF-8, NUL 结尾) + 6B
// + (headerLength - 98) 剩余字节；分区名偏移 = 4+4+4+8+4+4+16+16 = 60。
// 头总长 = headerLength。
// 容错（行为观察）：每条目数据后可有 0-3 字节 4 字节对齐填充；dataLength==0
// 或分区名为空 = 列表结束标记（不追加）；首条目 magic 前可有前导字节（按字节
// 扫描首个 magic）。

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>

namespace hisi {

struct AppPartition {
    QString name;
    QByteArray header; // 分区头（含 fileSeq@20..24 大端）
    QByteArray data;   // 分区数据
};

// 解析 update.app 条目表；失败返回 false 并填 error
bool parseUpdateApp(const QByteArray &data, QList<AppPartition> &out, QString *error);

} // namespace hisi
