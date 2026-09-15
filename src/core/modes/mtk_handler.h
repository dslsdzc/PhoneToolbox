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

// MTK DA partition info
struct MtkPartition {
    QString name;
    quint64 offset;
    quint64 length;
};

// F1-3 集成点：BROM 直刷路由（骨架，绕开 mtk_bridge JSON-RPC 主路径）。
// 枚举 → libusb 打开 → BromSession → （**未接线**：DA1/DA2 两阶段属 Task 9，见 .cpp 说明）。
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
