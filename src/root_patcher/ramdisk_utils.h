#pragma once
#include <QByteArray>
#include <QString>

namespace patcher {

// 检测 ramdisk 压缩格式。成功返回 true 并设置 format（"gzip"/"lz4"/"lzma"/"xz"）；
// 无法识别返回 false（format 不变，内容视为未压缩原始数据）。
// 注意：lz4 legacy 与 lz4 frame 均报告为 "lz4"，解压时按魔数区分。
// 格式依据（联网验证）：lz4 legacy 魔数 0x184C2102（liblz4 legacy/lz4io.c、
// magiskboot）、lz4 frame 魔数 0x184D2204/0x184C2103（LZ4 frame 规范、magiskboot）、
// xz 魔数 FD 37 7A 58 5A 00（XZ 规范）、lzma-alone 头（liblzma alone_decoder.c、
// magiskboot v25.2 check_fmt：属性 0x5D 且大小字段 MSB ∈ {0xFF, 0x00}）。
bool detectRamdiskFormat(const QByteArray &head, QString &format);

// 按检测结果解压：gzip/lz4(legacy 或 frame)/lzma(alone)/xz。
// 未压缩或无法识别时原样返回 true；失败返回 false 并写入 error（非空）。
bool decompressRamdisk(const QByteArray &raw, QByteArray &out, QString *error);

// 按 format 重压："gzip"/"lz4"(legacy 块流)/"lzma"(alone 容器)/"xz"；
// "raw" 或未知格式原样返回 data。
QByteArray compressRamdisk(const QByteArray &data, const QString &format);

} // namespace patcher
