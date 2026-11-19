#ifndef ROOT_AGENT_INSTALLER_H
#define ROOT_AGENT_INSTALLER_H

#include <QObject>
#include <QString>

// 在桌面端内部完成：安装固定包名的模板 APK（已签名）。
// 不依赖 tools/*.sh、Gradle、apktool 或项目源码目录。
class RootAgentInstaller : public QObject
{
    Q_OBJECT

public:
    explicit RootAgentInstaller(QObject *parent = nullptr);

    // 异步执行；完成后发 finished。
    void start(const QString &serial, const QString &adbPath);

signals:
    void logMessage(const QString &text);
    void finished(bool ok, const QString &packageId, const QString &error);

private:
    bool run(const QString &serial, const QString &adbPath,
             QString *packageId, QString *error);
    QString findTemplateApk() const;
    bool runProcess(const QStringList &programAndArgs, int timeoutMs,
                    QString *output, QString *error) const;
};

#endif // ROOT_AGENT_INSTALLER_H
