#pragma once
#include <QByteArray>

namespace imgbspatch {

// bsdiff 应用（BSDIFF40 格式）: "BSDIFF40"(8) + ctrl_len/diff_len/new_len(各 8B LE) +
// bzip2(ctrl) + bzip2(diff) + bzip2(extra)。失败返回空。
QByteArray applyBsdiff(const QByteArray &oldData, const QByteArray &patch);

// puffpatch: "PUFFDIFF" 魔数 + gzip 流。Task 15 仅骨架 + 魔数校验，
// 完整 puffin 解压（gzip 段交替解压）留 Task 16 实现；当前一律返回空。
QByteArray applyPuffdiff(const QByteArray &oldData, const QByteArray &patch);

} // namespace imgbspatch
