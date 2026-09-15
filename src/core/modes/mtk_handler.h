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

// BromFlashRequest / BromLogFn / BromProgressFn 与刷写主体 bromFlashOnSession 都在
// mtk_payload.h（T9 审查 I1：搬到那里才能被离线用例覆盖）。
#include "core/modes/mtk_payload.h"

// MTK DA partition info
struct MtkPartition {
    QString name;
    quint64 offset;
    quint64 length;
};

// F1-3 集成点：BROM 直刷（D1-T9 落地；绕开 mtk_bridge JSON-RPC 主路径）。
// 本函数只剩**真机段**：枚举 → libusb 打开 → connect（握手）→ 交给 bromFlashOnSession。
// 后者（代际判定 → DA 解析/选择 → preloader → 引导链 → 设备分区表 → 计划 → 逐分区写 →
// FINISH）可离线逐帧测，见 tests/test_mtk_payload.cpp。
// 诚实边界：枚举/打开/握手段**离线不可验证**。失败即停、**不复位**（与 Phase B/C 同口径）。
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
