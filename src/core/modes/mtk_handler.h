#ifndef MTK_HANDLER_H
#define MTK_HANDLER_H

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QList>
#include <QPair>
#include <QProcess>
#include <QJsonObject>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QTimer>
#include <functional>

#include "core/modes/mtk_brom.h"
#include "core/modes/mtk_preloader_fetch.h"

// MTK DA partition info
struct MtkPartition {
    QString name;
    quint64 offset;
    quint64 length;
};

namespace mtkbrom {

// 回调（runBromFlash 的注入点：UI 落 OutputPanel，测试收集字符串）
using BromLogFn = std::function<void(const QString &message, bool isError)>;
using BromProgressFn = std::function<void(quint64 written, quint64 total)>;

// BROM 直刷请求（D1-T9）：调用方只负责"读 DA 文件 + 给镜像路径"，
// 解析/选条目/引导链/计划/写入全在 runBromFlash 内完成。
struct BromFlashRequest {
    QByteArray daFile;                    // DA 文件字节（调用方读入）
    QString daLabel;                      // 日志用（通常是文件路径）
    QStringList imagePaths;               // 待写镜像（计划层按文件名匹配**设备分区表**）
    PreloaderOptions preloader;           // 显式路径 / 固件目录 / 网络开关(默认关) / 缓存目录
    QList<PreloaderSource> preloaderSources;  // 网络来源清单（loadConfiguredSources）
    PreloaderDownloader downloader;       // 生产用 makeQtPreloaderDownloader()
};

} // namespace mtkbrom

// F1-3 集成点：BROM 直刷（D1-T9 落地；绕开 mtk_bridge JSON-RPC 主路径）。
// 流程：枚举 → libusb 打开 → 握手 → 芯片信息与代际判定（表外/非 LEGACY/IoT/DA v6 → 明确报错）
//   → DA 解析与条目选择 → preloader 两路径 → bromBringUpDa（DA1→0xC0→存储信息→stage2
//   →[EMI]→DA2→read_flash_info）→ **设备实读分区表** → 计划 → 逐分区写 → FINISH(0xD9) 收尾。
// 诚实边界：枚举/打开/握手段（libusb 真机路径）**离线不可验证**——被验证的是它下面两层
//   （bromBringUpDa 逐帧 + 计划层，见 tests/test_mtk_payload.cpp）。失败即停、**不复位**
//   （与 Phase B/C 同口径）。
bool runBromFlash(const mtkbrom::BromFlashRequest &req, const mtkbrom::BromLogFn &log,
                  const mtkbrom::BromProgressFn &progress, QString *error = nullptr);

// 【弃用桩 —— **T10 接线时必须删除**】F1 时代的旧入口（分区以字节列表传入）。
// 保留原因：flash_tool.cpp 的调用点要 T10 才改，删净会同时打断主程序与 test_pipeline 构建。
// 函数体只明确失败，不含任何刷写逻辑（真实现见上方 runBromFlash(BromFlashRequest)）。
bool runBromFlash(const QByteArray &daBinary,
                  const QList<QPair<QString, QByteArray>> &partitions,
                  QString *error = nullptr);

class MtkHandler : public QObject
{
    Q_OBJECT

public:
    explicit MtkHandler(QObject *parent = nullptr);
    ~MtkHandler();

    // Initialize: find bridge binary
    bool initialize();

    // Connection
    bool connect();
    void disconnect();

    // Bootloader unlock/lock
    bool seccfgUnlock();
    bool seccfgLock();

    // Partition operations
    QList<MtkPartition> listPartitions();
    bool readPartition(const QString &partition, const QString &outputPath);
    bool writePartition(const QString &partition, const QString &imagePath);
    bool readAllPartitions(const QString &directory);

    // Reset device
    bool resetDevice();

    bool isConnected() const { return m_connected; }
    QString lastError() const { return m_lastError; }
    QString bridgePath() const { return m_bridgePath; }

signals:
    void outputMessage(const QString &msg, bool isError);
    void progress(int percent);

private:
    // JSON-RPC
    QJsonObject sendRequest(const QString &method, const QJsonObject &params = {},
                            int timeoutMs = 120000);

    // Start/stop bridge process
    bool startBridge();
    void stopBridge();

    // Resource management (like AdbEmbedded)
    bool extractBridge();
    QString getPlatformBinaryName() const;

    QString m_bridgePath;
    QString m_tempBridgePath;
    bool m_initialized;
    bool m_connected;
    QString m_lastError;
    int m_requestId;

    QProcess *m_process;
    QByteArray m_readBuffer;
};

#endif // MTK_HANDLER_H
