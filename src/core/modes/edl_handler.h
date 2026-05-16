#ifndef EDL_HANDLER_H
#define EDL_HANDLER_H

#include <QObject>
#include <QString>
#include <QStringList>

struct libusb_context;
struct libusb_device_handle;

struct EDLPartition {
    QString name;
    QString filename;
    quint64 startSector;
    quint64 numSectors;
    quint64 sectorSize;
    bool isReadOnly;
};

class EDLHandler : public QObject
{
    Q_OBJECT

public:
    explicit EDLHandler(QObject *parent = nullptr);
    ~EDLHandler();

    // Sahara: load programmer ELF, switch to Firehose
    bool connectSahara(const QString &programmerPath);
    void disconnect();

    // Firehose commands
    bool firehoseConnect();
    QList<EDLPartition> listPartitions();
    bool readPartition(const EDLPartition &part, const QString &outputPath);
    bool writePartition(const EDLPartition &part, const QString &imagePath);
    bool writeRaw(const QString &filename, quint64 startSector,
                  quint64 numSectors, quint64 sectorSize);

    bool isConnected() const { return m_connected; }
    QString lastError() const { return m_lastError; }

signals:
    void outputMessage(const QString &msg, bool isError);
    void progress(int percent);

private:
    // Sahara protocol commands (from sahara_defs.py/cmd_t)
    enum SaharaCmd : quint32 {
        SAHARA_HELLO_REQ      = 0x01,
        SAHARA_HELLO_RSP      = 0x02,
        SAHARA_READ_DATA      = 0x03,
        SAHARA_END_TRANSFER   = 0x04,
        SAHARA_DONE_REQ       = 0x05,
        SAHARA_DONE_RSP       = 0x06,
        SAHARA_RESET_REQ      = 0x07,
        SAHARA_RESET_RSP      = 0x08,
        SAHARA_MEMORY_DEBUG   = 0x09,
        SAHARA_MEMORY_READ    = 0x0A,
        SAHARA_CMD_READY      = 0x0B,
        SAHARA_SWITCH_MODE    = 0x0C,
        SAHARA_EXECUTE_REQ    = 0x0D,
        SAHARA_EXECUTE_RSP    = 0x0E,
        SAHARA_EXECUTE_DATA   = 0x0F,
        SAHARA_64BIT_MEMORY_DEBUG = 0x10,
        SAHARA_64BIT_MEMORY_READ = 0x11,
        SAHARA_64BIT_MEMORY_READ_DATA = 0x12,
    };
    enum SaharaMode : quint32 {
        SAHARA_MODE_IMAGE_TX_PENDING  = 0x00,
        SAHARA_MODE_IMAGE_TX_COMPLETE = 0x01,
        SAHARA_MODE_MEMORY_DEBUG      = 0x02,
        SAHARA_MODE_COMMAND           = 0x03,
    };
    enum SaharaStatus : quint32 {
        SAHARA_STATUS_SUCCESS           = 0x00,
        SAHARA_NAK_INVALID_CMD          = 0x01,
        SAHARA_NAK_PROTOCOL_MISMATCH    = 0x02,
        SAHARA_NAK_INVALID_TARGET_PROTOCOL = 0x03,
        SAHARA_NAK_INVALID_HOST_PROTOCOL   = 0x04,
        SAHARA_NAK_INVALID_PACKET_SIZE     = 0x05,
        SAHARA_NAK_UNEXPECTED_IMAGE_ID     = 0x06,
        SAHARA_NAK_INVALID_HEADER_SIZE     = 0x07,
        SAHARA_NAK_INVALID_DATA_SIZE       = 0x08,
    };

    // HELLO_REQ / HELLO_RSP payload: 12 x uint32_le (48 bytes)
    struct SaharaHello {
        quint32 version;
        quint32 versionSupported;
        quint32 cmdPacketLength;
        quint32 mode;
        quint32 reserved[6];
    };

    // READ_DATA payload (from device): 5 x uint32_le (20 bytes)
    struct SaharaReadData {
        quint32 imageId;
        quint32 dataOffset;
        quint32 dataLen;
    };

    // END_TRANSFER / DONE_RSP payload: 3 x uint32_le (12 bytes)
    struct SaharaEndTransfer {
        quint32 imageId;
        quint32 imageTxStatus;
    };

    bool sendSaharaCmd(SaharaCmd cmd, const QByteArray &payload = {});
    bool recvSaharaPacket(SaharaCmd &outCmd, QByteArray &outPayload, int timeoutMs = 10000);
    bool recvHelloReq(SaharaHello &hello, int timeoutMs = 10000);
    bool sendHelloResp(quint32 mode, quint32 version = 2);
    bool recvReadData(SaharaReadData &rd, int timeoutMs = 10000);
    bool recvEndTransfer(SaharaEndTransfer &et, int timeoutMs = 10000);
    bool sendDoneReq();
    bool recvDoneResp(SaharaEndTransfer &et, int timeoutMs = 10000);
    bool sendSwitchMode(SaharaMode mode);
    bool serveProgrammer(const QString &programmerPath);

    // Firehose: XML over USB
    bool sendFirehoseXml(const QString &xml, int timeoutMs = 30000);
    QString recvFirehoseResponse(int timeoutMs = 30000);
    bool waitFirehoseDone(int timeoutMs = 60000);

    // USB helpers (libusb)
    bool openUSB();
    void closeUSB();
    bool claimInterface();

    libusb_context *m_ctx;
    libusb_device_handle *m_devHandle;
    int m_outEp;
    int m_inEp;

    bool m_connected;
    bool m_firehoseMode;
    QString m_lastError;

    // Found device info
    uint16_t m_vid;
    uint16_t m_pid;
};

#endif // EDL_HANDLER_H
