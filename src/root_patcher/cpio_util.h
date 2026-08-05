#pragma once
#include <QByteArray>
#include <QMap>
#include <QString>

// newc cpio（070701/070702）读写。
// Task C7 从 magisk_patcher.cpp / kernelsu_patcher.cpp 的私有实现（逐字节
// 同源）提取合并为共享工具（C4 concern 5）。布局与 Magisk 的 cpio 实现
// 逐字节一致（联网验证 v25.2 的 cpio.cpp 与 master 的 cpio.rs）：110B 头
// （"070701" + 13×8 hex 字段）、名字 NUL 结尾、名字与数据均按 4 字节对齐；
// TRAILER!!! 结尾；Android 支持多段 cpio 拼接（TRAILER 后继续搜索下一个
// cpio 魔数）。硬链接条目与 Magisk 同样按原始数据透传（不特殊处理，nlink
// 重序列化时固定为 1）。
namespace patcher {

struct CpioEntry {
    quint32 mode = 0;
    QByteArray data;
};

class CpioArchive
{
public:
    // 解析失败返回 false 并写 err（绝不崩溃；所有偏移均做边界校验）
    bool parse(const QByteArray &buf, QString *err);

    bool exists(const QString &name) const;

    // 读取条目；不存在返回 false（out 可传 nullptr 仅作存在性探测）
    bool find(const QString &name, CpioEntry *out) const;

    // 改名（ksud cpio.mv 语义：原样保留模式与内容）；from 不存在返回 false
    bool rename(const QString &from, const QString &to);

    // 加入或替换（cpio add 语义）
    void addOrReplace(const QString &name, const CpioEntry &e);

    // Magisk 修补判定（ksud 同款保护："Cannot work with Magisk patched
    // image"）。Magisk 注入产物含 .backup 链（.backup 目录或 .backup/init）
    bool isMagiskPatched() const;

    // 键序排列输出，与 Magisk 的 std::map/BTreeMap 行为一致（稳定可复现）
    QByteArray serialize() const;

private:
    QMap<QString, CpioEntry> m_entries;
};

} // namespace patcher
