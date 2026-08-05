#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

// 最小 ZIP 读取（PKWARE 规范，覆盖实际使用的 STORE/DEFLATE）。
// Task C7 从 magisk_patcher.cpp / kernelsu_patcher.cpp / apatch_patcher.cpp
// 的私有实现（逐字节同源）提取合并为共享工具（C4 concern 5）。
// 局限（与 Magisk 官方产物一致）：zip64 归档明确拒绝（官方 APK/SU 包均为
// 普通 ZIP，联网验证 v25.2/v30.7 APK、SuperSU v2.46-2.82 刷入包）。
namespace patcher {

// 按 PK 魔数判断是否为 ZIP 归档（不校验 EOCD）。
bool isZip(const QByteArray &data);

// 中央目录条目名列表（供 LKM 包 / ksuinit.zip / AnyKernel3 包按名搜索）。
bool zipEntryNames(const QByteArray &zip, QStringList *out, QString *err);

// 按精确条目名提取（STORE 直取，DEFLATE 用 zlib raw inflate，输出大小以
// 目录字段为准，64MB 上限防恶意头强制分配）。失败返回 false 并写 err。
bool extractZipEntry(const QByteArray &zip, const QString &entryName, QByteArray &out,
                     QString *err);

// 按名称前缀提取首个匹配条目（APatch kpimg 形态：assets/kpimg*）。
bool extractZipEntryByPrefix(const QByteArray &zip, const QString &prefix, QByteArray &out,
                             QString *err);

} // namespace patcher
