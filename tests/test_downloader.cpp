#include <QtTest>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QFile>
#include <QDir>
#include <QSignalSpy>
#include <QNetworkProxy>
#include <QHostAddress>
#include <QDeadlineTimer>
#include "root_patcher/assets_downloader.h"

namespace {

// 轮询等待条件成立（固定 qWait 在慢 CI 上可能 flaky，需驱动事件循环）
template <typename Fn>
void waitFor(QDeadlineTimer deadline, Fn &&cond)
{
    while (!cond() && !deadline.hasExpired())
        QTest::qWait(10);
}

} // namespace

namespace {

// 本地 mock HTTP 服务器：不依赖真实网络。单连接单请求，收齐请求头后
// 回固定响应并关闭（HTTP/1.0 + Connection: close）。
class MockHttpServer : public QObject
{
    Q_OBJECT
public:
    MockHttpServer(int statusCode, const QByteArray &body, QObject *parent = nullptr)
        : QObject(parent), m_statusCode(statusCode), m_body(body)
    {
        connect(&m_server, &QTcpServer::newConnection, this, &MockHttpServer::onNewConnection);
        m_server.listen(QHostAddress::LocalHost, 0);
    }

    bool isListening() const { return m_server.isListening(); }
    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1/asset.bin").arg(m_server.serverPort());
    }
    int requestCount() const { return m_requests; }
    // 置 false 后：收齐请求头但永不响应（模拟挂起下载，供取消测试用）
    void setRespond(bool respond) { m_respond = respond; }

private slots:
    void onNewConnection()
    {
        while (m_server.hasPendingConnections()) {
            auto *sock = m_server.nextPendingConnection();
            connect(sock, &QTcpSocket::readyRead, this, [this, sock]() {
                m_buf[sock] += sock->readAll();
                if (!m_buf.value(sock).contains("\r\n\r\n"))
                    return; // 请求头未收齐，等待后续数据
                ++m_requests;
                if (!m_respond)
                    return; // 挂起模式：收下请求但不回复
                QByteArray resp;
                resp += "HTTP/1.0 " + QByteArray::number(m_statusCode);
                resp += (m_statusCode == 200) ? " OK\r\n" : " Error\r\n";
                if (m_statusCode == 200) {
                    resp += "Content-Type: application/octet-stream\r\n";
                    resp += "Content-Length: " + QByteArray::number(m_body.size()) + "\r\n";
                }
                resp += "Connection: close\r\n\r\n";
                if (m_statusCode == 200)
                    resp += m_body;
                sock->write(resp);
                sock->disconnectFromHost();
                m_buf.remove(sock);
            });
            connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        }
    }

private:
    QTcpServer m_server;
    int m_statusCode;
    QByteArray m_body;
    QHash<QTcpSocket *, QByteArray> m_buf;
    int m_requests = 0;
    bool m_respond = true;
};

// 测试期间强制不走系统代理（本地 127.0.0.1 也可能被 CI 环境 http_proxy 劫持），
// 退出时恢复。
class NoProxyScope
{
public:
    NoProxyScope() : m_old(QNetworkProxy::applicationProxy())
    {
        QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);
    }
    ~NoProxyScope() { QNetworkProxy::setApplicationProxy(m_old); }

private:
    QNetworkProxy m_old;
};

} // namespace

class TestDownloader : public QObject
{
    Q_OBJECT
private slots:
    void manualFileShortcut();
    void cacheWriteRead();
    void cacheHitEmitsFinished();
    void downloadOverLocalServer();
    void downloadFailureCleansPartial();
    void cancelAbortsDownload();
    void cancelTwoConcurrentDownloads();
};

void TestDownloader::manualFileShortcut()
{
    patcher::AssetsDownloader dl;
    const QString f = "/tmp/fake-magisk.apk";
    dl.setManualFile("magisk", f);
    QCOMPARE(dl.manualFile("magisk"), f);
    QVERIFY(dl.hasCached("magisk")); // 手动指定视为可用
}

void TestDownloader::cacheWriteRead()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    patcher::AssetsDownloader dl;
    dl.setCacheDir(dir.path());
    // 模拟已存在的缓存文件（key 目录须先创建）
    QVERIFY(QDir().mkpath(dir.path() + "/magisk-v30.7"));
    QFile f(dir.path() + "/magisk-v30.7/magisk.apk");
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("fake");
    f.close();
    QVERIFY(dl.hasCached("magisk-v30.7"));
    QVERIFY(QFile::exists(dl.cachedPath("magisk-v30.7")));
}

void TestDownloader::cacheHitEmitsFinished()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    patcher::AssetsDownloader dl;
    dl.setCacheDir(dir.path());
    QVERIFY(QDir().mkpath(dir.path() + "/ksu-v3.2.5"));
    const QString cached = dir.path() + "/ksu-v3.2.5/KernelSU_v3.2.5_32525-release.apk";
    QFile f(cached);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("cached bytes");
    f.close();

    QSignalSpy ok(&dl, &patcher::AssetsDownloader::downloadFinished);
    QSignalSpy fail(&dl, &patcher::AssetsDownloader::downloadFailed);
    // 指向不可达地址的 URL：命中缓存即直接成功完成，绝不发起网络请求
    dl.downloadAsync(QUrl("http://127.0.0.1:1/unreachable.apk"), "ksu-v3.2.5");
    QVERIFY(ok.wait(5000));
    QCOMPARE(ok.count(), 1);
    QCOMPARE(fail.count(), 0);
    QCOMPARE(ok.first().at(0).toString(), "ksu-v3.2.5");
    QCOMPARE(ok.first().at(1).toString(), cached);
}

