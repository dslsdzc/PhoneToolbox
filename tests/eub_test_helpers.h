// tests/eub_test_helpers.h
//
// EUB 真样本（reference/eub-samples/，gitignored）的**共享 gating**：Task 1b 在 test_eub_payload
// 与 test_eub_loadout 里逐字复制过同一份（含宏、路径、存在性判据），抽到这里只此一份 —— 改 gating
// 口径（文案 / SKIP↔FAIL 切换）不会再漏掉另一个文件。先例：tests/mtk_test_helpers.h。
// **样本内容绝不进仓库**：CMake 只把目录**路径**编进这两个目标（CMakeLists.txt 的
// EUB_SAMPLES_DIR 分支）；本头不读样本，只提供路径与存在性判据。
#pragma once

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QString>

// 真样本目录（CMake 传入；未传入时为空串 → 存在性判据恒 false → 真样本槽一律 SKIP/FAIL）
#ifndef EUB_SAMPLES_DIR
#define EUB_SAMPLES_DIR ""
#endif
// 1 = 真样本缺失时 FAIL 而非 SKIP（CMake 侧 EUB_SAMPLES_REQUIRED=ON 时传入；默认 0 保持离线友好）。
#ifndef EUB_SAMPLES_REQUIRED
#define EUB_SAMPLES_REQUIRED 0
#endif

// 真样本缺失时的统一处置。QSKIP **不改退出码**（ctest 报 pass），故验证跑一律用 REQUIRED=ON
// 把"没跑"变成"红"—— 否则这些槽会以绿灯的名义静默空转。
// 包成宏的两个理由（**不是**"怕漏花括号"：Qt6 的 QFAIL/QSKIP 各自展开为一次函数调用，
// 不再像 Qt5 那样自带 return，两种写法都安全）：
//   ① 两种模式共用一份文案与一处切法 —— 改口径不必在两个用例文件里各改一遍；
//   ② 切换 REQUIRED 只动这一个宏，调用点（`EUB_SKIP_OR_FAIL(file);`）两种模式下都是单条语句。
#if EUB_SAMPLES_REQUIRED
#define EUB_SKIP_OR_FAIL(what)                                                                     \
    do {                                                                                           \
        QFAIL(qPrintable(QStringLiteral("真样本缺失，但本次构建要求真样本（EUB_SAMPLES_REQUIRED=ON）：") \
                         + QString(what)));                                                        \
    } while (0)
#else
#define EUB_SKIP_OR_FAIL(what)                                                                     \
    do {                                                                                           \
        QSKIP(qPrintable(QStringLiteral("真样本缺失（reference/ 为 gitignored）：") + QString(what))); \
    } while (0)
#endif

namespace eubtest {

// 样本路径（样本只读，绝不写回）
inline QString samplePath(const QString &fileName)
{
    return QString::fromLatin1(EUB_SAMPLES_DIR) + QLatin1Char('/') + fileName;
}

inline bool sampleAvailable(const QString &fileName)
{
    return QFileInfo::exists(samplePath(fileName));
}

// 打开失败 → 空数组（调用方自行判非空；仓内约定：失败不返回半份数据）
inline QByteArray readSample(const QString &fileName)
{
    QFile f(samplePath(fileName));
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace eubtest
