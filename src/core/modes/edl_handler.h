#ifndef EDL_HANDLER_H
#define EDL_HANDLER_H

#include <QObject>
#include <QString>
#include <QStringList>

#include "core/edl/edl_libusb_transport.h"   // edl::LibusbEdlTransport（唯一的真机传输实现）
#include "core/edl/edl_session.h"            // edl::EdlSession（Firehose 作业编排 + 数据面）

struct EDLPartition {
    QString name;
    QString filename;
    quint64 startSector;
    quint64 numSectors;
    quint64 sectorSize;
    bool isReadOnly;
};

// EDL（9008）通道的**薄封装**（Phase B Task 7：由旧单体实现退化而来）。
//
// 公开 API 与两个信号与旧实现**逐字一致**（FlashTool/FlashPanel 的调用点不得改动），内部一律改走
// `edl_libusb_transport`（libusb）+ `edl_session`（Firehose 作业编排 + 数据面）：
//   connectSahara   → saharaLoadProgrammer（载 programmer）→ close → waitReenumerate（等重枚举）
//   firehoseConnect → EdlSession::beginFirehose（configure 协商，**发完等响应**）
//   listPartitions  → getstorageinfo（逐 LUN；Firehose **没有**枚举分区名的命令，见下）
//   readPartition   → EdlSession::readBack（正确的 <read> 属性集 + 越界早拒）
//   writePartition  → EdlSession::writePlan（<program> 含 physical_partition_number + 真实数据面）
//   writeRaw        → 同上（单条目计划）
//
// 顺带修掉的 5 处既有缺陷（协议速查 §6，逐条）：
//   1. 帧格式：不再有 4 字节长度前缀，**裸 XML**（两参照一致）；
//   2. ACK 判定改严格（`parseFirehoseResponse` 的 `value=="ACK"`）——旧判定恒假，**旧写入路径从未成功过**；
//   3. configure 改为发完等响应（旧实现只发不读）；
//   4. `<program>` 补 `physical_partition_number`，并真正推送镜像数据（旧实现只发 XML、无数据面）；
//   5. `<read>` 属性名改 `num_partition_sectors`、补 `physical_partition_number`、去掉 `filename`。
//
// ⚠️ 两条**行为约束**（不是缺陷，是 Firehose 协议本身的边界，调用方须知）：
//   * `listPartitions()` 返回的是**逐 LUN 的卷**（`name = "lun<N>"`），不是分区名 —— Firehose 没有
//     "列出分区"的命令，能问到的只有存储几何。分区名请以刷写计划 / GPT 为准（写路径的 LUN 从
//     "lun<N>" 名称反解）。这也是既有 FRP 清除路径（按名字找 "frp" 分区）在本通道下的固有限制。
//   * 写路径要求设备**已在 Firehose**（`connectSahara()` 结束即是）；本类不会替你重做 Sahara 引导。
class EDLHandler : public QObject
{
    Q_OBJECT

public:
    explicit EDLHandler(QObject *parent = nullptr);
    ~EDLHandler();

    // Sahara: 载入 programmer ELF，并等设备重枚举为 Firehose（成功 ⇒ isConnected() == true）
    bool connectSahara(const QString &programmerPath);
    void disconnect();

    // Firehose commands
    bool firehoseConnect();
    QList<EDLPartition> listPartitions();
    bool readPartition(const EDLPartition &part, const QString &outputPath);
    bool writePartition(const EDLPartition &part, const QString &imagePath);
    // ⚠️ 语义修正（Task 7 Q3 裁定）：`filename` 是**镜像文件路径**（旧实现只把它当包内裸文件名塞进
    // `<program filename=...>`，且没有任何数据面）。文件不存在 → 中文错误；`numSectors == 0` →
    // 按镜像文件事实推导（sparse 按**展开后**大小）；目标 LUN 固定 0（旧签名无 LUN 参数）。
    bool writeRaw(const QString &filename, quint64 startSector,
                  quint64 numSectors, quint64 sectorSize);

    bool isConnected() const { return m_connected; }
    QString lastError() const { return m_lastError; }

signals:
    void outputMessage(const QString &msg, bool isError);
    void progress(int percent);

private:
    bool ensureFirehoseConfigured();     // configure 只在需要时做一次（协商结果留在会话内）
    // getstorageinfo 单次查询；`reportedLunCount` 非空时回填设备上报的 LUN 数（0 = 没报，见 firehose.h）
    bool queryStorageInfo(quint32 lun, edl::StorageInfo &out, quint32 *reportedLunCount, QString *error);
    // writePartition / writeRaw 的单条目写实现；`numSectors == 0` → 按镜像事实推导，
    // `upperBoundSectors > 0` → 目标窗口上限（超出即拒，0 = 不限）
    bool writeImageEntry(const QString &imageFile, const QString &label, quint32 lun,
                         quint64 startSector, quint64 numSectors, quint32 sectorSize,
                         quint64 upperBoundSectors);
    bool fail(const QString &msg);       // m_lastError = msg + outputMessage(msg, true)

    // 声明顺序即初始化顺序：m_session 绑定 m_transport，故 m_transport 必须在前
    edl::LibusbEdlTransport m_transport;
    edl::EdlSession        m_session;
    bool    m_connected  = false;
    bool    m_configured = false;        // beginFirehose 是否已成功（写/读路径的前置）
    QString m_lastError;
    QString m_lastProgressKey;           // 进度去重键（stage + detail）：write 阶段每块都回调
};

#endif // EDL_HANDLER_H
