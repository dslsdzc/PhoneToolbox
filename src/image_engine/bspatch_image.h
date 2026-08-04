#pragma once
#include <QByteArray>

namespace imgbspatch {

// bsdiff 应用（BSDIFF40 格式）: "BSDIFF40"(8) + ctrl_len/diff_len/new_len(各 8B LE) +
// bzip2(ctrl) + bzip2(diff) + bzip2(extra)。失败（格式损坏/越界/解压错误）返回空并置
// *ok=false；成功时 newLen==0 合法返回空（ok=true）。ok 可传 nullptr。
QByteArray applyBsdiff(const QByteArray &oldData, const QByteArray &patch, bool *ok = nullptr);

// puffpatch: "PUFFDIFF" 魔数 + gzip 流。Task 15 仅骨架 + 魔数校验，
// 完整 puffin 解压（gzip 段交替解压）留 Task 16 实现；当前一律返回空。
QByteArray applyPuffdiff(const QByteArray &oldData, const QByteArray &patch);

} // namespace imgbspatch