void TestDownloader::downloadOverLocalServer()
{
    // 不走真实网络：QTcpServer 本地 mock
    NoProxyScope noProxy;
    MockHttpServer server(200, QByteArray("mock-magisk-apk-bytes"));
    QVERIFY(server.isListening());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    patcher::AssetsDownloader dl;
    dl.setCacheDir(dir.path());

    QSignalSpy ok(&dl, &patcher::AssetsDownloader::downloadFinished);
    QSignalSpy fail(&dl, &patcher::AssetsDownloader::downloadFailed);
    dl.downloadAsync(QUrl(server.baseUrl()), "magisk-v30.7");
    QVERIFY2(ok.wait(10000), "下载完成信号未发出");
    QCOMPARE(fail.count(), 0);
    QCOMPARE(ok.first().at(0).toString(), "magisk-v30.7");
    const QString path = ok.first().at(1).toString();
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), QByteArray("mock-magisk-apk-bytes"));
    f.close();
    QVERIFY(dl.hasCached("magisk-v30.7"));
    QCOMPARE(dl.cachedPath("magisk-v30.7"), path);
    QCOMPARE(server.requestCount(), 1);
}

void TestDownloader::downloadFailureCleansPartial()
{
    NoProxyScope noProxy;
    MockHttpServer server(500, QByteArray());
    QVERIFY(server.isListening());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    patcher::AssetsDownloader dl;
    dl.setCacheDir(dir.path());

    QSignalSpy ok(&dl, &patcher::AssetsDownloader::downloadFinished);
    QSignalSpy fail(&dl, &patcher::AssetsDownloader::downloadFailed);
    dl.downloadAsync(QUrl(server.baseUrl()), "magisk-bad");
    QVERIFY2(fail.wait(10000), "下载失败信号未发出");
    QCOMPARE(ok.count(), 0);
    QCOMPARE(fail.first().at(0).toString(), "magisk-bad");
    // 失败后不得残留部分文件（.part 须被清理）
    const QDir d(dir.path() + "/magisk-bad");
    QVERIFY(d.entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
}

void TestDownloader::cancelAbortsDownload()
{
    NoProxyScope noProxy;
    MockHttpServer server(200, QByteArray("never delivered"), nullptr);
    server.setRespond(false); // 永不响应：制造挂起下载
    QVERIFY(server.isListening());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    patcher::AssetsDownloader dl;
    dl.setCacheDir(dir.path());

    QSignalSpy ok(&dl, &patcher::AssetsDownloader::downloadFinished);
    QSignalSpy fail(&dl, &patcher::AssetsDownloader::downloadFailed);
    dl.downloadAsync(QUrl(server.baseUrl()), "slow-key");
    waitFor(QDeadlineTimer(5000), [&]() { return server.requestCount() >= 1; });
    QCOMPARE(server.requestCount(), 1);
    dl.cancel();

    // abort() 会在 cancel() 内同步触发 finished（信号先于 wait 已发出），
    // 而 Qt 6.11 的 QSignalSpy::wait() 只观察调用后的新信号 —— 两种时序都接受
    QVERIFY2(fail.count() > 0 || fail.wait(5000), "取消后失败信号未发出");
    QCOMPARE(ok.count(), 0);
    QCOMPARE(fail.first().at(0).toString(), "slow-key");
    // 取消后不得残留部分文件（.part 须被清理）
    const QDir d(dir.path() + "/slow-key");
    QVERIFY(d.entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
}

void TestDownloader::cancelTwoConcurrentDownloads()
{
    NoProxyScope noProxy;
    MockHttpServer server(200, QByteArray("never delivered"), nullptr);
    server.setRespond(false); // 永不响应：两个下载都挂起
    QVERIFY(server.isListening());

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    patcher::AssetsDownloader dl;
    dl.setCacheDir(dir.path());

    QSignalSpy ok(&dl, &patcher::AssetsDownloader::downloadFinished);
    QSignalSpy fail(&dl, &patcher::AssetsDownloader::downloadFailed);
    dl.downloadAsync(QUrl(server.baseUrl()), "slow-key-a");
    dl.downloadAsync(QUrl(server.baseUrl()), "slow-key-b");
    waitFor(QDeadlineTimer(5000), [&]() { return server.requestCount() >= 2; });
    QCOMPARE(server.requestCount(), 2);
    dl.cancel(); // 并发在途下载整体取消：迭代 m_replies 的同时 abort 会触发同步 remove

    QVERIFY2(fail.count() == 2 || fail.wait(5000), "两个取消失败信号未收齐");
    QCOMPARE(ok.count(), 0);
    QList<QString> keys;
    for (const auto &args : fail)
        keys << args.at(0).toString();
    keys.sort();
    QCOMPARE(keys, QList<QString>({QStringLiteral("slow-key-a"), QStringLiteral("slow-key-b")}));
    // 两个 key 的部分文件均须清理
    for (const QString &k : keys) {
        const QDir d(dir.path() + "/" + k);
        QVERIFY(d.entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty());
    }
}

// 注意：须用 QTEST_GUILESS_MAIN（QCoreApplication + 事件循环）。
// Qt 6.11 起 QTEST_APPLESS_MAIN 不创建 QCoreApplication，QSignalSpy::wait
// 与 QNetworkAccessManager 都无法工作。
QTEST_GUILESS_MAIN(TestDownloader)
#include "test_downloader.moc"
