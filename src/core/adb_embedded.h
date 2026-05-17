#ifndef ADB_EMBEDDED_H
#define ADB_EMBEDDED_H

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTemporaryDir>

class AdbEmbedded : public QObject
{
    Q_OBJECT

public:
    static AdbEmbedded& instance();

    bool initialize();
    QString executeCommand(const QString &command, int timeout = 30000);
    QString getDeviceInfo(const QString &serial, const QString &prop);

    QString getAdbPath() const;
    QString getFastbootPath() const;

    // 状态查询
    bool isSystemAdb() const { return m_useSystemAdb; }
    bool isDownloaded() const { return m_downloaded; }

private:
    AdbEmbedded(QObject *parent = nullptr);
    ~AdbEmbedded();

    bool findSystemAdb();
    bool downloadPlatformTools();
    bool extractZip(const QString &zipPath, const QString &destDir);

    QTemporaryDir m_tempDir;
    QString m_adbPath;
    QString m_fastbootPath;
    bool m_initialized = false;
    bool m_useSystemAdb = false;
    bool m_downloaded = false;
};

#endif // ADB_EMBEDDED_H
