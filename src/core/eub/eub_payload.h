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
#include <QList>
#include <QString>
#include <QStringList>

namespace eub {

struct SbootSource {
    QString description;        // 展示用，例如 "BL_G930F.tar.md5 内的 sboot.bin.lz4（已解压）"
    bool    wasCompressed = false;
};

// 成功：out 非空。失败：false + 中文 *error（含"包里有什么"或"该文件不是有效的 LZ4/镜像"），
// 且 out 被清空（仓内约定，同 eub_loadout.h：调用方忽略返回值也拿不到陈旧字节）。
// source 可传 nullptr（不关心来源描述时）。
bool loadSbootBytes(const QString &path, QByteArray &out, SbootSource *source, QString *error);

// 按 baseNames 顺序从**同一个 tar**内取条目：每名先找**同名**条目、没有才找同名 + ".lz4"；
// basename（最后一段 '/' 之后）大小写不敏感；取出后按**内容**判据解压 —— 与 loadSbootBytes 的
// 条目选择/解压规则逐条同款（同一份实现，见 .cpp）。
//
// 用途：Exynos9830 的 extraFiles（ldfw.img / tzsw.img）—— 参照流程要求"段之后另发"的文件就取自
// 用户给的**同一个 BL 包**：hubble.py:152-185 把 BL tar 的全部条目解出、逐个尝试 lz4.frame.decompress，
// 随后 :329-341 按 ExynosData/Exynos9830.json:3 的 files_to_send 逐个发送。
// 证据等级**单源**（hubble，只有 9830 有该字段）：本仓无真机、无真样本，本函数只到"按给定名字把
// 条目取出来"这一层证据，不对设备行为做任何断言。
//
// 语义：
//   * 成功 → out 按 baseNames 的**请求顺序**填好（每个元素非空），error 清空；
//   * 失败 → false + 中文 *error（**列出缺失的名字**与包内条目摘要，可行动），out 被清空；
//   * baseNames 为空 = 没有要求任何条目 → 直接成功（out 清空）且**不读文件**。
bool loadNamedEntriesFromTar(const QString &tarPath, const QStringList &baseNames,
                             QList<QByteArray> &out, QString *error);

// LZ4 frame 魔数 04 22 4D 18（纯判据，测试可直接调）
bool looksLikeLz4Frame(const QByteArray &data);

} // namespace eub
