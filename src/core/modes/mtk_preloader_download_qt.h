#pragma once

// 生产下载器：QNetworkAccessManager（同步等待 + 超时）。与纯模块**分开一个文件**，是为了让
// mtk_preloader_fetch 的测试目标不必链 Qt Network（用例全程用注入的 lambda，不联网）。
//
// 只在用户**显式**开启网络获取（PreloaderOptions::allowNetwork）且配置了来源清单时才会被调用；
// 拿到字节后一律由 resolvePreloader 做 sha256 校验（本下载器**不做**任何信任判断）。
#include "core/modes/mtk_preloader_fetch.h"

namespace mtkbrom {

// 返回的 PreloaderDownloader：超时/网络错误/HTTP 错误 → false + error（中文诊断）。
PreloaderDownloader makeQtPreloaderDownloader(int timeoutMs = 30000);

} // namespace mtkbrom
