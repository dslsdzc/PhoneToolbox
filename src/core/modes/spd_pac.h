#pragma once

// 展锐 `.pac` 内的 FDL 提取（纯解析，计划 Task 1）
//
// 与 spd_storage（协议层/刷写链）分开的理由：本模块只依赖 Qt Core + imgpac 解析器 ——
// 不含 libusb，单测目标不必把 USB 传输链拖进来（spd_storage.cpp 的 runSpdFlash 经
// spd_flash.cpp 引用 libusb 符号）。FDL 名判定 isFdlPartition 也落在这里：判定与提取
// 是同一件事，散在两处容易出现第二套判据。
//
// 诚实边界：
//   • 本模块是纯解析 —— 提取出的字节与包内切片逐字节一致（离线可完整验证）；
//     提取出的 FDL 在真机上是否可用，属既有 SPD 刷写链的未验证范围（真机待持机人）。
//   • 判名口径沿用既有纯函数 isFdlPartition（fdl / fdl1 / fdl2，大小写不敏感）。
//     它**不区分代次**：本模块按名字含 fdl1/fdl2 细分，裸 "fdl" 不猜代次（判成两边都缺，
//     文案列出包内条目）—— 猜错会把 FDL2 的字节当 FDL1 上传，风险高于让用户手选。
//   • 包内条目用了其它命名（既不是 fdl1 也不是 fdl2）时同样提取失败：按 spec §3.2，
//     失败即回退到手选 FDL（现有行为不变）。

#include <QByteArray>
#include <QString>

namespace spd {

// 从 pac 提取出的一份 FDL：name = 分区 ID（如 "FDL1"），fileName = 容器内文件名
//（如 "fdl1.bin"），data = 包内数据切片。
struct ExtractedFdl {
    QString name;
    QString fileName;
    QByteArray data;
};

// 从 pac 内提取 FDL1 与 FDL2（按 isFdlPartition 判名 + 名字含 fdl1/fdl2 细分代次）。
// 两个都找到（且都非 0 字节）→ true 并填好两个出参；否则 false + 中文 *error：
// 缺哪个 / 包内有哪些条目（可行动：用户据此判断是不是选错了包，或回退手选）。
// 任何失败路径都不留半份数据（出参在入口清空，仅成功时写入）。
bool extractFdlsFromPac(const QString &pacPath, ExtractedFdl &fdl1, ExtractedFdl &fdl2,
                        QString *error);

// 把提取出的两份 FDL 落盘到目录 dir，供 SPD 通道按路径读（runSpdFlash 的 fdl1Path/fdl2Path）。
// 文件名带条目名：优先容器内文件名（用户在包里看得到），为空回退分区 ID；两份重名时加
// FDL1-/FDL2- 前缀区分。两个都写成功才 true 并填好 fdl1Path/fdl2Path；
// 任一失败 → false + 可行动 *error，且**不留半份产物**（本次已写出的文件删除）——
// 只有 FDL1 落盘时若继续，通道会拿不到 FDL2 或把 FDL1 的字节发去 FDL2。
// 出参在入口清空。空载荷（0 字节）拒写：空 FDL 发出去也起不了作用，失败点会被推到设备侧。
bool writeExtractedFdls(const ExtractedFdl &fdl1, const ExtractedFdl &fdl2, const QString &dir,
                        QString &fdl1Path, QString &fdl2Path, QString *error);

// 纯函数（单测）：fdl/fdl1/fdl2 分区名判定（大小写不敏感）。两处用途：
//   ① 刷写链跳过 FDL 条目（FDL 单独上传，不参与普通刷写）；
//   ② 本模块据此判定包内条目是不是 FDL（不另立第二套判据）。
bool isFdlPartition(const QString &partitionName);

} // namespace spd
