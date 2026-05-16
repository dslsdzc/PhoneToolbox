#ifndef MTK_HANDLER_H
#define MTK_HANDLER_H

#include <QObject>
#include <QString>
#include <QStringList>
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
