#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

namespace imgtar {

struct TarEntry {
    QString name;
    QByteArray data;
    bool isDir = false;
    bool isSymlink = false;
    QString linkTarget;
};

// 流式条目索引（不读数据区，内存 O(1)）：offset = 该条目数据区在文件内的**绝对偏移**。
// 目录/符号链接/零长度条目的 offset 无意义（等于其数据区起点，size=0）。
struct TarIndexEntry {
    QString name;
    quint64 offset = 0;
    quint64 size = 0;
    bool isDir = false;
};

// 只读 tar 头块建立索引（`.tar.md5` 的校验行不计入；`tarEnd` 出参 = 归档区结束偏移）。
// 名字规则与 extractTar 一致（ustar 前缀字段非空时拼成 "prefix/name"，尾部 '/' 去掉）。
// 坏 size 字段 / 数据区越界 / 文件打不开 → false + 中文 *error（不返回半个索引）。
bool indexTarStream(const QString &tarPath, QList<TarIndexEntry> &out,
                    quint64 *tarEnd = nullptr, QString *error = nullptr);

bool extractTar(const QByteArray &tar, QList<TarEntry> &entries);
QByteArray buildTar(const QList<TarEntry> &entries);      // Task 11
QByteArray appendMd5Footer(const QByteArray &tar);        // 三星 Odin 兼容尾部
bool verifyMd5Footer(const QByteArray &tarMd5);

// ---- 流式接口（GB 级大文件路径，内存 O(chunk)，Task G3）----
// progress(bytes)：字节级进度回调，单调递增；buildTarStream 范围 [0, 输入文件总字节]，
// extractTarStream 范围 [0, 归档字节数]（不含尾部 MD5 校验行）。可传空回调。
// 失败返回 false 且 error 非空（可传 nullptr 忽略）；失败时正在写入的输出文件会被删除。
bool buildTarStream(const QStringList &inputFiles, const QString &outPath,
                    const std::function<void(quint64)> &progress = {},
                    QString *error = nullptr);
// 按条目流式解包到 outDir（目录结构展开）。条目名含 ".." 成分或前导 '/' → 静默跳过
// （不落盘）；符号链接条目不落盘（对齐 worker 语义）；自动校验三星 .tar.md5 尾部
// MD5 校验行（无校验行则跳过；有校验行且不符 → false + error）。
bool extractTarStream(const QString &tarPath, const QString &outDir,
                      const std::function<void(quint64)> &progress = {},
                      QString *error = nullptr);
// 流式追加三星 .tar.md5 校验行（先写 tar 再追加：增量读算 MD5 后追加 "32hex  firmware.tar.md5\n"），
// 产物与 appendMd5Footer(整读) 逐字节一致。
bool appendMd5FooterStream(const QString &tarPath, QString *error = nullptr);
// 流式校验尾部 MD5 校验行：无校验行 → hasFooter=false 且返回 true；有校验行
// （无论校验是否通过）→ hasFooter=true；有校验行且不符 → false + error。
bool verifyMd5FooterStream(const QString &tarMd5Path, bool *hasFooter = nullptr,
                           QString *error = nullptr);

} // namespace imgtar
