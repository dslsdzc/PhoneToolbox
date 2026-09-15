// src/core/modes/mtk_preloader_download_qt.cpp
//
// 生产下载器（Qt Network）。**本文件永不进测试目标**（见 CMakeLists 的 test_mtk_preloader_fetch
// 分支：只编 mtk_preloader_fetch.cpp）—— 用例里的下载器一律是注入的 lambda，测试不联网。
//
// 行为：同步 GET（嵌套 QEventLoop）+ 单次超时；超时 → abort；网络/HTTP 错误 → false + error。
// 重定向策略 NoLessSafeRedirectPolicy（不降级到 http）。**不做任何信任判断** —— 校验在
// resolvePreloader（sha256 fail-closed）。
#include "core/modes/mtk_preloader_download_qt.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace mtkbrom {

PreloaderDownloader makeQtPreloaderDownloader(int timeoutMs)
{
    return [timeoutMs](const PreloaderSource &src, QByteArray *out, QString *error) -> bool {
        QNetworkAccessManager nam;
        QNetworkRequest req{QUrl(src.url)};
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply *reply = nam.get(req);
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(timeoutMs);
        loop.exec();
        if (!timer.isActive()) {                 // 定时器先响 = 超时（正常完成路径上它仍在跑）
            reply->abort();
            if (error) *error = QStringLiteral("下载超时（%1 ms）").arg(timeoutMs);
            reply->deleteLater();
            return false;
        }
        if (reply->error() != QNetworkReply::NoError) {
            if (error) *error = reply->errorString();
            reply->deleteLater();
            return false;
        }
        *out = reply->readAll();
        reply->deleteLater();
        return true;
    };
}

} // namespace mtkbrom
