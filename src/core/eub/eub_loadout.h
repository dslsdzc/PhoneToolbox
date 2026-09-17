// src/core/eub/eub_loadout.h
//
// 每 SoC 的 sboot.bin 切段表 + 查表/切段/摘要工具。**表的唯一来源是设计 spec §5.4**
// （`docs/superpowers/specs/2026-09-17-exynos-eub-design.md`），每条 sourceNote 指到
// reference/ 里的 file:line；改动任何数字前先改 spec（tests/test_eub_loadout.cpp 逐值钉死）。
//
// 证据等级（EubLoadout::evidence，取值固定这四个字符串）：
//   "双源一致"        —— 两个独立实现对同一 SoC 的偏移/长度完全一致（9610）
//   "单源+实战报告"   —— 只有一份脚本，但第三方用它做过实测恢复（8890）
//   "双源分歧（采信 hubble 连续切法）" —— 两源不一致，本仓采信 hubble 的连续切法（7580）
//   "单源"            —— 单一来源，未对拍（8895/7885/9810/9820/9830）
#pragma once
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>

#include "eub_protocol.h"

namespace eub {

struct EubSegment {
    QString name;       // 段名（工具间命名不一，以偏移为准；facts §C2）
    quint64 offset = 0; // sboot.bin 内绝对偏移
    quint64 length = 0;
};

struct EubLoadout {
    QString soc;               // "Exynos9610"（与设备 iProduct 对齐）
    QStringList models;        // 参照里出现过的机型（仅展示）
    QString evidence;          // 上面四档之一
    QString sourceNote;        // 出处（reference/<repo>/<file>:<line>）
    QByteArray sbootSha1;      // 参照所用 sboot 修订的 sha1（hex 文本；无记录则为空）
    EubFrameStyle style;       // 帧头/尾风格（facts §B4/§B5）
    QList<EubSegment> segments;// 按序发送；重发段照列（facts §C5）
    // 参照流程要求"段之后另发"的 BL 包内文件（9830：ldfw.img/tzsw.img，facts §C7；发送逻辑见
    // hubble.py:329-341）。**证据等级单源**：本仓无真机；真样本已就位（facts §H），"9830 需要这两个文件"是参照流程
    // 的要求，不是本仓的实测结论。
    // 本字段**只记录名单**：载荷由调用方按同一份名单从 BL 包取出（eub::loadNamedEntriesFromTar，
    // 见 eub_payload.h）后传给 EubSession::run 的 extras —— 数量不符时 run 在切段前 fail-closed
    //（一个字节都不写，见 eub_session.cpp 的入口校验）。
    QStringList extraFiles;
    bool responseSupport = false;  // 设备会回显（facts §C7/§C8）
};

// 全部 8 张表（顺序 = spec §5.4 的行序）
QList<EubLoadout> allLoadouts();

// 按 SoC 名查表：大小写不敏感（"exynos9610" == "Exynos9610"）；未知 → false + 列出支持的 SoC
// （失败时 out 被清空 —— 调用方即使忽略返回值也不会拿到陈旧表项）
bool eubLoadoutFor(const QString &socName, EubLoadout &out, QString *error);

// 切段：任一 (offset + length) 越界 → false 且 out **保持为空**（绝不截断，spec §7）
bool splitSboot(const QByteArray &sboot, const EubLoadout &lo,
                QList<QPair<QString, QByteArray>> &out, QString *error);

// 从镜像内容反推 SoC 名（facts §A4：hubble.py:195 的 `EXYNOS[0-9]+`；结果形如 "Exynos9610"）。
// 设备 iProduct 是 "SEC S5PC210 Test B/D" 之类的极老 SoC 才需要它。找不到 → 空串。
QString detectSocFromImage(const QByteArray &sboot);

// SHA-1 十六进制小写（用于与表项的 sbootSha1 对照展示，facts §C9）
QString sha1Hex(const QByteArray &data);

} // namespace eub
