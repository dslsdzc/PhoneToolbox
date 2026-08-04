#pragma once
#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>

namespace imghw {

struct AppFile {
    QString name;
    quint32 type = 0;       // 0x01=system 0x02=boot 0x03=recovery 0x04=userdata 0x05=signature 0x06=crc ...
    quint32 rawSize = 0;
    quint32 compSize = 0;
    quint32 offset = 0;     // 数据段内偏移
};

// 华为 update.app: 512B 头（魔数 0x55 0xAA）+ 64B 文件表条目 * N + 数据段
// 文件名探测假设: 条目文件名 32B —— 检查前两字符是否为 UTF-16LE（字节 1、3 为 0）,
// 是则按 UTF-16LE 解析（\0 截断），否则按 ASCII 解析（\0 截断）。
// 该假设基于实测样本，不同厂商变体可能不同（本任务按 ASCII 前两字符探测）。
bool isUpdateApp(const QByteArray &header);   // 0x55 0xAA
bool parseUpdateApp(const QByteArray &app, QList<AppFile> &out, QString *error);
QByteArray extractFile(const QByteArray &app, const AppFile &f, QString *error);
QByteArray buildUpdateApp(const QList<AppFile> &files);

// ---- 签名材料配置（spec 2026-08-04 补充; 不内置签名材料）----
// sigHeaderType: update.bin 签名头处理类型:
//   "08" = 签名长度 4B 直接从签名头起始读; "06" = 先跳 16B 再读 4B 签名长度。
struct SignConfig {
    QString sigHeaderType;    // 签名头处理类型（"08"/"06" 等）
    quint32 sigLenOffset = 0; // 签名长度字段相对偏移
    bool haveKey = false;     // 是否具备签名密钥
    QString keyPath;          // 密钥文件路径（用户导入，不内置）
};
// 从用户导入的 JSON 配置文件加载签名参数/密钥路径（失败返回 false + error）
bool loadSignConfig(const QString &jsonPath, SignConfig &out, QString *error);
// 从外部仓库下载签名配置（骨架: 需 Qt6::Network 接线，本任务实现 URL 校验 +
// 用户数据目录缓存优先; 无缓存且未接线时返回 false —— 不内置签名材料）
bool fetchSignConfig(const QString &repoUrl, const QString &version, SignConfig &out, QString *error);

// 重打包: 头 + 文件表 + 顺序数据段（数据段按文件表顺序紧凑排列，保持偏移对齐）。
// 每条数据的长度必须与条目 compSize 一致，否则整体返回空。
// sign == nullptr: 产物标记"未签名"（文件表无 type 0x05 条目）;
// 提供时: 校验配置并追加 type 0x05 签名占位条目（实际签名应用留扩展）。
QByteArray buildUpdateAppWithData(const QList<QPair<AppFile, QByteArray>> &files,
                                  const SignConfig *sign = nullptr);

// ---- update.bin L2 型解析 ----
// 布局: 178B 文件头 + 2B 分区信息总长度 + N×87B（分区长度 8B @48; GUID 32B 尾）+ "update/info.bin" 16B
struct BinPartition {
    QString name;       // L2 型无 ASCII 分区名, 固定 "partN" 占位（待真实样本核对）
    quint64 size = 0;   // 分区长度
    QString guid;       // 32B GUID 的十六进制小写表示
};
bool parseUpdateBin(const QByteArray &bin, QList<BinPartition> &out, QString *error);

} // namespace imghw
