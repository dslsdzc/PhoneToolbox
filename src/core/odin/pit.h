// src/core/odin/pit.h
//
// PIT（Partition Information Table，三星 Odin 的分区表）解析模型。
// 布局依据（逐条带出处）：
//   * reference/heimdall/libpit/source/libpit.h:43-303 —— 28 字节头 / 132 字节条目 / 小端
//   * reference/thor/TheAirBlow.Thor.Library/PIT/PitData.cs:19-54 —— 头部字段语义（COM_TAR2 / 平台名）
//   * reference/samloader-rs/pit/src/lib.rs:27-33,79-93,155-160 —— UFS 枚举（deviceType=8）与扇区换算
// 真样本实证：docs/superpowers/specs/samsung-odin-facts.md §3（9+1 个真 PIT）
#pragma once
#include <QByteArray>
#include <QList>
#include <QString>

namespace odin {

// PIT 条目（132 字节）。**三方解读不一致的字段一律只透传原值、不做语义解释**。
struct PitEntry {
    quint32 binaryType = 0;         // +0   0=AP / 1=CP（libpit.h:55-80；Heimdall FlashAction.cpp:253 按它选 phone/modem）
    quint32 deviceType = 0;         // +4   0=OneNand/1=File(FAT)/2=MMC/3=All（libpit.h:66-72）；**8=UFS** 仅 samloader-rs
                                    //      有（pit/src/lib.rs:79-93）→ 按数值透传，**不裁枚举**
    quint32 identifier = 0;         // +8   分区 ID（结束序列包要用；Heimdall FlashAction.cpp:274）
    quint32 attributes = 0;         // +12  **不做位域解释**：Heimdall(1=Write/2=STL)/Thor 枚举下标/libmagic(attr&2) 互不一致
    quint32 updateAttributes = 0;   // +16  同上：Heimdall 说 fota=1/secure=2，真数据却出现 5 → 只透传
    quint32 blockSizeOrOffset = 0;  // +20  libpit.h:162："不同设备的不同 Loke 版本解释不同" → 不解释
    quint32 blockCount = 0;         // +24  分区扇区数（**0 = 未声明**，真样本 J1POP3G 的 USERDATA 即如此）
    quint32 fileOffset = 0;         // +28  Obsolete（libpit.h:93）
    quint32 fileSize = 0;           // +32  Obsolete（libpit.h:94）
    QString partitionName;          // +36  char[32]
    QString flashFilename;          // +68  char[32] "USB flash filename"；真数据含字面量 "-"（= 无镜像）
    QString fotaFilename;           // +100 char[32] FOTA；真数据 "remained" / "remained\r\n"

    // 分区字节数：扇区单位按 deviceType 换算（samloader-rs pit/src/lib.rs:155-160：UFS→4096B、其余→512B）。
    // blockCount == 0 时返回 0（分区大小未声明 —— 真样本存在这种条目）。
    quint64 partitionBytes() const;
    // libpit.h:107-110：有分区名才算"可刷写条目"。
    bool isFlashable() const { return !partitionName.isEmpty(); }
    // 是否声明了"要写的镜像文件"：flashFilename 非空且不是占位符 "-"。
    bool hasImageName() const;
};

struct PitTable {
    QByteArray comTar2;        // 头 8..15：ASCII "COM_TAR2"（Thor PitData.cs:24-26）
    QByteArray cpuBlId;        // 头 16..23：ASCII 平台名，NUL 填充（真数据 "MSM8974"/"SPRD8735"/"LSI3475"…）
    quint16 luCount = 0;       // 头 24..25 —— **不当 padding**（Heimdall 读成 unknown7 的半个 u16）。
                               // 真数据 J1POP3G=0 / SM-Q7MQ(SM8750)=4 ⇒ **只记不拒**，不得断言恒非 0。
    quint16 reserved = 0;      // 头 26..27
    quint64 trailingBytes = 0; // 28+count*132 之后的尾部长度（签名块，长度不定；**原样收不解释**，只记长度）
    QList<PitEntry> entries;

    // 按分区名查（**大小写不敏感**，返回首个命中）；会话对账用（设备 PIT 优先）。
    const PitEntry *findByName(const QString &name) const;
    int indexOfName(const QString &name) const;    // -1 = 无
};

// 清洗 PIT 的 char[32] 字段：截首个 NUL + 去 CR/LF + 去首尾空白。
// 真数据含字面 CR/LF（j1xlte 的 fotaFilename = b'remained\r\n'）与 NUL 填充。
QString cleanPitString(const QByteArray &field);

// 纯函数：从**内存字节**解析 PIT（真 PIT 也可能来自包内 .pit 条目或设备 dump —— 后两者没有独立文件，
// 故核心是字节版，读文件只是外壳）。成功时清空并填充 out。
// 失败（太短 / 魔数不符 / 条目数 0 / 长度不足声明条目数）→ false + 中文 *error。
bool parsePit(const QByteArray &data, PitTable &out, QString *error);
// 读文件 + parsePit（用户显式指定 PIT 文件时用）。
bool parsePitFile(const QString &path, PitTable &out, QString *error);

} // namespace odin
