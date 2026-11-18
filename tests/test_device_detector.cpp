#include <QtTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QElapsedTimer>
#include <QThread>

#include "core/device_detector.h"

// 性能回归用例（H2）：
//   1) forceRefresh() 不得阻塞调用线程（探测在后台线程执行）；
//   2) 同一台设备连续两轮轮询，第二轮不得重复查询设备详情（缓存）；
//   3) 首次采集设备详情时，getprop 必须一次 dump 拿全，不得逐属性开进程。
//
// 用假 adb / fastboot 复现"设备在线"：每次 shell 调用 sleep 50ms。改造前
// forceRefresh 同步执行 13 条命令（约 650ms），以上三条都会失败。
//
// 假脚本是 POSIX shell —— Windows 上跳过（本用例的验证对象是 Linux 开发机上的卡顿）。
#ifdef Q_OS_WIN
#define PT_FAKE_TOOLS_SUPPORTED 0
#else
#define PT_FAKE_TOOLS_SUPPORTED 1
#endif

class TestDeviceDetector : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void refreshDoesNotBlockCaller();
    void secondPollReusesCachedDeviceInfo();
    void propDumpFetchedOnce();

private:
    int countAdbLines(const QString &suffix) const;
    void writeExecutable(const QString &path, const QByteArray &body);

    QTemporaryDir m_dir;
    QString m_logPath;
};

void TestDeviceDetector::writeExecutable(const QString &path, const QByteArray &body)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(body);
    f.close();
#ifndef Q_OS_WIN
    QFile::setPermissions(path, QFile::permissions(path)
                                  | QFileDevice::ExeOwner | QFileDevice::ExeGroup
                                  | QFileDevice::ExeOther);
#endif
}

void TestDeviceDetector::initTestCase()
{
    if (!PT_FAKE_TOOLS_SUPPORTED)
        QSKIP("fake adb/fastboot scripts need a POSIX shell");

    QVERIFY(m_dir.isValid());
    const QString toolsDir = m_dir.filePath(QStringLiteral("sdk/platform-tools"));
    QVERIFY(QDir().mkpath(toolsDir));
    m_logPath = m_dir.filePath(QStringLiteral("adb-calls.log"));

    // 每次调用先落一行日志；shell 调用统一 sleep 50ms 模拟真机往返。
    writeExecutable(toolsDir + QStringLiteral("/adb"), QByteArray(R"SH(#!/bin/sh
printf 'adb: %s\n' "$*" >> "$FAKE_ADB_LOG"
if [ "$*" = "devices -l" ]; then
  printf 'FAKE123                device product:test model:Test_Phone device:test transport_id:1\n'
  exit 0
fi
case "$*" in
  *"-s FAKE123 shell getprop")
    sleep 0.05
    cat <<'EOF'
[ro.product.manufacturer]: [TestCorp]
[ro.product.model]: [Test Phone]
[ro.product.device]: [test]
[ro.build.version.release]: [14]
[ro.build.display.id]: [TEST.1.0]
[ro.build.version.sdk]: [34]
[ro.build.version.security_patch]: [2026-01-01]
[ro.build.fingerprint]: [test/fingerprint]
EOF
    ;;
  *"shell getprop "*)  sleep 0.05; echo "fallback" ;;
  *"shell su -c id")   sleep 0.05; echo "uid=0(root) gid=0(root)" ;;
  *"shell service call"*) sleep 0.05; echo "00000000" ;;
  *"shell cat /proc/cpuinfo"*) sleep 0.05; echo "8" ;;
  *"shell cat /proc/meminfo"*) sleep 0.05; echo "MemTotal:        8000000 kB" ;;
  *"shell dumpsys battery"*)   sleep 0.05; echo "  level: 80" ;;
  *) sleep 0.05; echo "" ;;
esac
exit 0
)SH"));

    writeExecutable(toolsDir + QStringLiteral("/fastboot"), QByteArray(R"SH(#!/bin/sh
if [ "$*" = "devices -l" ]; then exit 0; fi
echo "FAILED (remote: 'unknown command')" >&2
exit 1
)SH"));

    qputenv("ANDROID_HOME", m_dir.filePath(QStringLiteral("sdk")).toUtf8());
    qputenv("FAKE_ADB_LOG", m_logPath.toUtf8());
}

void TestDeviceDetector::init()
{
    if (!PT_FAKE_TOOLS_SUPPORTED)
        QSKIP("fake adb/fastboot scripts need a POSIX shell");
    QFile::remove(m_logPath);
}

int TestDeviceDetector::countAdbLines(const QString &suffix) const
{
    QFile f(m_logPath);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    int count = 0;
    const QByteArray needle = QByteArray("adb: ") + suffix.toUtf8();
    for (const QByteArray &line : f.readAll().split('\n')) {
        if (line == needle)
            ++count;
    }
    return count;
}

void TestDeviceDetector::refreshDoesNotBlockCaller()
{
    DeviceDetector detector;
    QSignalSpy connected(&detector, &DeviceDetector::deviceConnected);

    detector.startMonitoring();

    QElapsedTimer timer;
    timer.start();
    detector.forceRefresh();
    const qint64 elapsedMs = timer.elapsed();

    QVERIFY2(elapsedMs < 100,
             qPrintable(QStringLiteral("forceRefresh 阻塞调用线程 %1ms（应提交到后台线程）")
                            .arg(elapsedMs)));
    QTRY_VERIFY_WITH_TIMEOUT(connected.count() >= 1, 5000);

    detector.stopMonitoring();
}

void TestDeviceDetector::secondPollReusesCachedDeviceInfo()
{
    DeviceDetector detector;
    QSignalSpy connected(&detector, &DeviceDetector::deviceConnected);

    detector.startMonitoring();
    detector.forceRefresh();
    QTRY_VERIFY_WITH_TIMEOUT(connected.count() >= 1, 5000);
    // 等第一轮把详情查询做完（同步路径下 13 条命令约 650ms）。
    QTest::qWait(800);

    const int firstRound = countAdbLines(QStringLiteral("-s FAKE123 shell su -c id"));
    QVERIFY2(firstRound >= 1, "第一轮没有采集设备详情，用例前提不成立");

    detector.forceRefresh();
    QTest::qWait(1000);

    const int secondRound = countAdbLines(QStringLiteral("-s FAKE123 shell su -c id"));
    QCOMPARE(secondRound, firstRound);

    detector.stopMonitoring();
}

void TestDeviceDetector::propDumpFetchedOnce()
{
    DeviceDetector detector;
    QSignalSpy connected(&detector, &DeviceDetector::deviceConnected);

    detector.startMonitoring();
    detector.forceRefresh();
    QTRY_VERIFY_WITH_TIMEOUT(connected.count() >= 1, 5000);
    QTest::qWait(500);

    // 一次 `getprop` dump 拿全部属性；逐属性命令一次都不该出现。
    QCOMPARE(countAdbLines(QStringLiteral("-s FAKE123 shell getprop")), 1);
    QCOMPARE(countAdbLines(QStringLiteral("-s FAKE123 shell getprop ro.product.manufacturer")), 0);

    detector.stopMonitoring();
}

QTEST_GUILESS_MAIN(TestDeviceDetector)
#include "test_device_detector.moc"
