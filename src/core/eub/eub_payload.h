// src/core/eub/eub_payload.h
//
// 载荷来源解析（设计 spec §D8）：把用户指定的**一个文件**变成 sboot.bin 字节。
//   ① 裸镜像（非 tar 的任何文件）
//   ② LZ4 frame 压缩的镜像（.lz4；三星 BL 包内即此格式 —— reference/hubble/hubble.py:174 用
//      lz4.frame.decompress，本仓 imgcomp::lz4Decompress 同为 LZ4 **frame** 格式，
//      见 src/image_engine/compression/lz4_wrapper.cpp:21-，用 LZ4F_* API）
//   ③ BL_*.tar.md5：用 imgtar::indexTarStream 找 sboot.bin / sboot.bin.lz4 条目（流式，整包不进内存），
//      读出条目字节后若是 LZ4 再解压
// **不做**：从 AP 包找 boot.img（spec §D8 只认 BL 是承载物）、分发任何三星签名二进制（facts §F8）。
//
// ⚠️ 真机路径未验证：本机没有任何 Exynos 设备、也没有真样本（载荷由用户自备，facts §F1/§F8）——
// 本层只到"按 spec §D8 与 facts §C1 解析用户文件"这一层证据，不对设备行为做任何断言。
#pragma once
#include <QByteArray>
#include <QString>

namespace eub {

struct SbootSource {
    QString description;        // 展示用，例如 "BL_G930F.tar.md5 内的 sboot.bin.lz4（已解压）"
    bool    wasCompressed = false;
};

// 成功：out 非空。失败：false + 中文 *error（含"包里有什么"或"该文件不是有效的 LZ4/镜像"），
// 且 out 被清空（仓内约定，同 eub_loadout.h：调用方忽略返回值也拿不到陈旧字节）。
// source 可传 nullptr（不关心来源描述时）。
bool loadSbootBytes(const QString &path, QByteArray &out, SbootSource *source, QString *error);

// LZ4 frame 魔数 04 22 4D 18（纯判据，测试可直接调）
bool looksLikeLz4Frame(const QByteArray &data);

} // namespace eub
