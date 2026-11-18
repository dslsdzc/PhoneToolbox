#include <QtTest>
#include <QTemporaryDir>
#include <QFile>

#include "core/process_runner.h"

// 进程成败判定回归：
//   以前 FlashTool 只看输出里有没有 FAILED —— 进程没启动、超时、退出码非 0
//   但输出干净时都会被当成成功。这里把四种情况钉死。
class TestProcessRunner : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void successWhenExitZero();
    void failsWhenExitNonZero();
    void failsWhenProgramMissing();
    void failsOnTimeout();

private:
    QString writeScript(const QString &name, const QByteArray &body);

    QTemporaryDir m_dir;
};

QString TestProcessRunner::writeScript(const QString &name, const QByteArray &body)
{
    const QString path = m_dir.filePath(name);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(body);
    f.close();
#ifndef Q_OS_WIN
    QFile::setPermissions(path, QFile::permissions(path)
                                  | QFileDevice::ExeOwner | QFileDevice::ExeGroup
                                  | QFileDevice::ExeOther);
#endif
    return path;
}

void TestProcessRunner::initTestCase()
{
    QVERIFY(m_dir.isValid());
#ifdef Q_OS_WIN
    QSKIP("这些假脚本需要 POSIX shell");
#endif
}

void TestProcessRunner::successWhenExitZero()
{
    const QString tool = writeScript(QStringLiteral("ok.sh"),
                                     "#!/bin/sh\necho hello\n");
    QVERIFY(!tool.isEmpty());

    const ProcessResult result = runExternalProcess(tool, {}, 5000);
    QVERIFY(result.started);
    QVERIFY(!result.timedOut);
    QCOMPARE(result.exitCode, 0);
    QVERIFY(result.success());
    QCOMPARE(result.stdoutText.trimmed(), QStringLiteral("hello"));
}

void TestProcessRunner::failsWhenExitNonZero()
{
    const QString tool = writeScript(QStringLiteral("fail.sh"),
                                     "#!/bin/sh\necho boom >&2\nexit 3\n");
    QVERIFY(!tool.isEmpty());

    const ProcessResult result = runExternalProcess(tool, {}, 5000);
    QVERIFY(result.started);
    QVERIFY(!result.timedOut);
    QCOMPARE(result.exitCode, 3);
    QVERIFY(!result.success()); // 关键：输出里没有 FAILED 也必须判失败
    QVERIFY(result.errorSummary().contains(QStringLiteral("退出码 3")));
    QVERIFY(result.errorSummary().contains(QStringLiteral("boom")));
}

void TestProcessRunner::failsWhenProgramMissing()
{
    const ProcessResult result = runExternalProcess(
        m_dir.filePath(QStringLiteral("does-not-exist")), {}, 1000);
    QVERIFY(!result.started);
    QVERIFY(!result.success());
    QVERIFY(!result.errorSummary().isEmpty());
}

void TestProcessRunner::failsOnTimeout()
{
    const QString tool = writeScript(QStringLiteral("slow.sh"),
                                     "#!/bin/sh\nsleep 5\n");
    QVERIFY(!tool.isEmpty());

    const ProcessResult result = runExternalProcess(tool, {}, 200);
    QVERIFY(result.started);
    QVERIFY(result.timedOut);
    QVERIFY(!result.success()); // 超时绝不能算成功
    QVERIFY(result.errorSummary().contains(QStringLiteral("超时")));
}

QTEST_APPLESS_MAIN(TestProcessRunner)
#include "test_process_runner.moc"
